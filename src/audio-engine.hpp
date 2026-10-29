/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "display-state.hpp"

#include <obs.h>

#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct whisper_context;
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

	// Set active channel mode for suggestion prioritization (Scripture vs Lyrics)
	void setChannelMode(Channel ch);
	Channel channelMode() const;

	// Internal callback from OBS
	void processAudio(const float *samples, uint32_t frames, bool muted);

signals:
	void levelChanged(float level);
	void suggestionDetected(Channel channel, const QString &reference, const QString &text, int confidence);
	void statusChanged(bool listening, const QString &sourceName);
	void modelLoadedChanged(bool loaded, const QString &statusText);

private:
	AudioEngine();
	~AudioEngine() override;

	void startWorker();
	void stopWorker();
	void workerLoop();

	// Speech inference & Pattern matchers
	void analyzeBuffer(const std::vector<float> &chunk);
	QString transcribeChunk(const std::vector<float> &chunk);
	bool detectScripture(const QString &transcript);
	bool detectLyrics(const QString &transcript);

	std::atomic<bool> m_listening{false};
	std::atomic<float> m_level{0.0f};
	std::atomic<Channel> m_channel{Channel::Scripture};
	std::atomic<bool> m_modelLoaded{false};

	mutable std::mutex m_sourceMutex;
	QString m_currentSourceName;
	obs_source_t *m_currentSource = nullptr;

	// Whisper speech model
	whisper_context *m_whisperCtx = nullptr;
	std::mutex m_whisperMutex;
	QString m_modelStatusText{QStringLiteral("Model not loaded")};

	// Resampler & Ring buffer (pre-allocated 10 seconds @ 16kHz)
	double m_resamplePos = 0.0;
	static constexpr size_t kBufferSize = 160000;
	std::vector<float> m_ringBuffer;
	std::atomic<size_t> m_writeIndex{0};
	std::atomic<size_t> m_readIndex{0};

	std::atomic<bool> m_workerRunning{false};
	std::unique_ptr<std::thread> m_workerThread;

	uint64_t m_lastSuggestionTime = 0;
	QString m_lastSuggestedRef;
	QString m_recentTranscript;
};

// Compact VU Meter Widget
class AudioMeterWidget : public QWidget {
	Q_OBJECT

public:
	explicit AudioMeterWidget(QWidget *parent = nullptr);
	~AudioMeterWidget() override = default;

protected:
	void paintEvent(QPaintEvent *event) override;

private slots:
	void tick();

private:
	float m_displayLevel = 0.0f;
	QTimer *m_timer = nullptr;
};
