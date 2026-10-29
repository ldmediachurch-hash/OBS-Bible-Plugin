/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "display-state.hpp"
#include "lyrics.hpp"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct whisper_context;

struct SpeechSuggestion {
	Channel channel = Channel::Scripture;
	QString reference; // e.g. "Romans 8:28" or "Amazing Grace · Verse 2"
	QString text;      // verse text or clean song title
	QString sectionCode;
	int confidence = 0;
	double timestampSeconds = 0.0;
	bool tentative = false;
	bool isNextSlide = false;
};

class DetectionCore : public QObject {
	Q_OBJECT

public:
	explicit DetectionCore(QObject *parent = nullptr);
	~DetectionCore() override;

	bool loadModel(const QString &path);
	bool isModelLoaded() const;
	QString modelStatusText() const;
	QString currentModelPath() const;
	QString backendInfo() const;

	void setChannelMode(Channel ch);
	Channel channelMode() const;

	void setDebugLogging(bool enabled);
	bool debugLogging() const;

	void setFollowMode(bool enabled);
	bool followMode() const;

	void setAutoAdvance(bool enabled);
	bool autoAdvance() const;

	void setActiveSong(const QString &title, int currentSlideIndex, const QString &nextSongTitle = QString());
	void clearActiveSong();

	// Ingestion: Feed 16 kHz float PCM samples (mono)
	void pushPcmSamples(const float *samples, size_t count, double streamTimestampSec = -1.0);

	// Direct window processing (used by evaluation harness or tests)
	void processWindowDirect(const std::vector<float> &chunk, double windowEndSec = -1.0);

	void reset();

	// Normalizers and Matchers (accessible for tests/eval)
	static QString normalizeSpokenText(QString s);
	static int levenshtein(const QString &s1, const QString &s2);

	bool detectScripture(const QString &transcript, double streamTimeSec = 0.0, bool isTentative = false);
	bool detectLyrics(const QString &transcript, double streamTimeSec = 0.0, bool isTentative = false);
	bool detectBibleQuotes(const QString &normalizedText, double streamTimeSec = 0.0);

signals:
	void suggestionDetected(const SpeechSuggestion &sugg);
	void transcriptCommitted(const QString &text, double timestampSec);
	void transcriptTentative(const QString &text, double timestampSec);
	void modelLoadedChanged(bool loaded, const QString &statusText);
	void stageTimed(const QString &stage, double ms);
	void autoAdvanceRequested();

private:
	void startWorker();
	void stopWorker();
	void workerLoop();

	void analyzeBuffer(const std::vector<float> &chunk, double windowEndSec);
	QString transcribeChunk(const std::vector<float> &chunk);

	std::atomic<bool> m_modelLoaded{false};
	std::atomic<Channel> m_channel{Channel::Scripture};
	std::atomic<bool> m_debugLogging{false};
	std::atomic<bool> m_followMode{true};
	std::atomic<bool> m_autoAdvance{false};

	whisper_context *m_whisperCtx = nullptr;
	std::mutex m_whisperMutex;
	QString m_modelStatusText{QStringLiteral("Model not loaded")};
	QString m_modelPath;
	QString m_backendInfo;

	// Ring buffer (10 seconds @ 16kHz = 160,000 samples)
	static constexpr size_t kBufferSize = 160000;
	std::vector<float> m_ringBuffer;
	std::atomic<size_t> m_writeIndex{0};
	std::atomic<size_t> m_readIndex{0};
	std::atomic<double> m_streamTimeSec{0.0};

	std::atomic<bool> m_workerRunning{false};
	std::unique_ptr<std::thread> m_workerThread;

	// LocalAgreement-2 transcript stabilizer state
	QStringList m_prevHypothesisWords;
	QStringList m_committedWords;
	QStringList m_tentativeWords;

	// Context memory for follow-on verses (e.g. "verse 29", "the next verse", "chapter 9")
	QString m_contextBook;
	int m_contextChapter = 0;
	int m_contextVerse = 0;
	double m_contextStreamTime = -1000.0;
	uint64_t m_contextWallTimeMs = 0;

	// Follow mode lyrics state
	std::mutex m_activeSongMutex;
	QString m_activeSongTitle;
	int m_activeSlideIndex = -1;
	QString m_nextSongTitle;
	int m_nextSlideConsecutiveMatches = 0;

	// Suggestion cooldown & deduplication
	uint64_t m_lastSuggestionWallTime = 0;
	double m_lastSuggestionStreamTime = -100.0;
	QString m_lastSuggestedRef;
};
