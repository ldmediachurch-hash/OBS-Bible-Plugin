/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "bible.hpp"
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
class QToolButton;
class QVBoxLayout;

class ScriptureDock : public QWidget {
	Q_OBJECT

public:
	explicit ScriptureDock(QWidget *parent = nullptr);
	~ScriptureDock() override;

	void saveConfig();

public slots:
	void next();
	void prev();
	void goLive();
	void clearLive();
	bool showQuery(const QString &query, bool fromList = false);

private slots:
	void runSearch();
	void selectionChanged();
	void translationChanged();
	void addToList();
	void removeFromList();
	void moveListItemUp();
	void moveListItemDown();
	void loadList();
	void saveList();
	void openListItem(QListWidgetItem *item);
	void importBible();
	void removeBible();
	void openBiblesFolder();
	void openGearMenu();

protected:
	void keyPressEvent(QKeyEvent *event) override;

private:
	void setupLayout();
	void refreshTranslations(const QString &primaryAbbrev, const QString &secondaryAbbrev);
	void setChapter(int book, int chapter, int selectVerse);
	VerseRef currentVerse() const;
	void updatePreview();
	void updateLiveButton();
	void pushToOutput();
	void loadConfig();
	void registerHotkeys();
	BiblePtr primary() const;
	BiblePtr secondary() const;
	bool openListIndex(int index, bool fromEnd);

	// Layout widgets
	QScrollArea *scrollArea = nullptr;
	QWidget *scrollContent = nullptr;
	QVBoxLayout *contentLayout = nullptr;

	// Search & Options
	QLineEdit *searchEdit = nullptr;
	QToolButton *gearBtn = nullptr;
	QComboBox *primaryBox = nullptr;
	QComboBox *secondaryBox = nullptr;

	// Verses section
	QLabel *versesHeaderLabel = nullptr;
	QListWidget *results = nullptr;

	// Thumbnail & Controls
	MiniProgramThumbnail *thumbnail = nullptr;
	QPushButton *prevBtn = nullptr;
	QPushButton *liveButton = nullptr;
	QPushButton *clearButton = nullptr;
	QPushButton *nextBtn = nullptr;
	QLabel *shortcutHintLabel = nullptr;

	// Service list section
	QLabel *serviceHeaderLabel = nullptr;
	QToolButton *serviceAddBtn = nullptr;
	QToolButton *serviceRemBtn = nullptr;
	QToolButton *serviceUpBtn = nullptr;
	QToolButton *serviceDownBtn = nullptr;
	QListWidget *serviceList = nullptr;

	bool live = false;
	bool follow = false;
	int listIndex = -1;
	int rangeEnd = 0;
	bool searchIsKeyword = false;

	obs_hotkey_id hkNext = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkPrev = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkLive = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkClear = OBS_INVALID_HOTKEY_ID;
};
