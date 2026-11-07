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
#include "worship-word-dock.hpp"
#include "scripture/scripture_bible_structure.hpp"

#include <obs.h>
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

namespace {

static std::string bookToOsis(int bookNumber, const QString &bookName)
{
	if (bookNumber >= 1 && bookNumber <= 66) {
		const auto &books = ldsd::bibleStructure();
		if (static_cast<size_t>(bookNumber - 1) < books.size()) {
			return books[bookNumber - 1].id;
		}
	}

	const QString norm = Canon::normalize(bookName);
	for (const auto &b : ldsd::bibleStructure()) {
		if (norm == Canon::normalize(QString::fromStdString(b.name)) ||
		    norm == Canon::normalize(QString::fromStdString(b.id))) {
			return b.id;
		}
	}

	if (bookNumber >= 1 && bookNumber <= Canon::count()) {
		const QString engName = Canon::normalize(Canon::englishName(bookNumber));
		for (const auto &b : ldsd::bibleStructure()) {
			if (engName == Canon::normalize(QString::fromStdString(b.name))) {
				return b.id;
			}
		}
	}

	return std::string();
}

static std::vector<ldsd::Verse> buildVerseList(const BiblePtr &bible)
{
	std::vector<ldsd::Verse> verses;
	if (!bible)
		return verses;
	bible->ensureLoaded();
	verses.reserve(31102);
	for (const auto &b : bible->books) {
		std::string osis = bookToOsis(b.number, b.name);
		if (osis.empty())
			continue;
		for (size_t c = 0; c < b.chapters.size(); ++c) {
			int chNum = static_cast<int>(c + 1);
			const auto &chapVerses = b.chapters[c];
			for (int v = 0; v < chapVerses.size(); ++v) {
				int vNum = v + 1;
				verses.push_back({osis, chNum, vNum, chapVerses[v].toStdString()});
			}
		}
	}
	return verses;
}

} // namespace

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
	if (!transcript.trimmed().isEmpty()) {
		emit rawTranscriptPass(transcript.trimmed(), windowEndSec);
	}

	if (m_debugLogging.load(std::memory_order_relaxed)) {
		fprintf(stderr,
			"[LDWW Core] @ %.2fs: [Committed: \"%s\"] [Tentative: \"%s\"] (VAD=%.1fms, Whisper=%.1fms)\n",
			windowEndSec, committedText.toUtf8().constData(), tentativeText.toUtf8().constData(), vadMs,
			whisperMs);
	}

	// 4. Pattern matching with ldsd::LiveScripture for Scripture and fuzzy matcher for Lyrics
	auto tMatchStart = std::chrono::steady_clock::now();

	if (m_bibleReloadNeeded.exchange(false, std::memory_order_acq_rel) || !m_liveScripture) {
		rebuildScriptureIndex();
	}

	int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	std::string newText = transcript.trimmed().toStdString();
	bool isFinal = true;

	bool scriptureMatched = false;
	if (m_liveScripture && !newText.empty()) {
		auto refs = m_liveScripture->push(newText, isFinal, nowMs);
		for (const auto &r : refs) {
			scriptureMatched = true;
			int rConf = static_cast<int>(std::round(r.confidence * 100.0));
			emit scriptureRefDetected(QString::fromStdString(r.label), rConf,
						  QString::fromStdString(r.status),
						  QString::fromStdString(r.kind), windowEndSec);

			auto *dock = WorshipWordDock::instance();
			if (dock) {
				QMetaObject::invokeMethod(dock, [dock, r]() {
					dock->showSuggestion(r);
				}, Qt::QueuedConnection);
			}

			SpeechSuggestion sugg;
			sugg.channel = Channel::Scripture;
			sugg.reference = QString::fromStdString(r.label);
			sugg.text = QString::fromStdString(r.text);
			sugg.confidence = static_cast<int>(std::round(r.confidence * 100.0));
			sugg.tentative = (r.status == "preview");
			sugg.timestampSeconds = windowEndSec;
			emit suggestionDetected(sugg);
		}
	}

	if (ch == Channel::Scripture) {
		if (!scriptureMatched) {
			detectLyrics(combinedText, windowEndSec, false);
		}
	} else {
		if (!detectLyrics(combinedText, windowEndSec, false)) {
			// Scripture fallback handled above
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
	wparams.n_threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

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

void DetectionCore::setPrimaryBible(const QString &abbrev)
{
	if (m_primaryBibleAbbrev != abbrev) {
		m_primaryBibleAbbrev = abbrev;
		m_bibleReloadNeeded.store(true, std::memory_order_release);
	}
}

void DetectionCore::rebuildScriptureIndex()
{
	BiblePtr bible;
	if (!m_primaryBibleAbbrev.isEmpty()) {
		bible = BibleLibrary::instance().byAbbrev(m_primaryBibleAbbrev);
	}
	if (!bible) {
		bible = BibleLibrary::instance().byAbbrev(QStringLiteral("KJV"));
	}
	if (!bible) {
		auto all = BibleLibrary::instance().bibles();
		if (!all.empty()) {
			bible = all.front();
		}
	}

	if (bible) {
		m_scriptureVerses = buildVerseList(bible);
		m_liveScripture = std::make_unique<ldsd::LiveScripture>(&m_scriptureVerses);
	} else {
		m_scriptureVerses.clear();
		m_liveScripture = std::make_unique<ldsd::LiveScripture>(nullptr);
	}
}

bool DetectionCore::detectScripture(const QString &rawTranscript, double streamTimeSec, bool isTentative)
{
	Q_UNUSED(rawTranscript);
	Q_UNUSED(streamTimeSec);
	Q_UNUSED(isTentative);
	return false;
}

bool DetectionCore::detectBibleQuotes(const QString &normalizedText, double streamTimeSec)
{
	Q_UNUSED(normalizedText);
	Q_UNUSED(streamTimeSec);
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
