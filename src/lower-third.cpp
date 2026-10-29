/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "lower-third.hpp"
#include "display-state.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

namespace {

inline QColor obsColorToQColor(uint32_t val, int alpha = 255)
{
	int r = static_cast<int>(val & 0xFF);
	int g = static_cast<int>((val >> 8) & 0xFF);
	int b = static_cast<int>((val >> 16) & 0xFF);
	return QColor(r, g, b, alpha);
}

struct LtSettings {
	bool isLyrics = false;

	// Background style & customization
	std::string bgStyle = "light";       // "light", "dark", "custom", "transparent", "image"
	uint32_t customBgColor = 0x00FFFFFF; // OBS BGR (white)
	int bgOpacity = 100;                 // 0 to 100%
	std::string bgImagePath;
	std::string bgImageFit = "cover"; // "cover", "fit", "stretch"
	int cornerRadius = 26;            // px at 1080p

	// Shadow controls
	bool enableShadow = true;
	int shadowOpacity = 100; // 0 to 100%

	// Border / Boundary controls
	bool enableBorder = false;
	uint32_t borderColor = 0x00FFFFFF; // OBS BGR (white)
	int borderWidth = 0;               // px at 1080p

	// Typography & Font
	std::string fontFamily = "Inter";
	std::string fontWeight = "medium"; // "regular", "medium", "semibold", "bold", "extrabold"
	int fontSize = 40;
	int maxLines = 3;
	std::string textAlign = "left"; // "left", "center", "right"

	// Colors
	bool useCustomColors = false;
	uint32_t textColor = 0x0015110F;   // OBS BGR for #0F1115
	uint32_t kickerColor = 0x007A706A; // OBS BGR for #6A707A

	// Header row controls
	bool showKicker = true;
	bool showSignalDot = true;

	// Layout & Placement
	bool top = false;
	bool stacked = false; // scripture dual translations
	int marginX = 96;
	int marginY = 64;

	// Chips & Metadata
	bool chips = true;     // scripture translation labels
	bool showCcli = false; // lyrics CCLI

	// Animation & Sample
	int fadeMs = 300;
	bool sample = false;
};

struct Palette {
	QColor card, text, kicker, chipBg, chipText, divider, shadow, border;
};

Palette paletteFor(const LtSettings &s)
{
	Palette pal;
	if (s.bgStyle == "dark") {
		pal = {QColor(15, 17, 21, 240),   QColor(255, 255, 255), QColor(163, 169, 178),
		       QColor(255, 255, 255, 30), QColor(230, 232, 235), QColor(255, 255, 255, 40),
		       QColor(0, 0, 0),           QColor(0, 0, 0, 0)};
	} else if (s.bgStyle == "transparent") {
		pal = {QColor(0, 0, 0, 0),        QColor(255, 255, 255), QColor(230, 232, 235),
		       QColor(255, 255, 255, 40), QColor(255, 255, 255), QColor(255, 255, 255, 40),
		       QColor(0, 0, 0),           QColor(0, 0, 0, 0)};
	} else if (s.bgStyle == "custom") {
		QColor bg = obsColorToQColor(s.customBgColor);
		bool lightBg = bg.lightness() > 128;
		pal.card = bg;
		pal.text = lightBg ? QColor(15, 17, 21) : QColor(255, 255, 255);
		pal.kicker = lightBg ? QColor(106, 112, 122) : QColor(163, 169, 178);
		pal.chipBg = lightBg ? QColor(241, 242, 244) : QColor(255, 255, 255, 30);
		pal.chipText = lightBg ? QColor(62, 68, 77) : QColor(230, 232, 235);
		pal.divider = lightBg ? QColor(227, 229, 232) : QColor(255, 255, 255, 40);
		pal.shadow = QColor(0, 0, 0);
		pal.border = QColor(0, 0, 0, 0);
	} else {
		// "light" or "image"
		pal = {QColor(255, 255, 255), QColor(15, 17, 21),    QColor(106, 112, 122), QColor(241, 242, 244),
		       QColor(62, 68, 77),    QColor(227, 229, 232), QColor(6, 10, 26),     QColor(0, 0, 0, 0)};
	}

	// Apply card background opacity
	if (s.bgStyle != "transparent") {
		int alpha = std::clamp(static_cast<int>(s.bgOpacity * 255 / 100), 0, 255);
		pal.card.setAlpha(alpha);
	}

	// Apply custom text/kicker colors if enabled
	if (s.useCustomColors) {
		pal.text = obsColorToQColor(s.textColor);
		pal.kicker = obsColorToQColor(s.kickerColor);
	}

	// Apply border color if enabled
	if (s.enableBorder && s.borderWidth > 0) {
		pal.border = obsColorToQColor(s.borderColor);
	}

	return pal;
}

const QColor kSignalRed(255, 45, 45);

struct Laid {
	std::unique_ptr<QTextLayout> layout;
	qreal height = 0;
	int lines = 0;
};

Laid layText(const QString &text, const QFont &font, qreal width, qreal lineHeight, Qt::Alignment align = Qt::AlignLeft)
{
	Laid l;
	l.layout = std::make_unique<QTextLayout>(text, font);
	QTextOption opt;
	opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	l.layout->setTextOption(opt);
	QFontMetricsF fm(font);
	const qreal glyphOffset = (lineHeight - fm.height()) / 2.0;
	l.layout->beginLayout();
	qreal y = 0;
	for (;;) {
		QTextLine line = l.layout->createLine();
		if (!line.isValid())
			break;
		line.setLineWidth(width);
		qreal lineX = 0;
		if (align & Qt::AlignHCenter)
			lineX = std::max<qreal>(0.0, (width - line.naturalTextWidth()) / 2.0);
		else if (align & Qt::AlignRight)
			lineX = std::max<qreal>(0.0, width - line.naturalTextWidth());
		line.setPosition(QPointF(lineX, y + glyphOffset));
		y += lineHeight;
		l.lines++;
	}
	l.layout->endLayout();
	l.height = y;
	return l;
}

int resolveFontWeight(const std::string &w)
{
	if (w == "bold")
		return 700;
	if (w == "semibold")
		return 600;
	if (w == "regular")
		return 400;
	if (w == "extrabold")
		return 800;
	return 500; // Medium
}

QFont makeFont(const QString &family, int weight, qreal px)
{
	QFont f;
	if (!family.isEmpty() && family != "Inter" && family != "default")
		f.setFamily(family);
	else
		f.setFamily(QStringLiteral("Inter"));
	f.setPixelSize(std::max(1, static_cast<int>(std::lround(px))));
	f.setWeight(static_cast<QFont::Weight>(weight));
	f.setHintingPreference(QFont::PreferNoHinting);
	f.setStyleStrategy(QFont::PreferAntialias);
	return f;
}

struct Chip {
	QString text;
	QFont font;
	qreal w = 0, h = 0, padX = 0;
};

Chip makeChip(const QString &text, qreal scale, const QString &fontFamily)
{
	Chip c;
	c.text = text;
	c.font = makeFont(fontFamily, 600, 15 * scale);
	c.font.setLetterSpacing(QFont::AbsoluteSpacing, 0.06 * 15 * scale);
	QFontMetricsF fm(c.font);
	c.padX = 11 * scale;
	c.w = fm.horizontalAdvance(text) + 2 * c.padX;
	c.h = 28 * scale;
	return c;
}

void drawChip(QPainter &p, const Chip &c, qreal x, qreal y, const Palette &pal)
{
	QPainterPath path;
	path.addRoundedRect(QRectF(x, y, c.w, c.h), c.h / 2, c.h / 2);
	p.fillPath(path, pal.chipBg);
	p.setFont(c.font);
	p.setPen(pal.chipText);
	p.drawText(QRectF(x, y, c.w, c.h), Qt::AlignCenter, c.text);
}

// Renders the card for `content`. Returns the image and where its top-left goes on the canvas.
QImage renderCard(const DisplayContent &content, const LtSettings &s, uint32_t canvasW, uint32_t canvasH, QPointF *pos)
{
	const qreal scale = canvasH / 1080.0;
	const Palette pal = paletteFor(s);
	const qreal mx = s.marginX * scale;
	const qreal cardW = std::max<qreal>(200.0, canvasW - 2 * mx);
	const qreal padX = 40 * scale, padTop = 26 * scale, padBottom = 30 * scale;
	const qreal shadowPad = 44 * scale;
	const qreal innerW = cardW - 2 * padX;
	const size_t n = content.entries.size();
	const bool twoCols = n == 2 && !s.stacked;
	const qreal colGap = 64 * scale;
	const qreal colW = twoCols ? (innerW - colGap) / 2 : innerW;

	const QString fontFamily = QString::fromStdString(s.fontFamily);
	const int fontWeight = resolveFontWeight(s.fontWeight);

	// kicker: red dot + REFERENCE (+ chip when there is one translation or CCLI for lyrics)
	QFont kicker = makeFont(fontFamily, 600, 19 * scale);
	kicker.setLetterSpacing(QFont::AbsoluteSpacing, 0.16 * 19 * scale);
	QFontMetricsF kfm(kicker);
	const qreal kickerH = s.showKicker ? 28 * scale : 0.0;
	const qreal kickerGap = s.showKicker ? 14 * scale : 0.0;

	// fit the text: shrink up to 30% before letting the card grow
	const qreal basePx = s.fontSize * scale;
	qreal px = basePx;
	std::vector<Laid> laid;
	const qreal chipBlock = (!s.isLyrics && s.chips && n > 1) ? 28 * scale + 12 * scale : 0;

	Qt::Alignment bodyAlign = Qt::AlignLeft;
	if (s.textAlign == "center")
		bodyAlign = Qt::AlignHCenter;
	else if (s.textAlign == "right")
		bodyAlign = Qt::AlignRight;

	for (;;) {
		laid.clear();
		QFont body = makeFont(fontFamily, fontWeight, px);
		const qreal lh = px * 1.32;
		int worst = 0;
		for (const auto &e : content.entries) {
			laid.push_back(
				layText(e.text.isEmpty() ? QStringLiteral("—") : e.text, body, colW, lh, bodyAlign));
			worst = std::max(worst, laid.back().lines);
		}
		if (worst <= s.maxLines || px <= basePx * 0.7)
			break;
		px -= std::max<qreal>(1.0, scale);
	}

	qreal contentH = 0;
	if (twoCols) {
		for (const auto &l : laid)
			contentH = std::max(contentH, chipBlock + l.height);
	} else {
		for (size_t i = 0; i < laid.size(); i++)
			contentH += chipBlock + laid[i].height + (i + 1 < laid.size() ? 22 * scale : 0);
	}
	const qreal cardH = padTop + kickerH + kickerGap + contentH + padBottom;

	const int imgW = static_cast<int>(std::ceil(cardW + 2 * shadowPad));
	const int imgH = static_cast<int>(std::ceil(cardH + 2 * shadowPad));
	QImage img(imgW, imgH, QImage::Format_RGBA8888_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);

	const QRectF card(shadowPad, shadowPad, cardW, cardH);
	const qreal radius = s.cornerRadius * scale;

	// Drop shadow
	if (s.enableShadow && s.bgStyle != "transparent") {
		const int layers = 22;
		for (int i = layers; i >= 1; i--) {
			const qreal spread = i * (shadowPad * 0.85 / layers);
			QColor c = pal.shadow;
			const qreal t = 1.0 - static_cast<qreal>(i) / layers;
			float baseAlpha = (s.bgStyle == "dark") ? 0.05f : 0.028f;
			float alpha = (baseAlpha * t * t + 0.002f) * (s.shadowOpacity / 100.0f);
			c.setAlphaF(std::clamp(alpha, 0.0f, 1.0f));
			QPainterPath sp;
			sp.addRoundedRect(card.adjusted(-spread, -spread + 14 * scale, spread, spread + 14 * scale),
					  radius + spread, radius + spread);
			p.fillPath(sp, c);
		}
	}

	QPainterPath cardPath;
	cardPath.addRoundedRect(card, radius, radius);

	// Card background
	if (s.bgStyle == "transparent") {
		// No background fill (floating text)
	} else if (s.bgStyle == "image" && !s.bgImagePath.empty()) {
		QImage bgImg(QString::fromStdString(s.bgImagePath));
		if (!bgImg.isNull()) {
			p.save();
			p.setClipPath(cardPath);
			if (s.bgOpacity < 100)
				p.setOpacity(s.bgOpacity / 100.0);
			if (s.bgImageFit == "stretch") {
				p.drawImage(card, bgImg);
			} else if (s.bgImageFit == "fit") {
				QImage scaled = bgImg.scaled(card.size().toSize(), Qt::KeepAspectRatio,
							     Qt::SmoothTransformation);
				QRectF target((card.width() - scaled.width()) / 2 + card.left(),
					      (card.height() - scaled.height()) / 2 + card.top(), scaled.width(),
					      scaled.height());
				p.drawImage(target, scaled);
			} else {
				// cover
				QImage scaled = bgImg.scaled(card.size().toSize(), Qt::KeepAspectRatioByExpanding,
							     Qt::SmoothTransformation);
				QRectF target((card.width() - scaled.width()) / 2 + card.left(),
					      (card.height() - scaled.height()) / 2 + card.top(), scaled.width(),
					      scaled.height());
				p.drawImage(target, scaled);
			}
			p.restore();
		} else {
			p.fillPath(cardPath, pal.card);
		}
	} else {
		p.fillPath(cardPath, pal.card);
	}

	// Card border / boundary
	if (s.enableBorder && s.borderWidth > 0 && pal.border.alpha() > 0) {
		p.save();
		QPen bPen(pal.border, s.borderWidth * scale);
		bPen.setJoinStyle(Qt::RoundJoin);
		p.setPen(bPen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(card, radius, radius);
		p.restore();
	}

	qreal x = card.left() + padX;
	qreal y = card.top() + padTop;

	// Kicker row (Reference / Song Title)
	if (s.showKicker) {
		const qreal dot = 12 * scale;
		qreal refX = x;
		if (s.showSignalDot) {
			p.setPen(Qt::NoPen);
			p.setBrush(kSignalRed);
			p.drawEllipse(QRectF(x, y + (kickerH - dot) / 2, dot, dot));
			p.setBrush(Qt::NoBrush);
			refX = x + dot + 12 * scale;
		}

		p.setFont(kicker);
		p.setPen(pal.kicker);
		const QString ref = content.reference.toUpper();

		Qt::Alignment kickerAlign = Qt::AlignVCenter |
					    (s.textAlign == "center"
						     ? Qt::AlignHCenter
						     : (s.textAlign == "right" ? Qt::AlignRight : Qt::AlignLeft));

		if (s.isLyrics) {
			qreal availW = innerW - (s.showSignalDot ? (dot + 12 * scale) : 0);
			if (s.showCcli && !content.ccli.trimmed().isEmpty()) {
				Chip c = makeChip(QStringLiteral("CCLI %1").arg(content.ccli.trimmed()), scale,
						  fontFamily);
				const qreal chipX = card.right() - padX - c.w;
				drawChip(p, c, chipX, y + (kickerH - c.h) / 2, pal);
				availW = std::max<qreal>(0.0, chipX - 14 * scale - refX);
			}
			p.drawText(QRectF(refX, y, availW, kickerH), kickerAlign, ref);
		} else {
			p.drawText(QRectF(refX, y, innerW - (refX - x), kickerH), kickerAlign, ref);
			if (s.chips && n == 1 && !content.entries[0].label.isEmpty()) {
				Chip c = makeChip(content.entries[0].label, scale, fontFamily);
				drawChip(p, c, refX + kfm.horizontalAdvance(ref) + 14 * scale, y + (kickerH - c.h) / 2,
					 pal);
			}
		}
		y += kickerH + kickerGap;
	}

	// Body text
	for (size_t i = 0; i < laid.size(); i++) {
		qreal cx = x;
		qreal cy = y;
		if (twoCols) {
			cx = x + static_cast<qreal>(i) * (colW + colGap);
		}
		if (chipBlock > 0) {
			Chip c = makeChip(content.entries[i].label, scale, fontFamily);
			drawChip(p, c, cx, cy, pal);
			cy += chipBlock;
		}

		// Floating text drop shadow for transparent mode
		if (s.bgStyle == "transparent" && s.enableShadow) {
			p.save();
			QColor textShadow(0, 0, 0, static_cast<int>(180 * s.shadowOpacity / 100));
			p.setPen(textShadow);
			laid[i].layout->draw(&p, QPointF(cx + 2 * scale, cy + 2 * scale));
			p.restore();
		}

		p.setPen(pal.text);
		laid[i].layout->draw(&p, QPointF(cx, cy));
		if (!twoCols)
			y = cy + laid[i].height + 22 * scale;
	}
	if (twoCols) {
		p.setPen(QPen(pal.divider, std::max<qreal>(1.0, 2 * scale)));
		const qreal dx = x + colW + colGap / 2;
		p.drawLine(QPointF(dx, card.top() + padTop + kickerH + kickerGap),
			   QPointF(dx, card.bottom() - padBottom));
	}
	p.end();

	pos->setX(mx - shadowPad);
	pos->setY(s.top ? s.marginY * scale - shadowPad : canvasH - s.marginY * scale - cardH - shadowPad);
	return img;
}

struct LowerThird {
	obs_source_t *source = nullptr;
	Channel channel = Channel::Scripture;

	std::mutex settingsMutex;
	LtSettings pending;
	bool settingsChanged = true;

	LtSettings s;
	uint64_t seenVersion = 0;
	DisplayContent shown;
	bool hasContent = false;
	bool showingSample = false;

	float alpha = 0.0f;
	float cross = 1.0f;

	QImage curImg, prevImg;
	QPointF curPos, prevPos;
	bool curDirty = false, prevDirty = false, dropPrev = false;
	gs_texture_t *curTex = nullptr;
	gs_texture_t *prevTex = nullptr;
	gs_effect_t *effect = nullptr;
	uint32_t cx = 1920, cy = 1080;
};

DisplayContent sampleContent()
{
	DisplayContent c;
	c.visible = true;
	c.reference = QStringLiteral("Romans 8:28");
	c.entries.push_back({QStringLiteral("KJV"),
			     QStringLiteral("And we know that all things work together for good to them that love God, "
					    "to them who are the called according to his purpose.")});
	return c;
}

DisplayContent sampleLyricsContent()
{
	DisplayContent c;
	c.visible = true;
	c.reference = QStringLiteral("AMAZING GRACE · VERSE 1");
	c.ccli = QStringLiteral("22025");
	c.entries.push_back(
		{QString(), QStringLiteral("Amazing grace! How sweet the sound\nThat saved a wretch like me!")});
	return c;
}

void readCommonSettings(LtSettings &s, obs_data_t *d, bool isLyrics)
{
	s.isLyrics = isLyrics;

	const char *themeStr = obs_data_get_string(d, "theme");
	s.bgStyle = (themeStr && strlen(themeStr) > 0) ? themeStr : "light";

	s.customBgColor = static_cast<uint32_t>(obs_data_get_int(d, "bg_color"));
	s.bgOpacity = static_cast<int>(obs_data_get_int(d, "bg_opacity"));
	if (!obs_data_has_user_value(d, "bg_opacity"))
		s.bgOpacity = 100;

	const char *imgPath = obs_data_get_string(d, "bg_image");
	s.bgImagePath = imgPath ? imgPath : "";

	const char *imgFit = obs_data_get_string(d, "bg_image_fit");
	s.bgImageFit = (imgFit && strlen(imgFit) > 0) ? imgFit : "cover";

	s.cornerRadius = static_cast<int>(obs_data_get_int(d, "corner_radius"));
	if (!obs_data_has_user_value(d, "corner_radius"))
		s.cornerRadius = 26;

	s.enableShadow = obs_data_get_bool(d, "enable_shadow");
	s.shadowOpacity = static_cast<int>(obs_data_get_int(d, "shadow_opacity"));
	if (!obs_data_has_user_value(d, "shadow_opacity"))
		s.shadowOpacity = 100;

	s.enableBorder = obs_data_get_bool(d, "enable_border");
	s.borderColor = static_cast<uint32_t>(obs_data_get_int(d, "border_color"));
	s.borderWidth = static_cast<int>(obs_data_get_int(d, "border_width"));
	if (!obs_data_has_user_value(d, "border_width"))
		s.borderWidth = 2;

	const char *ff = obs_data_get_string(d, "font_family");
	s.fontFamily = (ff && strlen(ff) > 0) ? ff : "Inter";

	const char *fw = obs_data_get_string(d, "font_weight");
	s.fontWeight = (fw && strlen(fw) > 0) ? fw : "medium";

	s.fontSize = static_cast<int>(obs_data_get_int(d, "font_size"));
	if (s.fontSize <= 0)
		s.fontSize = isLyrics ? 44 : 40;

	s.maxLines = static_cast<int>(obs_data_get_int(d, "max_lines"));
	if (s.maxLines <= 0)
		s.maxLines = 3;

	const char *al = obs_data_get_string(d, "align");
	s.textAlign = (al && strlen(al) > 0) ? al : "left";

	s.useCustomColors = obs_data_get_bool(d, "use_custom_colors");
	s.textColor = static_cast<uint32_t>(obs_data_get_int(d, "text_color"));
	s.kickerColor = static_cast<uint32_t>(obs_data_get_int(d, "kicker_color"));

	s.showKicker = obs_data_get_bool(d, "show_kicker");
	s.showSignalDot = obs_data_get_bool(d, "show_dot");

	s.top = strcmp(obs_data_get_string(d, "position"), "top") == 0;
	s.marginX = static_cast<int>(obs_data_get_int(d, "margin_x"));
	s.marginY = static_cast<int>(obs_data_get_int(d, "margin_y"));
	s.fadeMs = static_cast<int>(obs_data_get_int(d, "fade_ms"));
	s.sample = obs_data_get_bool(d, "sample");
}

void readScriptureSettings(LtSettings &s, obs_data_t *d)
{
	readCommonSettings(s, d, false);
	s.stacked = strcmp(obs_data_get_string(d, "layout"), "stacked") == 0;
	s.chips = obs_data_get_bool(d, "chips");
	s.showCcli = false;
}

void readLyricsSettings(LtSettings &s, obs_data_t *d)
{
	readCommonSettings(s, d, true);
	s.stacked = false;
	s.chips = false;
	s.showCcli = obs_data_get_bool(d, "show_ccli");
}

const char *lt_get_name(void *)
{
	return obs_module_text("LDScripture.LowerThird");
}

const char *lyrics_lt_get_name(void *)
{
	return obs_module_text("LDScripture.LyricsLowerThird");
}

void lt_update(void *data, obs_data_t *settings)
{
	auto *lt = static_cast<LowerThird *>(data);
	std::lock_guard<std::mutex> lock(lt->settingsMutex);
	readScriptureSettings(lt->pending, settings);
	lt->settingsChanged = true;
}

void lyrics_lt_update(void *data, obs_data_t *settings)
{
	auto *lt = static_cast<LowerThird *>(data);
	std::lock_guard<std::mutex> lock(lt->settingsMutex);
	readLyricsSettings(lt->pending, settings);
	lt->settingsChanged = true;
}

LowerThird *createLowerThird(obs_data_t *settings, obs_source_t *source, Channel channel, bool isLyrics)
{
	auto *lt = new LowerThird();
	lt->source = source;
	lt->channel = channel;
	lt->pending.isLyrics = isLyrics;
	lt->s.isLyrics = isLyrics;
	obs_enter_graphics();
	char *path = obs_module_file("effects/ld-fade.effect");
	if (path) {
		char *err = nullptr;
		lt->effect = gs_effect_create_from_file(path, &err);
		if (err) {
			blog(LOG_WARNING, "[LD Scripture] effect error: %s", err);
			bfree(err);
		}
		bfree(path);
	}
	obs_leave_graphics();
	if (isLyrics)
		lyrics_lt_update(lt, settings);
	else
		lt_update(lt, settings);
	return lt;
}

void *lt_create(obs_data_t *settings, obs_source_t *source)
{
	return createLowerThird(settings, source, Channel::Scripture, false);
}

void *lyrics_lt_create(obs_data_t *settings, obs_source_t *source)
{
	return createLowerThird(settings, source, Channel::Lyrics, true);
}

void lt_destroy(void *data)
{
	auto *lt = static_cast<LowerThird *>(data);
	obs_enter_graphics();
	if (lt->curTex)
		gs_texture_destroy(lt->curTex);
	if (lt->prevTex)
		gs_texture_destroy(lt->prevTex);
	if (lt->effect)
		gs_effect_destroy(lt->effect);
	obs_leave_graphics();
	delete lt;
}

void setCommonDefaults(obs_data_t *d, bool isLyrics)
{
	obs_data_set_default_string(d, "theme", "light");
	obs_data_set_default_int(d, "bg_color", 0xFFFFFFFF);
	obs_data_set_default_int(d, "bg_opacity", 100);
	obs_data_set_default_string(d, "bg_image", "");
	obs_data_set_default_string(d, "bg_image_fit", "cover");
	obs_data_set_default_int(d, "corner_radius", 26);
	obs_data_set_default_bool(d, "enable_shadow", true);
	obs_data_set_default_int(d, "shadow_opacity", 100);
	obs_data_set_default_bool(d, "enable_border", false);
	obs_data_set_default_int(d, "border_color", 0x00FFFFFF);
	obs_data_set_default_int(d, "border_width", 2);
	obs_data_set_default_string(d, "font_family", "Inter");
	obs_data_set_default_string(d, "font_weight", "medium");
	obs_data_set_default_int(d, "font_size", isLyrics ? 44 : 40);
	obs_data_set_default_int(d, "max_lines", 3);
	obs_data_set_default_string(d, "align", "left");
	obs_data_set_default_bool(d, "use_custom_colors", false);
	obs_data_set_default_int(d, "text_color", 0xFF0F1115);
	obs_data_set_default_int(d, "kicker_color", 0xFF6A707A);
	obs_data_set_default_bool(d, "show_kicker", true);
	obs_data_set_default_bool(d, "show_dot", true);
	obs_data_set_default_string(d, "position", "bottom");
	obs_data_set_default_int(d, "margin_x", 96);
	obs_data_set_default_int(d, "margin_y", 64);
	obs_data_set_default_int(d, "fade_ms", 300);
	obs_data_set_default_bool(d, "sample", false);
}

void lt_defaults(obs_data_t *d)
{
	setCommonDefaults(d, false);
	obs_data_set_default_string(d, "layout", "side");
	obs_data_set_default_bool(d, "chips", true);
}

void lyrics_lt_defaults(obs_data_t *d)
{
	setCommonDefaults(d, true);
	obs_data_set_default_bool(d, "show_ccli", false);
}

bool settings_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const char *theme = obs_data_get_string(settings, "theme");
	bool isCustom = strcmp(theme, "custom") == 0;
	bool isImage = strcmp(theme, "image") == 0;
	bool isTransparent = strcmp(theme, "transparent") == 0;
	bool hasShadow = obs_data_get_bool(settings, "enable_shadow");
	bool hasBorder = obs_data_get_bool(settings, "enable_border");
	bool customColors = obs_data_get_bool(settings, "use_custom_colors");
	bool showKicker = obs_data_get_bool(settings, "show_kicker");

	obs_property_set_visible(obs_properties_get(props, "bg_color"), isCustom);
	obs_property_set_visible(obs_properties_get(props, "bg_opacity"), !isTransparent);
	obs_property_set_visible(obs_properties_get(props, "bg_image"), isImage);
	obs_property_set_visible(obs_properties_get(props, "bg_image_fit"), isImage);
	obs_property_set_visible(obs_properties_get(props, "corner_radius"), !isTransparent);

	obs_property_set_visible(obs_properties_get(props, "shadow_opacity"), hasShadow);

	obs_property_set_visible(obs_properties_get(props, "border_color"), hasBorder);
	obs_property_set_visible(obs_properties_get(props, "border_width"), hasBorder);

	obs_property_set_visible(obs_properties_get(props, "text_color"), customColors);
	obs_property_set_visible(obs_properties_get(props, "kicker_color"), customColors);

	obs_property_set_visible(obs_properties_get(props, "show_dot"), showKicker);

	return true;
}

void addCommonProperties(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_add_list(props, "theme", "Background Style", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Light Card (White)", "light");
	obs_property_list_add_string(p, "Dark Card (Black)", "dark");
	obs_property_list_add_string(p, "Custom Color Card", "custom");
	obs_property_list_add_string(p, "Transparent (No Background / Floating Text)", "transparent");
	obs_property_list_add_string(p, "Custom Background Image", "image");
	obs_property_set_modified_callback(p, settings_modified);

	obs_properties_add_color(props, "bg_color", "Custom Card Color");
	obs_properties_add_int(props, "bg_opacity", "Card Opacity (%)", 0, 100, 5);

	obs_properties_add_path(props, "bg_image", "Background Image File", OBS_PATH_FILE,
				"Image Files (*.png *.jpg *.jpeg *.bmp *.webp *.svg)", nullptr);
	p = obs_properties_add_list(props, "bg_image_fit", "Image Scaling", OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Cover (Fill entire card)", "cover");
	obs_property_list_add_string(p, "Fit (Maintain aspect ratio)", "fit");
	obs_property_list_add_string(p, "Stretch (Fill exactly)", "stretch");

	obs_properties_add_int(props, "corner_radius", "Corner Rounding (px)", 0, 60, 2);

	p = obs_properties_add_bool(props, "enable_shadow", "Drop Shadow");
	obs_property_set_modified_callback(p, settings_modified);
	obs_properties_add_int(props, "shadow_opacity", "Shadow Intensity (%)", 0, 100, 5);

	p = obs_properties_add_bool(props, "enable_border", "Card Border / Boundary");
	obs_property_set_modified_callback(p, settings_modified);
	obs_properties_add_color(props, "border_color", "Border Color");
	obs_properties_add_int(props, "border_width", "Border Thickness (px)", 1, 10, 1);

	p = obs_properties_add_list(props, "font_family", "Font Family", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Inter (Modern UI)", "Inter");
	obs_property_list_add_string(p, "Arial", "Arial");
	obs_property_list_add_string(p, "Helvetica", "Helvetica");
	obs_property_list_add_string(p, "Roboto", "Roboto");
	obs_property_list_add_string(p, "Segoe UI", "Segoe UI");
	obs_property_list_add_string(p, "Georgia (Serif)", "Georgia");
	obs_property_list_add_string(p, "Times New Roman (Classic)", "Times New Roman");
	obs_property_list_add_string(p, "Montserrat", "Montserrat");
	obs_property_list_add_string(p, "Open Sans", "Open Sans");

	p = obs_properties_add_list(props, "font_weight", "Font Weight", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Regular (400)", "regular");
	obs_property_list_add_string(p, "Medium (500)", "medium");
	obs_property_list_add_string(p, "SemiBold (600)", "semibold");
	obs_property_list_add_string(p, "Bold (700)", "bold");
	obs_property_list_add_string(p, "ExtraBold (800)", "extrabold");

	// Numeric font size box with visible numbers (not hidden slider)
	obs_properties_add_int(props, "font_size", "Text Size (px at 1080p)", 16, 120, 1);
	obs_properties_add_int(props, "max_lines", "Max Lines Before Shrink", 1, 8, 1);

	p = obs_properties_add_list(props, "align", "Text Alignment", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Left", "left");
	obs_property_list_add_string(p, "Center", "center");
	obs_property_list_add_string(p, "Right", "right");

	p = obs_properties_add_bool(props, "use_custom_colors", "Custom Text & Title Colors");
	obs_property_set_modified_callback(p, settings_modified);
	obs_properties_add_color(props, "text_color", "Body Text Color");
	obs_properties_add_color(props, "kicker_color", "Title / Kicker Color");

	p = obs_properties_add_bool(props, "show_kicker", "Show Title / Reference Bar");
	obs_property_set_modified_callback(p, settings_modified);
	obs_properties_add_bool(props, "show_dot", "Show Red Live Dot");

	p = obs_properties_add_list(props, "position", "Screen Position", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Bottom", "bottom");
	obs_property_list_add_string(p, "Top", "top");

	obs_properties_add_int(props, "margin_x", "Side Margin (px at 1080p)", 0, 800, 4);
	obs_properties_add_int(props, "margin_y", "Edge Margin (px at 1080p)", 0, 800, 4);
	obs_properties_add_int(props, "fade_ms", "Transition Fade (ms)", 0, 2000, 50);
	obs_properties_add_bool(props, "sample", "Show Sample Card for Positioning");
}

obs_properties_t *lt_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	addCommonProperties(props);

	obs_property_t *p = obs_properties_add_list(props, "layout", "Two Translations Layout", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Side by side", "side");
	obs_property_list_add_string(p, "Stacked", "stacked");

	obs_properties_add_bool(props, "chips", "Show Translation Badges");
	return props;
}

obs_properties_t *lyrics_lt_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	addCommonProperties(props);

	obs_properties_add_bool(props, "show_ccli", "Show CCLI Number");
	return props;
}

uint32_t lt_width(void *data)
{
	return static_cast<LowerThird *>(data)->cx;
}

uint32_t lt_height(void *data)
{
	return static_cast<LowerThird *>(data)->cy;
}

void lt_tick(void *data, float seconds)
{
	auto *lt = static_cast<LowerThird *>(data);

	bool settingsChanged = false;
	obs_video_info ovi;
	if (obs_get_video_info(&ovi) && (ovi.base_width != lt->cx || ovi.base_height != lt->cy)) {
		lt->cx = ovi.base_width;
		lt->cy = ovi.base_height;
		settingsChanged = true;
	}
	{
		std::lock_guard<std::mutex> lock(lt->settingsMutex);
		if (lt->settingsChanged) {
			lt->s = lt->pending;
			lt->settingsChanged = false;
			settingsChanged = true;
		}
	}

	uint64_t ver = 0;
	DisplayContent c = DisplayState::get(lt->channel).snapshot(&ver);
	const bool empty = c.entries.empty() && c.reference.isEmpty();
	const bool useSample = lt->s.sample && empty;
	if (useSample)
		c = (lt->channel == Channel::Lyrics) ? sampleLyricsContent() : sampleContent();

	const bool textChanged = !lt->hasContent || !c.sameText(lt->shown) || useSample != lt->showingSample;
	if ((settingsChanged || (ver != lt->seenVersion && textChanged) || useSample != lt->showingSample) &&
	    !(c.entries.empty() && c.reference.isEmpty())) {
		if (!settingsChanged && lt->alpha > 0.01f && lt->hasContent && textChanged && lt->s.fadeMs > 0) {
			lt->prevImg = lt->curImg;
			lt->prevPos = lt->curPos;
			lt->prevDirty = true;
			lt->cross = 0.0f;
		} else {
			lt->cross = 1.0f;
			lt->dropPrev = true;
		}
		lt->curImg = renderCard(c, lt->s, lt->cx, lt->cy, &lt->curPos);
		lt->curDirty = true;
		lt->hasContent = true;
		lt->shown = c;
	}
	lt->showingSample = useSample;
	lt->seenVersion = ver;

	const float target = (c.visible && lt->hasContent) ? 1.0f : 0.0f;
	const float step = lt->s.fadeMs > 0 ? seconds * 1000.0f / static_cast<float>(lt->s.fadeMs) : 1.0f;
	if (lt->alpha < target)
		lt->alpha = std::min(target, lt->alpha + step);
	else if (lt->alpha > target)
		lt->alpha = std::max(target, lt->alpha - step);
	if (lt->cross < 1.0f) {
		lt->cross = std::min(1.0f, lt->cross + step);
		if (lt->cross >= 1.0f)
			lt->dropPrev = true;
	}
}

gs_texture_t *uploadTexture(gs_texture_t *old, const QImage &img)
{
	if (old)
		gs_texture_destroy(old);
	if (img.isNull())
		return nullptr;
	const uint8_t *bits = img.constBits();
	return gs_texture_create(static_cast<uint32_t>(img.width()), static_cast<uint32_t>(img.height()), GS_RGBA, 1,
				 &bits, 0);
}

void drawTex(LowerThird *lt, gs_texture_t *tex, const QPointF &pos, float opacity, float yOffset)
{
	if (!tex || opacity <= 0.001f)
		return;
	gs_effect_t *effect = lt->effect ? lt->effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	if (lt->effect)
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "opacity"), opacity);
	gs_matrix_push();
	gs_matrix_translate3f(static_cast<float>(pos.x()), static_cast<float>(pos.y()) + yOffset, 0.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, 0, 0);
	gs_matrix_pop();
}

void lt_render(void *data, gs_effect_t *)
{
	auto *lt = static_cast<LowerThird *>(data);
	if (lt->curDirty) {
		lt->curTex = uploadTexture(lt->curTex, lt->curImg);
		lt->curDirty = false;
	}
	if (lt->prevDirty) {
		lt->prevTex = uploadTexture(lt->prevTex, lt->prevImg);
		lt->prevDirty = false;
		lt->dropPrev = false;
	}
	if (lt->dropPrev) {
		if (lt->prevTex)
			gs_texture_destroy(lt->prevTex);
		lt->prevTex = nullptr;
		lt->dropPrev = false;
	}
	if (lt->alpha <= 0.001f || !lt->curTex)
		return;

	const float scale = static_cast<float>(lt->cy) / 1080.0f;
	const float eased = 1.0f - (1.0f - lt->alpha) * (1.0f - lt->alpha);
	const float slide = (1.0f - eased) * 22.0f * scale * (lt->s.top ? -1.0f : 1.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	if (lt->prevTex && lt->cross < 1.0f) {
		const float inOp = std::min(1.0f, lt->cross * 2.0f);
		const float outOp = std::min(1.0f, (1.0f - lt->cross) * 2.0f);
		drawTex(lt, lt->prevTex, lt->prevPos, lt->alpha * outOp, slide);
		drawTex(lt, lt->curTex, lt->curPos, lt->alpha * inOp, slide);
	} else {
		drawTex(lt, lt->curTex, lt->curPos, lt->alpha, slide);
	}
	gs_blend_state_pop();
}

} // namespace

void register_lower_third_source()
{
	obs_source_info info = {};
	info.id = "ld_scripture_lower_third";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = lt_get_name;
	info.create = lt_create;
	info.destroy = lt_destroy;
	info.update = lt_update;
	info.get_defaults = lt_defaults;
	info.get_properties = lt_properties;
	info.get_width = lt_width;
	info.get_height = lt_height;
	info.video_tick = lt_tick;
	info.video_render = lt_render;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	obs_register_source(&info);
}

void register_lyrics_lower_third_source()
{
	obs_source_info info = {};
	info.id = "ld_lyrics_lower_third";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = lyrics_lt_get_name;
	info.create = lyrics_lt_create;
	info.destroy = lt_destroy;
	info.update = lyrics_lt_update;
	info.get_defaults = lyrics_lt_defaults;
	info.get_properties = lyrics_lt_properties;
	info.get_width = lt_width;
	info.get_height = lt_height;
	info.video_tick = lt_tick;
	info.video_render = lt_render;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	obs_register_source(&info);
}
