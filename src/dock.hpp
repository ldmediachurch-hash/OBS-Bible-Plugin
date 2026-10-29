/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "bible.hpp"

#include <obs.h>

#include <QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

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
	void loadList();
	void saveList();
	void openListItem(QListWidgetItem *item);
	void importBible();
	void removeBible();
	void openBiblesFolder();

private:
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

	QLineEdit *searchEdit = nullptr;
	QComboBox *primaryBox = nullptr;
	QComboBox *secondaryBox = nullptr;
	QListWidget *results = nullptr;
	QLabel *preview = nullptr;
	QLabel *status = nullptr;
	QLabel *liveBadge = nullptr;
	QPushButton *liveButton = nullptr;
	QPushButton *clearButton = nullptr;
	QListWidget *serviceList = nullptr;

	bool live = false;
	bool follow = false; // while live, moving the selection updates the screen
	int listIndex = -1;
	int rangeEnd = 0; // last verse of a range opened from the service list
	bool searchIsKeyword = false;

	obs_hotkey_id hkNext = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkPrev = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkLive = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkClear = OBS_INVALID_HOTKEY_ID;
};
