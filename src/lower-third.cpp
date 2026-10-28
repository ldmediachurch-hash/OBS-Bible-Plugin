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

namespace {

struct LtSettings {
	bool isLyrics = false;
	bool dark = false;
	bool top = false;
	bool stacked = false;
	bool chips = true;
	bool sample = false;
	int fontSize = 40;
	int maxLines = 3;
	int marginX = 96;
	int marginY = 64;
	int fadeMs = 300;
	bool centerAlign = false;
	bool showCcli = false;
};

struct Palette {
	QColor card, text, kicker, chipBg, chipText, divider, shadow;
};

Palette paletteFor(const LtSettings &s)
{
	if (s.dark)
		return {QColor(15, 17, 21, 240), QColor(255, 255, 255),
			QColor(163, 169, 178),   QColor(255, 255, 255, 30),
			QColor(230, 232, 235),   QColor(255, 255, 255, 40),
			QColor(0, 0, 0)};
	return {QColor(255, 255, 255), QColor(15, 17, 21),    QColor(106, 112, 122), QColor(241, 242, 244),
		QColor(62, 68, 77),    QColor(227, 229, 232), QColor(6, 10, 26)};
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
		line.setPosition(QPointF(lineX, y + glyphOffset));
		y += lineHeight;
		l.lines++;
	}
	l.layout->endLayout();
	l.height = y;
	return l;
}

QFont makeFont(const char *family, int weight, qreal px)
{
	QFont f(QString::fromUtf8(family));
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

Chip makeChip(const QString &text, qreal scale)
{
	Chip c;
	c.text = text;
	c.font = makeFont("Inter", 600, 15 * scale);
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

	// kicker: red dot + REFERENCE (+ chip when there is one translation or CCLI for lyrics)
	QFont kicker = makeFont("Inter", 600, 19 * scale);
	kicker.setLetterSpacing(QFont::AbsoluteSpacing, 0.16 * 19 * scale);
	QFontMetricsF kfm(kicker);
	const qreal kickerH = 28 * scale;

	// fit the text: shrink up to 30% before letting the card grow
	const qreal basePx = s.fontSize * scale;
	qreal px = basePx;
	std::vector<Laid> laid;
	const qreal chipBlock = (!s.isLyrics && s.chips && n > 1) ? 28 * scale + 12 * scale : 0;
	const Qt::Alignment bodyAlign = (s.isLyrics && s.centerAlign) ? Qt::AlignHCenter : Qt::AlignLeft;

	for (;;) {
		laid.clear();
		QFont body = makeFont("Inter", 500, px);
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
	const qreal cardH = padTop + kickerH + 14 * scale + contentH + padBottom;

	const int imgW = static_cast<int>(std::ceil(cardW + 2 * shadowPad));
	const int imgH = static_cast<int>(std::ceil(cardH + 2 * shadowPad));
	QImage img(imgW, imgH, QImage::Format_RGBA8888_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);

	const QRectF card(shadowPad, shadowPad, cardW, cardH);
	const qreal radius = 26 * scale;

	// soft shadow: layered rounded rects, offset downwards
	const int layers = 22;
	for (int i = layers; i >= 1; i--) {
		const qreal spread = i * (shadowPad * 0.85 / layers);
		QColor c = pal.shadow;
		const qreal t = 1.0 - static_cast<qreal>(i) / layers;
		c.setAlphaF(static_cast<float>((s.dark ? 0.05 : 0.028) * t * t + 0.002));
		QPainterPath sp;
		sp.addRoundedRect(card.adjusted(-spread, -spread + 14 * scale, spread, spread + 14 * scale),
				  radius + spread, radius + spread);
		p.fillPath(sp, c);
	}
	QPainterPath cardPath;
	cardPath.addRoundedRect(card, radius, radius);
	p.fillPath(cardPath, pal.card);

	// kicker row
	qreal x = card.left() + padX;
	qreal y = card.top() + padTop;
	const qreal dot = 12 * scale;
	p.setPen(Qt::NoPen);
	p.setBrush(kSignalRed);
	p.drawEllipse(QRectF(x, y + (kickerH - dot) / 2, dot, dot));
	p.setBrush(Qt::NoBrush);
	p.setFont(kicker);
	p.setPen(pal.kicker);
	const QString ref = content.reference.toUpper();
	const qreal refX = x + dot + 12 * scale;

	if (s.isLyrics) {
		qreal availW = innerW - (dot + 12 * scale);
		if (s.showCcli && !content.ccli.trimmed().isEmpty()) {
			Chip c = makeChip(QStringLiteral("CCLI %1").arg(content.ccli.trimmed()), scale);
			const qreal chipX = card.right() - padX - c.w;
			drawChip(p, c, chipX, y + (kickerH - c.h) / 2, pal);
			availW = std::max<qreal>(0.0, chipX - 14 * scale - refX);
		}
		p.drawText(QRectF(refX, y, availW, kickerH), Qt::AlignVCenter | Qt::AlignLeft, ref);
	} else {
		p.drawText(QRectF(refX, y, innerW, kickerH), Qt::AlignVCenter | Qt::AlignLeft, ref);
		if (s.chips && n == 1 && !content.entries[0].label.isEmpty()) {
			Chip c = makeChip(content.entries[0].label, scale);
			drawChip(p, c, refX + kfm.horizontalAdvance(ref) + 14 * scale, y + (kickerH - c.h) / 2, pal);
		}
	}
	y += kickerH + 14 * scale;

	// body text
	for (size_t i = 0; i < laid.size(); i++) {
		qreal cx = x;
		qreal cy = y;
		if (twoCols) {
			cx = x + static_cast<qreal>(i) * (colW + colGap);
		}
		if (chipBlock > 0) {
			Chip c = makeChip(content.entries[i].label, scale);
			drawChip(p, c, cx, cy, pal);
			cy += chipBlock;
		}
		p.setPen(pal.text);
		laid[i].layout->draw(&p, QPointF(cx, cy));
		if (!twoCols)
			y = cy + laid[i].height + 22 * scale;
	}
	if (twoCols) {
		p.setPen(QPen(pal.divider, std::max<qreal>(1.0, 2 * scale)));
		const qreal dx = x + colW + colGap / 2;
		p.drawLine(QPointF(dx, card.top() + padTop + kickerH + 14 * scale),
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

void readScriptureSettings(LtSettings &s, obs_data_t *d)
{
	s.isLyrics = false;
	s.dark = strcmp(obs_data_get_string(d, "theme"), "dark") == 0;
	s.top = strcmp(obs_data_get_string(d, "position"), "top") == 0;
	s.stacked = strcmp(obs_data_get_string(d, "layout"), "stacked") == 0;
	s.chips = obs_data_get_bool(d, "chips");
	s.sample = obs_data_get_bool(d, "sample");
	s.fontSize = static_cast<int>(obs_data_get_int(d, "font_size"));
	s.maxLines = static_cast<int>(obs_data_get_int(d, "max_lines"));
	s.marginX = static_cast<int>(obs_data_get_int(d, "margin_x"));
	s.marginY = static_cast<int>(obs_data_get_int(d, "margin_y"));
	s.fadeMs = static_cast<int>(obs_data_get_int(d, "fade_ms"));
	s.centerAlign = false;
	s.showCcli = false;
}

void readLyricsSettings(LtSettings &s, obs_data_t *d)
{
	s.isLyrics = true;
	s.dark = strcmp(obs_data_get_string(d, "theme"), "dark") == 0;
	s.top = strcmp(obs_data_get_string(d, "position"), "top") == 0;
	s.stacked = false;
	s.chips = false;
	s.sample = obs_data_get_bool(d, "sample");
	s.fontSize = static_cast<int>(obs_data_get_int(d, "font_size"));
	s.maxLines = static_cast<int>(obs_data_get_int(d, "max_lines"));
	s.marginX = static_cast<int>(obs_data_get_int(d, "margin_x"));
	s.marginY = static_cast<int>(obs_data_get_int(d, "margin_y"));
	s.fadeMs = static_cast<int>(obs_data_get_int(d, "fade_ms"));
	s.centerAlign = strcmp(obs_data_get_string(d, "align"), "center") == 0;
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

void lt_defaults(obs_data_t *d)
{
	obs_data_set_default_string(d, "theme", "light");
	obs_data_set_default_string(d, "position", "bottom");
	obs_data_set_default_string(d, "layout", "side");
	obs_data_set_default_bool(d, "chips", true);
	obs_data_set_default_bool(d, "sample", false);
	obs_data_set_default_int(d, "font_size", 40);
	obs_data_set_default_int(d, "max_lines", 3);
	obs_data_set_default_int(d, "margin_x", 96);
	obs_data_set_default_int(d, "margin_y", 64);
	obs_data_set_default_int(d, "fade_ms", 300);
}

void lyrics_lt_defaults(obs_data_t *d)
{
	obs_data_set_default_string(d, "theme", "light");
	obs_data_set_default_string(d, "position", "bottom");
	obs_data_set_default_string(d, "align", "left");
	obs_data_set_default_int(d, "font_size", 44);
	obs_data_set_default_int(d, "max_lines", 3);
	obs_data_set_default_int(d, "margin_x", 96);
	obs_data_set_default_int(d, "margin_y", 64);
	obs_data_set_default_bool(d, "show_ccli", false);
	obs_data_set_default_int(d, "fade_ms", 300);
	obs_data_set_default_bool(d, "sample", false);
}

obs_properties_t *lt_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_list(props, "theme", "Style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Light (white card)", "light");
	obs_property_list_add_string(p, "Dark (black card)", "dark");
	p = obs_properties_add_list(props, "position", "Position", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Bottom", "bottom");
	obs_property_list_add_string(p, "Top", "top");
	p = obs_properties_add_list(props, "layout", "Two translations", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Side by side", "side");
	obs_property_list_add_string(p, "Stacked", "stacked");
	obs_properties_add_int_slider(props, "font_size", "Text size (at 1080p)", 24, 72, 1);
	obs_properties_add_int_slider(props, "max_lines", "Lines before the text shrinks", 1, 6, 1);
	obs_properties_add_int(props, "margin_x", "Side margin (px at 1080p)", 0, 600, 4);
	obs_properties_add_int(props, "margin_y", "Edge margin (px at 1080p)", 0, 600, 4);
	obs_properties_add_bool(props, "chips", "Show translation labels");
	obs_properties_add_int_slider(props, "fade_ms", "Fade (ms)", 0, 1500, 50);
	obs_properties_add_bool(props, "sample", "Show a sample verse while setting up (turn off before service)");
	return props;
}

obs_properties_t *lyrics_lt_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_list(props, "theme", "Style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Light (white card)", "light");
	obs_property_list_add_string(p, "Dark (black card)", "dark");
	p = obs_properties_add_list(props, "position", "Position", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Bottom", "bottom");
	obs_property_list_add_string(p, "Top", "top");
	p = obs_properties_add_list(props, "align", "Text alignment", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Left", "left");
	obs_property_list_add_string(p, "Center", "center");
	obs_properties_add_int_slider(props, "font_size", "Text size (at 1080p)", 24, 72, 1);
	obs_properties_add_int_slider(props, "max_lines", "Lines before the text shrinks", 1, 6, 1);
	obs_properties_add_int(props, "margin_x", "Side margin (px at 1080p)", 0, 600, 4);
	obs_properties_add_int(props, "margin_y", "Edge margin (px at 1080p)", 0, 600, 4);
	obs_properties_add_bool(props, "show_ccli", "Show CCLI number");
	obs_properties_add_int_slider(props, "fade_ms", "Fade (ms)", 0, 1500, 50);
	obs_properties_add_bool(props, "sample", "Show a sample lyric while setting up (turn off before service)");
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
		// new card fades in over the first half while the old one stays solid, then the old one fades out,
		// so the card never looks see-through mid-change
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
