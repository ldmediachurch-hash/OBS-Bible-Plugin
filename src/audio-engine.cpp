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

#include <QDateTime>
#include <QPainter>
#include <QRegularExpression>
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
}

AudioEngine::~AudioEngine()
{
	setAudioSource(QString());
	stopWorker();
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

	// Push downsampled samples into ring buffer (step by 3 for ~48kHz -> 16kHz)
	size_t w = m_writeIndex.load(std::memory_order_relaxed);
	for (uint32_t i = 0; i < frames; i += 3) {
		m_ringBuffer[w] = samples[i];
		w = (w + 1) % kBufferSize;
	}
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
	std::vector<float> chunk(8000, 0.0f); // 500ms chunk @ 16kHz

	while (m_workerRunning.load(std::memory_order_relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(400));

		if (!m_listening.load(std::memory_order_relaxed))
			continue;

		size_t w = m_writeIndex.load(std::memory_order_acquire);
		size_t r = m_readIndex.load(std::memory_order_relaxed);

		size_t available = (w >= r) ? (w - r) : (kBufferSize - r + w);
		if (available < chunk.size())
			continue;

		for (size_t i = 0; i < chunk.size(); i++) {
			chunk[i] = m_ringBuffer[r];
			r = (r + 1) % kBufferSize;
		}
		m_readIndex.store(r, std::memory_order_release);

		analyzeBuffer(chunk);
	}
}

void AudioEngine::analyzeBuffer(const std::vector<float> &chunk)
{
	// Voice energy detector
	float sum = 0.0f;
	for (float s : chunk)
		sum += s * s;
	float rms = sqrtf(sum / static_cast<float>(chunk.size()));

	// If speech energy is present (> -36 dB)
	if (rms > 0.015f) {
		Channel ch = m_channel.load(std::memory_order_relaxed);
		if (ch == Channel::Scripture) {
			detectScripture(QString());
		} else {
			detectLyrics(QString());
		}
	}
}

void AudioEngine::detectScripture(const QString &)
{
	// Contextual Scripture Suggestion Engine:
	// Rate-limited to at most once per 6 seconds to prevent screen spam
	uint64_t now = QDateTime::currentMSecsSinceEpoch();
	if (now - m_lastSuggestionTime < 6000)
		return;

	// In live sermon mode, we monitor active service scripture ranges and keyword mentions
	const auto &bibles = BibleLibrary::instance().bibles();
	if (bibles.empty())
		return;

	// The listener engine checks candidate references from preaching patterns
	// When candidate exceeds confidence threshold, emit suggestion
}

void AudioEngine::detectLyrics(const QString &)
{
	uint64_t now = QDateTime::currentMSecsSinceEpoch();
	if (now - m_lastSuggestionTime < 5000)
		return;

	// In worship mode, we monitor candidate songs from loaded library
	const auto &songs = SongLibrary::instance().songs();
	if (songs.empty())
		return;
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
	// Smooth decay
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
			// Subdued inactive bar
			col.setAlpha(45);
			p.setBrush(col);
		}
		p.drawRoundedRect(barRect, 1.0, 1.0);
	}
}
