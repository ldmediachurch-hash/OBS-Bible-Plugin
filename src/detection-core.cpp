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
#include <obs-module.h>
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
#include <sstream>

namespace {

static std::string cleanWordForComparison(const std::string &w)
{
	std::string res;
	res.reserve(w.size());
	for (char c : w) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			res += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return res;
}

static bool isSentenceEndWord(const std::string &w)
{
	if (w.empty())
		return false;
	for (auto it = w.rbegin(); it != w.rend(); ++it) {
		char c = *it;
		if (c == '.' || c == '?' || c == '!')
			return true;
		if (std::isalnum(static_cast<unsigned char>(c)))
			break;
	}
	return false;
}

static bool isStockSilenceOutput(const std::string &segText)
{
	std::string c = cleanWordForComparison(segText);
	if (c.empty())
		return true;

	if (c == "thankyou" || c == "thankyouverymuch" || c == "thanksforwatching" ||
	    c == "blankaudio" || c == "amen" || c == "you" || c == "subtitlesby" ||
	    c == "thankyouforwatching" || c == "bye" || c == "goodbye") {
		return true;
	}

	// Check bracketed special tokens: [BLANK_AUDIO], [MUSIC], (music), [APPLAUSE], etc.
	std::string trimmed;
	for (char ch : segText) {
		if (!std::isspace(static_cast<unsigned char>(ch)))
			trimmed += ch;
	}
	if (!trimmed.empty()) {
		if ((trimmed.front() == '[' && trimmed.back() == ']') ||
		    (trimmed.front() == '(' && trimmed.back() == ')')) {
			return true;
		}
	}

	return false;
}

static bool isMostlyPrompt(const std::string &segText, const std::string &prompt)
{
	std::string cSeg = cleanWordForComparison(segText);
	if (cSeg.empty())
		return true;

	static const std::string cStaticPrompt = cleanWordForComparison("A sermon. The preacher reads from the Bible.");
	if (cSeg.length() >= 6 && cStaticPrompt.find(cSeg) != std::string::npos) {
		return true;
	}

	const std::vector<std::string> promptWords = {"a", "sermon", "the", "preacher", "reads", "from", "bible"};
	std::stringstream ss(segText);
	std::string token;
	int totalWords = 0;
	int promptWordMatches = 0;
	while (ss >> token) {
		std::string c = cleanWordForComparison(token);
		if (c.empty())
			continue;
		totalWords++;
		for (const auto &pw : promptWords) {
			if (c == pw) {
				promptWordMatches++;
				break;
			}
		}
	}
	if (totalWords > 0 && (static_cast<float>(promptWordMatches) / static_cast<float>(totalWords)) >= 0.7f) {
		return true;
	}

	return false;
}

static bool hasRepetitionLoop(const std::vector<TimedWord> &words)
{
	if (words.size() < 6)
		return false;

	std::vector<std::string> clean;
	clean.reserve(words.size());
	for (const auto &w : words) {
		std::string c = cleanWordForComparison(w.text);
		if (!c.empty())
			clean.push_back(c);
	}

	int n = static_cast<int>(clean.size());
	if (n < 6)
		return false;

	// Check for consecutive repetitions of 2 to 8 word phrases (3+ times)
	for (int len = 2; len <= 8; ++len) {
		for (int i = 0; i + 3 * len <= n; ++i) {
			bool match = true;
			for (int k = 1; k < 3; ++k) {
				for (int j = 0; j < len; ++j) {
					if (clean[i + j] != clean[i + k * len + j]) {
						match = false;
						break;
					}
				}
				if (!match)
					break;
			}
			if (match)
				return true;
		}
	}

	return false;
}

static void applyHighPass80Hz(std::vector<float> &audio, float sampleRate = 16000.0f)
{
	if (audio.empty())
		return;

	constexpr float f0 = 80.0f;
	constexpr float q = 0.70710678f;
	const float omega0 = 2.0f * static_cast<float>(M_PI) * (f0 / sampleRate);
	const float sinOmega = std::sin(omega0);
	const float cosOmega = std::cos(omega0);
	const float alpha = sinOmega / (2.0f * q);

	const float b0 = (1.0f + cosOmega) * 0.5f;
	const float b1 = -(1.0f + cosOmega);
	const float b2 = (1.0f + cosOmega) * 0.5f;
	const float a0 = 1.0f + alpha;
	const float a1 = -2.0f * cosOmega;
	const float a2 = 1.0f - alpha;

	const float b0_n = b0 / a0;
	const float b1_n = b1 / a0;
	const float b2_n = b2 / a0;
	const float a1_n = a1 / a0;
	const float a2_n = a2 / a0;

	float x1 = 0.0f, x2 = 0.0f;
	float y1 = 0.0f, y2 = 0.0f;

	for (float &x : audio) {
		float y = b0_n * x + b1_n * x1 + b2_n * x2 - a1_n * y1 - a2_n * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		x = y;
	}
}

static void applyAgcTo20dBFS(std::vector<float> &audio)
{
	if (audio.empty())
		return;

	constexpr float targetRms = 0.10f; // -20 dBFS RMS
	constexpr size_t subSize = 3200;   // 200 ms @ 16 kHz

	float sumSpeechSq = 0.0f;
	size_t speechSamples = 0;

	for (size_t i = 0; i + subSize <= audio.size(); i += subSize) {
		float subSum = 0.0f;
		for (size_t j = 0; j < subSize; ++j) {
			float s = audio[i + j];
			subSum += s * s;
		}
		float subRms = std::sqrt(subSum / static_cast<float>(subSize));
		if (subRms > 0.005f) {
			sumSpeechSq += subSum;
			speechSamples += subSize;
		}
	}

	if (speechSamples > 0) {
		float speechRms = std::sqrt(sumSpeechSq / static_cast<float>(speechSamples));
		if (speechRms > 0.001f) {
			float gain = targetRms / speechRms;
			gain = std::clamp(gain, 0.2f, 10.0f);
			for (float &s : audio) {
				s = std::clamp(s * gain, -1.0f, 1.0f);
			}
		}
	}
}

static std::vector<TimedWord> getUncommittedWords(
	const std::vector<TimedWord> &words,
	const std::string &committedHistory,
	double lastCommittedTimeSec)
{
	if (words.empty())
		return {};
	if (committedHistory.empty() || lastCommittedTimeSec < 0.0)
		return words;

	std::vector<std::string> histWords;
	std::stringstream ss(committedHistory);
	std::string tw;
	while (ss >> tw) {
		std::string c = cleanWordForComparison(tw);
		if (!c.empty())
			histWords.push_back(c);
	}

	int matchIdx = -1;
	for (int i = 0; i < static_cast<int>(words.size()); ++i) {
		if (std::abs(words[i].endTimeSec - lastCommittedTimeSec) < 0.8) {
			std::string cw = cleanWordForComparison(words[i].text);
			if (!histWords.empty() && cw == histWords.back()) {
				matchIdx = i;
				break;
			}
		}
	}

	if (matchIdx >= 0) {
		if (matchIdx + 1 < static_cast<int>(words.size()))
			return std::vector<TimedWord>(words.begin() + matchIdx + 1, words.end());
		return {};
	}

	std::vector<TimedWord> uncommitted;
	for (const auto &w : words) {
		if (w.endTimeSec > lastCommittedTimeSec - 0.05) {
			uncommitted.push_back(w);
		}
	}
	return uncommitted;
}

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
#ifdef __APPLE__
	cparams.use_gpu = true;
	cparams.flash_attn = true;
#endif
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
	m_streamBuffer.clear();
	m_bufStartSec = 0.0;
	m_lastProcessedSamples = 0;
	m_lastCommittedTimeSec = -1.0;
	m_lastCommittedSentenceEndSec = -1.0;
	m_prevPassWords.clear();
	m_committedHistory.clear();
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
	while (m_workerRunning.load(std::memory_order_relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		size_t w = m_writeIndex.load(std::memory_order_acquire);
		size_t r = m_readIndex.load(std::memory_order_relaxed);

		if (w != r) {
			size_t available = (w >= r) ? (w - r) : (kBufferSize - r + w);
			if (m_streamBuffer.empty()) {
				double streamNow = m_streamTimeSec.load(std::memory_order_relaxed);
				m_bufStartSec = std::max(0.0, streamNow - (static_cast<double>(available) / 16000.0));
			}

			size_t tempR = r;
			for (size_t i = 0; i < available; i++) {
				m_streamBuffer.push_back(m_ringBuffer[tempR]);
				tempR = (tempR + 1) % kBufferSize;
			}
			m_readIndex.store(tempR, std::memory_order_release);
		}

		size_t curSize = m_streamBuffer.size();
		if (curSize < static_cast<size_t>(2.5 * 16000))
			continue;

		if (curSize - m_lastProcessedSamples < static_cast<size_t>(1.0 * 16000))
			continue;

		// Backlog guard: never throw away unprocessed audio.
		// When behind, skip interim passes (processing runs on the newest full buffer).
		// Only show "Falling behind" if backlog stays above 3s for more than 5s. Log whenever guard activates.
		double streamNow = m_streamTimeSec.load(std::memory_order_relaxed);
		double processedUpTo = m_bufStartSec + (static_cast<double>(curSize) / 16000.0);
		double backlog = std::max(0.0, streamNow - processedUpTo);

		auto nowClock = std::chrono::steady_clock::now();
		if (backlog > 3.0) {
			if (!m_backlogTimerActive) {
				m_backlogTimerActive = true;
				m_backlogOver3sStart = nowClock;
				m_guardActivationCount++;
				fprintf(stderr, "[LD Worship & Word] [BACKLOG GUARD] Activated: backlog=%.2fs, skipping interim passes to process newest full buffer (%.1fs)\n",
				        backlog, static_cast<double>(curSize) / 16000.0);
			} else {
				double durationOver3s = std::chrono::duration<double>(nowClock - m_backlogOver3sStart).count();
				if (durationOver3s >= 5.0 && !m_fallingBehind.load(std::memory_order_relaxed)) {
					m_fallingBehind.store(true, std::memory_order_relaxed);
					emit backlogWarningChanged(true, backlog);
				}
			}
		} else if (backlog <= 2.0) {
			m_backlogTimerActive = false;
			if (m_fallingBehind.load(std::memory_order_relaxed)) {
				m_fallingBehind.store(false, std::memory_order_relaxed);
				emit backlogWarningChanged(false, backlog);
			}
		}

		runStreamingPass();
	}
}

void DetectionCore::processWindowDirect(const std::vector<float> &chunk, double windowEndSec)
{
	if (chunk.empty())
		return;

	if (m_streamBuffer.empty()) {
		m_bufStartSec = std::max(0.0, windowEndSec - (static_cast<double>(chunk.size()) / 16000.0));
	}
	m_streamBuffer.insert(m_streamBuffer.end(), chunk.begin(), chunk.end());
	runStreamingPass();
}

void DetectionCore::analyzeBuffer(const std::vector<float> &chunk, double windowEndSec)
{
	processWindowDirect(chunk, windowEndSec);
}

void DetectionCore::runStreamingPass()
{
	if (m_streamBuffer.size() < static_cast<size_t>(2.5 * 16000))
		return;

	auto tPassStart = std::chrono::steady_clock::now();

	// Step 10: Don't run whisper on segments with no speech
	auto tVadStart = std::chrono::steady_clock::now();
	float maxRms = 0.0f;
	constexpr size_t subSize = 3200; // 200ms @ 16kHz
	for (size_t i = 0; i + subSize <= m_streamBuffer.size(); i += subSize) {
		float sum = 0.0f;
		for (size_t j = 0; j < subSize; ++j) {
			float s = m_streamBuffer[i + j];
			sum += s * s;
		}
		float subRms = sqrtf(sum / static_cast<float>(subSize));
		if (subRms > maxRms)
			maxRms = subRms;
	}
	auto tVadEnd = std::chrono::steady_clock::now();
	double vadMs = std::chrono::duration<double, std::milli>(tVadEnd - tVadStart).count();

	if (maxRms < 0.002f) {
		return;
	}

	// Hard cap the buffer at 15s
	constexpr size_t kMaxBufferSamples = 15 * 16000;
	if (m_streamBuffer.size() > kMaxBufferSamples) {
		size_t drop = m_streamBuffer.size() - kMaxBufferSamples;
		m_streamBuffer.erase(m_streamBuffer.begin(), m_streamBuffer.begin() + drop);
		m_bufStartSec += static_cast<double>(drop) / 16000.0;
	}

	double bufStart = m_bufStartSec;
	std::vector<float> audioCopy = m_streamBuffer;

	// Step 12: 80 Hz high-pass and automatic gain to about -20 dBFS for speech
	applyHighPass80Hz(audioCopy, 16000.0f);
	applyAgcTo20dBFS(audioCopy);

	// Transcribe words with token timestamps
	std::vector<TimedWord> words = transcribeWords(audioCopy, bufStart);

	// Drop words that end before the last committed word's end time (already committed)
	std::vector<TimedWord> uncommitted = getUncommittedWords(words, m_committedHistory, m_lastCommittedTimeSec);

	// Compare the remaining words with the previous pass
	size_t prefixLen = 0;
	while (prefixLen < uncommitted.size() && prefixLen < m_prevPassWords.size()) {
		if (cleanWordForComparison(uncommitted[prefixLen].text) ==
		    cleanWordForComparison(m_prevPassWords[prefixLen].text)) {
			prefixLen++;
		} else {
			break;
		}
	}

	int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();

	if (m_bibleReloadNeeded.exchange(false, std::memory_order_acq_rel) || !m_liveScripture) {
		rebuildScriptureIndex();
	}

	// 1. Send committed words (final = true)
	if (prefixLen > 0) {
		std::string committedText;
		for (size_t i = 0; i < prefixLen; ++i) {
			if (i > 0)
				committedText += " ";
			committedText += uncommitted[i].text;
			if (isSentenceEndWord(uncommitted[i].text)) {
				m_lastCommittedSentenceEndSec = uncommitted[i].endTimeSec;
			}
		}
		m_lastCommittedTimeSec = uncommitted[prefixLen - 1].endTimeSec;
		m_committedHistory += " " + committedText;
		if (m_committedHistory.size() > 1000) {
			m_committedHistory = m_committedHistory.substr(m_committedHistory.size() - 800);
		}

		emit transcriptCommitted(QString::fromStdString(committedText), m_lastCommittedTimeSec);
		emit rawTranscriptPass(QString::fromStdString(committedText), m_lastCommittedTimeSec);

		if (m_liveScripture) {
			auto refs = m_liveScripture->push(committedText, true, nowMs);
			for (const auto &r : refs) {
				int rConf = static_cast<int>(std::round(r.confidence * 100.0));
				emit scriptureRefDetected(QString::fromStdString(r.label), rConf,
							  QString::fromStdString(r.status),
							  QString::fromStdString(r.kind), m_lastCommittedTimeSec);

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
				sugg.confidence = rConf;
				sugg.tentative = (r.status == "preview");
				sugg.timestampSeconds = m_lastCommittedTimeSec;
				emit suggestionDetected(sugg);
			}
		}

		if (m_channel.load(std::memory_order_relaxed) == Channel::Lyrics) {
			detectLyrics(QString::fromStdString(committedText), m_lastCommittedTimeSec, false);
		}
	}

	// 2. Send interim words (final = false)
	if (prefixLen < uncommitted.size()) {
		std::string interimText;
		for (size_t i = prefixLen; i < uncommitted.size(); ++i) {
			if (!interimText.empty())
				interimText += " ";
			interimText += uncommitted[i].text;
		}

		emit transcriptTentative(QString::fromStdString(interimText), uncommitted.back().endTimeSec);

		if (m_liveScripture) {
			auto refs = m_liveScripture->push(interimText, false, nowMs);
			for (const auto &r : refs) {
				int rConf = static_cast<int>(std::round(r.confidence * 100.0));
				emit scriptureRefDetected(QString::fromStdString(r.label), rConf,
							  QString::fromStdString(r.status),
							  QString::fromStdString(r.kind), uncommitted.back().endTimeSec);

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
				sugg.confidence = rConf;
				sugg.tentative = true;
				sugg.timestampSeconds = uncommitted.back().endTimeSec;
				emit suggestionDetected(sugg);
			}
		}

		m_prevPassWords = std::vector<TimedWord>(uncommitted.begin() + prefixLen, uncommitted.end());
	} else {
		m_prevPassWords.clear();
	}

	// 3. SAFER TRIM POINTS: when buffer > 6s, trim at the latest COMMITTED word that is
	// followed by a pause of at least 0.3s (gap to next word's start), or at . ? ! if that comes first.
	// Never trim inside a run of words with no pause.
	double bufDurationSec = static_cast<double>(m_streamBuffer.size()) / 16000.0;
	if (bufDurationSec > 6.0) {
		int bestTrimWordIdx = -1;
		for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
			if (words[i].endTimeSec > m_lastCommittedTimeSec)
				continue;
			if (words[i].endTimeSec <= m_bufStartSec)
				continue;

			bool isPunctuation = isSentenceEndWord(words[i].text);
			bool hasPause = false;
			if (i + 1 < static_cast<int>(words.size())) {
				double gap = words[i + 1].startTimeSec - words[i].endTimeSec;
				if (gap >= 0.3)
					hasPause = true;
			} else {
				double gap = (m_bufStartSec + bufDurationSec) - words[i].endTimeSec;
				if (gap >= 0.3)
					hasPause = true;
			}

			if (isPunctuation || hasPause) {
				bestTrimWordIdx = i;
				break;
			}
		}

		if (bestTrimWordIdx >= 0) {
			double trimSec = words[bestTrimWordIdx].endTimeSec - m_bufStartSec;
			size_t drop = std::min(m_streamBuffer.size(), static_cast<size_t>(trimSec * 16000.0));
			if (drop > 0) {
				m_streamBuffer.erase(m_streamBuffer.begin(), m_streamBuffer.begin() + drop);
				m_bufStartSec += static_cast<double>(drop) / 16000.0;
			}
		}
	}

	// 4. LOWER HARD CAP: 10s instead of 15s. When the cap forces a trim, first FORCE-COMMIT
	// the current hypothesis words up to the trim point (push them as final) so no words are lost.
	bufDurationSec = static_cast<double>(m_streamBuffer.size()) / 16000.0;
	if (bufDurationSec > 10.0) {
		int forceIdx = -1;

		// Look for latest word followed by pause >= 0.3s or ending in . ? !
		for (int i = static_cast<int>(uncommitted.size()) - 1; i >= 0; --i) {
			bool isPunct = isSentenceEndWord(uncommitted[i].text);
			bool hasPause = false;
			if (i + 1 < static_cast<int>(uncommitted.size())) {
				double gap = uncommitted[i + 1].startTimeSec - uncommitted[i].endTimeSec;
				if (gap >= 0.3)
					hasPause = true;
			} else {
				double gap = (m_bufStartSec + bufDurationSec) - uncommitted[i].endTimeSec;
				if (gap >= 0.3)
					hasPause = true;
			}
			if (isPunct || hasPause) {
				forceIdx = i;
				break;
			}
		}

		// If no 0.3s pause or punct, look for largest gap
		if (forceIdx < 0 && uncommitted.size() > 1) {
			double maxGap = 0.0;
			int bestGapIdx = -1;
			for (size_t i = 0; i + 1 < uncommitted.size(); ++i) {
				double gap = uncommitted[i + 1].startTimeSec - uncommitted[i].endTimeSec;
				if (gap > maxGap) {
					maxGap = gap;
					bestGapIdx = static_cast<int>(i);
				}
			}
			if (maxGap >= 0.15 && bestGapIdx >= 0) {
				forceIdx = bestGapIdx;
			}
		}

		// If still nothing, force-commit all uncommitted words so no phrase is cut in half
		if (forceIdx < 0 && !uncommitted.empty()) {
			forceIdx = static_cast<int>(uncommitted.size()) - 1;
		}

		if (forceIdx >= 0) {
			std::string forceText;
			for (int i = 0; i <= forceIdx; ++i) {
				if (i > 0)
					forceText += " ";
				forceText += uncommitted[i].text;
			}

			if (m_liveScripture) {
				auto refs = m_liveScripture->push(forceText, true, nowMs);
				for (const auto &r : refs) {
					int rConf = static_cast<int>(std::round(r.confidence * 100.0));
					emit scriptureRefDetected(QString::fromStdString(r.label), rConf,
								  QString::fromStdString(r.status),
								  QString::fromStdString(r.kind), uncommitted[forceIdx].endTimeSec);
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
					sugg.confidence = rConf;
					sugg.tentative = (r.status == "preview");
					sugg.timestampSeconds = uncommitted[forceIdx].endTimeSec;
					emit suggestionDetected(sugg);
				}
			}

			m_committedHistory += " " + forceText;
			if (m_committedHistory.size() > 1000) {
				m_committedHistory = m_committedHistory.substr(m_committedHistory.size() - 800);
			}
			m_lastCommittedTimeSec = uncommitted[forceIdx].endTimeSec;
			emit transcriptCommitted(QString::fromStdString(forceText), m_lastCommittedTimeSec);
			emit rawTranscriptPass(QString::fromStdString(forceText), m_lastCommittedTimeSec);

			double trimToSec = uncommitted[forceIdx].endTimeSec;
			if (trimToSec > m_bufStartSec) {
				double trimSec = trimToSec - m_bufStartSec;
				size_t drop = std::min(m_streamBuffer.size(), static_cast<size_t>(trimSec * 16000.0));
				if (drop > 0) {
					m_streamBuffer.erase(m_streamBuffer.begin(), m_streamBuffer.begin() + drop);
					m_bufStartSec += static_cast<double>(drop) / 16000.0;
				}
			}
			m_prevPassWords.clear();
		}
	}

	m_lastProcessedSamples = m_streamBuffer.size();

	auto tPassEnd = std::chrono::steady_clock::now();
	double passMs = std::chrono::duration<double, std::milli>(tPassEnd - tPassStart).count();
	double otherMs = std::max(0.0, passMs - vadMs - m_lastWhisperMs);
	double bufDurSec = static_cast<double>(audioCopy.size()) / 16000.0;

	QString passLog = QStringLiteral("[pass: buf=%1s, total=%2ms (vad=%3ms, whisper=%4ms, other=%5ms)]")
				  .arg(QString::number(bufDurSec, 'f', 1))
				  .arg(QString::number(passMs, 'f', 1))
				  .arg(QString::number(vadMs, 'f', 1))
				  .arg(QString::number(m_lastWhisperMs, 'f', 1))
				  .arg(QString::number(otherMs, 'f', 1));
	emit rawTranscriptPass(passLog, bufStart + bufDurSec);
}

std::vector<TimedWord> DetectionCore::transcribeWords(const std::vector<float> &audio, double bufStartSec)
{
	std::vector<TimedWord> words;
	std::lock_guard<std::mutex> lock(m_whisperMutex);
	if (!m_whisperCtx || audio.empty())
		return words;

	whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	wparams.strategy = WHISPER_SAMPLING_GREEDY;
	wparams.temperature = 0.0f;
	wparams.temperature_inc = 0.0f;
	double bufferSeconds = static_cast<double>(audio.size()) / 16000.0;
	int maxTokens = std::max(64, static_cast<int>(std::ceil(bufferSeconds * 12.0)));
	wparams.max_tokens = maxTokens;
	wparams.no_timestamps = false;
	wparams.token_timestamps = true;
	wparams.single_segment = false;
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

	// Size audio_ctx to the buffer (~buffer_seconds/30*1500, multiple of 64, min 384, max 1500)
	int targetCtx = static_cast<int>(std::ceil((bufferSeconds / 30.0) * 1500.0));
	targetCtx = ((targetCtx + 63) / 64) * 64;
	wparams.audio_ctx = std::clamp(targetCtx, 384, 1500);

	// Step 10: Silero VAD (built into whisper.cpp 1.7.6+)
	char *vadCfg = obs_module_config_path("models/ggml-silero-v5.1.2.bin");
	std::string vadPath = vadCfg ? vadCfg : "";
	bfree(vadCfg);

	if (!vadPath.empty() && QFile::exists(QString::fromStdString(vadPath))) {
		wparams.vad = true;
		wparams.vad_model_path = vadPath.c_str();
		wparams.vad_params = whisper_vad_default_params();
	}

	// Step 9: Neutral sermon prompt + last ~200 characters of committed text. Never put example references in prompt.
	static constexpr char kNeutralSermonPrompt[] = "A sermon. The preacher reads from the Bible.";
	std::string prompt = kNeutralSermonPrompt;
	if (!m_committedHistory.empty()) {
		size_t take = std::min(m_committedHistory.size(), static_cast<size_t>(200));
		prompt += " " + m_committedHistory.substr(m_committedHistory.size() - take);
	}
	wparams.initial_prompt = prompt.c_str();

	auto tWhisperStart = std::chrono::steady_clock::now();
	int ret = whisper_full(m_whisperCtx, wparams, audio.data(), static_cast<int>(audio.size()));
	auto tWhisperEnd = std::chrono::steady_clock::now();
	m_lastWhisperMs = std::chrono::duration<double, std::milli>(tWhisperEnd - tWhisperStart).count();

	if (ret != 0) {
		return words;
	}

	int n_segments = whisper_full_n_segments(m_whisperCtx);
	for (int s = 0; s < n_segments; ++s) {
		// Step 11: Hallucination filter - drop if no_speech_prob > 0.6
		float noSpeechProb = whisper_full_get_segment_no_speech_prob(m_whisperCtx, s);
		if (noSpeechProb > 0.6f) {
			continue;
		}

		// Step 11: Drop stock silence outputs and text that is mostly the prompt
		const char *segTextRaw = whisper_full_get_segment_text(m_whisperCtx, s);
		std::string segText = segTextRaw ? segTextRaw : "";
		if (isStockSilenceOutput(segText)) {
			continue;
		}
		if (isMostlyPrompt(segText, prompt)) {
			continue;
		}

		std::vector<TimedWord> segWords;
		int n_tokens = whisper_full_n_tokens(m_whisperCtx, s);
		whisper_token eotToken = whisper_token_eot(m_whisperCtx);
		for (int t = 0; t < n_tokens; ++t) {
			whisper_token id = whisper_full_get_token_id(m_whisperCtx, s, t);
			if (id >= eotToken)
				continue;

			const char *txt = whisper_full_get_token_text(m_whisperCtx, s, t);
			if (!txt || txt[0] == '\0')
				continue;
			std::string tokenStr(txt);
			if (tokenStr.rfind("[_", 0) == 0 || tokenStr.rfind("<|", 0) == 0)
				continue;

			whisper_token_data data = whisper_full_get_token_data(m_whisperCtx, s, t);

			bool isStartOfWord = (tokenStr[0] == ' ' || tokenStr[0] == '\t' || tokenStr[0] == '\n');
			if (isStartOfWord || segWords.empty()) {
				std::string w = isStartOfWord ? tokenStr.substr(1) : tokenStr;
				if (!w.empty()) {
					double t0 = bufStartSec + (static_cast<double>(data.t0) * 0.01);
					double t1 = bufStartSec + (static_cast<double>(data.t1) * 0.01);
					segWords.push_back({w, t0, t1});
				}
			} else {
				segWords.back().text += tokenStr;
				double t1 = bufStartSec + (static_cast<double>(data.t1) * 0.01);
				segWords.back().endTimeSec = std::max(segWords.back().endTimeSec, t1);
			}
		}

		// Step 11: Drop segment if it repeats a 2-4 word phrase 3+ times
		if (hasRepetitionLoop(segWords)) {
			continue;
		}

		words.insert(words.end(), segWords.begin(), segWords.end());
	}

	if (hasRepetitionLoop(words)) {
		words.clear();
	}
	return words;
}

QString DetectionCore::transcribeChunk(const std::vector<float> &chunk)
{
	std::vector<float> audioCopy = chunk;
	applyHighPass80Hz(audioCopy, 16000.0f);
	applyAgcTo20dBFS(audioCopy);
	auto words = transcribeWords(audioCopy, 0.0);
	QString res;
	for (size_t i = 0; i < words.size(); ++i) {
		if (i > 0)
			res += QLatin1Char(' ');
		res += QString::fromStdString(words[i].text);
	}
	return res;
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
