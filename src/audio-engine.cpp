/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "audio-engine.hpp"
#include "bible.hpp"
#include "lyrics.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <whisper.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace {

static void on_source_audio(void *param, obs_source_t *, const struct audio_data *data, bool muted)
{
	if (!param)
		return;
	auto *engine = static_cast<AudioEngine *>(param);
	if (!data || data->frames == 0 || muted) {
		engine->processAudio(nullptr, 0, muted);
		return;
	}
	const float *samples = reinterpret_cast<const float *>(data->data[0]);
	engine->processAudio(samples, data->frames, muted);
}

struct SourceEnumCtx {
	QList<QString> list;
};

static bool enum_audio_sources(void *param, obs_source_t *source)
{
	auto *ctx = static_cast<SourceEnumCtx *>(param);
	if (!source)
		return true;
	uint32_t flags = obs_source_get_output_flags(source);
	if (flags & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		if (name && strlen(name) > 0)
			ctx->list.append(QString::fromUtf8(name));
	}
	return true;
}

QString findExistingModelPath()
{
	char *bundled = obs_module_file("models/ggml-tiny.en.bin");
	if (bundled) {
		QString s = QString::fromUtf8(bundled);
		bfree(bundled);
		if (QFile::exists(s))
			return s;
	}
	char *cfg = obs_module_config_path("models/ggml-tiny.en.bin");
	if (cfg) {
		QString s = QString::fromUtf8(cfg);
		bfree(cfg);
		if (QFile::exists(s))
			return s;
	}
	return QString();
}

QString defaultModelDestPath()
{
	char *cfg = obs_module_config_path("models/ggml-tiny.en.bin");
	QString s = cfg ? QString::fromUtf8(cfg) : QString();
	bfree(cfg);
	return s;
}

QString normalizeSpokenText(QString s)
{
	s = s.toLower();
	s.replace(QRegularExpression(QStringLiteral("[,.:;?!\\-\\(\\)—_]")), QStringLiteral(" "));

	// Roman numerals before Bible books: e.g. "ii timothy" -> "2 timothy"
	s.replace(QRegularExpression(QStringLiteral("\\bi\\s+(?=(?:corinthians|samuel|kings|chronicles|thessalonians|timothy|peter|john))")), QStringLiteral("1 "));
	s.replace(QRegularExpression(QStringLiteral("\\bii\\s+(?=(?:corinthians|samuel|kings|chronicles|thessalonians|timothy|peter|john))")), QStringLiteral("2 "));
	s.replace(QRegularExpression(QStringLiteral("\\biii\\s+(?=(?:john))")), QStringLiteral("3 "));

	// Saint / St prefixes
	s.replace(QRegularExpression(QStringLiteral("\\b(?:st|saint)\\s+(?=(?:matthew|mark|luke|john|peter|paul|james|jude))")), QStringLiteral(""));

	// Ordinals
	s.replace(QRegularExpression(QStringLiteral("\\bfirst\\b")), QStringLiteral("1"));
	s.replace(QRegularExpression(QStringLiteral("\\bsecond\\b")), QStringLiteral("2"));
	s.replace(QRegularExpression(QStringLiteral("\\bthird\\b")), QStringLiteral("3"));
	s.replace(QRegularExpression(QStringLiteral("\\bfourth\\b")), QStringLiteral("4"));
	s.replace(QRegularExpression(QStringLiteral("\\bfifth\\b")), QStringLiteral("5"));
	s.replace(QRegularExpression(QStringLiteral("\\b1st\\b")), QStringLiteral("1"));
	s.replace(QRegularExpression(QStringLiteral("\\b2nd\\b")), QStringLiteral("2"));
	s.replace(QRegularExpression(QStringLiteral("\\b3rd\\b")), QStringLiteral("3"));

	// Numbers
	static const struct {
		const char *word;
		int val;
	} kNumWords[] = {{"one", 1},        {"two", 2},       {"three", 3},     {"four", 4},      {"five", 5},
			 {"six", 6},        {"seven", 7},     {"eight", 8},     {"nine", 9},      {"ten", 10},
			 {"eleven", 11},    {"twelve", 12},   {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15},
			 {"sixteen", 16},   {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20},
			 {"thirty", 30},    {"forty", 40},    {"fifty", 50},    {"sixty", 60},    {"seventy", 70},
			 {"eighty", 80},    {"ninety", 90},   {"hundred", 100}};
	for (const auto &nw : kNumWords) {
		s.replace(QRegularExpression(QStringLiteral("\\b%1\\b").arg(nw.word)), QString::number(nw.val));
	}

	// Composite numbers e.g. "20 8" -> "28"
	static const int tens[] = {20, 30, 40, 50, 60, 70, 80, 90};
	for (int t : tens) {
		for (int u = 1; u <= 9; ++u) {
			s.replace(QStringLiteral("%1 %2").arg(t).arg(u), QString::number(t + u));
		}
	}

	// Psalm 119 and hundred composite: "100 19" -> "119"
	for (int u = 1; u <= 99; ++u) {
		s.replace(QStringLiteral("100 %1").arg(u), QString::number(100 + u));
	}

	return s.simplified();
}

} // namespace

AudioEngine &AudioEngine::instance()
{
	static AudioEngine s_instance;
	return s_instance;
}

AudioEngine::AudioEngine()
	: m_ringBuffer(kBufferSize, 0.0f)
{
	startWorker();
	loadModel();
}

AudioEngine::~AudioEngine()
{
	setAudioSource(QString());
	stopWorker();

	std::lock_guard<std::mutex> lock(m_whisperMutex);
	if (m_whisperCtx) {
		whisper_free(m_whisperCtx);
		m_whisperCtx = nullptr;
	}
}

QList<QString> AudioEngine::availableAudioSources()
{
	SourceEnumCtx ctx;
	obs_enum_sources(enum_audio_sources, &ctx);
	return ctx.list;
}

void AudioEngine::setAudioSource(const QString &sourceName)
{
	std::lock_guard<std::mutex> lock(m_sourceMutex);

	if (m_currentSource) {
		obs_source_remove_audio_capture_callback(m_currentSource, on_source_audio, this);
		obs_source_release(m_currentSource);
		m_currentSource = nullptr;
	}

	m_currentSourceName = sourceName;
	m_level.store(0.0f, std::memory_order_relaxed);

	if (!sourceName.isEmpty() && sourceName != QStringLiteral("None") && sourceName != QStringLiteral("Off")) {
		m_currentSource = obs_get_source_by_name(sourceName.toUtf8().constData());
		if (m_currentSource) {
			obs_source_add_audio_capture_callback(m_currentSource, on_source_audio, this);
			m_listening.store(true, std::memory_order_relaxed);
			emit statusChanged(true, sourceName);
			return;
		}
	}

	m_listening.store(false, std::memory_order_relaxed);
	emit statusChanged(false, QString());
}

QString AudioEngine::currentAudioSource() const
{
	std::lock_guard<std::mutex> lock(m_sourceMutex);
	return m_currentSourceName;
}

bool AudioEngine::isListening() const
{
	return m_listening.load(std::memory_order_relaxed);
}

float AudioEngine::currentLevel() const
{
	return m_level.load(std::memory_order_relaxed);
}

bool AudioEngine::isModelLoaded() const
{
	return m_modelLoaded.load(std::memory_order_relaxed);
}

QString AudioEngine::modelStatusText() const
{
	return m_modelStatusText;
}

void AudioEngine::loadModel()
{
	QString path = findExistingModelPath();
	if (path.isEmpty()) {
		m_modelStatusText = QStringLiteral("Model not found on disk");
		m_modelLoaded.store(false, std::memory_order_relaxed);
		emit modelLoadedChanged(false, m_modelStatusText);

		// Trigger background download to user config directory
		QString dest = defaultModelDestPath();
		if (!dest.isEmpty()) {
			QFileInfo fi(dest);
			QDir().mkpath(fi.dir().absolutePath());

			// Download via curl in background
			std::thread([this, dest]() {
				QStringList args;
				args << QStringLiteral("-L") << QStringLiteral("-s") << QStringLiteral("-o") << dest
				     << QStringLiteral(
					    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin");
				int code = QProcess::execute(QStringLiteral("curl"), args);
				if (code == 0 && QFile::exists(dest)) {
					loadModel();
				}
			}).detach();
		}
		return;
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
		m_modelStatusText = QStringLiteral("Speech Model Active (Offline)");
		obs_log(LOG_INFO, "[LD Audio] Whisper speech model loaded from %s", path.toUtf8().constData());
		emit modelLoadedChanged(true, m_modelStatusText);
	} else {
		m_modelLoaded.store(false, std::memory_order_relaxed);
		m_modelStatusText = QStringLiteral("Failed to load Whisper context");
		obs_log(LOG_WARNING, "[LD Audio] Could not initialize Whisper model from %s",
			path.toUtf8().constData());
		emit modelLoadedChanged(false, m_modelStatusText);
	}
}

void AudioEngine::setChannelMode(Channel ch)
{
	m_channel.store(ch, std::memory_order_relaxed);
}

Channel AudioEngine::channelMode() const
{
	return m_channel.load(std::memory_order_relaxed);
}

void AudioEngine::processAudio(const float *samples, uint32_t frames, bool muted)
{
	if (!samples || frames == 0 || muted) {
		m_level.store(0.0f, std::memory_order_relaxed);
		return;
	}

	// Calculate RMS level for live confidence meter
	float sum = 0.0f;
	for (uint32_t i = 0; i < frames; i++) {
		float s = samples[i];
		sum += s * s;
	}
	float rms = sqrtf(sum / static_cast<float>(frames));

	// Convert to normalized dB range: -50 dB (noise floor) to 0 dB
	float dB = (rms > 0.00001f) ? 20.0f * log10f(rms) : -60.0f;
	float norm = std::clamp((dB + 50.0f) / 50.0f, 0.0f, 1.0f);
	m_level.store(norm, std::memory_order_relaxed);

	// Resample to 16,000 Hz using linear interpolation
	uint32_t inRate = 48000;
	struct obs_audio_info aoi;
	if (obs_get_audio_info(&aoi) && aoi.samples_per_sec > 0) {
		inRate = aoi.samples_per_sec;
	}

	const double ratio = static_cast<double>(inRate) / 16000.0;
	size_t w = m_writeIndex.load(std::memory_order_relaxed);

	while (m_resamplePos < static_cast<double>(frames) - 1.0) {
		size_t idx0 = static_cast<size_t>(m_resamplePos);
		size_t idx1 = idx0 + 1;
		float frac = static_cast<float>(m_resamplePos - idx0);
		float sample = (1.0f - frac) * samples[idx0] + frac * samples[idx1];

		m_ringBuffer[w] = sample;
		w = (w + 1) % kBufferSize;

		m_resamplePos += ratio;
	}
	m_resamplePos -= static_cast<double>(frames);
	if (m_resamplePos < 0.0)
		m_resamplePos = 0.0;

	m_writeIndex.store(w, std::memory_order_release);
}

void AudioEngine::startWorker()
{
	m_workerRunning.store(true, std::memory_order_relaxed);
	m_workerThread = std::make_unique<std::thread>(&AudioEngine::workerLoop, this);
}

void AudioEngine::stopWorker()
{
	m_workerRunning.store(false, std::memory_order_relaxed);
	if (m_workerThread && m_workerThread->joinable()) {
		m_workerThread->join();
	}
	m_workerThread.reset();
}

void AudioEngine::workerLoop()
{
	// 3.5 seconds of audio @ 16kHz = 56,000 samples
	constexpr size_t kChunkSize = 56000;
	// Hop / advance by 1.25 seconds = 20,000 samples (64% overlap)
	constexpr size_t kHopSize = 20000;

	std::vector<float> chunk(kChunkSize, 0.0f);

	while (m_workerRunning.load(std::memory_order_relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));

		if (!m_listening.load(std::memory_order_relaxed))
			continue;

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

		analyzeBuffer(chunk);
	}
}

void AudioEngine::analyzeBuffer(const std::vector<float> &chunk)
{
	// Check peak energy in 250ms sub-windows so quiet speech isn't cut off
	float maxSubRms = 0.0f;
	constexpr size_t subSize = 4000; // 250ms @ 16kHz
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

	// Gate threshold: 0.003f (-50 dB)
	if (maxSubRms < 0.003f)
		return;

	QString transcript = transcribeChunk(chunk);
	if (transcript.isEmpty())
		return;

	// Maintain rolling context of last transcribed words to never lose cross-chunk phrases
	QString fullContext = m_recentTranscript;
	if (!fullContext.isEmpty())
		fullContext += QLatin1Char(' ');
	fullContext += transcript;

	QStringList words = fullContext.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (words.size() > 30) {
		words = words.mid(words.size() - 30);
		fullContext = words.join(QLatin1Char(' '));
	}
	m_recentTranscript = transcript;

	Channel ch = m_channel.load(std::memory_order_relaxed);
	if (ch == Channel::Scripture) {
		if (!detectScripture(fullContext))
			detectLyrics(fullContext);
	} else {
		if (!detectLyrics(fullContext))
			detectScripture(fullContext);
	}
}

QString AudioEngine::transcribeChunk(const std::vector<float> &chunk)
{
	std::lock_guard<std::mutex> lock(m_whisperMutex);
	if (!m_whisperCtx)
		return QString();

	whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	wparams.print_progress = false;
	wparams.print_special = false;
	wparams.print_realtime = false;
	wparams.print_timestamps = false;
	wparams.language = "en";
	wparams.n_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 4);
	wparams.translate = false;
	wparams.no_context = true;
	wparams.single_segment = false;
	wparams.initial_prompt =
		"Church service scripture reading and worship song lyrics. Books of the Bible: Genesis, Exodus, Leviticus, Numbers, Deuteronomy, Joshua, Judges, Ruth, Samuel, Kings, Chronicles, Ezra, Nehemiah, Esther, Job, Psalms, Proverbs, Ecclesiastes, Song of Solomon, Isaiah, Jeremiah, Ezekiel, Daniel, Matthew, Mark, Luke, John, Acts, Romans, Corinthians, Galatians, Ephesians, Philippians, Colossians, Thessalonians, Timothy, Titus, Philemon, Hebrews, James, Peter, Revelation. Chapter and verse numbers. Praise the Lord, Jesus, Holy Spirit, Chorus, Verse.";

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

bool AudioEngine::detectScripture(const QString &rawTranscript)
{
	uint64_t now = QDateTime::currentMSecsSinceEpoch();
	if (now - m_lastSuggestionTime < 2500)
		return false;

	QString text = normalizeSpokenText(rawTranscript);
	if (text.isEmpty())
		return false;

	struct BookAlias {
		const char *alias;
		const char *canonical;
	};

	static const BookAlias kAliases[] = {
		// Multi-word aliases first
		{"1 thessalonians", "1 Thessalonians"}, {"first thessalonians", "1 Thessalonians"}, {"1st thessalonians", "1 Thessalonians"},
		{"2 thessalonians", "2 Thessalonians"}, {"second thessalonians", "2 Thessalonians"}, {"2nd thessalonians", "2 Thessalonians"},
		{"1 chronicles", "1 Chronicles"}, {"first chronicles", "1 Chronicles"}, {"1st chronicles", "1 Chronicles"},
		{"2 chronicles", "2 Chronicles"}, {"second chronicles", "2 Chronicles"}, {"2nd chronicles", "2 Chronicles"},
		{"1 corinthians", "1 Corinthians"}, {"first corinthians", "1 Corinthians"}, {"1st corinthians", "1 Corinthians"},
		{"2 corinthians", "2 Corinthians"}, {"second corinthians", "2 Corinthians"}, {"2nd corinthians", "2 Corinthians"},
		{"song of solomon", "Song of Solomon"}, {"song of songs", "Song of Solomon"}, {"canticles", "Song of Solomon"},
		{"1 timothy", "1 Timothy"}, {"first timothy", "1 Timothy"}, {"1st timothy", "1 Timothy"},
		{"2 timothy", "2 Timothy"}, {"second timothy", "2 Timothy"}, {"2nd timothy", "2 Timothy"},
		{"1 samuel", "1 Samuel"}, {"first samuel", "1 Samuel"}, {"1st samuel", "1 Samuel"},
		{"2 samuel", "2 Samuel"}, {"second samuel", "2 Samuel"}, {"2nd samuel", "2 Samuel"},
		{"1 kings", "1 Kings"}, {"first kings", "1 Kings"}, {"1st kings", "1 Kings"},
		{"2 kings", "2 Kings"}, {"second kings", "2 Kings"}, {"2nd kings", "2 Kings"},
		{"1 peter", "1 Peter"}, {"first peter", "1 Peter"}, {"1st peter", "1 Peter"},
		{"2 peter", "2 Peter"}, {"second peter", "2 Peter"}, {"2nd peter", "2 Peter"},
		{"1 john", "1 John"}, {"first john", "1 John"}, {"1st john", "1 John"},
		{"2 john", "2 John"}, {"second john", "2 John"}, {"2nd john", "2 John"},
		{"3 john", "3 John"}, {"third john", "3 John"}, {"3rd john", "3 John"},
		{"revelations", "Revelation"}, {"revelation", "Revelation"},
		{"psalms", "Psalms"}, {"psalm", "Psalms"},
		{"genesis", "Genesis"}, {"exodus", "Exodus"}, {"leviticus", "Leviticus"},
		{"numbers", "Numbers"}, {"deuteronomy", "Deuteronomy"}, {"joshua", "Joshua"},
		{"judges", "Judges"}, {"ruth", "Ruth"}, {"ezra", "Ezra"}, {"nehemiah", "Nehemiah"},
		{"esther", "Esther"}, {"job", "Job"}, {"proverbs", "Proverbs"},
		{"ecclesiastes", "Ecclesiastes"}, {"isaiah", "Isaiah"}, {"jeremiah", "Jeremiah"},
		{"lamentations", "Lamentations"}, {"ezekiel", "Ezekiel"}, {"daniel", "Daniel"},
		{"hosea", "Hosea"}, {"joel", "Joel"}, {"amos", "Amos"}, {"obadiah", "Obadiah"},
		{"jonah", "Jonah"}, {"micah", "Micah"}, {"nahum", "Nahum"}, {"habakkuk", "Habakkuk"},
		{"zephaniah", "Zephaniah"}, {"haggai", "Haggai"}, {"zechariah", "Zechariah"},
		{"malachi", "Malachi"}, {"matthew", "Matthew"}, {"mark", "Mark"},
		{"luke", "Luke"}, {"john", "John"}, {"acts", "Acts"}, {"romans", "Romans"},
		{"galatians", "Galatians"}, {"ephesians", "Ephesians"}, {"philippians", "Philippians"},
		{"colossians", "Colossians"}, {"titus", "Titus"}, {"philemon", "Philemon"},
		{"hebrews", "Hebrews"}, {"james", "James"}, {"jude", "Jude"}
	};

	for (const auto &b : kAliases) {
		// Pattern 1: Standard [Book] [Chapter] [Verse] with connectors
		QRegularExpression rx(QStringLiteral(
			"\\b%1\\b(?:\\s*(?:chapter|ch)?\\s*(\\d+))?(?:\\s*(?:[:,\-]|and\\s+(?:verse|v)?|from\\s+(?:verse|v)?|starting\\s+(?:at\\s+)?(?:verse|v)?|at\\s+(?:verse|v)?|verse|verses|v)?\\s*(\\d+))?"
		).arg(QRegularExpression::escape(QString::fromUtf8(b.alias))));

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

				if (ref != m_lastSuggestedRef) {
					m_lastSuggestedRef = ref;
					m_lastSuggestionTime = now;
					emit suggestionDetected(Channel::Scripture, ref,
								QStringLiteral("Detected from audio"), 95);
					return true;
				}
			}
		}

		// Pattern 2: Inverted order: Verse [V] of [Book] [Chapter]
		QRegularExpression rxInv(QStringLiteral(
			"\\b(?:verse|verses|v)\\s*(\\d+)\\s*(?:of|in|from)\\s*%1\\b(?:\\s*(?:chapter|ch)?\\s*(\\d+))?"
		).arg(QRegularExpression::escape(QString::fromUtf8(b.alias))));

		QRegularExpressionMatch mInv = rxInv.match(text);
		if (mInv.hasMatch()) {
			int vs = mInv.captured(1).toInt();
			int ch = mInv.captured(2).toInt();
			if (ch > 0 && vs > 0) {
				QString ref = QStringLiteral("%1 %2:%3").arg(b.canonical).arg(ch).arg(vs);
				if (ref != m_lastSuggestedRef) {
					m_lastSuggestedRef = ref;
					m_lastSuggestionTime = now;
					emit suggestionDetected(Channel::Scripture, ref,
								QStringLiteral("Detected from audio"), 95);
					return true;
				}
			}
		}
	}

	// Reverse quote lookup
	static const struct {
		const char *phrase;
		const char *ref;
		const char *verse;
	} kQuotes[] = {
		{"for god so loved the world", "John 3:16",
		 "For God so loved the world, that he gave his only begotten Son..."},
		{"the lord is my shepherd", "Psalm 23:1", "The Lord is my shepherd; I shall not want."},
		{"all things work together for good", "Romans 8:28",
		 "And we know that all things work together for good to them that love God..."},
		{"i can do all things through christ", "Philippians 4:13",
		 "I can do all things through Christ which strengtheneth me."},
		{"trust in the lord with all thine heart", "Proverbs 3:5",
		 "Trust in the Lord with all thine heart; and lean not unto thine own understanding."},
		{"in the beginning god created", "Genesis 1:1",
		 "In the beginning God created the heaven and the earth."},
		{"be strong and courageous", "Joshua 1:9",
		 "Be strong and of a good courage; be not afraid, neither be thou dismayed."},
		{"the joy of the lord is your strength", "Nehemiah 8:10",
		 "The joy of the LORD is your strength."},
		{"by his stripes we are healed", "Isaiah 53:5",
		 "With his stripes we are healed."},
		{"if god be for us who can be against us", "Romans 8:31",
		 "If God be for us, who can be against us?"}
	};

	for (const auto &q : kQuotes) {
		if (text.contains(QString::fromUtf8(q.phrase))) {
			QString ref = QString::fromUtf8(q.ref);
			if (ref != m_lastSuggestedRef) {
				m_lastSuggestedRef = ref;
				m_lastSuggestionTime = now;
				emit suggestionDetected(Channel::Scripture, ref, QString::fromUtf8(q.verse), 92);
				return true;
			}
		}
	}

	return false;
}

bool AudioEngine::detectLyrics(const QString &rawTranscript)
{
	uint64_t now = QDateTime::currentMSecsSinceEpoch();
	if (now - m_lastSuggestionTime < 2500)
		return false;

	QString text = normalizeSpokenText(rawTranscript);
	if (text.length() < 5)
		return false;

	QStringList transcriptWords = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (transcriptWords.isEmpty())
		return false;

	static const QSet<QString> kStopWords = {
		QStringLiteral("the"), QStringLiteral("and"), QStringLiteral("a"),
		QStringLiteral("to"), QStringLiteral("in"), QStringLiteral("is"),
		QStringLiteral("of"), QStringLiteral("that"), QStringLiteral("it"),
		QStringLiteral("for"), QStringLiteral("on"), QStringLiteral("with"),
		QStringLiteral("at"), QStringLiteral("by"), QStringLiteral("from"),
		QStringLiteral("up"), QStringLiteral("an"), QStringLiteral("my"),
		QStringLiteral("your"), QStringLiteral("we"), QStringLiteral("i")
	};

	struct Candidate {
		QString songTitle;
		QString sectionName;
		QString sectionCode;
		QString matchedLine;
		int confidence = 0;
	};

	Candidate best;

	const auto &songs = SongLibrary::instance().songs();
	for (const auto &song : songs) {
		QString normTitle = normalizeSpokenText(song.title);

		// Check 1: Direct Song Title match
		if (!normTitle.isEmpty() && normTitle.length() >= 5) {
			if (text.contains(normTitle)) {
				Candidate c;
				c.songTitle = song.title;
				c.matchedLine = song.title;
				c.confidence = 96;
				if (c.confidence > best.confidence)
					best = c;
			}
		}

		// Check 2: Sections & Slides matching
		for (const auto &sec : song.sections) {
			for (const auto &slide : sec.rawSlides) {
				for (const auto &line : slide) {
					QString normLine = normalizeSpokenText(line);
					if (normLine.length() < 6)
						continue;

					// A) Substring match (either direction)
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

					// B) N-Gram / Phrase match: Check for 3 consecutive matching words
					QStringList lineWords = normLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
					if (lineWords.size() >= 3 && transcriptWords.size() >= 3) {
						bool ngramMatched = false;
						for (int li = 0; li <= lineWords.size() - 3; ++li) {
							if (kStopWords.contains(lineWords[li]) &&
							    kStopWords.contains(lineWords[li + 1]) &&
							    kStopWords.contains(lineWords[li + 2]))
								continue;

							QString trigram = QStringLiteral("%1 %2 %3")
										  .arg(lineWords[li], lineWords[li + 1], lineWords[li + 2]);
							if (text.contains(trigram)) {
								Candidate c;
								c.songTitle = song.title;
								c.sectionName = sec.name;
								c.sectionCode = sec.shortCode;
								c.matchedLine = line;
								c.confidence = 90;
								if (c.confidence > best.confidence)
									best = c;
								ngramMatched = true;
								break;
							}
						}
						if (ngramMatched)
							continue;
					}

					// C) Keyword Overlap match: Count shared significant words
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
						float ratio = static_cast<float>(matchCount) / static_cast<float>(totalSig);
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

	if (best.confidence >= 80) {
		QString displayRef;
		if (!best.sectionName.isEmpty()) {
			displayRef = QStringLiteral("%1 · %2").arg(best.songTitle, best.sectionName);
		} else {
			displayRef = best.songTitle;
		}

		if (displayRef != m_lastSuggestedRef) {
			m_lastSuggestedRef = displayRef;
			m_lastSuggestionTime = now;
			emit suggestionDetected(Channel::Lyrics, displayRef, best.songTitle, best.confidence);
			return true;
		}
	}

	return false;
}

// AudioMeterWidget Implementation
AudioMeterWidget::AudioMeterWidget(QWidget *parent)
	: QWidget(parent)
{
	setFixedSize(54, 14);
	setToolTip(QStringLiteral("Audio Signal Meter"));

	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, &AudioMeterWidget::tick);
	m_timer->start(33); // 30 FPS update
}

void AudioMeterWidget::tick()
{
	float target = AudioEngine::instance().currentLevel();
	if (target >= m_displayLevel) {
		m_displayLevel = target;
	} else {
		m_displayLevel = std::max(0.0f, m_displayLevel * 0.82f - 0.005f);
	}
	update();
}

void AudioMeterWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QRectF bounds = rect();
	const qreal h = bounds.height();

	// Dark pill background
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(18, 20, 24));
	p.drawRoundedRect(bounds, h / 2, h / 2);

	const int numBars = 9;
	const qreal gap = 2.0;
	const qreal padX = 4.0;
	const qreal barW = (bounds.width() - 2 * padX - (numBars - 1) * gap) / numBars;
	const qreal barH = h - 6.0;
	const qreal barY = 3.0;

	int activeBars = static_cast<int>(std::round(m_displayLevel * numBars));

	for (int i = 0; i < numBars; i++) {
		qreal x = padX + i * (barW + gap);
		QRectF barRect(x, barY, barW, barH);

		QColor col;
		if (i < 6)
			col = QColor(16, 185, 129); // Emerald Green
		else if (i < 8)
			col = QColor(245, 158, 11); // Amber / Yellow
		else
			col = QColor(239, 68, 68); // Red Peak

		if (i < activeBars) {
			p.setBrush(col);
		} else {
			col.setAlpha(45);
			p.setBrush(col);
		}
		p.drawRoundedRect(barRect, 1.0, 1.0);
	}
}
