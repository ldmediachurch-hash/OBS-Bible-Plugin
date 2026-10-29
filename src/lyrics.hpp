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

struct SongSection {
	QString name;      // e.g. "Verse 1", "Chorus"
	QString shortCode; // e.g. "V1", "C"
	std::vector<QStringList> rawSlides;
};

struct Song {
	QString title;
	QString author;
	QString ccli;
	QString copyright;
	QString order; // raw order string, e.g. "V1 V2 C V3"
	QString filePath;
	bool builtIn = false;

	std::vector<SongSection> sections;

	bool isValid() const { return !title.isEmpty() || !sections.empty(); }
};

struct SongSlide {
	QString title;
	QString sectionName;
	QString sectionCode;
	QString ccli;
	QStringList lines;

	QString text() const { return lines.join(QLatin1Char('\n')); }
	QString kicker() const
	{
		if (sectionName.isEmpty())
			return title.toUpper();
		return QStringLiteral("%1 · %2").arg(title.toUpper(), sectionName.toUpper());
	}
	QString listLabel() const
	{
		const QString firstLine = lines.isEmpty() ? QString() : lines.front();
		if (sectionName.isEmpty())
			return QStringLiteral("%1 · %2").arg(title.toUpper(), firstLine);
		return QStringLiteral("%1 · %2").arg(sectionName.toUpper(), firstLine);
	}
};

QString sectionToShortCode(const QString &name);
Song parseNativeSong(const QString &content, const QString &fallbackTitle = QString());
Song parseOpenLyricsXml(const QString &xmlContent, QString *error = nullptr);
Song parseSongXml(const QString &xmlContent, QString *error = nullptr);
std::vector<Song> parseSongJson(const QByteArray &jsonData, QString *error = nullptr);
QString serializeNativeSong(const Song &song);
std::vector<SongSlide> generateSlides(const Song &song, int linesPerSlide);

class SongLibrary {
public:
	static SongLibrary &instance();

	void load(const QString &sampleDir, const QString &userDir);
	std::vector<Song> songs() const;
	Song songByTitle(const QString &title) const;
	Song songByPath(const QString &path) const;
	std::vector<Song> search(const QString &query) const;

	bool importSongFile(const QString &srcPath, QString *importedTitle, QString *error);
	int importSongFileCount(const QString &srcPath, QString *firstImportedTitle, QString *error);
	bool saveSong(const Song &song, const QString &suggestedFileName, QString *savedPath, QString *error);
	bool removeSong(const QString &titleOrPath, QString *error);
	QString userDir() const { return userPath; }

private:
	mutable std::mutex mutex;
	std::vector<Song> list;
	QString userPath;
};
