/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "detection-core.hpp"
#include "display-state.hpp"

#include <obs.h>
#include <media-io/audio-resampler.h>

#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class AudioMeterWidget;

class AudioEngine : public QObject {
	Q_OBJECT

public:
	static AudioEngine &instance();

	// Enumerate active OBS audio sources (Mic/Aux, Desktop, etc.)
	static QList<QString> availableAudioSources();

	// Start or stop listening to a specific OBS audio source
	void setAudioSource(const QString &sourceName);
	QString currentAudioSource() const;

	bool isListening() const;
	float currentLevel() const; // 0.0f to 1.0f (thread-safe normalized dB)

	// Speech Model status
	bool isModelLoaded() const;
	QString modelStatusText() const;
	void loadModel();
	bool loadModelFile(const QString &path);

	// Set active channel mode for suggestion prioritization (Scripture vs Lyrics)
	void setChannelMode(Channel ch);
	Channel channelMode() const;

	DetectionCore &core() { return m_core; }

	// Internal callback from OBS
	void processAudio(const float *samples, uint32_t frames, bool muted);

signals:
	void levelChanged(float level);
	void suggestionDetected(Channel channel, const QString &reference, const QString &text, int confidence,
				bool tentative, bool isNextSlide);
	void statusChanged(bool listening, const QString &sourceName);
	void modelLoadedChanged(bool loaded, const QString &statusText);

private:
	AudioEngine();
	~AudioEngine() override;

	std::atomic<bool> m_listening{false};
	std::atomic<float> m_level{0.0f};

	mutable std::mutex m_sourceMutex;
	QString m_currentSourceName;
	obs_source_t *m_currentSource = nullptr;

	audio_resampler_t *m_resampler = nullptr;
	uint32_t m_resamplerInRate = 0;

	double m_resamplePos = 0.0;
	std::vector<float> m_resampleBuffer;

	DetectionCore m_core;
};

// 5-bar Mini Signal Meter Widget (Mockup fidelity)
class AudioMeterWidget : public QWidget {
	Q_OBJECT

public:
	explicit AudioMeterWidget(QWidget *parent = nullptr);
	~AudioMeterWidget() override = default;

	QSize sizeHint() const override { return QSize(28, 14); }

protected:
	void paintEvent(QPaintEvent *event) override;

private slots:
	void tick();

private:
	float m_displayLevel = 0.0f;
	QTimer *m_timer = nullptr;
};
