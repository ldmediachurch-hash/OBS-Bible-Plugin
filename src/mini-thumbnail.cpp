/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "mini-thumbnail.hpp"

#include <QPainterPath>

MiniProgramThumbnail::MiniProgramThumbnail(QWidget *parent)
	: QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	setMinimumHeight(110);
}

void MiniProgramThumbnail::updateContent(const DisplayContent &content, bool isLive)
{
	m_content = content;
	m_isLive = isLive;
	update();
}

void MiniProgramThumbnail::setLive(bool isLive)
{
	if (m_isLive != isLive) {
		m_isLive = isLive;
		update();
	}
}

QSize MiniProgramThumbnail::sizeHint() const
{
	return QSize(280, 158); // 16:9 aspect ratio
}

int MiniProgramThumbnail::heightForWidth(int width) const
{
	return (width * 9) / 16;
}

void MiniProgramThumbnail::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QRectF r = rect().adjusted(1, 1, -1, -1);

	// 1. Draw 16:9 Video Canvas
	QPainterPath bgPath;
	bgPath.addRoundedRect(r, 6, 6);
	p.fillPath(bgPath, QColor(16, 18, 22)); // #101216 deep canvas

	p.setPen(QPen(QColor(255, 255, 255, 20), 1));
	p.drawPath(bgPath);

	// 2. Draw Top-Left Status Badge
	const int badgeY = static_cast<int>(r.top()) + 10;
	const int badgeX = static_cast<int>(r.left()) + 12;

	QFont badgeFont = font();
	badgeFont.setPointSize(8);
	badgeFont.setBold(true);
	p.setFont(badgeFont);

	if (m_isLive) {
		// Live red dot
		p.setBrush(QColor(255, 45, 45)); // #FF2D2D
		p.setPen(Qt::NoPen);
		p.drawEllipse(QPointF(badgeX + 4, badgeY + 5), 3.5, 3.5);

		// "ON AIR" text
		p.setPen(QColor(255, 45, 45));
		p.drawText(badgeX + 13, badgeY + 10, QStringLiteral("ON AIR"));
	} else {
		// "PREVIEW" text
		p.setPen(QColor(113, 120, 134)); // #717886
		p.drawText(badgeX, badgeY + 10, QStringLiteral("PREVIEW"));
	}

	// 3. Draw Lower Third Graphic
	bool hasEntries = false;
	for (const auto &e : m_content.entries) {
		if (!e.text.trimmed().isEmpty()) {
			hasEntries = true;
			break;
		}
	}

	if (m_content.visible && (hasEntries || !m_content.reference.isEmpty())) {
		const double boxMargin = 12.0;
		const double boxH = std::max(46.0, r.height() * 0.38);
		const double boxW = r.width() - (boxMargin * 2.0);
		const double boxY = r.bottom() - boxH - boxMargin;
		const QRectF boxRect(r.left() + boxMargin, boxY, boxW, boxH);

		// Lower-third card background
		QPainterPath cardPath;
		cardPath.addRoundedRect(boxRect, 5, 5);
		p.fillPath(cardPath, QColor(255, 255, 255, 240));

		// Kicker reference
		if (!m_content.reference.isEmpty()) {
			QFont kickerFont = font();
			kickerFont.setPointSize(7);
			kickerFont.setBold(true);
			kickerFont.setCapitalization(QFont::AllUppercase);
			p.setFont(kickerFont);
			p.setPen(QColor(106, 112, 122)); // slate

			QRectF kickerRect(boxRect.left() + 8, boxRect.top() + 4, boxRect.width() - 16, 12);
			p.drawText(kickerRect, Qt::AlignLeft | Qt::AlignVCenter, m_content.reference);
		}

		// Main text
		QString mainText;
		for (const auto &e : m_content.entries) {
			if (!mainText.isEmpty())
				mainText += QLatin1Char('\n');
			mainText += e.text;
		}

		QFont textFont = font();
		textFont.setPointSize(8);
		textFont.setBold(true);
		p.setFont(textFont);
		p.setPen(QColor(15, 17, 21)); // dark ink

		const double textTop = m_content.reference.isEmpty() ? (boxRect.top() + 6) : (boxRect.top() + 16);
		QRectF textRect(boxRect.left() + 8, textTop, boxRect.width() - 16, boxRect.bottom() - textTop - 4);
		p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, mainText);
	} else {
		// Empty placeholder state
		QFont emptyFont = font();
		emptyFont.setPointSize(9);
		p.setFont(emptyFont);
		p.setPen(QColor(74, 81, 96));
		p.drawText(r, Qt::AlignCenter, QStringLiteral("No active slide"));
	}
}
