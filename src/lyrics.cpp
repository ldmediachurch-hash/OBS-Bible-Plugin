/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "lyrics.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

QString cleanFileName(const QString &title)
{
	QString s = title.trimmed();
	static const QRegularExpression invalidChars(QStringLiteral("[\\\\/:*?\"<>|]"));
	s.replace(invalidChars, QStringLiteral("-"));
	if (s.isEmpty())
		s = QStringLiteral("Untitled");
	return s;
}

} // namespace

QString sectionToShortCode(const QString &name)
{
	const QString s = name.trimmed();
	static const QRegularExpression re(
		QStringLiteral("^(Verse|Chorus|Bridge|Pre-Chorus|PreChorus|Tag|Ending)\\s*(\\d*)"),
		QRegularExpression::CaseInsensitiveOption);
	const QRegularExpressionMatch m = re.match(s);
	if (m.hasMatch()) {
		const QString type = m.captured(1).toLower();
		const QString num = m.captured(2);
		QString prefix;
		if (type == QStringLiteral("verse"))
			prefix = QStringLiteral("V");
		else if (type.startsWith(QStringLiteral("chorus")))
			prefix = QStringLiteral("C");
		else if (type.startsWith(QStringLiteral("bridge")))
			prefix = QStringLiteral("B");
		else if (type.startsWith(QStringLiteral("pre")))
			prefix = QStringLiteral("PC");
		else if (type.startsWith(QStringLiteral("tag")))
			prefix = QStringLiteral("T");
		else if (type.startsWith(QStringLiteral("ending")))
			prefix = QStringLiteral("E");
		return prefix + num;
	}
	return s.left(3).toUpper();
}

Song parseNativeSong(const QString &content, const QString &fallbackTitle)
{
	Song song;
	QString text = content;
	if (text.startsWith(QChar(0xFEFF)))
		text.remove(0, 1);
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

	const QStringList allLines = text.split(QLatin1Char('\n'));
	bool inHeader = true;
	int lineIdx = 0;

	static const QRegularExpression headerRe(QStringLiteral("^(Title|Author|CCLI|Copyright|Order)\\s*:\\s*(.*)$"),
						 QRegularExpression::CaseInsensitiveOption);

	while (lineIdx < allLines.size() && inHeader) {
		const QString line = allLines[lineIdx].trimmed();
		if (line.isEmpty()) {
			inHeader = false;
			lineIdx++;
			break;
		}
		if (line.startsWith(QLatin1Char('['))) {
			inHeader = false;
			break;
		}
		const QRegularExpressionMatch hm = headerRe.match(line);
		if (hm.hasMatch()) {
			const QString key = hm.captured(1).toLower();
			const QString val = hm.captured(2).trimmed();
			if (key == QStringLiteral("title"))
				song.title = val;
			else if (key == QStringLiteral("author"))
				song.author = val;
			else if (key == QStringLiteral("ccli"))
				song.ccli = val;
			else if (key == QStringLiteral("copyright"))
				song.copyright = val;
			else if (key == QStringLiteral("order"))
				song.order = val;
		} else {
			// Unrecognized header line before blank line -> not a header, lyrics begin
			inHeader = false;
			break;
		}
		lineIdx++;
	}

	if (song.title.isEmpty())
		song.title = fallbackTitle;

	SongSection currentSection;
	QStringList currentSlideLines;
	bool hasSectionHeader = false;

	auto finishSlide = [&]() {
		if (!currentSlideLines.isEmpty()) {
			currentSection.rawSlides.push_back(currentSlideLines);
			currentSlideLines.clear();
		}
	};

	auto finishSection = [&]() {
		finishSlide();
		if (!currentSection.rawSlides.empty() || !currentSection.name.isEmpty()) {
			if (currentSection.shortCode.isEmpty() && !currentSection.name.isEmpty())
				currentSection.shortCode = sectionToShortCode(currentSection.name);
			song.sections.push_back(currentSection);
			currentSection = SongSection();
		}
	};

	static const QRegularExpression secRe(QStringLiteral("^\\[([^\\]]+)\\]$"));

	for (; lineIdx < allLines.size(); lineIdx++) {
		const QString rawLine = allLines[lineIdx];
		const QString trimmed = rawLine.trimmed();

		const QRegularExpressionMatch sm = secRe.match(trimmed);
		if (sm.hasMatch()) {
			finishSection();
			hasSectionHeader = true;
			currentSection.name = sm.captured(1).trimmed();
			currentSection.shortCode = sectionToShortCode(currentSection.name);
			continue;
		}

		if (trimmed.isEmpty()) {
			finishSlide();
		} else {
			currentSlideLines.push_back(trimmed);
		}
	}

	finishSection();

	// If no section header was ever found, assign the slides to a single section
	if (!hasSectionHeader && song.sections.size() == 1) {
		song.sections[0].name.clear();
		song.sections[0].shortCode.clear();
	}

	return song;
}

Song parseOpenLyricsXml(const QString &xmlContent, QString *error)
{
	Song song;
	QXmlStreamReader xml(xmlContent);

	while (!xml.atEnd()) {
		xml.readNext();
		if (xml.isStartElement()) {
			const QString tag = xml.name().toString().toLower();
			if (tag == QStringLiteral("title")) {
				if (song.title.isEmpty())
					song.title = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("author")) {
				if (song.author.isEmpty())
					song.author = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("cclino")) {
				song.ccli = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("copyright")) {
				song.copyright = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("verseorder")) {
				song.order = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("verse")) {
				const QString vName =
					xml.attributes().value(QStringLiteral("name")).toString().trimmed();
				SongSection sec;
				static const QRegularExpression vRe(QStringLiteral("^([a-zA-Z]+)(\\d*)$"));
				const QRegularExpressionMatch vm = vRe.match(vName);
				if (vm.hasMatch()) {
					const QString p = vm.captured(1).toLower();
					const QString num = vm.captured(2);
					if (p == QStringLiteral("v"))
						sec.name = num.isEmpty() ? QStringLiteral("Verse")
									 : QStringLiteral("Verse %1").arg(num);
					else if (p == QStringLiteral("c"))
						sec.name = num.isEmpty() ? QStringLiteral("Chorus")
									 : QStringLiteral("Chorus %1").arg(num);
					else if (p == QStringLiteral("b"))
						sec.name = num.isEmpty() ? QStringLiteral("Bridge")
									 : QStringLiteral("Bridge %1").arg(num);
					else if (p == QStringLiteral("p") || p == QStringLiteral("pc"))
						sec.name = num.isEmpty() ? QStringLiteral("Pre-Chorus")
									 : QStringLiteral("Pre-Chorus %1").arg(num);
					else if (p == QStringLiteral("t"))
						sec.name = num.isEmpty() ? QStringLiteral("Tag")
									 : QStringLiteral("Tag %1").arg(num);
					else if (p == QStringLiteral("e"))
						sec.name = num.isEmpty() ? QStringLiteral("Ending")
									 : QStringLiteral("Ending %1").arg(num);
					else
						sec.name = vName;
				} else {
					sec.name = vName.isEmpty() ? QStringLiteral("Verse") : vName;
				}
				sec.shortCode = sectionToShortCode(sec.name);

				while (!xml.atEnd()) {
					xml.readNext();
					if (xml.isEndElement() && xml.name() == QLatin1String("verse"))
						break;
					if (xml.isStartElement() && xml.name() == QLatin1String("lines")) {
						QString slideText;
						while (!xml.atEnd()) {
							xml.readNext();
							if (xml.isEndElement() && xml.name() == QLatin1String("lines"))
								break;
							if (xml.isStartElement() && xml.name() == QLatin1String("br")) {
								slideText += QLatin1Char('\n');
							} else if (xml.isCharacters()) {
								slideText += xml.text().toString();
							}
						}
						QStringList rawLines;
						for (const QString &ln : slideText.split(QLatin1Char('\n'))) {
							const QString t = ln.trimmed();
							if (!t.isEmpty())
								rawLines.push_back(t);
						}
						if (!rawLines.isEmpty())
							sec.rawSlides.push_back(rawLines);
					}
				}
				if (!sec.rawSlides.empty())
					song.sections.push_back(sec);
			}
		}
	}

	if (xml.hasError()) {
		if (error)
			*error = xml.errorString();
		return Song();
	}

	return song;
}

Song parseSongXml(const QString &xmlContent, QString *error)
{
	if (xmlContent.contains(QLatin1String("<verse"), Qt::CaseInsensitive)) {
		Song s = parseOpenLyricsXml(xmlContent, error);
		if (s.isValid())
			return s;
	}

	Song song;
	QXmlStreamReader xml(xmlContent);
	QString rawLyrics;

	while (!xml.atEnd()) {
		xml.readNext();
		if (xml.isStartElement()) {
			const QString tag = xml.name().toString().toLower();
			if (tag == QStringLiteral("title")) {
				if (song.title.isEmpty())
					song.title = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("author")) {
				if (song.author.isEmpty())
					song.author = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("ccli") || tag == QStringLiteral("cclino")) {
				song.ccli = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("copyright")) {
				song.copyright = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("presentation") || tag == QStringLiteral("verseorder") ||
				   tag == QStringLiteral("order")) {
				song.order = xml.readElementText().trimmed();
			} else if (tag == QStringLiteral("lyrics") || tag == QStringLiteral("words")) {
				rawLyrics = xml.readElementText();
			}
		}
	}

	if (!rawLyrics.isEmpty()) {
		QStringList cleanedLines;
		for (const QString &line : rawLyrics.split(QLatin1Char('\n'))) {
			QString trimmed = line.trimmed();
			if (trimmed.startsWith(QLatin1Char('.')) || trimmed.startsWith(QLatin1Char(';')))
				continue;
			cleanedLines.push_back(line);
		}
		Song parsed = parseNativeSong(cleanedLines.join(QLatin1Char('\n')), song.title);
		song.sections = parsed.sections;
		if (song.title.isEmpty())
			song.title = parsed.title;
	}

	return song;
}

static Song parseJsonObjectToSong(const QJsonObject &obj)
{
	Song song;

	for (const char *k : {"title", "name", "song", "title_name"}) {
		if (obj.contains(QLatin1String(k)) && obj.value(QLatin1String(k)).isString()) {
			song.title = obj.value(QLatin1String(k)).toString().trimmed();
			break;
		}
	}

	for (const char *k : {"author", "artist", "composer", "writer", "authors"}) {
		if (obj.contains(QLatin1String(k))) {
			if (obj.value(QLatin1String(k)).isString()) {
				song.author = obj.value(QLatin1String(k)).toString().trimmed();
			} else if (obj.value(QLatin1String(k)).isArray()) {
				QStringList authors;
				for (const QJsonValue av : obj.value(QLatin1String(k)).toArray())
					authors.push_back(av.toString().trimmed());
				song.author = authors.join(QStringLiteral(", "));
			}
			break;
		}
	}

	for (const char *k : {"ccli", "ccli_number", "ccliNo", "ccliNumber"}) {
		if (obj.contains(QLatin1String(k))) {
			if (obj.value(QLatin1String(k)).isString())
				song.ccli = obj.value(QLatin1String(k)).toString().trimmed();
			else if (obj.value(QLatin1String(k)).isDouble())
				song.ccli = QString::number(obj.value(QLatin1String(k)).toInt());
			break;
		}
	}

	if (obj.contains(QLatin1String("copyright")) && obj.value(QLatin1String("copyright")).isString())
		song.copyright = obj.value(QLatin1String("copyright")).toString().trimmed();

	for (const char *k : {"order", "sequence", "arrangement", "verseOrder", "presentation"}) {
		if (obj.contains(QLatin1String(k)) && obj.value(QLatin1String(k)).isString()) {
			song.order = obj.value(QLatin1String(k)).toString().trimmed();
			break;
		}
	}

	for (const char *k : {"lyrics", "text", "content", "words"}) {
		if (obj.contains(QLatin1String(k)) && obj.value(QLatin1String(k)).isString()) {
			const QString ltext = obj.value(QLatin1String(k)).toString();
			Song parsed = parseNativeSong(ltext, song.title);
			if (!parsed.sections.empty()) {
				song.sections = parsed.sections;
				if (song.title.isEmpty())
					song.title = parsed.title;
				if (song.author.isEmpty())
					song.author = parsed.author;
				return song;
			}
		}
	}

	QJsonArray secArray;
	for (const char *k : {"sections", "verses", "slides", "parts", "stanzas"}) {
		if (obj.contains(QLatin1String(k)) && obj.value(QLatin1String(k)).isArray()) {
			secArray = obj.value(QLatin1String(k)).toArray();
			break;
		}
	}

	if (!secArray.isEmpty()) {
		for (const auto &item : secArray) {
			if (item.isObject()) {
				const QJsonObject so = item.toObject();
				SongSection sec;
				for (const char *nk : {"name", "title", "label", "type", "section", "part"}) {
					if (so.contains(QLatin1String(nk)) && so.value(QLatin1String(nk)).isString()) {
						sec.name = so.value(QLatin1String(nk)).toString().trimmed();
						break;
					}
				}
				if (sec.name.isEmpty())
					sec.name = QStringLiteral("Verse %1").arg(song.sections.size() + 1);
				sec.shortCode = sectionToShortCode(sec.name);

				for (const char *lk : {"lines", "slides", "text", "content", "lyrics"}) {
					if (so.contains(QLatin1String(lk))) {
						if (so.value(QLatin1String(lk)).isArray()) {
							QStringList lines;
							for (const QJsonValue lv :
							     so.value(QLatin1String(lk)).toArray()) {
								if (lv.isString()) {
									lines.push_back(lv.toString().trimmed());
								} else if (lv.isArray()) {
									QStringList sublines;
									for (const QJsonValue slv : lv.toArray())
										sublines.push_back(
											slv.toString().trimmed());
									if (!sublines.isEmpty())
										sec.rawSlides.push_back(sublines);
								}
							}
							if (!lines.isEmpty())
								sec.rawSlides.push_back(lines);
						} else if (so.value(QLatin1String(lk)).isString()) {
							QString t = so.value(QLatin1String(lk)).toString();
							QStringList lines;
							for (const QString &ln : t.split(QLatin1Char('\n'))) {
								QString s = ln.trimmed();
								if (!s.isEmpty())
									lines.push_back(s);
							}
							if (!lines.isEmpty())
								sec.rawSlides.push_back(lines);
						}
						break;
					}
				}
				if (!sec.rawSlides.empty())
					song.sections.push_back(sec);
			} else if (item.isString()) {
				QString t = item.toString().trimmed();
				if (!t.isEmpty()) {
					SongSection sec;
					sec.name = QStringLiteral("Verse %1").arg(song.sections.size() + 1);
					sec.shortCode = sectionToShortCode(sec.name);
					QStringList lines;
					for (const QString &ln : t.split(QLatin1Char('\n'))) {
						QString s = ln.trimmed();
						if (!s.isEmpty())
							lines.push_back(s);
					}
					if (!lines.isEmpty()) {
						sec.rawSlides.push_back(lines);
						song.sections.push_back(sec);
					}
				}
			}
		}
	} else {
		for (auto it = obj.begin(); it != obj.end(); ++it) {
			const QString k = it.key();
			if (k == QStringLiteral("title") || k == QStringLiteral("author") ||
			    k == QStringLiteral("ccli") || k == QStringLiteral("copyright") ||
			    k == QStringLiteral("order"))
				continue;
			if (it.value().isString() || it.value().isArray()) {
				SongSection sec;
				sec.name = k;
				sec.shortCode = sectionToShortCode(k);
				if (it.value().isString()) {
					for (const QString &ln : it.value().toString().split(QLatin1Char('\n'))) {
						QString s = ln.trimmed();
						if (!s.isEmpty())
							sec.rawSlides.push_back({s});
					}
				} else if (it.value().isArray()) {
					QStringList lines;
					for (const QJsonValue lv : it.value().toArray())
						if (lv.isString())
							lines.push_back(lv.toString().trimmed());
					if (!lines.isEmpty())
						sec.rawSlides.push_back(lines);
				}
				if (!sec.rawSlides.empty())
					song.sections.push_back(sec);
			}
		}
	}

	return song;
}

std::vector<Song> parseSongJson(const QByteArray &jsonData, QString *error)
{
	std::vector<Song> songs;
	QJsonParseError pe;
	QJsonDocument doc = QJsonDocument::fromJson(jsonData, &pe);
	if (doc.isNull()) {
		if (error)
			*error = QStringLiteral("JSON error: %1").arg(pe.errorString());
		return songs;
	}

	if (doc.isArray()) {
		for (const QJsonValue item : doc.array()) {
			if (item.isObject()) {
				Song s = parseJsonObjectToSong(item.toObject());
				if (s.isValid())
					songs.push_back(s);
			}
		}
	} else if (doc.isObject()) {
		const QJsonObject root = doc.object();
		for (const char *k : {"songs", "items", "data", "lyrics"}) {
			if (root.contains(QLatin1String(k)) && root.value(QLatin1String(k)).isArray()) {
				for (const QJsonValue item : root.value(QLatin1String(k)).toArray()) {
					if (item.isObject()) {
						Song s = parseJsonObjectToSong(item.toObject());
						if (s.isValid())
							songs.push_back(s);
					}
				}
				if (!songs.empty())
					return songs;
			}
		}

		Song s = parseJsonObjectToSong(root);
		if (s.isValid())
			songs.push_back(s);
	}

	return songs;
}

QString serializeNativeSong(const Song &song)
{
	QString out;
	QTextStream ts(&out);

	if (!song.title.isEmpty())
		ts << "Title: " << song.title << "\n";
	if (!song.author.isEmpty())
		ts << "Author: " << song.author << "\n";
	if (!song.ccli.isEmpty())
		ts << "CCLI: " << song.ccli << "\n";
	if (!song.copyright.isEmpty())
		ts << "Copyright: " << song.copyright << "\n";
	if (!song.order.isEmpty())
		ts << "Order: " << song.order << "\n";

	ts << "\n";

	for (size_t si = 0; si < song.sections.size(); si++) {
		const auto &sec = song.sections[si];
		if (!sec.name.isEmpty())
			ts << "[" << sec.name << "]\n";
		for (size_t ri = 0; ri < sec.rawSlides.size(); ri++) {
			for (const QString &line : sec.rawSlides[ri])
				ts << line << "\n";
			if (ri + 1 < sec.rawSlides.size())
				ts << "\n";
		}
		if (si + 1 < song.sections.size())
			ts << "\n";
	}

	return out;
}

std::vector<SongSlide> generateSlides(const Song &song, int linesPerSlide)
{
	std::vector<SongSlide> slides;
	const int lps = std::max(1, std::min(4, linesPerSlide));

	auto addSectionSlides = [&](const SongSection &sec) {
		for (const QStringList &raw : sec.rawSlides) {
			if (raw.isEmpty())
				continue;
			for (int i = 0; i < raw.size(); i += lps) {
				const int take = std::min(lps, static_cast<int>(raw.size()) - i);
				QStringList chunk;
				for (int j = 0; j < take; j++)
					chunk.push_back(raw[i + j]);
				SongSlide sl;
				sl.title = song.title;
				sl.sectionName = sec.name;
				sl.sectionCode = sec.shortCode;
				sl.ccli = song.ccli;
				sl.lines = chunk;
				slides.push_back(sl);
			}
		}
	};

	if (!song.order.trimmed().isEmpty()) {
		static const QRegularExpression splitRe(QStringLiteral("[,\\s]+"));
		const QStringList tokens = song.order.split(splitRe, Qt::SkipEmptyParts);
		bool matchedAny = false;
		for (const QString &tok : tokens) {
			const QString t = tok.trimmed().toUpper();
			const QString tNoSpace = QString(t).replace(QLatin1Char(' '), QString());
			const SongSection *matched = nullptr;
			for (const auto &sec : song.sections) {
				if (sec.shortCode.toUpper() == t || sec.name.toUpper() == t) {
					matched = &sec;
					break;
				}
				const QString sNoSpace =
					QString(sec.name.toUpper()).replace(QLatin1Char(' '), QString());
				if (sNoSpace == tNoSpace) {
					matched = &sec;
					break;
				}
			}
			if (matched) {
				addSectionSlides(*matched);
				matchedAny = true;
			}
		}
		if (matchedAny)
			return slides;
	}

	for (const auto &sec : song.sections)
		addSectionSlides(sec);

	return slides;
}

/* ------------------------------------------------------------------ SongLibrary */

SongLibrary &SongLibrary::instance()
{
	static SongLibrary s;
	return s;
}

void SongLibrary::load(const QString &sampleDir, const QString &userDir)
{
	std::lock_guard<std::mutex> lock(mutex);
	userPath = userDir;
	QDir uDir(userDir);
	if (!uDir.exists())
		uDir.mkpath(QStringLiteral("."));

	// On first run, copy sample songs if library is empty
	const QStringList userFiles = uDir.entryList({QStringLiteral("*.txt")}, QDir::Files);
	if (userFiles.isEmpty() && !sampleDir.isEmpty()) {
		QDir sDir(sampleDir);
		const QStringList samples = sDir.entryList({QStringLiteral("*.txt")}, QDir::Files);
		for (const QString &f : samples) {
			QFile::copy(sDir.filePath(f), uDir.filePath(f));
		}
	}

	list.clear();
	const QStringList files = uDir.entryList({QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
	for (const QString &f : files) {
		const QString fullPath = uDir.filePath(f);
		QFile file(fullPath);
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			QTextStream in(&file);
			const QString content = in.readAll();
			const QFileInfo fi(fullPath);
			Song song = parseNativeSong(content, fi.completeBaseName());
			song.filePath = fullPath;
			if (song.isValid())
				list.push_back(song);
		}
	}
}

std::vector<Song> SongLibrary::songs() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return list;
}

Song SongLibrary::songByTitle(const QString &title) const
{
	std::lock_guard<std::mutex> lock(mutex);
	const QString t = title.trimmed().toLower();
	for (const auto &s : list) {
		if (s.title.trimmed().toLower() == t)
			return s;
	}
	return Song();
}

Song SongLibrary::songByPath(const QString &path) const
{
	std::lock_guard<std::mutex> lock(mutex);
	for (const auto &s : list) {
		if (s.filePath == path)
			return s;
	}
	return Song();
}

std::vector<Song> SongLibrary::search(const QString &query) const
{
	std::lock_guard<std::mutex> lock(mutex);
	const QString q = query.trimmed().toLower();
	if (q.isEmpty())
		return list;

	std::vector<Song> titleMatches;
	std::vector<Song> otherMatches;

	for (const auto &s : list) {
		if (s.title.toLower().contains(q)) {
			titleMatches.push_back(s);
			continue;
		}
		if (s.author.toLower().contains(q)) {
			otherMatches.push_back(s);
			continue;
		}
		bool lyricsMatch = false;
		for (const auto &sec : s.sections) {
			for (const auto &sl : sec.rawSlides) {
				for (const auto &ln : sl) {
					if (ln.toLower().contains(q)) {
						lyricsMatch = true;
						break;
					}
				}
				if (lyricsMatch)
					break;
			}
			if (lyricsMatch)
				break;
		}
		if (lyricsMatch)
			otherMatches.push_back(s);
	}

	titleMatches.insert(titleMatches.end(), otherMatches.begin(), otherMatches.end());
	return titleMatches;
}

int SongLibrary::importSongFileCount(const QString &srcPath, QString *firstImportedTitle, QString *error)
{
	QFile src(srcPath);
	if (!src.open(QIODevice::ReadOnly)) {
		if (error)
			*error = QStringLiteral("Could not open file: %1").arg(srcPath);
		return 0;
	}

	const QFileInfo fi(srcPath);
	const QString ext = fi.suffix().toLower();
	const QByteArray rawData = src.readAll();
	src.close();

	std::vector<Song> importedSongs;

	if (ext == QStringLiteral("json")) {
		importedSongs = parseSongJson(rawData, error);
		for (auto &s : importedSongs) {
			if (s.title.isEmpty())
				s.title = fi.completeBaseName();
		}
	} else if (ext == QStringLiteral("xml") || rawData.trimmed().startsWith('<')) {
		const QString content = QString::fromUtf8(rawData);
		Song song = parseSongXml(content, error);
		if (song.title.isEmpty())
			song.title = fi.completeBaseName();
		if (song.isValid())
			importedSongs.push_back(song);
	} else {
		QTextStream in(rawData);
		const QString content = in.readAll();
		Song song = parseNativeSong(content, fi.completeBaseName());
		if (song.isValid())
			importedSongs.push_back(song);
	}

	if (importedSongs.empty()) {
		if (error && error->isEmpty())
			*error = QStringLiteral("Could not parse any valid songs from %1").arg(fi.fileName());
		return 0;
	}

	int savedCount = 0;
	for (const auto &song : importedSongs) {
		const QString serialized = serializeNativeSong(song);
		const QString safeName = cleanFileName(song.title) + QStringLiteral(".txt");
		const QString destPath = QDir(userPath).filePath(safeName);

		QFile dest(destPath);
		if (dest.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
			QTextStream out(&dest);
			out << serialized;
			dest.close();
			savedCount++;
		}
	}

	if (savedCount == 0) {
		if (error)
			*error = QStringLiteral("Could not write song(s) to folder: %1").arg(userPath);
		return 0;
	}

	if (firstImportedTitle && !importedSongs.empty())
		*firstImportedTitle = importedSongs.front().title;

	load(QString(), userPath);
	return savedCount;
}

bool SongLibrary::importSongFile(const QString &srcPath, QString *importedTitle, QString *error)
{
	return importSongFileCount(srcPath, importedTitle, error) > 0;
}

bool SongLibrary::saveSong(const Song &song, const QString &suggestedFileName, QString *savedPath, QString *error)
{
	const QString serialized = serializeNativeSong(song);
	QString base = cleanFileName(suggestedFileName.isEmpty() ? song.title : suggestedFileName);
	if (!base.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive))
		base += QStringLiteral(".txt");

	const QString destPath = QDir(userPath).filePath(base);
	QFile dest(destPath);
	if (!dest.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)
			*error = QStringLiteral("Could not write song file: %1").arg(destPath);
		return false;
	}
	QTextStream out(&dest);
	out << serialized;
	dest.close();

	if (savedPath)
		*savedPath = destPath;

	load(QString(), userPath);
	return true;
}

bool SongLibrary::removeSong(const QString &titleOrPath, QString *error)
{
	std::lock_guard<std::mutex> lock(mutex);
	QString targetPath = titleOrPath;
	for (const auto &s : list) {
		if (s.title.trimmed().toLower() == titleOrPath.trimmed().toLower() || s.filePath == titleOrPath) {
			targetPath = s.filePath;
			break;
		}
	}

	QFile f(targetPath);
	if (!f.exists()) {
		if (error)
			*error = QStringLiteral("File not found: %1").arg(targetPath);
		return false;
	}
	if (!f.remove()) {
		if (error)
			*error = QStringLiteral("Could not delete file: %1").arg(targetPath);
		return false;
	}

	load(QString(), userPath);
	return true;
}
