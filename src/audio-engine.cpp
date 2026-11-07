/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "audio-engine.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <thread>

namespace {

static void on_source_audio(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	if (!param)
		return;
	auto *engine = static_cast<AudioEngine *>(param);
	engine->processAudio(data, source, muted);
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

struct ModelDef {
	const char *id;
	const char *name;
	const char *filename;
	const char *url;
};

static const ModelDef kModels[] = {
	{"small.en-q5_1", "small.en (Recommended, fast & accurate)", "ggml-small.en-q5_1.bin",
	 "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin"},
	{"base.en", "base.en (Fastest, low CPU)", "ggml-base.en.bin",
	 "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"},
	{"large-v3-turbo-q5_0", "large-v3-turbo (High accuracy, needs a fast Mac)", "ggml-large-v3-turbo-q5_0.bin",
	 "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin"},
};

static bool readWavFile(const QString &filePath, std::vector<float> &out16kMono, double &durationSec)
{
	QFile f(filePath);
	if (!f.open(QIODevice::ReadOnly)) {
		return false;
	}

	QByteArray data = f.readAll();
	f.close();

	if (data.size() < 44) {
		return false;
	}

	const char *bytes = data.constData();
	if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0) {
		return false;
	}

	uint16_t audioFormat = 0;
	uint16_t numChannels = 0;
	uint32_t sampleRate = 0;
	uint16_t bitsPerSample = 0;
	const char *audioData = nullptr;
	size_t audioDataSize = 0;

	size_t pos = 12;
	while (pos + 8 <= static_cast<size_t>(data.size())) {
		const char *chunkId = bytes + pos;
		uint32_t chunkSize = *reinterpret_cast<const uint32_t *>(bytes + pos + 4);
		pos += 8;

		if (memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
			audioFormat = *reinterpret_cast<const uint16_t *>(bytes + pos);
			numChannels = *reinterpret_cast<const uint16_t *>(bytes + pos + 2);
			sampleRate = *reinterpret_cast<const uint32_t *>(bytes + pos + 4);
			bitsPerSample = *reinterpret_cast<const uint16_t *>(bytes + pos + 14);
		} else if (memcmp(chunkId, "data", 4) == 0) {
			audioData = bytes + pos;
			audioDataSize = chunkSize;
			break;
		}
		pos += chunkSize;
	}

	if (!audioData || audioDataSize == 0 || sampleRate == 0 || numChannels == 0) {
		return false;
	}

	std::vector<float> inFloatMono;
	size_t numSamples = 0;

	if (audioFormat == 1 && bitsPerSample == 16) {
		const int16_t *pcm = reinterpret_cast<const int16_t *>(audioData);
		size_t totalWords = audioDataSize / sizeof(int16_t);
		numSamples = totalWords / numChannels;
		inFloatMono.reserve(numSamples);

		for (size_t i = 0; i < numSamples; ++i) {
			float sum = 0.0f;
			for (uint16_t ch = 0; ch < numChannels; ++ch) {
				sum += pcm[i * numChannels + ch] / 32768.0f;
			}
			inFloatMono.push_back(sum / numChannels);
		}
	} else if (audioFormat == 3 && bitsPerSample == 32) {
		const float *pcm = reinterpret_cast<const float *>(audioData);
		numSamples = audioDataSize / (sizeof(float) * numChannels);
		inFloatMono.reserve(numSamples);

		for (size_t i = 0; i < numSamples; ++i) {
			float sum = 0.0f;
			for (uint16_t ch = 0; ch < numChannels; ++ch) {
				sum += pcm[i * numChannels + ch];
			}
			inFloatMono.push_back(sum / numChannels);
		}
	} else {
		return false;
	}

	durationSec = static_cast<double>(numSamples) / static_cast<double>(sampleRate);

	if (sampleRate == 16000) {
		out16kMono = std::move(inFloatMono);
	} else {
		double ratio = 16000.0 / static_cast<double>(sampleRate);
		size_t outCount = static_cast<size_t>(numSamples * ratio);
		out16kMono.resize(outCount);
		for (size_t i = 0; i < outCount; ++i) {
			double srcIdx = i / ratio;
			size_t idx0 = static_cast<size_t>(srcIdx);
			size_t idx1 = std::min(idx0 + 1, numSamples - 1);
			float frac = static_cast<float>(srcIdx - idx0);
			out16kMono[i] = inFloatMono[idx0] * (1.0f - frac) + inFloatMono[idx1] * frac;
		}
	}

	return true;
}

static const ModelDef *findModelDef(const QString &id)
{
	for (const auto &m : kModels) {
		if (id == QString::fromUtf8(m.id))
			return &m;
	}
	return nullptr;
}

static QString getModelFilePath(const char *filename)
{
	QString rel = QStringLiteral("models/") + QString::fromUtf8(filename);
	char *cfg = obs_module_config_path(rel.toUtf8().constData());
	if (cfg) {
		QString s = QString::fromUtf8(cfg);
		bfree(cfg);
		if (QFile::exists(s))
			return s;
	}
	char *bundled = obs_module_file(rel.toUtf8().constData());
	if (bundled) {
		QString s = QString::fromUtf8(bundled);
		bfree(bundled);
		if (QFile::exists(s))
			return s;
	}
	return QString();
}

} // namespace

AudioEngine &AudioEngine::instance()
{
	static AudioEngine s_instance;
	return s_instance;
}

AudioEngine::AudioEngine()
{
	connect(&m_core, &DetectionCore::suggestionDetected, this, [this](const SpeechSuggestion &sugg) {
		emit suggestionDetected(sugg.channel, sugg.reference, sugg.text, sugg.confidence, sugg.tentative,
					sugg.isNextSlide);
	});

	connect(&m_core, &DetectionCore::modelLoadedChanged, this, &AudioEngine::modelLoadedChanged);
	connect(&m_core, &DetectionCore::rawTranscriptPass, this, &AudioEngine::rawTranscriptPass);
	connect(&m_core, &DetectionCore::scriptureRefDetected, this, &AudioEngine::scriptureRefDetected);
	connect(&m_core, &DetectionCore::scriptureSuggestion, this, &AudioEngine::scriptureSuggestion);

	char *vadCfgInit = obs_module_config_path("models/ggml-silero-v5.1.2.bin");
	if (vadCfgInit) {
		m_core.setVadModelPath(std::string(vadCfgInit));
		bfree(vadCfgInit);
	}

	loadModel();
}

AudioEngine::~AudioEngine()
{
	setAudioSource(QString());
	if (m_resampler) {
		audio_resampler_destroy(m_resampler);
		m_resampler = nullptr;
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
		m_listening.store(false, std::memory_order_relaxed);
		m_level.store(0.0f, std::memory_order_relaxed);
		emit levelChanged(0.0f);
	}

	m_currentSourceName = sourceName.trimmed();

	if (!m_currentSourceName.isEmpty()) {
		m_currentSource = obs_get_source_by_name(m_currentSourceName.toUtf8().constData());
		if (m_currentSource) {
			obs_source_add_audio_capture_callback(m_currentSource, on_source_audio, this);
			m_listening.store(true, std::memory_order_relaxed);
			obs_log(LOG_INFO, "[LD Audio] Listening to audio source: %s",
				m_currentSourceName.toUtf8().constData());
		} else {
			obs_log(LOG_WARNING, "[LD Audio] Source not found: %s",
				m_currentSourceName.toUtf8().constData());
		}
	}

	emit statusChanged(m_listening.load(std::memory_order_relaxed), m_currentSourceName);
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
	return m_core.isModelLoaded();
}

QString AudioEngine::modelStatusText() const
{
	return m_core.modelStatusText();
}

bool AudioEngine::loadModelFile(const QString &path)
{
	return m_core.loadModel(path);
}

QString AudioEngine::defaultModelId()
{
	return QStringLiteral("small.en-q5_1");
}

QString AudioEngine::currentModelChoice() const
{
	if (m_modelChoice.isEmpty()) {
		return defaultModelId();
	}
	return m_modelChoice;
}

void AudioEngine::setModelChoice(const QString &modelId)
{
	m_modelChoice = modelId;
	loadModel();
}

void AudioEngine::loadCustomModelFile(const QString &path)
{
	m_modelChoice = QStringLiteral("custom:") + path;
	m_core.loadModel(path);
}

void AudioEngine::loadModel()
{
	QString choice = currentModelChoice();
	if (choice.startsWith(QStringLiteral("custom:"))) {
		QString path = choice.mid(7);
		if (QFile::exists(path)) {
			m_core.loadModel(path);
			return;
		}
	}

	const ModelDef *def = findModelDef(choice);
	if (!def) {
		def = findModelDef(defaultModelId());
	}
	if (!def)
		return;

	QString existing = getModelFilePath(def->filename);
	if (!existing.isEmpty()) {
		m_core.loadModel(existing);
		return;
	}

	// Also check fallback models if default isn't downloaded yet
	for (const auto &m : kModels) {
		QString alt = getModelFilePath(m.filename);
		if (!alt.isEmpty()) {
			// Found an existing alternate model, but initiate download for the chosen one
			m_core.loadModel(alt);
			break;
		}
	}

	// Check if Silero VAD model exists, download in background if missing
	char *vadCfg = obs_module_config_path("models/ggml-silero-v5.1.2.bin");
	if (vadCfg) {
		QString vadPath = QString::fromUtf8(vadCfg);
		bfree(vadCfg);
		m_core.setVadModelPath(vadPath.toStdString());
		if (!QFile::exists(vadPath)) {
			std::thread([vadPath]() {
				QStringList args;
				args << QStringLiteral("-L") << QStringLiteral("-s") << QStringLiteral("-o") << vadPath
				     << QStringLiteral("https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin");
				QProcess::execute(QStringLiteral("curl"), args);
			}).detach();
		}
	}

	// Not on disk - download it to config folder
	downloadModel(QString::fromUtf8(def->id));
}

void AudioEngine::downloadModel(const QString &modelId)
{
	if (m_downloading.load(std::memory_order_relaxed))
		return;

	const ModelDef *def = findModelDef(modelId);
	if (!def)
		return;

	char *cfg = obs_module_config_path("models");
	QString dirPath = cfg ? QString::fromUtf8(cfg) : QString();
	bfree(cfg);
	if (dirPath.isEmpty())
		return;

	QDir().mkpath(dirPath);
	QString destPath = dirPath + QLatin1Char('/') + QString::fromUtf8(def->filename);
	QString partPath = destPath + QStringLiteral(".part");

	m_downloading.store(true, std::memory_order_relaxed);
	m_downloadPercent.store(0, std::memory_order_relaxed);
	emit modelDownloadProgress(0);

	auto *proc = new QProcess();
	QObject::connect(proc, &QProcess::readyReadStandardError, [this, proc]() {
		QByteArray err = proc->readAllStandardError();
		static QRegularExpression rx(QStringLiteral(R"((\d{1,3}(?:\.\d+)?)%)"));
		QRegularExpressionMatchIterator it = rx.globalMatch(QString::fromUtf8(err));
		int lastPct = -1;
		while (it.hasNext()) {
			auto match = it.next();
			lastPct = static_cast<int>(std::round(match.captured(1).toDouble()));
		}
		if (lastPct >= 0 && lastPct <= 100) {
			m_downloadPercent.store(lastPct, std::memory_order_relaxed);
			emit modelDownloadProgress(lastPct);
		}
	});

	QObject::connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
			 [this, proc, destPath, partPath](int exitCode, QProcess::ExitStatus) {
		m_downloading.store(false, std::memory_order_relaxed);
		proc->deleteLater();
		if (exitCode == 0 && QFile::exists(partPath)) {
			QFile::remove(destPath);
			QFile::rename(partPath, destPath);
			emit modelDownloadProgress(100);
			m_core.loadModel(destPath);
		} else {
			QFile::remove(partPath);
			emit modelDownloadProgress(-1);
			emit modelLoadedChanged(false, QStringLiteral("Model download failed"));
		}
	});

	QStringList args;
	args << QStringLiteral("-#") << QStringLiteral("-L") << QStringLiteral("-o") << partPath
	     << QString::fromUtf8(def->url);
	proc->start(QStringLiteral("curl"), args);
}

void AudioEngine::setChannelMode(Channel ch)
{
	m_core.setChannelMode(ch);
}

Channel AudioEngine::channelMode() const
{
	return m_core.channelMode();
}

void AudioEngine::processAudio(const struct audio_data *data, obs_source_t *source, bool muted)
{
	if (!data || data->frames == 0 || muted || !data->data[0]) {
		m_level.store(0.0f, std::memory_order_relaxed);
		return;
	}

	const float *samples = reinterpret_cast<const float *>(data->data[0]);

	// Calculate RMS level for signal meter
	float sum = 0.0f;
	for (uint32_t i = 0; i < data->frames; i++) {
		float s = samples[i];
		sum += s * s;
	}
	float rms = sqrtf(sum / static_cast<float>(data->frames));

	// Normalized dB range: -50 dB (noise floor) to 0 dB
	float dB = (rms > 0.00001f) ? 20.0f * log10f(rms) : -60.0f;
	float norm = std::clamp((dB + 50.0f) / 50.0f, 0.0f, 1.0f);
	m_level.store(norm, std::memory_order_relaxed);

	// Query OBS sample rate and speaker layout
	uint32_t inRate = 48000;
	enum speaker_layout layout = SPEAKERS_UNKNOWN;
	if (source) {
		layout = obs_source_get_speaker_layout(source);
	}
	struct obs_audio_info aoi;
	if (obs_get_audio_info(&aoi)) {
		if (aoi.samples_per_sec > 0)
			inRate = aoi.samples_per_sec;
		if (layout == SPEAKERS_UNKNOWN)
			layout = aoi.speakers;
	}
	if (layout == SPEAKERS_UNKNOWN) {
		layout = SPEAKERS_STEREO;
	}

	// Use OBS audio_resampler_t (media-io/audio-resampler.h) converting to 16 kHz mono float
	if (!m_resampler || m_resamplerInRate != inRate || m_resamplerLayout != layout) {
		if (m_resampler) {
			audio_resampler_destroy(m_resampler);
			m_resampler = nullptr;
		}

		struct resample_info src_info;
		src_info.samples_per_sec = inRate;
		src_info.format = AUDIO_FORMAT_FLOAT_PLANAR;
		src_info.speakers = layout;

		struct resample_info dst_info;
		dst_info.samples_per_sec = 16000;
		dst_info.format = AUDIO_FORMAT_FLOAT;
		dst_info.speakers = SPEAKERS_MONO;

		m_resampler = audio_resampler_create(&dst_info, &src_info);
		m_resamplerInRate = inRate;
		m_resamplerLayout = layout;
	}

	if (m_resampler) {
		uint8_t *output[MAX_AV_PLANES] = {nullptr};
		uint32_t out_frames = 0;
		uint64_t ts_offset = 0;
		bool ok = audio_resampler_resample(m_resampler, output, &out_frames, &ts_offset,
						   (const uint8_t *const *)data->data, data->frames);
		if (ok && out_frames > 0 && output[0]) {
			const float *resampled = reinterpret_cast<const float *>(output[0]);
			m_core.pushPcmSamples(resampled, out_frames);
		}
	}
}

void AudioEngine::runAudioFileTest(const QString &filePath, bool realTime)
{
	std::vector<float> samples;
	double duration = 0.0;
	if (!readWavFile(filePath, samples, duration)) {
		fprintf(stderr, "[LD Worship & Word] Failed to read audio file: %s\n",
			filePath.toUtf8().constData());
		return;
	}

	fprintf(stderr, "[LD Worship & Word] Running audio file test (%s): %s (%.1f sec, %zu samples)\n",
		realTime ? "real-time" : "batch", filePath.toUtf8().constData(), duration, samples.size());

	m_core.reset();

	constexpr size_t kChunkSize = 16000;
	double streamTime = 0.0;
	auto wallStart = std::chrono::steady_clock::now();

	size_t offset = 0;
	while (offset < samples.size()) {
		size_t targetOffset;
		if (realTime) {
			auto now = std::chrono::steady_clock::now();
			double wallElapsed = std::chrono::duration<double>(now - wallStart).count();
			targetOffset = std::min(samples.size(), static_cast<size_t>(wallElapsed * 16000.0));
			if (targetOffset <= offset) {
				double nextChunkTime = static_cast<double>(offset + kChunkSize) / 16000.0;
				if (nextChunkTime > wallElapsed) {
					std::this_thread::sleep_for(std::chrono::duration<double>(nextChunkTime - wallElapsed));
				}
				auto nowAfter = std::chrono::steady_clock::now();
				double wallAfter = std::chrono::duration<double>(nowAfter - wallStart).count();
				targetOffset = std::min(samples.size(), static_cast<size_t>(wallAfter * 16000.0));
			}
		} else {
			targetOffset = std::min(samples.size(), offset + kChunkSize);
		}

		if (targetOffset <= offset) {
			targetOffset = std::min(samples.size(), offset + kChunkSize);
		}

		std::vector<float> chunk(samples.begin() + offset, samples.begin() + targetOffset);
		streamTime = static_cast<double>(targetOffset) / 16000.0;
		offset = targetOffset;

		m_core.processWindowDirect(chunk, streamTime);
	}

	std::vector<float> silence(16000, 0.0f);
	streamTime += 1.0;
	m_core.processWindowDirect(silence, streamTime);

	fprintf(stderr, "[LD Worship & Word] Audio file test complete.\n");
}

// 5-bar Mini Signal Meter (Mockup fidelity)
AudioMeterWidget::AudioMeterWidget(QWidget *parent) : QWidget(parent)
{
	setFixedSize(26, 14);
	setToolTip(QStringLiteral("Audio Signal Meter"));

	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, &AudioMeterWidget::tick);
	m_timer->start(33); // ~30 fps
}

void AudioMeterWidget::tick()
{
	float target = AudioEngine::instance().currentLevel();

	if (target > m_displayLevel) {
		m_displayLevel = target;
	} else {
		m_displayLevel = m_displayLevel * 0.82f + target * 0.18f;
	}

	if (m_displayLevel < 0.01f)
		m_displayLevel = 0.0f;

	update();
}

void AudioMeterWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	constexpr int numBars = 5;
	constexpr int barWidth = 3;
	constexpr int barGap = 2;
	const int h = height();

	// 5-bar ascending heights: 4px, 6px, 8px, 11px, 14px
	static const int barHeights[numBars] = {4, 6, 8, 11, 14};

	int activeBars = static_cast<int>(std::round(m_displayLevel * static_cast<float>(numBars)));
	activeBars = std::clamp(activeBars, 0, numBars);

	for (int i = 0; i < numBars; i++) {
		int x = i * (barWidth + barGap);
		int bHeight = barHeights[i];
		int y = h - bHeight;
		QRect barRect(x, y, barWidth, bHeight);

		QColor col;
		if (i < 3) {
			col = QColor(16, 185, 129); // Emerald green #10B981
		} else if (i == 3) {
			col = QColor(245, 158, 11); // Amber #F59E0B
		} else {
			col = QColor(239, 68, 68); // Red #EF4444
		}

		if (i >= activeBars) {
			col.setAlpha(45);
		}

		p.fillRect(barRect, col);
	}
}
