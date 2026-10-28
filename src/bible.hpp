/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <mutex>
#include <vector>

struct BibleBook {
	int number = 0;                    // canonical 1..66 (Genesis..Revelation); other books keep numbers above 66
	QString name;                      // name as written in this translation
	std::vector<QStringList> chapters; // chapters[c - 1][v - 1]
};

class Bible {
public:
	QString name;
	QString abbrev;
	QString language;
	QString path;         // stored ld-bible JSON file
	bool builtIn = false; // shipped with the plugin (cannot be removed)

	// Loads the verses from `path` the first time they are needed.
	bool ensureLoaded(QString *error = nullptr);
	bool isLoaded() const { return loaded; }

	const BibleBook *book(int number) const;
	QString verse(int bookNumber, int chapter, int verse) const;
	int chapterCount(int bookNumber) const;
	int verseCount(int bookNumber, int chapter) const;

	std::vector<BibleBook> books;
	bool loaded = false;

private:
	std::mutex loadMutex;
};

using BiblePtr = std::shared_ptr<Bible>;

struct ParsedRef {
	bool valid = false;
	int book = 0;
	int chapter = 0;
	int v1 = 0; // 0 = whole chapter
	int v2 = 0; // 0 = single verse
};

struct VerseRef {
	int book = 0;
	int chapter = 0;
	int verse = 0;
};

namespace Canon {
int count();                         // 66
QString englishName(int number);     // "Romans"
QString normalize(const QString &s); // lower case, no spaces/dots/accents
} // namespace Canon

class BibleLibrary {
public:
	static BibleLibrary &instance();

	void load(const QString &builtInDir, const QString &userDir);
	std::vector<BiblePtr> bibles() const;
	BiblePtr byAbbrev(const QString &abbrev) const;
	QString userDir() const { return userPath; }

	// Imports an XML or JSON Bible. On success the Bible is saved in the user folder and added.
	BiblePtr importFile(const QString &file, const QString &name, const QString &abbrev, QString *error);
	// Saves an already-parsed Bible (from parseBibleFile) into the user folder and adds it.
	BiblePtr install(BiblePtr parsed, const QString &name, const QString &abbrev, QString *error);
	// Keyword search in one translation (all words must appear). Returns up to `limit` verses.
	std::vector<VerseRef> search(Bible *bible, const QString &words, int limit) const;
	bool remove(const QString &abbrev, QString *error);

	// "Rom 8:28", "1 John 3:16-18", "Psalm 23", "Jude 3", or a book name in the translation's own language.
	ParsedRef parse(const QString &text, Bible *names) const;
	QString bookName(Bible *bible, int number) const;
	QString formatRef(Bible *bible, int book, int chapter, int v1, int v2 = 0) const;

private:
	mutable std::mutex mutex;
	std::vector<BiblePtr> list;
	QString userPath;
};

// Parses a Bible file (Zefania, OSIS, OpenSong, "bible/book/chapter/verse" XML, or common JSON layouts).
BiblePtr parseBibleFile(const QString &file, QString *error, QString *suggestedName, QString *suggestedAbbrev);
bool saveBible(const Bible &bible, const QString &path, QString *error);
// Reads only the name/abbreviation of a stored Bible (fast; verses load later).
BiblePtr openStoredBible(const QString &path, bool builtIn);
