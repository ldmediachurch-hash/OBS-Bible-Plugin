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
	static QString defaultModelId();
	QString currentModelChoice() const;
	void setModelChoice(const QString &modelId);
	void loadCustomModelFile(const QString &path);
	bool isDownloading() const { return m_downloading.load(std::memory_order_relaxed); }
	int downloadPercent() const { return m_downloadPercent.load(std::memory_order_relaxed); }

	// Set active channel mode for suggestion prioritization (Scripture vs Lyrics)
	void setChannelMode(Channel ch);
	Channel channelMode() const;

	void setPrimaryBible(const QString &abbrev) { m_core.setPrimaryBible(abbrev); }

	DetectionCore &core() { return m_core; }

	// Run WAV audio file test through streaming pipeline (real-time or faster-than-real-time)
	void runAudioFileTest(const QString &filePath, bool realTime = true);

	// Internal callback from OBS
	void processAudio(const struct audio_data *data, obs_source_t *source, bool muted);

signals:
	void levelChanged(float level);
	void suggestionDetected(Channel channel, const QString &reference, const QString &text, int confidence,
				bool tentative, bool isNextSlide);
	void rawTranscriptPass(const QString &text, double timestampSec);
	void scriptureRefDetected(const QString &label, int confidence, const QString &status, const QString &kind,
				  double timestampSec);
	void statusChanged(bool listening, const QString &sourceName);
	void modelLoadedChanged(bool loaded, const QString &statusText);
	void modelDownloadProgress(int percent);

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
	speaker_layout m_resamplerLayout = SPEAKERS_UNKNOWN;

	DetectionCore m_core;

	QString m_modelChoice;
	std::atomic<bool> m_downloading{false};
	std::atomic<int> m_downloadPercent{0};
	void downloadModel(const QString &modelId);
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
