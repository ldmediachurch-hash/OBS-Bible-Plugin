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
#include "mini-thumbnail.hpp"

#include <obs.h>

#include <QWidget>

#include <vector>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QSplitter;
class QToolButton;
class QVBoxLayout;

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
	void jumpToSection(const QString &shortCode);

private slots:
	void runSearch();
	void searchTextChanged(const QString &text);
	void songSelected(QListWidgetItem *item);
	void slideSelectionChanged();
	void setLinesPerSlide(int lines);
	void addToList();
	void removeFromList();
	void moveListItemUp();
	void moveListItemDown();
	void loadList();
	void saveList();
	void openListItem(QListWidgetItem *item);
	void newSongDialog();
	void editSongDialog();
	void importSongs();
	void openSongsFolder();
	void openGearMenu();
	void selectCustomModel();

protected:
	void keyPressEvent(QKeyEvent *event) override;

private:
	void setupLayout();
	void refreshSongList(const QString &filter = QString());
	void rebuildSlides();
	void rebuildSectionButtons();
	void updatePreview();
	void updateLiveButton();
	void pushToOutput();
	void loadConfig();
	void registerHotkeys();
	bool openListIndex(int index, bool fromEnd = false);

	// Layout widgets
	QScrollArea *scrollArea = nullptr;
	QWidget *scrollContent = nullptr;
	QVBoxLayout *contentLayout = nullptr;

	// Search & Gear
	QLineEdit *searchEdit = nullptr;
	QToolButton *gearBtn = nullptr;

	// Library section
	QLabel *libraryHeaderLabel = nullptr;
	QToolButton *importBtn = nullptr;
	QListWidget *songList = nullptr;

	// Slides section
	QLabel *slidesHeaderLabel = nullptr;
	QToolButton *linesBtn = nullptr;
	QWidget *sectionContainer = nullptr;
	QHBoxLayout *sectionLayout = nullptr;
	QListWidget *slideList = nullptr;

	// Thumbnail & Controls
	MiniProgramThumbnail *thumbnail = nullptr;
	QPushButton *prevBtn = nullptr;
	QPushButton *liveButton = nullptr;
	QPushButton *blankButton = nullptr;
	QPushButton *clearButton = nullptr;
	QPushButton *nextBtn = nullptr;
	QLabel *shortcutHintLabel = nullptr;

	// Setlist section
	QLabel *setlistHeaderLabel = nullptr;
	QToolButton *setlistAddBtn = nullptr;
	QToolButton *setlistRemBtn = nullptr;
	QToolButton *setlistUpBtn = nullptr;
	QToolButton *setlistDownBtn = nullptr;
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
};
