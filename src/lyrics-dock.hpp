/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "lyrics.hpp"

#include <obs.h>

#include <QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QToolButton;

class LyricsDock : public QWidget {
	Q_OBJECT

public:
	explicit LyricsDock(QWidget *parent = nullptr);
	~LyricsDock() override;

	void saveConfig();

public slots:
	void next();
	void prev();
	void goLive();
	void clearLive();
	void toggleBlank();
	bool openSong(const QString &titleOrPath, bool fromList = false);

private slots:
	void runSearch();
	void searchTextChanged(const QString &text);
	void songSelected(QListWidgetItem *item);
	void slideSelectionChanged();
	void linesPerSlideChanged();
	void jumpToSection(const QString &shortCode);
	void addToList();
	void removeFromList();
	void loadList();
	void saveList();
	void openListItem(QListWidgetItem *item);
	void newSongDialog();
	void editSongDialog();
	void importSongs();
	void openSongsFolder();

private:
	void refreshSongList(const QString &filter = QString());
	void rebuildSlides();
	void rebuildSectionButtons();
	void updatePreview();
	void updateLiveButton();
	void pushToOutput();
	void loadConfig();
	void registerHotkeys();
	bool openListIndex(int index, bool fromEnd = false);

	QLineEdit *searchEdit = nullptr;
	QComboBox *linesBox = nullptr;
	QListWidget *songList = nullptr;
	QListWidget *slideList = nullptr;
	QWidget *sectionContainer = nullptr;
	QLabel *preview = nullptr;
	QLabel *status = nullptr;
	QLabel *liveBadge = nullptr;
	QPushButton *liveButton = nullptr;
	QPushButton *blankButton = nullptr;
	QPushButton *clearButton = nullptr;
	QListWidget *setlist = nullptr;

	Song currentSong;
	std::vector<SongSlide> currentSlides;
	int currentSlideIndex = -1;

	bool live = false;
	bool follow = false;
	bool blanked = false;
	int listIndex = -1;
	int linesPerSlide = 2;

	obs_hotkey_id hkNext = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkPrev = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkLive = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkClear = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkBlank = OBS_INVALID_HOTKEY_ID;
};
