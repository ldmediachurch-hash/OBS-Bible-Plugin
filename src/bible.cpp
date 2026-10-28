/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "bible.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>

#include <algorithm>
#include <map>

/* ------------------------------------------------------------------ canon */

static const char *kEnglish[66] = {"Genesis",
				   "Exodus",
				   "Leviticus",
				   "Numbers",
				   "Deuteronomy",
				   "Joshua",
				   "Judges",
				   "Ruth",
				   "1 Samuel",
				   "2 Samuel",
				   "1 Kings",
				   "2 Kings",
				   "1 Chronicles",
				   "2 Chronicles",
				   "Ezra",
				   "Nehemiah",
				   "Esther",
				   "Job",
				   "Psalms",
				   "Proverbs",
				   "Ecclesiastes",
				   "Song of Solomon",
				   "Isaiah",
				   "Jeremiah",
				   "Lamentations",
				   "Ezekiel",
				   "Daniel",
				   "Hosea",
				   "Joel",
				   "Amos",
				   "Obadiah",
				   "Jonah",
				   "Micah",
				   "Nahum",
				   "Habakkuk",
				   "Zephaniah",
				   "Haggai",
				   "Zechariah",
				   "Malachi",
				   "Matthew",
				   "Mark",
				   "Luke",
				   "John",
				   "Acts",
				   "Romans",
				   "1 Corinthians",
				   "2 Corinthians",
				   "Galatians",
				   "Ephesians",
				   "Philippians",
				   "Colossians",
				   "1 Thessalonians",
				   "2 Thessalonians",
				   "1 Timothy",
				   "2 Timothy",
				   "Titus",
				   "Philemon",
				   "Hebrews",
				   "James",
				   "1 Peter",
				   "2 Peter",
				   "1 John",
				   "2 John",
				   "3 John",
				   "Jude",
				   "Revelation"};

// OSIS codes, USFM codes and common short forms (already normalized: lower case, no spaces/dots)
static const char *kAliases[] = {
	// 1 Genesis
	"1:gen gn ge",
	"2:exod exo ex",
	"3:lev lv le",
	"4:num nm nu nb",
	"5:deut deu dt de",
	"6:josh jos jsh",
	"7:judg jdg jg jdgs",
	"8:ruth rut rth ru",
	"9:1sam 1sa 1sm 1s",
	"10:2sam 2sa 2sm 2s",
	"11:1kgs 1ki 1kg 1k",
	"12:2kgs 2ki 2kg 2k",
	"13:1chr 1ch",
	"14:2chr 2ch",
	"15:ezra ezr",
	"16:neh ne",
	"17:esth est es",
	"18:job jb",
	"19:ps psa pss psm psalm psalms",
	"20:prov pro prv pr",
	"21:eccl ecc ec qoh",
	"22:song sng sos ss songofsongs canticles",
	"23:isa is",
	"24:jer je jr",
	"25:lam la",
	"26:ezek ezk eze",
	"27:dan dn da",
	"28:hos ho",
	"29:joel jol jl",
	"30:amos amo am",
	"31:obad oba ob",
	"32:jonah jon jnh",
	"33:mic mi",
	"34:nah nam na",
	"35:hab hb",
	"36:zeph zep zp",
	"37:hag hg",
	"38:zech zec zc",
	"39:mal ml",
	"40:matt mat mt",
	"41:mark mrk mk mr",
	"42:luke luk lk",
	"43:john jhn jn",
	"44:acts act ac",
	"45:rom ro rm",
	"46:1cor 1co",
	"47:2cor 2co",
	"48:gal ga",
	"49:eph ep",
	"50:phil php pp",
	"51:col",
	"52:1thess 1th 1ts",
	"53:2thess 2th 2ts",
	"54:1tim 1ti 1tm",
	"55:2tim 2ti 2tm",
	"56:titus tit ti",
	"57:phlm phm philem",
	"58:heb",
	"59:jas jm james",
	"60:1pet 1pe 1pt 1p",
	"61:2pet 2pe 2pt 2p",
	"62:1john 1jn 1jo 1jhn",
	"63:2john 2jn 2jo 2jhn",
	"64:3john 3jn 3jo 3jhn",
	"65:jude jud jd",
	"66:rev rv re revelations apocalypse",
};

int Canon::count()
{
	return 66;
}

QString Canon::englishName(int number)
{
	if (number >= 1 && number <= 66)
		return QString::fromUtf8(kEnglish[number - 1]);
	return QStringLiteral("Book %1").arg(number);
}

QString Canon::normalize(const QString &s)
{
	QString d = s.normalized(QString::NormalizationForm_KD).toLower();
	QString out;
	out.reserve(d.size());
	for (const QChar c : d) {
		if (c.isLetterOrNumber())
			out.append(c);
	}
	return out;
}

static const QHash<QString, int> &aliasTable()
{
	static QHash<QString, int> table = [] {
		QHash<QString, int> t;
		for (int i = 0; i < 66; i++)
			t.insert(Canon::normalize(QString::fromUtf8(kEnglish[i])), i + 1);
		for (const char *line : kAliases) {
			QString l = QString::fromUtf8(line);
			int colon = static_cast<int>(l.indexOf(':'));
			int number = l.left(colon).toInt();
			const QStringList keys = l.mid(colon + 1).split(' ', Qt::SkipEmptyParts);
			for (const QString &k : keys) {
				if (!t.contains(k))
					t.insert(k, number);
			}
		}
		return t;
	}();
	return table;
}

// Returns a canonical book number for an English name, abbreviation, OSIS or USFM code; 0 if unknown.
static int identifyBook(const QString &raw)
{
	QString key = Canon::normalize(raw);
	if (key.isEmpty())
		return 0;
	const auto &t = aliasTable();
	auto it = t.find(key);
	if (it != t.end())
		return it.value();
	return 0;
}

/* ------------------------------------------------------------------ text cleanup */

static QString cleanVerse(QString t)
{
	static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
	static const QRegularExpression spaces(QStringLiteral("\\s+"));
	static const QRegularExpression beforePunct(QStringLiteral(" +([,;:.?!])"));
	t.remove(tags);
	t.remove(QChar(0x00B6)); // pilcrow
	t.remove(QChar('{'));
	t.remove(QChar('}'));
	t.replace(spaces, QStringLiteral(" "));
	t.replace(beforePunct, QStringLiteral("\\1"));
	return t.trimmed();
}

/* ------------------------------------------------------------------ Bible */

const BibleBook *Bible::book(int number) const
{
	for (const auto &b : books) {
		if (b.number == number)
			return &b;
	}
	return nullptr;
}

QString Bible::verse(int bookNumber, int chapter, int v) const
{
	const BibleBook *b = book(bookNumber);
	if (!b || chapter < 1 || chapter > static_cast<int>(b->chapters.size()))
		return QString();
	const QStringList &c = b->chapters[static_cast<size_t>(chapter - 1)];
	if (v < 1 || v > c.size())
		return QString();
	return c.at(v - 1);
}

int Bible::chapterCount(int bookNumber) const
{
	const BibleBook *b = book(bookNumber);
	return b ? static_cast<int>(b->chapters.size()) : 0;
}

int Bible::verseCount(int bookNumber, int chapter) const
{
	const BibleBook *b = book(bookNumber);
	if (!b || chapter < 1 || chapter > static_cast<int>(b->chapters.size()))
		return 0;
	return static_cast<int>(b->chapters[static_cast<size_t>(chapter - 1)].size());
}

static bool readLdBooks(const QJsonObject &root, std::vector<BibleBook> &books)
{
	const QJsonArray arr = root.value(QStringLiteral("books")).toArray();
	books.clear();
	books.reserve(static_cast<size_t>(arr.size()));
	for (const QJsonValue bv : arr) {
		const QJsonObject bo = bv.toObject();
		BibleBook b;
		b.number = bo.value(QStringLiteral("number")).toInt();
		b.name = bo.value(QStringLiteral("name")).toString();
		const QJsonArray chapters = bo.value(QStringLiteral("chapters")).toArray();
		b.chapters.reserve(static_cast<size_t>(chapters.size()));
		for (const QJsonValue cv : chapters) {
			QStringList verses;
			const QJsonArray va = cv.toArray();
			verses.reserve(va.size());
			for (const QJsonValue v : va)
				verses.append(v.toString());
			b.chapters.push_back(std::move(verses));
		}
		books.push_back(std::move(b));
	}
	return !books.empty();
}

bool Bible::ensureLoaded(QString *error)
{
	std::lock_guard<std::mutex> lock(loadMutex);
	if (loaded)
		return true;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error)
			*error = QStringLiteral("Couldn't open %1").arg(path);
		return false;
	}
	QJsonParseError pe;
	QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
	if (doc.isNull()) {
		if (error)
			*error = pe.errorString();
		return false;
	}
	if (!readLdBooks(doc.object(), books)) {
		if (error)
			*error = QStringLiteral("No books found in %1").arg(path);
		return false;
	}
	loaded = true;
	return true;
}

/* ------------------------------------------------------------------ storage */

static QString jsonString(const QString &s)
{
	QJsonArray a;
	a.append(s);
	QByteArray b = QJsonDocument(a).toJson(QJsonDocument::Compact);
	return QString::fromUtf8(b.mid(1, b.size() - 2));
}

bool saveBible(const Bible &bible, const QString &path, QString *error)
{
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (error)
			*error = QStringLiteral("Couldn't write %1").arg(path);
		return false;
	}
	// Header fields first, so the name can be read without parsing the whole file.
	QString head =
		QStringLiteral("{\"format\":\"ld-bible-1\",\"name\":%1,\"abbrev\":%2,\"language\":%3,\"books\":[")
			.arg(jsonString(bible.name), jsonString(bible.abbrev), jsonString(bible.language));
	f.write(head.toUtf8());
	bool firstBook = true;
	for (const auto &b : bible.books) {
		QJsonObject bo;
		bo.insert(QStringLiteral("number"), b.number);
		bo.insert(QStringLiteral("name"), b.name);
		QJsonArray chapters;
		for (const auto &c : b.chapters)
			chapters.append(QJsonArray::fromStringList(c));
		bo.insert(QStringLiteral("chapters"), chapters);
		if (!firstBook)
			f.write(",");
		firstBook = false;
		f.write(QJsonDocument(bo).toJson(QJsonDocument::Compact));
	}
	f.write("]}");
	if (!f.commit()) {
		if (error)
			*error = QStringLiteral("Couldn't save %1").arg(path);
		return false;
	}
	return true;
}

BiblePtr openStoredBible(const QString &path, bool builtIn)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return nullptr;
	const QString head = QString::fromUtf8(f.read(4096));
	static const QRegularExpression nameRe(QStringLiteral("\"name\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
	static const QRegularExpression abbrevRe(QStringLiteral("\"abbrev\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
	static const QRegularExpression langRe(QStringLiteral("\"language\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
	auto unescape = [](const QString &s) {
		QJsonDocument d = QJsonDocument::fromJson(QStringLiteral("[\"%1\"]").arg(s).toUtf8());
		return d.isArray() ? d.array().at(0).toString() : s;
	};
	auto b = std::make_shared<Bible>();
	b->path = path;
	b->builtIn = builtIn;
	auto m = nameRe.match(head);
	b->name = m.hasMatch() ? unescape(m.captured(1)) : QFileInfo(path).completeBaseName();
	m = abbrevRe.match(head);
	b->abbrev = m.hasMatch() ? unescape(m.captured(1)) : QFileInfo(path).completeBaseName();
	m = langRe.match(head);
	b->language = m.hasMatch() ? unescape(m.captured(1)) : QString();
	return b;
}

/* ------------------------------------------------------------------ import: shared builder */

namespace {

struct BuildBook {
	int number = 0;
	QString name;
	std::map<int, std::map<int, QString>> chapters;
};

class Builder {
public:
	// Starts (or continues) a book. Number wins, then a recognised name/code, then the order in the file.
	void beginBook(int number, const QString &name)
	{
		int n = number > 0 ? number : identifyBook(name);
		if (n <= 0)
			n = sequential + 1;
		sequential = std::max(sequential, n);
		current = n;
		auto &b = books[n];
		b.number = n;
		if (b.name.isEmpty() && !name.isEmpty() && !isNumber(name) && !isCode(name, n))
			b.name = name;
		if (std::find(order.begin(), order.end(), n) == order.end())
			order.push_back(n);
	}
	void setChapter(int c) { chapter = c; }
	void addVerse(int v, const QString &text) { addVerse(current, chapter, v, text); }
	void addVerse(int book, int ch, int v, const QString &text)
	{
		QString t = cleanVerse(text);
		if (book <= 0 || ch <= 0 || v <= 0 || t.isEmpty())
			return;
		if (!books.count(book))
			beginBook(book, QString());
		QString &slot = books[book].chapters[ch][v];
		slot = slot.isEmpty() ? t : slot + QLatin1Char(' ') + t;
		verseCount++;
	}
	int currentBook() const { return current; }
	int currentChapter() const { return chapter; }
	int verses() const { return verseCount; }

	std::vector<BibleBook> finish() const
	{
		std::vector<BibleBook> out;
		std::vector<int> nums = order;
		std::sort(nums.begin(), nums.end());
		for (int n : nums) {
			const BuildBook &bb = books.at(n);
			if (bb.chapters.empty())
				continue;
			BibleBook b;
			b.number = n;
			b.name = bb.name.isEmpty() ? Canon::englishName(n) : bb.name;
			int maxChapter = bb.chapters.rbegin()->first;
			b.chapters.resize(static_cast<size_t>(maxChapter));
			for (const auto &cp : bb.chapters) {
				int maxVerse = cp.second.empty() ? 0 : cp.second.rbegin()->first;
				QStringList verses;
				for (int i = 0; i < maxVerse; i++)
					verses.append(QString());
				for (const auto &vp : cp.second)
					verses[vp.first - 1] = vp.second;
				b.chapters[static_cast<size_t>(cp.first - 1)] = verses;
			}
			out.push_back(std::move(b));
		}
		return out;
	}

	// "Gen", "1Cor", "JHN": a code rather than a display name (the English name is used instead)
	static bool isCode(const QString &s, int number)
	{
		if (number < 1 || number > 66 || identifyBook(s) != number)
			return false;
		return Canon::normalize(s) != Canon::normalize(Canon::englishName(number)) && s.size() <= 6 &&
		       !s.contains(QLatin1Char(' '));
	}

	static bool isNumber(const QString &s)
	{
		bool ok = false;
		s.trimmed().toInt(&ok);
		return ok;
	}

private:
	std::map<int, BuildBook> books;
	std::vector<int> order;
	int sequential = 0;
	int current = 0;
	int chapter = 0;
	int verseCount = 0;
};

int firstInt(const QString &s)
{
	static const QRegularExpression re(QStringLiteral("(\\d+)"));
	auto m = re.match(s);
	return m.hasMatch() ? m.captured(1).toInt() : 0;
}

// "Gen.1.1" or "Gen.1.1 Gen.1.2" -> book code, chapter, verse
bool splitOsisId(const QString &id, QString *code, int *chapter, int *verse)
{
	QString first = id.section(QLatin1Char(' '), 0, 0);
	first = first.section(QLatin1Char(':'), -1); // strip "KJV:" work prefix
	const QStringList parts = first.split(QLatin1Char('.'));
	if (parts.isEmpty())
		return false;
	if (code)
		*code = parts.value(0);
	if (chapter)
		*chapter = parts.size() > 1 ? parts.value(1).toInt() : 0;
	if (verse)
		*verse = parts.size() > 2 ? parts.value(2).toInt() : 0;
	return true;
}

/* ------------------------------------------------------------------ import: XML */

bool parseXml(const QByteArray &data, Builder &builder, QString *error, QString *name, QString *abbrev)
{
	QXmlStreamReader xml(data);
	bool inVerse = false;
	bool verseIsMilestone = false;
	int verseNumber = 0;
	int skipDepth = 0;
	QString text;
	QString pendingTitle;
	bool inWorkTitle = false;

	auto finishVerse = [&]() {
		if (inVerse)
			builder.addVerse(verseNumber, text);
		inVerse = false;
		verseIsMilestone = false;
		text.clear();
	};
	auto attr = [&](const char *n) {
		return xml.attributes().value(QLatin1String(n)).toString();
	};
	auto hasAttr = [&](const char *n) {
		return xml.attributes().hasAttribute(QLatin1String(n));
	};

	while (!xml.atEnd()) {
		auto token = xml.readNext();
		if (token == QXmlStreamReader::StartElement) {
			const QString el = xml.name().toString().toLower();
			if (skipDepth > 0) {
				skipDepth++;
				continue;
			}
			if (el == QLatin1String("xmlbible") || el == QLatin1String("bible")) {
				QString n = attr("biblename");
				if (n.isEmpty())
					n = attr("translation");
				if (n.isEmpty())
					n = attr("name");
				if (!n.isEmpty() && name && name->isEmpty())
					*name = n;
				continue;
			}
			if (el == QLatin1String("title") && !inVerse && name && name->isEmpty()) {
				inWorkTitle = true;
				pendingTitle.clear();
				continue;
			}
			if (el == QLatin1String("note") || el == QLatin1String("notes") ||
			    el == QLatin1String("footnote") || el == QLatin1String("rf") ||
			    el == QLatin1String("xref") || el == QLatin1String("fn") ||
			    el == QLatin1String("caption")) {
				skipDepth = 1;
				continue;
			}
			bool isBook = el == QLatin1String("biblebook") || el == QLatin1String("book") ||
				      el == QLatin1String("b") ||
				      (el == QLatin1String("div") && attr("type").toLower() == QLatin1String("book"));
			if (isBook) {
				finishVerse();
				int number = 0;
				QString bname;
				if (hasAttr("bnumber"))
					number = attr("bnumber").toInt();
				else if (hasAttr("number"))
					number = attr("number").toInt();
				else if (hasAttr("id") && Builder::isNumber(attr("id")))
					number = attr("id").toInt();
				if (hasAttr("bname"))
					bname = attr("bname");
				else if (hasAttr("name"))
					bname = attr("name");
				else if (hasAttr("n"))
					bname = attr("n");
				else if (hasAttr("osisid"))
					bname = attr("osisid");
				else if (hasAttr("osisID"))
					bname = attr("osisID");
				if (number == 0 && Builder::isNumber(bname))
					number = bname.toInt();
				builder.beginBook(number, bname);
				continue;
			}
			if (el == QLatin1String("chapter") || el == QLatin1String("c")) {
				if (hasAttr("eID"))
					continue;
				finishVerse();
				int c = 0;
				if (hasAttr("cnumber"))
					c = attr("cnumber").toInt();
				else if (hasAttr("number"))
					c = attr("number").toInt();
				else if (hasAttr("n"))
					c = firstInt(attr("n"));
				else if (hasAttr("osisID") || hasAttr("sID")) {
					QString code;
					int ch = 0;
					splitOsisId(hasAttr("osisID") ? attr("osisID") : attr("sID"), &code, &ch,
						    nullptr);
					int bn = identifyBook(code);
					if (bn > 0 && bn != builder.currentBook())
						builder.beginBook(bn, code);
					c = ch;
				} else
					c = builder.currentChapter() + 1;
				builder.setChapter(c);
				continue;
			}
			if (el == QLatin1String("vers") || el == QLatin1String("verse") || el == QLatin1String("v")) {
				if (hasAttr("eID")) {
					finishVerse();
					continue;
				}
				finishVerse();
				int v = 0;
				if (hasAttr("vnumber"))
					v = firstInt(attr("vnumber"));
				else if (hasAttr("number"))
					v = firstInt(attr("number"));
				else if (hasAttr("n"))
					v = firstInt(attr("n"));
				else if (hasAttr("osisID") || hasAttr("sID")) {
					QString code;
					int ch = 0;
					splitOsisId(hasAttr("osisID") ? attr("osisID") : attr("sID"), &code, &ch, &v);
					int bn = identifyBook(code);
					if (bn > 0 && bn != builder.currentBook())
						builder.beginBook(bn, code);
					if (ch > 0 && ch != builder.currentChapter())
						builder.setChapter(ch);
				}
				if (v <= 0)
					continue;
				inVerse = true;
				verseIsMilestone = hasAttr("sID");
				verseNumber = v;
				text.clear();
				continue;
			}
		} else if (token == QXmlStreamReader::EndElement) {
			const QString el = xml.name().toString().toLower();
			if (skipDepth > 0) {
				skipDepth--;
				continue;
			}
			if (inWorkTitle && el == QLatin1String("title")) {
				inWorkTitle = false;
				if (name && name->isEmpty())
					*name = pendingTitle.trimmed();
				continue;
			}
			if ((el == QLatin1String("vers") || el == QLatin1String("verse") || el == QLatin1String("v")) &&
			    inVerse && !verseIsMilestone) {
				finishVerse();
				continue;
			}
			if (el == QLatin1String("chapter") || el == QLatin1String("c") ||
			    el == QLatin1String("biblebook") || el == QLatin1String("book") ||
			    el == QLatin1String("b") || el == QLatin1String("div")) {
				if (inVerse && !verseIsMilestone)
					finishVerse();
			}
		} else if (token == QXmlStreamReader::Characters) {
			if (skipDepth > 0)
				continue;
			if (inWorkTitle)
				pendingTitle += xml.text();
			else if (inVerse)
				text += xml.text();
		}
	}
	finishVerse();
	if (xml.hasError() && builder.verses() == 0) {
		if (error)
			*error = QStringLiteral("XML error: %1 (line %2)").arg(xml.errorString()).arg(xml.lineNumber());
		return false;
	}
	Q_UNUSED(abbrev);
	return builder.verses() > 0;
}

/* ------------------------------------------------------------------ import: JSON */

QString firstString(const QJsonObject &o, std::initializer_list<const char *> keys)
{
	for (const char *k : keys) {
		const QJsonValue v = o.value(QLatin1String(k));
		if (v.isString())
			return v.toString();
		if (v.isDouble())
			return QString::number(v.toInt());
	}
	return QString();
}

int firstNumber(const QJsonObject &o, std::initializer_list<const char *> keys)
{
	for (const char *k : keys) {
		const QJsonValue v = o.value(QLatin1String(k));
		if (v.isDouble())
			return v.toInt();
		if (v.isString() && Builder::isNumber(v.toString()))
			return v.toString().toInt();
	}
	return 0;
}

void readVersesInto(Builder &b, int chapter, const QJsonValue &verses)
{
	if (verses.isArray()) {
		int i = 0;
		for (const QJsonValue v : verses.toArray()) {
			i++;
			if (v.isString())
				b.addVerse(b.currentBook(), chapter, i, v.toString());
			else if (v.isObject()) {
				const QJsonObject o = v.toObject();
				int n = firstNumber(o, {"verse", "number", "v", "verse_number", "verseNumber", "id"});
				QString t = firstString(o, {"text", "t", "content", "value", "verse_text"});
				b.addVerse(b.currentBook(), chapter, n > 0 ? n : i, t);
			}
		}
	} else if (verses.isObject()) {
		const QJsonObject o = verses.toObject();
		for (auto it = o.begin(); it != o.end(); ++it) {
			if (Builder::isNumber(it.key()))
				b.addVerse(b.currentBook(), chapter, it.key().toInt(),
					   it.value().isString() ? it.value().toString()
								 : firstString(it.value().toObject(), {"text", "t"}));
		}
	}
}

void readChaptersInto(Builder &b, const QJsonValue &chapters)
{
	if (chapters.isArray()) {
		int i = 0;
		for (const QJsonValue c : chapters.toArray()) {
			i++;
			if (c.isArray())
				readVersesInto(b, i, c);
			else if (c.isObject()) {
				const QJsonObject o = c.toObject();
				int n = firstNumber(o, {"chapter", "number", "c", "chapter_number", "chapterNumber",
							"id"});
				QJsonValue vs = o.value(QStringLiteral("verses"));
				if (vs.isUndefined())
					vs = o.value(QStringLiteral("verse"));
				readVersesInto(b, n > 0 ? n : i, vs);
			}
		}
	} else if (chapters.isObject()) {
		const QJsonObject o = chapters.toObject();
		for (auto it = o.begin(); it != o.end(); ++it) {
			if (Builder::isNumber(it.key()))
				readVersesInto(b, it.key().toInt(), it.value());
		}
	}
}

bool readBookObject(Builder &b, const QJsonObject &bo)
{
	QJsonValue chapters = bo.value(QStringLiteral("chapters"));
	if (chapters.isUndefined())
		chapters = bo.value(QStringLiteral("chapter"));
	if (chapters.isUndefined())
		return false;
	int number = firstNumber(bo, {"number", "book_number", "bookNumber", "bookid", "book_id", "id", "nr"});
	QString name = firstString(bo, {"name", "book_name", "bookName", "book", "title", "abbrev"});
	if (number > 66 && identifyBook(name) > 0) // some files number books 1..73 including Apocrypha
		number = identifyBook(name);
	b.beginBook(number, name);
	readChaptersInto(b, chapters);
	return true;
}

bool readFlatVerses(Builder &b, const QJsonArray &arr)
{
	int found = 0;
	QHash<QString, int> nameToNumber;
	for (const QJsonValue v : arr) {
		if (v.isObject()) {
			const QJsonObject o = v.toObject();
			if (o.contains(QStringLiteral("field")) && o.value(QStringLiteral("field")).isArray()) {
				// old scrollmapper layout: field = [id, book, chapter, verse, text]
				const QJsonArray f = o.value(QStringLiteral("field")).toArray();
				if (f.size() >= 5) {
					b.addVerse(f.at(1).toInt(), f.at(2).toInt(), f.at(3).toInt(),
						   f.at(4).toString());
					found++;
				}
				continue;
			}
			int ch = firstNumber(o, {"chapter", "c", "chapter_number", "chapterNumber"});
			int vn = firstNumber(o, {"verse", "v", "verse_number", "verseNumber"});
			QString text = firstString(o, {"text", "t", "content", "value", "verse_text"});
			if (ch <= 0 || vn <= 0)
				continue;
			int bn = firstNumber(o, {"book", "b", "book_id", "bookNumber", "book_number", "bookid"});
			QString bname = firstString(o, {"book_name", "bookName", "name", "book"});
			if (bn <= 0 || bn > 66) {
				int id = identifyBook(bname);
				if (id > 0)
					bn = id;
				else if (bn <= 0) {
					if (!nameToNumber.contains(bname))
						nameToNumber.insert(bname, static_cast<int>(nameToNumber.size()) + 1);
					bn = nameToNumber.value(bname);
				}
			}
			if (b.currentBook() != bn)
				b.beginBook(bn, Builder::isNumber(bname) ? QString() : bname);
			b.addVerse(bn, ch, vn, text);
			found++;
		} else if (v.isArray()) {
			// [book, chapter, verse, text]
			const QJsonArray f = v.toArray();
			if (f.size() >= 4) {
				b.addVerse(f.at(0).toInt(), f.at(1).toInt(), f.at(2).toInt(), f.at(3).toString());
				found++;
			}
		}
	}
	return found > 0;
}

bool parseJson(const QByteArray &data, Builder &b, QString *error, QString *name, QString *abbrev)
{
	QJsonParseError pe;
	QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
	if (doc.isNull()) {
		if (error)
			*error = QStringLiteral("JSON error: %1").arg(pe.errorString());
		return false;
	}
	auto meta = [&](const QJsonObject &o) {
		QJsonObject m = o.value(QStringLiteral("metadata")).toObject();
		QString n = firstString(o, {"name", "translation", "title", "version_name", "bible"});
		if (n.isEmpty())
			n = firstString(m, {"name", "translation", "title"});
		QString a = firstString(o, {"abbrev", "abbreviation", "shortname", "short_name", "version"});
		if (a.isEmpty())
			a = firstString(m, {"abbrev", "abbreviation", "shortname", "module"});
		if (name && name->isEmpty())
			*name = n;
		if (abbrev && abbrev->isEmpty())
			*abbrev = a;
	};

	if (doc.isArray()) {
		const QJsonArray arr = doc.array();
		if (!arr.isEmpty() && arr.at(0).isObject() &&
		    arr.at(0).toObject().contains(QStringLiteral("chapters"))) {
			for (const QJsonValue bv : arr)
				readBookObject(b, bv.toObject());
		} else {
			readFlatVerses(b, arr);
		}
		return b.verses() > 0;
	}

	QJsonObject root = doc.object();
	meta(root);
	if (root.value(QStringLiteral("format")).toString() == QLatin1String("ld-bible-1")) {
		std::vector<BibleBook> books;
		readLdBooks(root, books);
		for (const auto &bk : books) {
			b.beginBook(bk.number, bk.name);
			for (size_t c = 0; c < bk.chapters.size(); c++)
				for (int v = 0; v < bk.chapters[c].size(); v++)
					b.addVerse(bk.number, static_cast<int>(c) + 1, v + 1, bk.chapters[c].at(v));
		}
		return b.verses() > 0;
	}
	// wrappers seen in the wild
	for (const char *k : {"bible", "data", "XMLBIBLE"}) {
		if (root.value(QLatin1String(k)).isObject()) {
			QJsonObject inner = root.value(QLatin1String(k)).toObject();
			meta(inner);
			root = inner;
			break;
		}
	}
	if (root.value(QStringLiteral("books")).isArray()) {
		for (const QJsonValue bv : root.value(QStringLiteral("books")).toArray())
			readBookObject(b, bv.toObject());
		if (b.verses() > 0)
			return true;
	}
	if (root.value(QStringLiteral("verses")).isArray() &&
	    readFlatVerses(b, root.value(QStringLiteral("verses")).toArray()))
		return true;
	if (root.value(QStringLiteral("resultset")).isObject()) {
		QJsonValue rows = root.value(QStringLiteral("resultset")).toObject().value(QStringLiteral("row"));
		if (rows.isArray() && readFlatVerses(b, rows.toArray()))
			return true;
	}
	// { "Genesis": { "1": { "1": "text" } } }  or  { "Genesis": [ [ "v1", ... ], ... ] }
	for (auto it = root.begin(); it != root.end(); ++it) {
		if (it.value().isObject() || it.value().isArray()) {
			if (it.value().isObject() && it.value().toObject().contains(QStringLiteral("chapters"))) {
				QJsonObject bo = it.value().toObject();
				if (!bo.contains(QStringLiteral("name")))
					bo.insert(QStringLiteral("name"), it.key());
				readBookObject(b, bo);
				continue;
			}
			int before = b.verses();
			b.beginBook(0, it.key());
			readChaptersInto(b, it.value());
			Q_UNUSED(before);
		}
	}
	if (b.verses() == 0 && error)
		*error = QStringLiteral("Couldn't find any verses in this JSON file.");
	return b.verses() > 0;
}

} // namespace

BiblePtr parseBibleFile(const QString &file, QString *error, QString *suggestedName, QString *suggestedAbbrev)
{
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error)
			*error = QStringLiteral("Couldn't open %1").arg(file);
		return nullptr;
	}
	QByteArray data = f.readAll();
	if (data.startsWith("\xEF\xBB\xBF"))
		data.remove(0, 3);
	QString name, abbrev;
	Builder builder;
	int firstChar = 0;
	while (firstChar < data.size() && isspace(static_cast<unsigned char>(data.at(firstChar))))
		firstChar++;
	const char c = firstChar < data.size() ? data.at(firstChar) : '\0';
	bool ok = (c == '<') ? parseXml(data, builder, error, &name, &abbrev)
			     : parseJson(data, builder, error, &name, &abbrev);
	if (!ok) {
		if (error && error->isEmpty())
			*error = QStringLiteral(
				"This file doesn't look like a Bible in a format LD Scripture can read.");
		return nullptr;
	}
	auto bible = std::make_shared<Bible>();
	bible->books = builder.finish();
	bible->loaded = true;
	if (name.isEmpty())
		name = QFileInfo(file).completeBaseName();
	if (abbrev.isEmpty()) {
		// "KJV: King James Version" -> KJV
		static const QRegularExpression lead(QStringLiteral("^([A-Za-z0-9]{2,8})\\s*[:\\-–]"));
		auto m = lead.match(name);
		if (m.hasMatch())
			abbrev = m.captured(1).toUpper();
		else {
			const QStringList words =
				name.split(QRegularExpression(QStringLiteral("[\\s_\\-]+")), Qt::SkipEmptyParts);
			if (words.size() == 1)
				abbrev = words.first().left(6).toUpper();
			else
				for (const QString &w : words)
					if (w.at(0).isLetterOrNumber())
						abbrev += w.at(0).toUpper();
			abbrev = abbrev.left(8);
		}
	}
	if (suggestedName)
		*suggestedName = name;
	if (suggestedAbbrev)
		*suggestedAbbrev = abbrev;
	return bible;
}

/* ------------------------------------------------------------------ library */

BibleLibrary &BibleLibrary::instance()
{
	static BibleLibrary lib;
	return lib;
}

void BibleLibrary::load(const QString &builtInDir, const QString &userDir)
{
	std::lock_guard<std::mutex> lock(mutex);
	list.clear();
	userPath = userDir;
	QDir().mkpath(userDir);
	const QStringList preferred = {QStringLiteral("KJV"), QStringLiteral("WEB"), QStringLiteral("ASV"),
				       QStringLiteral("BBE")};
	std::vector<BiblePtr> builtIns;
	for (const QFileInfo &fi : QDir(builtInDir).entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
		if (auto b = openStoredBible(fi.absoluteFilePath(), true))
			builtIns.push_back(b);
	}
	std::sort(builtIns.begin(), builtIns.end(), [&](const BiblePtr &a, const BiblePtr &b) {
		auto ia = preferred.indexOf(a->abbrev), ib = preferred.indexOf(b->abbrev);
		if (ia < 0)
			ia = 99;
		if (ib < 0)
			ib = 99;
		return ia != ib ? ia < ib : a->abbrev < b->abbrev;
	});
	std::vector<BiblePtr> users;
	for (const QFileInfo &fi : QDir(userDir).entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
		if (auto b = openStoredBible(fi.absoluteFilePath(), false))
			users.push_back(b);
	}
	std::sort(users.begin(), users.end(),
		  [](const BiblePtr &a, const BiblePtr &b) { return a->abbrev.toLower() < b->abbrev.toLower(); });
	for (auto &u : users) {
		// an imported Bible replaces a built-in one with the same abbreviation
		builtIns.erase(std::remove_if(builtIns.begin(), builtIns.end(),
					      [&](const BiblePtr &b) { return b->abbrev == u->abbrev; }),
			       builtIns.end());
	}
	list = builtIns;
	list.insert(list.end(), users.begin(), users.end());
}

std::vector<BiblePtr> BibleLibrary::bibles() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return list;
}

BiblePtr BibleLibrary::byAbbrev(const QString &abbrev) const
{
	std::lock_guard<std::mutex> lock(mutex);
	for (const auto &b : list) {
		if (b->abbrev == abbrev)
			return b;
	}
	return nullptr;
}

BiblePtr BibleLibrary::importFile(const QString &file, const QString &name, const QString &abbrev, QString *error)
{
	QString n, a;
	BiblePtr bible = parseBibleFile(file, error, &n, &a);
	if (!bible)
		return nullptr;
	return install(bible, name.trimmed().isEmpty() ? n : name, abbrev.trimmed().isEmpty() ? a : abbrev, error);
}

BiblePtr BibleLibrary::install(BiblePtr bible, const QString &name, const QString &abbrev, QString *error)
{
	if (!bible)
		return nullptr;
	bible->name = name.trimmed();
	bible->abbrev = abbrev.trimmed();
	if (bible->abbrev.isEmpty()) {
		if (error)
			*error = QStringLiteral("Please give the translation a short name, e.g. NKJV.");
		return nullptr;
	}
	QString safe = bible->abbrev;
	safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_\\-]")), QStringLiteral("_"));
	bible->path = QDir(userPath).filePath(safe + QStringLiteral(".json"));
	bible->builtIn = false;
	bible->loaded = true;
	if (!saveBible(*bible, bible->path, error))
		return nullptr;
	std::lock_guard<std::mutex> lock(mutex);
	list.erase(std::remove_if(list.begin(), list.end(),
				  [&](const BiblePtr &b) { return b->abbrev == bible->abbrev; }),
		   list.end());
	list.push_back(bible);
	return bible;
}

std::vector<VerseRef> BibleLibrary::search(Bible *bible, const QString &words, int limit) const
{
	std::vector<VerseRef> out;
	if (!bible || !bible->ensureLoaded())
		return out;
	const QStringList terms = words.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (terms.isEmpty())
		return out;
	for (const auto &b : bible->books) {
		for (size_t c = 0; c < b.chapters.size(); c++) {
			const QStringList &verses = b.chapters[c];
			for (int v = 0; v < verses.size(); v++) {
				const QString &t = verses.at(v);
				bool all = true;
				for (const QString &term : terms) {
					if (!t.contains(term, Qt::CaseInsensitive)) {
						all = false;
						break;
					}
				}
				if (all) {
					out.push_back({b.number, static_cast<int>(c) + 1, v + 1});
					if (static_cast<int>(out.size()) >= limit)
						return out;
				}
			}
		}
	}
	return out;
}

bool BibleLibrary::remove(const QString &abbrev, QString *error)
{
	std::lock_guard<std::mutex> lock(mutex);
	for (auto it = list.begin(); it != list.end(); ++it) {
		if ((*it)->abbrev == abbrev) {
			if ((*it)->builtIn) {
				if (error)
					*error = QStringLiteral("%1 comes with LD Scripture and can't be removed.")
							 .arg(abbrev);
				return false;
			}
			QFile::remove((*it)->path);
			list.erase(it);
			return true;
		}
	}
	if (error)
		*error = QStringLiteral("%1 isn't installed.").arg(abbrev);
	return false;
}

QString BibleLibrary::bookName(Bible *bible, int number) const
{
	if (bible && bible->ensureLoaded()) {
		if (const BibleBook *b = bible->book(number)) {
			if (!b->name.isEmpty())
				return b->name;
		}
	}
	return Canon::englishName(number);
}

QString BibleLibrary::formatRef(Bible *bible, int book, int chapter, int v1, int v2) const
{
	QString name = bookName(bible, book);
	if (book == 19 && name == QLatin1String("Psalms"))
		name = QStringLiteral("Psalm");
	QString r = QStringLiteral("%1 %2").arg(name).arg(chapter);
	if (v1 > 0) {
		r += QStringLiteral(":%1").arg(v1);
		if (v2 > v1)
			r += QStringLiteral("–%1").arg(v2);
	}
	return r;
}

ParsedRef BibleLibrary::parse(const QString &input, Bible *names) const
{
	ParsedRef r;
	QString s = input.trimmed();
	s.replace(QChar(0x2013), QLatin1Char('-'));
	s.replace(QChar(0x2014), QLatin1Char('-'));
	// "I John", "II Tim", "III Jn" (roman numerals written with a space)
	static const QRegularExpression roman(QStringLiteral("^(iii|ii|i)\\s+(?=\\D)"),
					      QRegularExpression::CaseInsensitiveOption);
	auto rm = roman.match(s);
	if (rm.hasMatch())
		s = QString::number(rm.captured(1).size()) + QLatin1Char(' ') + s.mid(rm.capturedLength());

	static const QRegularExpression re(QStringLiteral(
		"^\\s*((?:[1-3]\\s*)?[^\\d\\s][^\\d]*?)\\.?\\s*(\\d+)(?:\\s*[:.,\\s]\\s*(\\d+)(?:\\s*-\\s*(\\d+))?)?\\s*$"));
	auto m = re.match(s);
	if (!m.hasMatch())
		return r;
	const QString key = Canon::normalize(m.captured(1));
	if (key.isEmpty())
		return r;

	int book = 0;
	// 1. the translation's own book names (e.g. Yoruba), 2. English names/abbreviations, 3. prefixes
	if (names && names->ensureLoaded()) {
		for (const auto &b : names->books) {
			if (Canon::normalize(b.name) == key) {
				book = b.number;
				break;
			}
		}
	}
	if (!book)
		book = identifyBook(key);
	if (!book && key.size() >= 2) {
		for (int i = 1; i <= 66 && !book; i++) {
			if (Canon::normalize(Canon::englishName(i)).startsWith(key))
				book = i;
		}
		if (!book && names && names->isLoaded()) {
			for (const auto &b : names->books) {
				if (Canon::normalize(b.name).startsWith(key)) {
					book = b.number;
					break;
				}
			}
		}
	}
	if (!book)
		return r;
	r.book = book;
	r.chapter = m.captured(2).toInt();
	r.v1 = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
	r.v2 = m.captured(4).isEmpty() ? 0 : m.captured(4).toInt();
	static const int oneChapter[] = {31, 57, 63, 64, 65}; // Obadiah, Philemon, 2 John, 3 John, Jude
	if (r.v1 == 0 && std::find(std::begin(oneChapter), std::end(oneChapter), book) != std::end(oneChapter)) {
		r.v1 = r.chapter;
		r.chapter = 1;
	}
	r.valid = true;
	return r;
}
