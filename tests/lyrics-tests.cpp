/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "../src/lyrics.hpp"

#include <QCoreApplication>
#include <QDebug>

#include <cstdlib>
#include <iostream>

#define ASSERT_TRUE(cond, msg)                                                               \
	do {                                                                                 \
		if (!(cond)) {                                                               \
			std::cerr << "FAILED: " << msg << " (" << #cond << ")" << std::endl; \
			std::exit(1);                                                        \
		}                                                                            \
	} while (false)

static void testHeaderParsing()
{
	const QString text = QStringLiteral("Title: Amazing Grace\n"
					    "Author: John Newton\n"
					    "CCLI: 22025\n"
					    "Copyright: Public Domain\n"
					    "Order: V1 V2 C V3\n\n"
					    "[Verse 1]\n"
					    "Line 1\n");
	const Song s = parseNativeSong(text, QStringLiteral("Fallback"));
	ASSERT_TRUE(s.title == QStringLiteral("Amazing Grace"), "Title parsed");
	ASSERT_TRUE(s.author == QStringLiteral("John Newton"), "Author parsed");
	ASSERT_TRUE(s.ccli == QStringLiteral("22025"), "CCLI parsed");
	ASSERT_TRUE(s.copyright == QStringLiteral("Public Domain"), "Copyright parsed");
	ASSERT_TRUE(s.order == QStringLiteral("V1 V2 C V3"), "Order parsed");
	ASSERT_TRUE(s.sections.size() == 1, "Section count");
}

static void testSectionLines()
{
	const QString text = QStringLiteral("[Verse 1]\nLine 1\n\n"
					    "[Chorus]\nChorus line\n\n"
					    "[Bridge]\nBridge line\n\n"
					    "[Pre-Chorus]\nPre line\n\n"
					    "[Tag]\nTag line\n\n"
					    "[Ending]\nEnding line\n");
	const Song s = parseNativeSong(text);
	ASSERT_TRUE(s.sections.size() == 6, "All 6 sections parsed");
	ASSERT_TRUE(s.sections[0].name == QStringLiteral("Verse 1") && s.sections[0].shortCode == QStringLiteral("V1"),
		    "Verse 1 code");
	ASSERT_TRUE(s.sections[1].name == QStringLiteral("Chorus") && s.sections[1].shortCode == QStringLiteral("C"),
		    "Chorus code");
	ASSERT_TRUE(s.sections[2].name == QStringLiteral("Bridge") && s.sections[2].shortCode == QStringLiteral("B"),
		    "Bridge code");
	ASSERT_TRUE(s.sections[3].name == QStringLiteral("Pre-Chorus") &&
			    s.sections[3].shortCode == QStringLiteral("PC"),
		    "Pre-Chorus code");
	ASSERT_TRUE(s.sections[4].name == QStringLiteral("Tag") && s.sections[4].shortCode == QStringLiteral("T"),
		    "Tag code");
	ASSERT_TRUE(s.sections[5].name == QStringLiteral("Ending") && s.sections[5].shortCode == QStringLiteral("E"),
		    "Ending code");
}

static void testBlankLineSlides()
{
	const QString text = QStringLiteral("[Verse 1]\n"
					    "Line 1\n"
					    "Line 2\n\n"
					    "Line 3\n"
					    "Line 4\n");
	const Song s = parseNativeSong(text);
	ASSERT_TRUE(s.sections.size() == 1, "Section count 1");
	ASSERT_TRUE(s.sections[0].rawSlides.size() == 2, "Two raw slides separated by blank line");
	ASSERT_TRUE(s.sections[0].rawSlides[0].size() == 2, "Slide 1 has 2 lines");
	ASSERT_TRUE(s.sections[0].rawSlides[0][0] == QStringLiteral("Line 1"), "Slide 1 line 1");
	ASSERT_TRUE(s.sections[0].rawSlides[0][1] == QStringLiteral("Line 2"), "Slide 1 line 2");
	ASSERT_TRUE(s.sections[0].rawSlides[1].size() == 2, "Slide 2 has 2 lines");
	ASSERT_TRUE(s.sections[0].rawSlides[1][0] == QStringLiteral("Line 3"), "Slide 2 line 1");
	ASSERT_TRUE(s.sections[0].rawSlides[1][1] == QStringLiteral("Line 4"), "Slide 2 line 2");
}

static void testSplittingFiveLineSlide()
{
	// Splitting a 5-line slide with "Lines per slide" = 2 into 2 + 2 + 1
	const QString text = QStringLiteral("[Verse 1]\n"
					    "Line 1\n"
					    "Line 2\n"
					    "Line 3\n"
					    "Line 4\n"
					    "Line 5\n");
	const Song s = parseNativeSong(text);
	const std::vector<SongSlide> slides = generateSlides(s, 2);
	ASSERT_TRUE(slides.size() == 3, "5 lines with lps=2 yields 3 slides");
	ASSERT_TRUE(slides[0].lines.size() == 2, "First slide has 2 lines");
	ASSERT_TRUE(slides[0].lines[0] == QStringLiteral("Line 1") && slides[0].lines[1] == QStringLiteral("Line 2"),
		    "First chunk 2 lines");
	ASSERT_TRUE(slides[1].lines.size() == 2, "Second slide has 2 lines");
	ASSERT_TRUE(slides[1].lines[0] == QStringLiteral("Line 3") && slides[1].lines[1] == QStringLiteral("Line 4"),
		    "Second chunk 2 lines");
	ASSERT_TRUE(slides[2].lines.size() == 1, "Third slide has 1 line");
	ASSERT_TRUE(slides[2].lines[0] == QStringLiteral("Line 5"), "Third chunk 1 line");
}

static void testOrderExpansion()
{
	const QString text = QStringLiteral("Title: Order Test\n"
					    "Order: V1 C V2 C\n\n"
					    "[Verse 1]\n"
					    "V1 line 1\n\n"
					    "[Verse 2]\n"
					    "V2 line 1\n\n"
					    "[Chorus]\n"
					    "Chorus line 1\n");
	const Song s = parseNativeSong(text);
	const std::vector<SongSlide> slides = generateSlides(s, 2);
	ASSERT_TRUE(slides.size() == 4, "Order expanded to 4 slides");
	ASSERT_TRUE(slides[0].sectionName == QStringLiteral("Verse 1"), "Slide 1 is Verse 1");
	ASSERT_TRUE(slides[1].sectionName == QStringLiteral("Chorus"), "Slide 2 is Chorus");
	ASSERT_TRUE(slides[2].sectionName == QStringLiteral("Verse 2"), "Slide 3 is Verse 2");
	ASSERT_TRUE(slides[3].sectionName == QStringLiteral("Chorus"), "Slide 4 is Chorus");
}

static void testOpenLyricsXml()
{
	const QString xml = QStringLiteral(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<song xmlns=\"http://openlyrics.info/namespace/2009/song\">\n"
		"  <properties>\n"
		"    <titles><title>Amazing Grace</title></titles>\n"
		"    <authors><author>John Newton</author></authors>\n"
		"    <ccliNo>22025</ccliNo>\n"
		"    <verseOrder>v1 c</verseOrder>\n"
		"  </properties>\n"
		"  <lyrics>\n"
		"    <verse name=\"v1\">\n"
		"      <lines>Amazing grace! How sweet the sound<br/>That saved a wretch like me!</lines>\n"
		"    </verse>\n"
		"    <verse name=\"c\">\n"
		"      <lines>Praise God, praise God<br/>Praise God, praise God</lines>\n"
		"    </verse>\n"
		"  </lyrics>\n"
		"</song>");
	QString err;
	const Song s = parseOpenLyricsXml(xml, &err);
	ASSERT_TRUE(err.isEmpty(), "OpenLyrics parsed with no error");
	ASSERT_TRUE(s.title == QStringLiteral("Amazing Grace"), "OpenLyrics title");
	ASSERT_TRUE(s.author == QStringLiteral("John Newton"), "OpenLyrics author");
	ASSERT_TRUE(s.ccli == QStringLiteral("22025"), "OpenLyrics ccli");
	ASSERT_TRUE(s.order == QStringLiteral("v1 c"), "OpenLyrics order");
	ASSERT_TRUE(s.sections.size() == 2, "OpenLyrics sections");
	ASSERT_TRUE(s.sections[0].rawSlides.size() == 1, "Verse 1 has 1 raw slide");
	ASSERT_TRUE(s.sections[0].rawSlides[0].size() == 2, "Verse 1 lines from <br/>");
	ASSERT_TRUE(s.sections[0].rawSlides[0][0] == QStringLiteral("Amazing grace! How sweet the sound"), "Line 1");
	ASSERT_TRUE(s.sections[0].rawSlides[0][1] == QStringLiteral("That saved a wretch like me!"), "Line 2");
}

static void testCrlfAndBom()
{
	QByteArray data;
	data.append("\xEF\xBB\xBF"); // UTF-8 BOM
	data.append("Title: BOM Song\r\n"
		    "Author: Test Author\r\n\r\n"
		    "[Verse 1]\r\n"
		    "Line 1\r\n"
		    "Line 2\r\n\r\n"
		    "Line 3\r\n");
	const QString content = QString::fromUtf8(data);
	const Song s = parseNativeSong(content);
	ASSERT_TRUE(s.title == QStringLiteral("BOM Song"), "BOM stripped and title parsed");
	ASSERT_TRUE(s.author == QStringLiteral("Test Author"), "CRLF author parsed");
	ASSERT_TRUE(s.sections.size() == 1, "Section count");
	ASSERT_TRUE(s.sections[0].rawSlides.size() == 2, "Two slides with CRLF");
	ASSERT_TRUE(s.sections[0].rawSlides[0][0] == QStringLiteral("Line 1"), "Line 1 CRLF");
	ASSERT_TRUE(s.sections[0].rawSlides[0][1] == QStringLiteral("Line 2"), "Line 2 CRLF");
	ASSERT_TRUE(s.sections[0].rawSlides[1][0] == QStringLiteral("Line 3"), "Line 3 CRLF");
}

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	std::cout << "Running lyrics parser tests..." << std::endl;

	testHeaderParsing();
	testSectionLines();
	testBlankLineSlides();
	testSplittingFiveLineSlide();
	testOrderExpansion();
	testOpenLyricsXml();
	testCrlfAndBom();

	std::cout << "All lyrics tests passed successfully!" << std::endl;
	return 0;
}
