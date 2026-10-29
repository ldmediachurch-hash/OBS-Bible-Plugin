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
#include <QTimer>

#include <algorithm>
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
	// Priority 1: Base model q5_1 (LDWW 1.2 default)
	const char *models[] = {"models/ggml-base.en-q5_1.bin", "models/ggml-base.en.bin", "models/ggml-tiny.en.bin"};
	for (const char *m : models) {
		char *bundled = obs_module_file(m);
		if (bundled) {
			QString s = QString::fromUtf8(bundled);
			bfree(bundled);
			if (QFile::exists(s))
				return s;
		}
		char *cfg = obs_module_config_path(m);
		if (cfg) {
			QString s = QString::fromUtf8(cfg);
			bfree(cfg);
			if (QFile::exists(s))
				return s;
		}
	}
	return QString();
}

QString defaultModelDestPath()
{
	char *cfg = obs_module_config_path("models/ggml-base.en-q5_1.bin");
	QString s = cfg ? QString::fromUtf8(cfg) : QString();
	bfree(cfg);
	return s;
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
		emit suggestionDetected(sugg.channel, sugg.reference, sugg.text, sugg.confidence, sugg.tentative, sugg.isNextSlide);
	});

	connect(&m_core, &DetectionCore::modelLoadedChanged, this, &AudioEngine::modelLoadedChanged);

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

void AudioEngine::loadModel()
{
	QString path = findExistingModelPath();
	if (path.isEmpty()) {
		QString dest = defaultModelDestPath();
		if (!dest.isEmpty()) {
			QFileInfo fi(dest);
			QDir().mkpath(fi.dir().absolutePath());

			// Download base.en-q5_1 model via curl in background if missing
			std::thread([this, dest]() {
				QStringList args;
				args << QStringLiteral("-L") << QStringLiteral("-s") << QStringLiteral("-o") << dest
				     << QStringLiteral(
					    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en-q5_1.bin");
				int code = QProcess::execute(QStringLiteral("curl"), args);
				if (code == 0 && QFile::exists(dest)) {
					loadModel();
				}
			}).detach();
		}
		emit modelLoadedChanged(false, QStringLiteral("Model not found on disk"));
		return;
	}

	m_core.loadModel(path);
}

void AudioEngine::setChannelMode(Channel ch)
{
	m_core.setChannelMode(ch);
}

Channel AudioEngine::channelMode() const
{
	return m_core.channelMode();
}

void AudioEngine::processAudio(const float *samples, uint32_t frames, bool muted)
{
	if (!samples || frames == 0 || muted) {
		m_level.store(0.0f, std::memory_order_relaxed);
		return;
	}

	// Calculate RMS level for signal meter
	float sum = 0.0f;
	for (uint32_t i = 0; i < frames; i++) {
		float s = samples[i];
		sum += s * s;
	}
	float rms = sqrtf(sum / static_cast<float>(frames));

	// Normalized dB range: -50 dB (noise floor) to 0 dB
	float dB = (rms > 0.00001f) ? 20.0f * log10f(rms) : -60.0f;
	float norm = std::clamp((dB + 50.0f) / 50.0f, 0.0f, 1.0f);
	m_level.store(norm, std::memory_order_relaxed);

	// Query OBS sample rate (e.g. 44100, 48000, 96000)
	uint32_t inRate = 48000;
	struct obs_audio_info aoi;
	if (obs_get_audio_info(&aoi) && aoi.samples_per_sec > 0) {
		inRate = aoi.samples_per_sec;
	}

	// Phase 1 Item 6: Use OBS audio_resampler_create (media-io) for 16kHz mono conversion
	if (!m_resampler || m_resamplerInRate != inRate) {
		if (m_resampler) {
			audio_resampler_destroy(m_resampler);
			m_resampler = nullptr;
		}

		struct resample_info src_info;
		src_info.samples_per_sec = inRate;
		src_info.format = AUDIO_FORMAT_FLOAT;
		src_info.speakers = SPEAKERS_MONO;

		struct resample_info dst_info;
		dst_info.samples_per_sec = 16000;
		dst_info.format = AUDIO_FORMAT_FLOAT;
		dst_info.speakers = SPEAKERS_MONO;

		m_resampler = audio_resampler_create(&dst_info, &src_info);
		m_resamplerInRate = inRate;
	}

	if (m_resampler) {
		uint8_t *output[MAX_AV_PLANES] = {nullptr};
		uint32_t out_frames = 0;
		uint64_t ts_offset = 0;
		const uint8_t *const input[MAX_AV_PLANES] = {reinterpret_cast<const uint8_t *>(samples)};

		bool ok = audio_resampler_resample(m_resampler, output, &out_frames, &ts_offset, input, frames);
		if (ok && out_frames > 0 && output[0]) {
			const float *resampledSamples = reinterpret_cast<const float *>(output[0]);
			m_core.pushPcmSamples(resampledSamples, out_frames);
			return;
		}
	}

	// Linear resampler fallback if media-io resampler is unallocated
	const double ratio = static_cast<double>(inRate) / 16000.0;
	m_resampleBuffer.clear();

	while (m_resamplePos < static_cast<double>(frames) - 1.0) {
		size_t idx0 = static_cast<size_t>(m_resamplePos);
		size_t idx1 = idx0 + 1;
		float frac = static_cast<float>(m_resamplePos - static_cast<double>(idx0));
		float sample = (1.0f - frac) * samples[idx0] + frac * samples[idx1];

		m_resampleBuffer.push_back(sample);
		m_resamplePos += ratio;
	}
	m_resamplePos -= static_cast<double>(frames);
	if (m_resamplePos < 0.0)
		m_resamplePos = 0.0;

	if (!m_resampleBuffer.empty()) {
		m_core.pushPcmSamples(m_resampleBuffer.data(), m_resampleBuffer.size());
	}
}

// 5-bar Mini Signal Meter (Mockup fidelity)
AudioMeterWidget::AudioMeterWidget(QWidget *parent)
	: QWidget(parent)
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
			col = QColor(239, 68, 68);  // Red #EF4444
		}

		if (i >= activeBars) {
			col.setAlpha(45);
		}

		p.fillRect(barRect, col);
	}
}
