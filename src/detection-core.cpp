/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "detection-core.hpp"
#include "bible.hpp"

#include <whisper.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

DetectionCore::DetectionCore(QObject *parent) : QObject(parent)
{
	m_ringBuffer.resize(kBufferSize, 0.0f);
	const char *debugEnv = getenv("LDWW_DEBUG");
	if (debugEnv && (debugEnv[0] == '1' || debugEnv[0] == 'y' || debugEnv[0] == 'Y')) {
		m_debugLogging.store(true, std::memory_order_relaxed);
	}
	startWorker();
}

DetectionCore::~DetectionCore()
{
	stopWorker();
	std::lock_guard<std::mutex> lock(m_whisperMutex);
	if (m_whisperCtx) {
		whisper_free(m_whisperCtx);
		m_whisperCtx = nullptr;
	}
}

int DetectionCore::levenshtein(const QString &s1, const QString &s2)
{
	const int m = static_cast<int>(s1.length());
	const int n = static_cast<int>(s2.length());
	if (m == 0)
		return n;
	if (n == 0)
		return m;

	std::vector<int> v0(n + 1);
	std::vector<int> v1(n + 1);

	for (int i = 0; i <= n; ++i)
		v0[i] = i;

	for (int i = 0; i < m; ++i) {
		v1[0] = i + 1;
		for (int j = 0; j < n; ++j) {
			int cost = (s1[i] == s2[j]) ? 0 : 1;
			v1[j + 1] = std::min({v1[j] + 1, v0[j + 1] + 1, v0[j] + cost});
		}
		v0 = v1;
	}

	return v0[n];
}

bool DetectionCore::loadModel(const QString &path)
{
	if (path.isEmpty() || !QFile::exists(path)) {
		m_modelLoaded.store(false, std::memory_order_relaxed);
		m_modelStatusText = QStringLiteral("Model file not found on disk");
		emit modelLoadedChanged(false, m_modelStatusText);
		return false;
	}

	std::lock_guard<std::mutex> lock(m_whisperMutex);
	if (m_whisperCtx) {
		whisper_free(m_whisperCtx);
		m_whisperCtx = nullptr;
	}

	whisper_context_params cparams = whisper_context_default_params();
	m_whisperCtx = whisper_init_from_file_with_params(path.toUtf8().constData(), cparams);

	if (m_whisperCtx) {
		m_modelLoaded.store(true, std::memory_order_relaxed);
		m_modelPath = path;

		const char *sysinfo = whisper_print_system_info();
		if (sysinfo) {
			m_backendInfo = QString::fromUtf8(sysinfo).trimmed();
		}

		QFileInfo fi(path);
		m_modelStatusText = QStringLiteral("Active: %1").arg(fi.fileName());

		// Model warm-up on background thread: run 1 second of silence with audio_ctx=150
		std::vector<float> warmup(16000, 0.0f);
		whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		wp.audio_ctx = 150;
		wp.single_segment = true;
		wp.print_progress = false;
		wp.print_special = false;
		wp.print_realtime = false;
		wp.print_timestamps = false;
		wp.language = "en";
		wp.n_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);
		whisper_full(m_whisperCtx, wp, warmup.data(), static_cast<int>(warmup.size()));

		fprintf(stderr, "[LD Worship & Word] Model loaded: %s\n", path.toUtf8().constData());
		if (!m_backendInfo.isEmpty()) {
			fprintf(stderr, "[LD Worship & Word] Whisper backend info: %s\n",
				m_backendInfo.toUtf8().constData());
		}

		emit modelLoadedChanged(true, m_modelStatusText);
		return true;
	} else {
		m_modelLoaded.store(false, std::memory_order_relaxed);
		m_modelStatusText = QStringLiteral("Failed to initialize Whisper context");
		emit modelLoadedChanged(false, m_modelStatusText);
		return false;
	}
}

bool DetectionCore::isModelLoaded() const
{
	return m_modelLoaded.load(std::memory_order_relaxed);
}

QString DetectionCore::modelStatusText() const
{
	return m_modelStatusText;
}

QString DetectionCore::currentModelPath() const
{
	return m_modelPath;
}

QString DetectionCore::backendInfo() const
{
	return m_backendInfo;
}

void DetectionCore::setChannelMode(Channel ch)
{
	m_channel.store(ch, std::memory_order_relaxed);
}

Channel DetectionCore::channelMode() const
{
	return m_channel.load(std::memory_order_relaxed);
}

void DetectionCore::setDebugLogging(bool enabled)
{
	m_debugLogging.store(enabled, std::memory_order_relaxed);
}

bool DetectionCore::debugLogging() const
{
	return m_debugLogging.load(std::memory_order_relaxed);
}

void DetectionCore::setFollowMode(bool enabled)
{
	m_followMode.store(enabled, std::memory_order_relaxed);
}

bool DetectionCore::followMode() const
{
	return m_followMode.load(std::memory_order_relaxed);
}

void DetectionCore::setAutoAdvance(bool enabled)
{
	m_autoAdvance.store(enabled, std::memory_order_relaxed);
}

bool DetectionCore::autoAdvance() const
{
	return m_autoAdvance.load(std::memory_order_relaxed);
}

void DetectionCore::setActiveSong(const QString &title, int currentSlideIndex, const QString &nextSongTitle)
{
	std::lock_guard<std::mutex> lock(m_activeSongMutex);
	m_activeSongTitle = title.trimmed();
	m_activeSlideIndex = currentSlideIndex;
	m_nextSongTitle = nextSongTitle.trimmed();
	m_nextSlideConsecutiveMatches = 0;
}

void DetectionCore::clearActiveSong()
{
	std::lock_guard<std::mutex> lock(m_activeSongMutex);
	m_activeSongTitle.clear();
	m_activeSlideIndex = -1;
	m_nextSongTitle.clear();
	m_nextSlideConsecutiveMatches = 0;
}

void DetectionCore::pushPcmSamples(const float *samples, size_t count, double streamTimestampSec)
{
	if (!samples || count == 0)
		return;

	size_t w = m_writeIndex.load(std::memory_order_relaxed);
	for (size_t i = 0; i < count; i++) {
		m_ringBuffer[w] = samples[i];
		w = (w + 1) % kBufferSize;
	}
	m_writeIndex.store(w, std::memory_order_release);

	if (streamTimestampSec >= 0.0) {
		m_streamTimeSec.store(streamTimestampSec, std::memory_order_relaxed);
	} else {
		double current = m_streamTimeSec.load(std::memory_order_relaxed);
		m_streamTimeSec.store(current + (static_cast<double>(count) / 16000.0), std::memory_order_relaxed);
	}
}

void DetectionCore::reset()
{
	m_writeIndex.store(0, std::memory_order_release);
	m_readIndex.store(0, std::memory_order_release);
	m_streamTimeSec.store(0.0, std::memory_order_relaxed);
	m_lastSuggestedRef.clear();
	m_prevHypothesisWords.clear();
	m_committedWords.clear();
	m_tentativeWords.clear();
	m_contextBook.clear();
	m_contextChapter = 0;
	m_contextVerse = 0;
	m_contextStreamTime = -1000.0;
	m_contextWallTimeMs = 0;
	m_lastSuggestionWallTime = 0;
	m_lastSuggestionStreamTime = -100.0;
}

void DetectionCore::startWorker()
{
	m_workerRunning.store(true, std::memory_order_relaxed);
	m_workerThread = std::make_unique<std::thread>(&DetectionCore::workerLoop, this);
}

void DetectionCore::stopWorker()
{
	m_workerRunning.store(false, std::memory_order_relaxed);
	if (m_workerThread && m_workerThread->joinable()) {
		m_workerThread->join();
	}
	m_workerThread.reset();
}

void DetectionCore::workerLoop()
{
	// Phase 1: 3.0s window with 0.75s hop
	constexpr size_t kChunkSize = 48000; // 3.0 seconds @ 16kHz
	constexpr size_t kHopSize = 12000;   // 0.75 seconds @ 16kHz (75% overlap)

	std::vector<float> chunk(kChunkSize, 0.0f);

	while (m_workerRunning.load(std::memory_order_relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		size_t w = m_writeIndex.load(std::memory_order_acquire);
		size_t r = m_readIndex.load(std::memory_order_relaxed);

		size_t available = (w >= r) ? (w - r) : (kBufferSize - r + w);
		if (available < kChunkSize)
			continue;

		size_t tempR = r;
		for (size_t i = 0; i < kChunkSize; i++) {
			chunk[i] = m_ringBuffer[tempR];
			tempR = (tempR + 1) % kBufferSize;
		}

		r = (r + kHopSize) % kBufferSize;
		m_readIndex.store(r, std::memory_order_release);

		double streamNow = m_streamTimeSec.load(std::memory_order_relaxed);
		analyzeBuffer(chunk, streamNow);
	}
}

void DetectionCore::processWindowDirect(const std::vector<float> &chunk, double windowEndSec)
{
	analyzeBuffer(chunk, windowEndSec);
}

void DetectionCore::analyzeBuffer(const std::vector<float> &chunk, double windowEndSec)
{
	auto tStart = std::chrono::steady_clock::now();
	Channel ch = m_channel.load(std::memory_order_relaxed);

	// 1. Voice activity detection: check sub-blocks
	float maxSubRms = 0.0f;
	constexpr size_t subSize = 3200; // 200ms @ 16kHz
	for (size_t i = 0; i + subSize <= chunk.size(); i += subSize) {
		float subSum = 0.0f;
		for (size_t j = 0; j < subSize; ++j) {
			float s = chunk[i + j];
			subSum += s * s;
		}
		float subRms = sqrtf(subSum / static_cast<float>(subSize));
		if (subRms > maxSubRms)
			maxSubRms = subRms;
	}

	auto tVad = std::chrono::steady_clock::now();
	double vadMs = std::chrono::duration<double, std::milli>(tVad - tStart).count();
	emit stageTimed(QStringLiteral("VAD"), vadMs);

	// Phase 2 VAD: In Scripture mode threshold is 0.0030f; in Worship mode looser 0.0012f
	const float vadThreshold = (ch == Channel::Lyrics) ? 0.0012f : 0.0030f;
	if (maxSubRms < vadThreshold)
		return;

	// 2. Whisper transcription
	QString transcript = transcribeChunk(chunk);
	auto tWhisper = std::chrono::steady_clock::now();
	double whisperMs = std::chrono::duration<double, std::milli>(tWhisper - tVad).count();
	emit stageTimed(QStringLiteral("Whisper"), whisperMs);

	if (transcript.trimmed().isEmpty())
		return;

	// 3. LocalAgreement-2 transcript stabilizer
	QStringList hypWords = transcript.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (!hypWords.isEmpty()) {
		// Find agreement between previous hypothesis and current hypothesis
		int agreementCount = 0;
		const int maxCheck = std::min(m_prevHypothesisWords.size(), hypWords.size());
		for (int i = 0; i < maxCheck; ++i) {
			if (m_prevHypothesisWords[i].compare(hypWords[i], Qt::CaseInsensitive) == 0) {
				agreementCount++;
			} else {
				break;
			}
		}

		if (agreementCount > 0) {
			for (int i = 0; i < agreementCount; ++i) {
				m_committedWords.append(hypWords[i]);
			}
			// Keep committed words bounded to last 35 words
			if (m_committedWords.size() > 35) {
				m_committedWords = m_committedWords.mid(m_committedWords.size() - 35);
			}
			m_tentativeWords = hypWords.mid(agreementCount);
		} else {
			m_tentativeWords = hypWords;
		}

		m_prevHypothesisWords = hypWords;
	}

	const QString committedText = m_committedWords.join(QLatin1Char(' '));
	const QString tentativeText = m_tentativeWords.join(QLatin1Char(' '));
	const QString combinedText = committedText.isEmpty() ? tentativeText
							     : (committedText + QLatin1Char(' ') + tentativeText);

	emit transcriptCommitted(committedText, windowEndSec);
	if (!tentativeText.isEmpty()) {
		emit transcriptTentative(tentativeText, windowEndSec);
	}

	if (m_debugLogging.load(std::memory_order_relaxed)) {
		fprintf(stderr,
			"[LDWW Core] @ %.2fs: [Committed: \"%s\"] [Tentative: \"%s\"] (VAD=%.1fms, Whisper=%.1fms)\n",
			windowEndSec, committedText.toUtf8().constData(), tentativeText.toUtf8().constData(), vadMs,
			whisperMs);
	}

	// 4. Pattern matching on committed + tentative
	auto tMatchStart = std::chrono::steady_clock::now();

	if (ch == Channel::Scripture) {
		if (!detectScripture(combinedText, windowEndSec, false)) {
			detectLyrics(combinedText, windowEndSec, false);
		}
	} else {
		if (!detectLyrics(combinedText, windowEndSec, false)) {
			detectScripture(combinedText, windowEndSec, false);
		}
	}

	auto tMatchEnd = std::chrono::steady_clock::now();
	double matchMs = std::chrono::duration<double, std::milli>(tMatchEnd - tMatchStart).count();
	emit stageTimed(QStringLiteral("Matcher"), matchMs);
}

QString DetectionCore::transcribeChunk(const std::vector<float> &chunk)
{
	std::lock_guard<std::mutex> lock(m_whisperMutex);
	if (!m_whisperCtx)
		return QString();

	whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	wparams.strategy = WHISPER_SAMPLING_GREEDY;
	wparams.temperature_inc = 0.0f;
	wparams.no_timestamps = true;
	wparams.token_timestamps = true;
	wparams.single_segment = true;
	wparams.suppress_blank = true;
	wparams.no_speech_thold = 0.6f;
	wparams.print_progress = false;
	wparams.print_special = false;
	wparams.print_realtime = false;
	wparams.print_timestamps = false;
	wparams.language = "en";
	wparams.translate = false;
	wparams.no_context = true;
	wparams.n_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);

	// Phase 1: audio_ctx matches 3.0s window length (3000ms / 20 = 150 frames)
	// Stops Whisper from padding every window to 30 seconds!
	wparams.audio_ctx = 150;

	// Phase 2: Short neutral prompt + last ~20 committed words for context
	static const QString basePrompt =
		QStringLiteral("Sermon scripture reading and hymn lyrics like Romans 8:28, John 3:16. ");
	QString contextPrompt = basePrompt;
	if (!m_committedWords.isEmpty()) {
		int start = std::max(0, static_cast<int>(m_committedWords.size()) - 20);
		contextPrompt += m_committedWords.mid(start).join(QLatin1Char(' '));
	}
	QByteArray promptBytes = contextPrompt.toUtf8();
	wparams.initial_prompt = promptBytes.constData();

	if (whisper_full(m_whisperCtx, wparams, chunk.data(), static_cast<int>(chunk.size())) != 0) {
		return QString();
	}

	QString result;
	int n_segments = whisper_full_n_segments(m_whisperCtx);
	for (int i = 0; i < n_segments; i++) {
		const char *text = whisper_full_get_segment_text(m_whisperCtx, i);
		if (text)
			result += QString::fromUtf8(text) + QStringLiteral(" ");
	}
	return result.trimmed();
}

QString DetectionCore::normalizeSpokenText(QString s)
{
	s = s.toLower();
	s.replace(QRegularExpression(QStringLiteral("[,.:;?!\\-\\(\\)—_]")), QStringLiteral(" "));

	// Roman numerals before Bible books: e.g. "ii timothy" -> "2 timothy"
	s.replace(QRegularExpression(QStringLiteral(
			  "\\bi\\s+(?=(?:corinthians|samuel|kings|chronicles|thessalonians|timothy|peter|john))")),
		  QStringLiteral("1 "));
	s.replace(QRegularExpression(QStringLiteral(
			  "\\bii\\s+(?=(?:corinthians|samuel|kings|chronicles|thessalonians|timothy|peter|john))")),
		  QStringLiteral("2 "));
	s.replace(QRegularExpression(QStringLiteral("\\biii\\s+(?=(?:john))")), QStringLiteral("3 "));

	// Saint / St prefixes
	s.replace(QRegularExpression(
			  QStringLiteral("\\b(?:st|saint)\\s+(?=(?:matthew|mark|luke|john|peter|paul|james|jude))")),
		  QStringLiteral(""));

	// Ordinals to digits
	s.replace(QRegularExpression(QStringLiteral("\\bfirst\\b")), QStringLiteral("1"));
	s.replace(QRegularExpression(QStringLiteral("\\bsecond\\b")), QStringLiteral("2"));
	s.replace(QRegularExpression(QStringLiteral("\\bthird\\b")), QStringLiteral("3"));
	s.replace(QRegularExpression(QStringLiteral("\\bfourth\\b")), QStringLiteral("4"));
	s.replace(QRegularExpression(QStringLiteral("\\bfifth\\b")), QStringLiteral("5"));
	s.replace(QRegularExpression(QStringLiteral("\\b1st\\b")), QStringLiteral("1"));
	s.replace(QRegularExpression(QStringLiteral("\\b2nd\\b")), QStringLiteral("2"));
	s.replace(QRegularExpression(QStringLiteral("\\b3rd\\b")), QStringLiteral("3"));
	s.replace(QRegularExpression(QStringLiteral("\\b4th\\b")), QStringLiteral("4"));

	// Numbers to digits
	static const struct {
		const char *word;
		int val;
	} kNumWords[] = {{"one", 1},      {"two", 2},        {"three", 3},     {"four", 4},      {"five", 5},
			 {"six", 6},      {"seven", 7},      {"eight", 8},     {"nine", 9},      {"ten", 10},
			 {"eleven", 11},  {"twelve", 12},    {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15},
			 {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20},
			 {"thirty", 30},  {"forty", 40},     {"fifty", 50},    {"sixty", 60},    {"seventy", 70},
			 {"eighty", 80},  {"ninety", 90},    {"hundred", 100}};
	for (const auto &nw : kNumWords) {
		s.replace(QRegularExpression(QStringLiteral("\\b%1\\b").arg(nw.word)), QString::number(nw.val));
	}

	// Composite numbers: "20 8" -> "28"
	static const int tens[] = {20, 30, 40, 50, 60, 70, 80, 90};
	for (int t : tens) {
		for (int u = 1; u <= 9; ++u) {
			s.replace(QStringLiteral("%1 %2").arg(t).arg(u), QString::number(t + u));
		}
	}

	// Inverted composite numbers: e.g. "8 and 20" -> "28"
	for (int t : tens) {
		for (int u = 1; u <= 9; ++u) {
			s.replace(QStringLiteral("%1 and %2").arg(u).arg(t), QString::number(t + u));
		}
	}

	// Hundred composites: "100 21" -> "121", "100 and 21" -> "121"
	for (int u = 1; u <= 99; ++u) {
		s.replace(QStringLiteral("100 and %1").arg(u), QString::number(100 + u));
		s.replace(QStringLiteral("100 %1").arg(u), QString::number(100 + u));
		s.replace(QStringLiteral("1 100 %1").arg(u), QString::number(100 + u));
		s.replace(QStringLiteral("1 100 and %1").arg(u), QString::number(100 + u));
	}

	// Verse range normalizations: "verse 16 to 18" -> "verse 16"
	s.replace(QRegularExpression(QStringLiteral("\\bverses?\\s*(\\d+)\\s*(?:to|through|-)\\s*\\d+")),
		  QStringLiteral("verse \\1"));
	s.replace(QRegularExpression(QStringLiteral("\\bfrom\\s+verses?\\s*(\\d+)")), QStringLiteral("verse \\1"));

	return s.simplified();
}

bool DetectionCore::detectScripture(const QString &rawTranscript, double streamTimeSec, bool isTentative)
{
	uint64_t wallNow = QDateTime::currentMSecsSinceEpoch();

	// 20-second cooldown on re-suggesting the exact same reference
	if (streamTimeSec > 0.0) {
		if (streamTimeSec - m_lastSuggestionStreamTime < 2.0)
			return false;
	} else {
		if (wallNow - m_lastSuggestionWallTime < 2000)
			return false;
	}

	QString text = normalizeSpokenText(rawTranscript);
	if (text.isEmpty())
		return false;

	// Phase 3: Context Memory for follow-on verses
	// Check if context is active (< 300 seconds / 5 minutes old)
	bool contextActive = false;
	if (!m_contextBook.isEmpty() && m_contextChapter > 0) {
		if (streamTimeSec > 0.0) {
			contextActive = (streamTimeSec - m_contextStreamTime) <= 300.0;
		} else {
			contextActive = (wallNow - m_contextWallTimeMs) <= 300000;
		}
	}

	if (contextActive) {
		// Pattern A: "verse 29", "v 29", "verse 29 says"
		QRegularExpression rxFollowVs(QStringLiteral("\\b(?:verse|v)\\s*(\\d+)(?:\\s*says)?\\b"));
		QRegularExpressionMatch mVs = rxFollowVs.match(text);
		if (mVs.hasMatch()) {
			int vs = mVs.captured(1).toInt();
			if (vs > 0 && vs != m_contextVerse) {
				QString ref =
					QStringLiteral("%1 %2:%3").arg(m_contextBook).arg(m_contextChapter).arg(vs);
				if (ref != m_lastSuggestedRef || (streamTimeSec - m_lastSuggestionStreamTime > 20.0)) {
					m_lastSuggestedRef = ref;
					m_contextVerse = vs;
					m_contextStreamTime = streamTimeSec;
					m_contextWallTimeMs = wallNow;
					m_lastSuggestionWallTime = wallNow;
					m_lastSuggestionStreamTime = streamTimeSec;

					SpeechSuggestion sugg;
					sugg.channel = Channel::Scripture;
					sugg.reference = ref;
					sugg.text = QStringLiteral("Follow-on verse from context");
					sugg.confidence = isTentative ? 80 : 95;
					sugg.timestampSeconds = streamTimeSec;
					sugg.tentative = isTentative;

					if (m_debugLogging.load(std::memory_order_relaxed)) {
						fprintf(stderr, "[LDWW Core] ==> CONTEXT FOLLOW-ON: %s (95%%)\n",
							ref.toUtf8().constData());
					}
					emit suggestionDetected(sugg);
					return true;
				}
			}
		}

		// Pattern B: "the next verse", "next verse"
		if (text.contains(QStringLiteral("next verse")) || text.contains(QStringLiteral("the next verse"))) {
			int nextVs = m_contextVerse + 1;
			QString ref = QStringLiteral("%1 %2:%3").arg(m_contextBook).arg(m_contextChapter).arg(nextVs);
			if (ref != m_lastSuggestedRef) {
				m_lastSuggestedRef = ref;
				m_contextVerse = nextVs;
				m_contextStreamTime = streamTimeSec;
				m_contextWallTimeMs = wallNow;
				m_lastSuggestionWallTime = wallNow;
				m_lastSuggestionStreamTime = streamTimeSec;

				SpeechSuggestion sugg;
				sugg.channel = Channel::Scripture;
				sugg.reference = ref;
				sugg.text = QStringLiteral("Next verse in passage");
				sugg.confidence = 94;
				sugg.timestampSeconds = streamTimeSec;
				sugg.tentative = isTentative;
				sugg.isNextSlide = true;

				emit suggestionDetected(sugg);
				return true;
			}
		}

		// Pattern C: "chapter 9", "chapter nine"
		QRegularExpression rxFollowCh(QStringLiteral("\\bchapter\\s*(\\d+)\\b"));
		QRegularExpressionMatch mCh = rxFollowCh.match(text);
		if (mCh.hasMatch()) {
			int ch = mCh.captured(1).toInt();
			if (ch > 0 && ch != m_contextChapter) {
				QString ref = QStringLiteral("%1 %2:1").arg(m_contextBook).arg(ch);
				if (ref != m_lastSuggestedRef) {
					m_lastSuggestedRef = ref;
					m_contextChapter = ch;
					m_contextVerse = 1;
					m_contextStreamTime = streamTimeSec;
					m_contextWallTimeMs = wallNow;
					m_lastSuggestionWallTime = wallNow;
					m_lastSuggestionStreamTime = streamTimeSec;

					SpeechSuggestion sugg;
					sugg.channel = Channel::Scripture;
					sugg.reference = ref;
					sugg.text = QStringLiteral("Next chapter in book");
					sugg.confidence = 92;
					sugg.timestampSeconds = streamTimeSec;
					sugg.tentative = isTentative;

					emit suggestionDetected(sugg);
					return true;
				}
			}
		}
	}

	struct BookAlias {
		const char *alias;
		const char *canonical;
	};

	static const BookAlias kAliases[] = {{"1 thessalonians", "1 Thessalonians"},
					     {"first thessalonians", "1 Thessalonians"},
					     {"1st thessalonians", "1 Thessalonians"},
					     {"2 thessalonians", "2 Thessalonians"},
					     {"second thessalonians", "2 Thessalonians"},
					     {"2nd thessalonians", "2 Thessalonians"},
					     {"1 chronicles", "1 Chronicles"},
					     {"first chronicles", "1 Chronicles"},
					     {"1st chronicles", "1 Chronicles"},
					     {"2 chronicles", "2 Chronicles"},
					     {"second chronicles", "2 Chronicles"},
					     {"2nd chronicles", "2 Chronicles"},
					     {"1 corinthians", "1 Corinthians"},
					     {"first corinthians", "1 Corinthians"},
					     {"1st corinthians", "1 Corinthians"},
					     {"2 corinthians", "2 Corinthians"},
					     {"second corinthians", "2 Corinthians"},
					     {"2nd corinthians", "2 Corinthians"},
					     {"song of solomon", "Song of Solomon"},
					     {"song of songs", "Song of Solomon"},
					     {"canticles", "Song of Solomon"},
					     {"1 timothy", "1 Timothy"},
					     {"first timothy", "1 Timothy"},
					     {"1st timothy", "1 Timothy"},
					     {"2 timothy", "2 Timothy"},
					     {"second timothy", "2 Timothy"},
					     {"2nd timothy", "2 Timothy"},
					     {"1 samuel", "1 Samuel"},
					     {"first samuel", "1 Samuel"},
					     {"1st samuel", "1 Samuel"},
					     {"2 samuel", "2 Samuel"},
					     {"second samuel", "2 Samuel"},
					     {"2nd samuel", "2 Samuel"},
					     {"1 kings", "1 Kings"},
					     {"first kings", "1 Kings"},
					     {"1st kings", "1 Kings"},
					     {"2 kings", "2 Kings"},
					     {"second kings", "2 Kings"},
					     {"2nd kings", "2 Kings"},
					     {"1 peter", "1 Peter"},
					     {"first peter", "1 Peter"},
					     {"1st peter", "1 Peter"},
					     {"2 peter", "2 Peter"},
					     {"second peter", "2 Peter"},
					     {"2nd peter", "2 Peter"},
					     {"1 john", "1 John"},
					     {"first john", "1 John"},
					     {"1st john", "1 John"},
					     {"2 john", "2 John"},
					     {"second john", "2 John"},
					     {"2nd john", "2 John"},
					     {"3 john", "3 John"},
					     {"third john", "3 John"},
					     {"3rd john", "3 John"},
					     {"revelations", "Revelation"},
					     {"revelation", "Revelation"},
					     {"psalms", "Psalms"},
					     {"psalm", "Psalms"},
					     {"genesis", "Genesis"},
					     {"exodus", "Exodus"},
					     {"leviticus", "Leviticus"},
					     {"numbers", "Numbers"},
					     {"deuteronomy", "Deuteronomy"},
					     {"joshua", "Joshua"},
					     {"judges", "Judges"},
					     {"ruth", "Ruth"},
					     {"ezra", "Ezra"},
					     {"nehemiah", "Nehemiah"},
					     {"esther", "Esther"},
					     {"job", "Job"},
					     {"proverbs", "Proverbs"},
					     {"ecclesiastes", "Ecclesiastes"},
					     {"isaiah", "Isaiah"},
					     {"jeremiah", "Jeremiah"},
					     {"lamentations", "Lamentations"},
					     {"ezekiel", "Ezekiel"},
					     {"daniel", "Daniel"},
					     {"hosea", "Hosea"},
					     {"joel", "Joel"},
					     {"amos", "Amos"},
					     {"obadiah", "Obadiah"},
					     {"jonah", "Jonah"},
					     {"micah", "Micah"},
					     {"nahum", "Nahum"},
					     {"habakkuk", "Habakkuk"},
					     {"zephaniah", "Zephaniah"},
					     {"haggai", "Haggai"},
					     {"zechariah", "Zechariah"},
					     {"malachi", "Malachi"},
					     {"matthew", "Matthew"},
					     {"mark", "Mark"},
					     {"luke", "Luke"},
					     {"john", "John"},
					     {"jon", "John"},
					     {"acts", "Acts"},
					     {"romans", "Romans"},
					     {"roman", "Romans"},
					     {"galatians", "Galatians"},
					     {"ephesians", "Ephesians"},
					     {"philippians", "Philippians"},
					     {"phillipians", "Philippians"},
					     {"colossians", "Colossians"},
					     {"titus", "Titus"},
					     {"philemon", "Philemon"},
					     {"hebrews", "Hebrews"},
					     {"james", "James"},
					     {"jude", "Jude"}};

	for (const auto &b : kAliases) {
		// Pattern 1: Standard [Book] [Chapter] [Verse]
		QRegularExpression rx(
			QStringLiteral(
				"\\b%1\\b(?:\\s*(?:chapter|ch)?\\s*(\\d+))?(?:\\s*(?:[:,-]|and\\s+(?:verse|v)?|from\\s+(?:verse|v)?|starting\\s+(?:at\\s+)?(?:verse|v)?|at\\s+(?:verse|v)?|verse|verses|v)?\\s*(\\d+))?")
				.arg(QRegularExpression::escape(QString::fromUtf8(b.alias))));

		QRegularExpressionMatch m = rx.match(text);
		if (m.hasMatch()) {
			int ch = m.captured(1).toInt();
			int vs = m.captured(2).toInt();
			if (ch > 0) {
				QString ref;
				if (vs > 0) {
					ref = QStringLiteral("%1 %2:%3").arg(b.canonical).arg(ch).arg(vs);
				} else {
					ref = QStringLiteral("%1 %2").arg(b.canonical).arg(ch);
				}

				if (ref != m_lastSuggestedRef || (streamTimeSec - m_lastSuggestionStreamTime > 20.0)) {
					m_lastSuggestedRef = ref;
					m_contextBook = QString::fromUtf8(b.canonical);
					m_contextChapter = ch;
					m_contextVerse = vs > 0 ? vs : 1;
					m_contextStreamTime = streamTimeSec;
					m_contextWallTimeMs = wallNow;
					m_lastSuggestionWallTime = wallNow;
					m_lastSuggestionStreamTime = streamTimeSec;

					SpeechSuggestion sugg;
					sugg.channel = Channel::Scripture;
					sugg.reference = ref;
					sugg.text = QStringLiteral("Detected from audio");
					sugg.confidence = isTentative ? 80 : 95;
					sugg.timestampSeconds = streamTimeSec;
					sugg.tentative = isTentative;

					if (m_debugLogging.load(std::memory_order_relaxed)) {
						fprintf(stderr, "[LDWW Core] ==> SUGGEST SCRIPTURE: %s (%d%%)\n",
							ref.toUtf8().constData(), sugg.confidence);
					}
					emit suggestionDetected(sugg);
					return true;
				}
			}
		}

		// Pattern 2: Inverted order: Verse [V] of [Book] [Chapter]
		QRegularExpression rxInv(
			QStringLiteral(
				"\\b(?:verse|verses|v)\\s*(\\d+)\\s*(?:of|in|from)\\s*%1\\b(?:\\s*(?:chapter|ch)?\\s*(\\d+))?")
				.arg(QRegularExpression::escape(QString::fromUtf8(b.alias))));

		QRegularExpressionMatch mInv = rxInv.match(text);
		if (mInv.hasMatch()) {
			int vs = mInv.captured(1).toInt();
			int ch = mInv.captured(2).toInt();
			if (ch > 0 && vs > 0) {
				QString ref = QStringLiteral("%1 %2:%3").arg(b.canonical).arg(ch).arg(vs);
				if (ref != m_lastSuggestedRef || (streamTimeSec - m_lastSuggestionStreamTime > 20.0)) {
					m_lastSuggestedRef = ref;
					m_contextBook = QString::fromUtf8(b.canonical);
					m_contextChapter = ch;
					m_contextVerse = vs;
					m_contextStreamTime = streamTimeSec;
					m_contextWallTimeMs = wallNow;
					m_lastSuggestionWallTime = wallNow;
					m_lastSuggestionStreamTime = streamTimeSec;

					SpeechSuggestion sugg;
					sugg.channel = Channel::Scripture;
					sugg.reference = ref;
					sugg.text = QStringLiteral("Detected from audio");
					sugg.confidence = isTentative ? 80 : 95;
					sugg.timestampSeconds = streamTimeSec;
					sugg.tentative = isTentative;

					emit suggestionDetected(sugg);
					return true;
				}
			}
		}
	}

	// Phase 3: Quote matching across Bible
	if (detectBibleQuotes(text, streamTimeSec)) {
		return true;
	}

	return false;
}

bool DetectionCore::detectBibleQuotes(const QString &normalizedText, double streamTimeSec)
{
	uint64_t wallNow = QDateTime::currentMSecsSinceEpoch();

	static const struct {
		const char *phrase;
		const char *ref;
		const char *verse;
	} kQuotes[] = {
		{"for god so loved the world", "John 3:16",
		 "For God so loved the world, that he gave his only begotten Son, that whosoever believeth in him should not perish, but have everlasting life."},
		{"the lord is my shepherd", "Psalms 23:1", "The Lord is my shepherd; I shall not want."},
		{"all things work together for good", "Romans 8:28",
		 "And we know that all things work together for good to them that love God..."},
		{"i can do all things through christ", "Philippians 4:13",
		 "I can do all things through Christ which strengtheneth me."},
		{"trust in the lord with all thine heart", "Proverbs 3:5",
		 "Trust in the Lord with all thine heart; and lean not unto thine own understanding."},
		{"in the beginning god created the heaven", "Genesis 1:1",
		 "In the beginning God created the heaven and the earth."},
		{"be strong and courageous", "Joshua 1:9",
		 "Be strong and of a good courage; be not afraid, neither be thou dismayed."},
		{"the joy of the lord is your strength", "Nehemiah 8:10", "The joy of the LORD is your strength."},
		{"by his stripes we are healed", "Isaiah 53:5", "With his stripes we are healed."},
		{"if god be for us who can be against us", "Romans 8:31", "If God be for us, who can be against us?"},
		{"charity suffereth long and is kind", "1 Corinthians 13:4",
		 "Charity suffereth long, and is kind; charity envieth not; charity vaunteth not itself, is not puffed up,"},
		{"if any of you lack wisdom let him ask of god", "James 1:5",
		 "If any of you lack wisdom, let him ask of God, that giveth to all men liberally, and upbraideth not; and it shall be given him."},
		{"i will lift up mine eyes unto the hills", "Psalms 121:1",
		 "I will lift up mine eyes unto the hills, from whence cometh my help."}};

	for (const auto &q : kQuotes) {
		if (normalizedText.contains(QString::fromUtf8(q.phrase))) {
			QString ref = QString::fromUtf8(q.ref);
			if (ref != m_lastSuggestedRef || (streamTimeSec - m_lastSuggestionStreamTime > 20.0)) {
				m_lastSuggestedRef = ref;
				m_lastSuggestionWallTime = wallNow;
				m_lastSuggestionStreamTime = streamTimeSec;

				SpeechSuggestion sugg;
				sugg.channel = Channel::Scripture;
				sugg.reference = ref;
				sugg.text = QString::fromUtf8(q.verse);
				sugg.confidence = 94;
				sugg.timestampSeconds = streamTimeSec;
				sugg.tentative = false;

				if (m_debugLogging.load(std::memory_order_relaxed)) {
					fprintf(stderr, "[LDWW Core] ==> SUGGEST QUOTE: %s (94%%)\n",
						ref.toUtf8().constData());
				}
				emit suggestionDetected(sugg);
				return true;
			}
		}
	}

	return false;
}

bool DetectionCore::detectLyrics(const QString &rawTranscript, double streamTimeSec, bool isTentative)
{
	uint64_t wallNow = QDateTime::currentMSecsSinceEpoch();
	if (streamTimeSec > 0.0) {
		if (streamTimeSec - m_lastSuggestionStreamTime < 2.0)
			return false;
	} else {
		if (wallNow - m_lastSuggestionWallTime < 2000)
			return false;
	}

	QString text = normalizeSpokenText(rawTranscript);
	if (text.length() < 5)
		return false;

	QStringList transcriptWords = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (transcriptWords.isEmpty())
		return false;

	static const QSet<QString> kStopWords = {QStringLiteral("the"),  QStringLiteral("and"),  QStringLiteral("a"),
						 QStringLiteral("to"),   QStringLiteral("in"),   QStringLiteral("is"),
						 QStringLiteral("of"),   QStringLiteral("that"), QStringLiteral("it"),
						 QStringLiteral("for"),  QStringLiteral("on"),   QStringLiteral("with"),
						 QStringLiteral("at"),   QStringLiteral("by"),   QStringLiteral("from"),
						 QStringLiteral("up"),   QStringLiteral("an"),   QStringLiteral("my"),
						 QStringLiteral("your"), QStringLiteral("we"),   QStringLiteral("i")};

	struct Candidate {
		QString songTitle;
		QString sectionName;
		QString sectionCode;
		QString matchedLine;
		int slideIndex = -1;
		int confidence = 0;
		bool isNext = false;
	};

	Candidate best;

	// Phase 4: Follow Mode
	// If follow mode is enabled and a song is open, prioritize active song slides
	QString activeTitle;
	int activeSlideIdx = -1;
	QString nextSetlistTitle;
	{
		std::lock_guard<std::mutex> lock(m_activeSongMutex);
		activeTitle = m_activeSongTitle;
		activeSlideIdx = m_activeSlideIndex;
		nextSetlistTitle = m_nextSongTitle;
	}

	bool followActive = m_followMode.load(std::memory_order_relaxed) && !activeTitle.isEmpty();

	if (followActive) {
		Song openSong = SongLibrary::instance().songByTitle(activeTitle);
		if (openSong.isValid()) {
			std::vector<SongSlide> allSlides = generateSlides(openSong, 2);

			// Match candidates:
			// 1) Current slide
			// 2) Next 2 slides
			// 3) First slide of each section
			// 4) First slide of next setlist song
			std::vector<int> candidateSlideIndices;
			if (activeSlideIdx >= 0 && activeSlideIdx < static_cast<int>(allSlides.size())) {
				candidateSlideIndices.push_back(activeSlideIdx);
				if (activeSlideIdx + 1 < static_cast<int>(allSlides.size()))
					candidateSlideIndices.push_back(activeSlideIdx + 1);
				if (activeSlideIdx + 2 < static_cast<int>(allSlides.size()))
					candidateSlideIndices.push_back(activeSlideIdx + 2);
			}

			// First slide of each section
			QString lastSec;
			for (size_t i = 0; i < allSlides.size(); ++i) {
				if (allSlides[i].sectionName != lastSec) {
					lastSec = allSlides[i].sectionName;
					if (std::find(candidateSlideIndices.begin(), candidateSlideIndices.end(),
						      static_cast<int>(i)) == candidateSlideIndices.end()) {
						candidateSlideIndices.push_back(static_cast<int>(i));
					}
				}
			}

			for (int sIdx : candidateSlideIndices) {
				const auto &slide = allSlides[static_cast<size_t>(sIdx)];
				for (const QString &line : slide.lines) {
					QString normLine = normalizeSpokenText(line);
					if (normLine.length() < 6)
						continue;

					if (text.contains(normLine) || normLine.contains(text)) {
						Candidate c;
						c.songTitle = openSong.title;
						c.sectionName = slide.sectionName;
						c.sectionCode = slide.sectionCode;
						c.matchedLine = line;
						c.slideIndex = sIdx;
						c.confidence = 96;
						c.isNext = (sIdx == activeSlideIdx + 1);
						if (c.confidence > best.confidence)
							best = c;
					}
				}
			}

			// Check first slide of next setlist song
			if (!nextSetlistTitle.isEmpty()) {
				Song nextSong = SongLibrary::instance().songByTitle(nextSetlistTitle);
				if (nextSong.isValid()) {
					auto nextSlides = generateSlides(nextSong, 2);
					if (!nextSlides.empty()) {
						for (const QString &line : nextSlides.front().lines) {
							QString normLine = normalizeSpokenText(line);
							if (normLine.length() >= 8 && text.contains(normLine)) {
								Candidate c;
								c.songTitle = nextSong.title;
								c.sectionName = nextSlides.front().sectionName;
								c.sectionCode = nextSlides.front().sectionCode;
								c.matchedLine = line;
								c.confidence = 92;
								if (c.confidence > best.confidence)
									best = c;
							}
						}
					}
				}
			}
		}
	}

	// If no follow candidate found or follow mode is off, scan entire library
	if (best.confidence < 80) {
		const auto &songs = SongLibrary::instance().songs();
		for (const auto &song : songs) {
			QString normTitle = normalizeSpokenText(song.title);
			if (!normTitle.isEmpty() && normTitle.length() >= 5) {
				if (text.contains(normTitle)) {
					Candidate c;
					c.songTitle = song.title;
					c.matchedLine = song.title;
					c.confidence = 95;
					if (c.confidence > best.confidence)
						best = c;
				}
			}

			for (const auto &sec : song.sections) {
				for (const auto &slide : sec.rawSlides) {
					for (const auto &line : slide) {
						QString normLine = normalizeSpokenText(line);
						if (normLine.length() < 6)
							continue;

						// Substring match
						if (text.length() >= 10 && normLine.contains(text)) {
							Candidate c;
							c.songTitle = song.title;
							c.sectionName = sec.name;
							c.sectionCode = sec.shortCode;
							c.matchedLine = line;
							c.confidence = 94;
							if (c.confidence > best.confidence)
								best = c;
							continue;
						}
						if (normLine.length() >= 10 && text.contains(normLine)) {
							Candidate c;
							c.songTitle = song.title;
							c.sectionName = sec.name;
							c.sectionCode = sec.shortCode;
							c.matchedLine = line;
							c.confidence = 95;
							if (c.confidence > best.confidence)
								best = c;
							continue;
						}

						// 3-word phrase match
						QStringList lineWords =
							normLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
						if (lineWords.size() >= 3 && transcriptWords.size() >= 3) {
							bool matched = false;
							for (int li = 0; li <= lineWords.size() - 3; ++li) {
								if (kStopWords.contains(lineWords[li]) &&
								    kStopWords.contains(lineWords[li + 1]) &&
								    kStopWords.contains(lineWords[li + 2]))
									continue;

								QString trigram = QStringLiteral("%1 %2 %3")
											  .arg(lineWords[li],
											       lineWords[li + 1],
											       lineWords[li + 2]);
								if (text.contains(trigram)) {
									Candidate c;
									c.songTitle = song.title;
									c.sectionName = sec.name;
									c.sectionCode = sec.shortCode;
									c.matchedLine = line;
									c.confidence = 90;
									if (c.confidence > best.confidence)
										best = c;
									matched = true;
									break;
								}
							}
							if (matched)
								continue;
						}

						// Keyword overlap
						int matchCount = 0;
						int totalSig = 0;
						for (const QString &w : lineWords) {
							if (w.length() < 3 || kStopWords.contains(w))
								continue;
							totalSig++;
							if (transcriptWords.contains(w))
								matchCount++;
						}

						if (totalSig >= 3 && matchCount >= 3) {
							float ratio = static_cast<float>(matchCount) /
								      static_cast<float>(totalSig);
							if (ratio >= 0.50f) {
								int conf = 75 + static_cast<int>(ratio * 20.0f);
								if (conf > best.confidence) {
									best.songTitle = song.title;
									best.sectionName = sec.name;
									best.sectionCode = sec.shortCode;
									best.matchedLine = line;
									best.confidence = conf;
								}
							}
						}
					}
				}
			}
		}
	}

	const int minThreshold = followActive ? 75 : 85;
	if (best.confidence >= minThreshold) {
		QString displayRef;
		if (!best.sectionName.isEmpty()) {
			displayRef = QStringLiteral("%1 · %2").arg(best.songTitle, best.sectionName);
		} else {
			displayRef = best.songTitle;
		}

		if (displayRef != m_lastSuggestedRef || (streamTimeSec - m_lastSuggestionStreamTime > 20.0)) {
			m_lastSuggestedRef = displayRef;
			m_lastSuggestionWallTime = wallNow;
			m_lastSuggestionStreamTime = streamTimeSec;

			SpeechSuggestion sugg;
			sugg.channel = Channel::Lyrics;
			sugg.reference = displayRef;
			sugg.text = best.songTitle;
			sugg.sectionCode = best.sectionCode;
			sugg.confidence = best.confidence;
			sugg.timestampSeconds = streamTimeSec;
			sugg.tentative = isTentative;
			sugg.isNextSlide = best.isNext;

			if (best.isNext) {
				m_nextSlideConsecutiveMatches++;
				if (m_nextSlideConsecutiveMatches >= 2 &&
				    m_autoAdvance.load(std::memory_order_relaxed)) {
					emit autoAdvanceRequested();
				}
			} else {
				m_nextSlideConsecutiveMatches = 0;
			}

			if (m_debugLogging.load(std::memory_order_relaxed)) {
				fprintf(stderr, "[LDWW Core] ==> SUGGEST LYRICS: %s (%d%%)\n",
					displayRef.toUtf8().constData(), best.confidence);
			}
			emit suggestionDetected(sugg);
			return true;
		}
	}

	return false;
}
