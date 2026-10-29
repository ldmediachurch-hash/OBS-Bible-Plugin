/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "display-state.hpp"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QWidget>

class MiniProgramThumbnail : public QWidget {
	Q_OBJECT

public:
	explicit MiniProgramThumbnail(QWidget *parent = nullptr);
	~MiniProgramThumbnail() override = default;

	void updateContent(const DisplayContent &content, bool isLive);
	void setLive(bool isLive);

	QSize sizeHint() const override;
	int heightForWidth(int width) const override;
	bool hasHeightForWidth() const override { return true; }

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	DisplayContent m_content;
	bool m_isLive = false;
};
