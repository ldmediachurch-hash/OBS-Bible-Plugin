/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "audio-engine.hpp"
#include "dock.hpp"
#include "lyrics-dock.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QToolButton;

class WorshipWordDock : public QWidget {
	Q_OBJECT

public:
	explicit WorshipWordDock(QWidget *parent = nullptr);
	~WorshipWordDock() override;

	void saveConfig();

	ScriptureDock *scriptureDock() const { return m_scripture; }
	LyricsDock *lyricsDock() const { return m_lyrics; }

public slots:
	void setMode(int mode); // 0 = Worship, 1 = Word
	void toggleMode();

private slots:
	void audioSourceChanged(int index);
	void refreshAudioSources();
	void onSuggestionDetected(Channel ch, const QString &ref, const QString &text, int confidence);
	void acceptSuggestion(bool goLive);
	void dismissSuggestion();

protected:
	void keyPressEvent(QKeyEvent *event) override;

private:
	void updateTabStyles();
	void loadConfig();

	QPushButton *m_worshipBtn = nullptr;
	QPushButton *m_wordBtn = nullptr;
	QStackedWidget *m_stack = nullptr;

	LyricsDock *m_lyrics = nullptr;
	ScriptureDock *m_scripture = nullptr;

	// Audio Assist bar
	QWidget *m_audioBar = nullptr;
	QLabel *m_micIcon = nullptr;
	QComboBox *m_audioCombo = nullptr;
	AudioMeterWidget *m_meter = nullptr;
	QToolButton *m_refreshAudioBtn = nullptr;

	// Suggestion Pill
	QWidget *m_suggestionPill = nullptr;
	QLabel *m_suggestionLabel = nullptr;
	QPushButton *m_previewBtn = nullptr;
	QPushButton *m_liveBtn = nullptr;
	QToolButton *m_dismissBtn = nullptr;

	Channel m_suggestedChannel = Channel::Scripture;
	QString m_suggestedRef;
	QString m_suggestedText;
};
