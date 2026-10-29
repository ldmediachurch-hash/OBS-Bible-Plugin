/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "lyrics-dock.hpp"
#include "audio-engine.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <util/platform.h>

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Roles {
	RoleSlideIndex = Qt::UserRole + 1,
	RoleSongPath,
	RoleSectionCode,
	RoleSectionName,
	RoleIsFirstOfSection,
	RoleIsLive,
	RoleIsNext
};

QString configPath()
{
	char *p = obs_module_config_path("lyrics-dock.json");
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

void ensureConfigDir()
{
	char *p = obs_module_config_path("");
	os_mkdirs(p);
	bfree(p);
}

template<const char *Slot> void hotkeyCallback(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	QMetaObject::invokeMethod(static_cast<LyricsDock *>(data), Slot, Qt::QueuedConnection);
}

const char kNext[] = "next";
const char kPrev[] = "prev";
const char kLive[] = "goLive";
const char kClear[] = "clearLive";

// Delegate for Slide Cards (Mockup fidelity)
class SlideCardDelegate : public QStyledItemDelegate {
public:
	explicit SlideCardDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

	void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		p->save();
		p->setRenderHint(QPainter::Antialiasing, true);

		QRect r = option.rect.adjusted(4, 4, -4, -4);
		bool isSelected = option.state & QStyle::State_Selected;
		bool isLive = index.data(RoleIsLive).toBool();
		bool isNext = index.data(RoleIsNext).toBool();
		bool isFirst = index.data(RoleIsFirstOfSection).toBool();
		QString sectionName = index.data(RoleSectionName).toString().toUpper();
		QString text = index.data(Qt::DisplayRole).toString();

		// Background
		QPainterPath path;
		path.addRoundedRect(r, 6, 6);
		if (isSelected) {
			p->fillPath(path, QColor(255, 255, 255, 18));
		} else {
			p->fillPath(path, QColor(255, 255, 255, 8));
		}

		// Border outline
		if (isLive) {
			// Solid white border
			p->setPen(QPen(QColor(255, 255, 255), 2.0));
			p->drawPath(path);
		} else if (isNext) {
			// Dashed border
			QPen dashPen(QColor(255, 255, 255, 110), 1.5, Qt::DashLine);
			p->setPen(dashPen);
			p->drawPath(path);
		} else {
			p->setPen(QPen(QColor(255, 255, 255, 20), 1.0));
			p->drawPath(path);
		}

		int contentTop = r.top() + 6;

		// Section header on first slide of each section
		if (isFirst && !sectionName.isEmpty()) {
			QFont secFont = option.font;
			secFont.setPointSize(8);
			secFont.setBold(true);
			secFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
			p->setFont(secFont);
			p->setPen(QColor(140, 147, 160));
			p->drawText(r.left() + 10, contentTop + 10, sectionName);
		}

		// Live badge on top right
		if (isLive) {
			const int dotX = r.right() - 48;
			const int dotY = contentTop + 6;
			p->setPen(Qt::NoPen);
			p->setBrush(QColor(255, 45, 45)); // #FF2D2D
			p->drawEllipse(QPointF(dotX, dotY), 3.5, 3.5);

			QFont liveFont = option.font;
			liveFont.setPointSize(8);
			liveFont.setBold(true);
			p->setFont(liveFont);
			p->setPen(QColor(255, 45, 45));
			p->drawText(dotX + 7, dotY + 4, QStringLiteral("LIVE"));
		}

		// Slide text lines
		QFont textFont = option.font;
		textFont.setPointSize(9);
		p->setFont(textFont);
		p->setPen(QColor(240, 243, 246));

		int textTop = isFirst ? (contentTop + 20) : (contentTop + 6);
		QRect textRect(r.left() + 10, textTop, r.width() - 20, r.bottom() - textTop - 6);
		p->drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);

		p->restore();
	}

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		bool isFirst = index.data(RoleIsFirstOfSection).toBool();
		int baseH = isFirst ? 68 : 52;
		return QSize(option.rect.width(), baseH);
	}
};

// Delegate for Song Library rows
class SongListDelegate : public QStyledItemDelegate {
public:
	explicit SongListDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

	void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		p->save();
		p->setRenderHint(QPainter::Antialiasing, true);

		QRect r = option.rect.adjusted(2, 2, -2, -2);
		bool isSelected = option.state & QStyle::State_Selected;

		if (isSelected) {
			QPainterPath path;
			path.addRoundedRect(r, 4, 4);
			p->fillPath(path, QColor(255, 255, 255, 16));

			// 2px white left bar
			p->fillRect(QRect(r.left(), r.top() + 4, 2, r.height() - 8), Qt::white);
		}

		QString title = index.data(Qt::DisplayRole).toString();
		QString author = index.data(Qt::UserRole + 10).toString();

		QFont titleFont = option.font;
		titleFont.setBold(isSelected);
		p->setFont(titleFont);
		p->setPen(isSelected ? Qt::white : QColor(220, 224, 230));

		QFontMetrics fmTitle(titleFont);
		QFont authorFont = option.font;
		authorFont.setPointSize(std::max(7, authorFont.pointSize() - 1));
		QFontMetrics fmAuthor(authorFont);

		const int margin = 8;
		const int titleW = static_cast<int>(r.width() * 0.65) - margin;
		int authorW = 0;
		if (!author.isEmpty()) {
			authorW = std::min(fmAuthor.horizontalAdvance(author) + 4, static_cast<int>(r.width() * 0.35));
		}
		int titleW = r.width() - margin * 2 - (authorW > 0 ? (authorW + 8) : 0);
		QString elidedTitle = fmTitle.elidedText(title, Qt::ElideRight, titleW);
		QRect titleRect(r.left() + margin, r.top(), titleW, r.height());
		p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);
		p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

		if (!author.isEmpty()) {
			QFont authorFont = option.font;
			authorFont.setPointSize(authorFont.pointSize() - 1);
			if (authorW > 0) {
				p->setFont(authorFont);
				p->setPen(QColor(130, 137, 150));
				const int authorX = r.left() + static_cast<int>(r.width() * 0.65);
				const int authorW = static_cast<int>(r.width() * 0.35) - margin;
				QRect authorRect(authorX, r.top(), authorW, r.height());
				p->drawText(authorRect, Qt::AlignRight | Qt::AlignVCenter, author);
				QString elidedAuthor = fmAuthor.elidedText(author, Qt::ElideRight, authorW);
				QRect authorRect(r.right() - margin - authorW, r.top(), authorW, r.height());
				p->drawText(authorRect, Qt::AlignRight | Qt::AlignVCenter, elidedAuthor);
			}

			p->restore();
		}

		QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
		{
			return QSize(option.rect.width(), 32);
		}
	};

	// Delegate for Setlist with numbered rows
	class SetlistDelegate : public QStyledItemDelegate {
	public:
		explicit SetlistDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

		void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
		{
			p->save();
			p->setRenderHint(QPainter::Antialiasing, true);

			QRect r = option.rect.adjusted(2, 2, -2, -2);
			bool isSelected = option.state & QStyle::State_Selected;

			if (isSelected) {
				QPainterPath path;
				path.addRoundedRect(r, 4, 4);
				p->fillPath(path, QColor(255, 255, 255, 16));
				p->fillRect(QRect(r.left(), r.top() + 4, 2, r.height() - 8), Qt::white);
			}

			QString numStr = QString::number(index.row() + 1);
			QString title = index.data(Qt::DisplayRole).toString();

			// Row number in muted gray
			QFont numFont = option.font;
			numFont.setPointSize(numFont.pointSize() - 1);
			p->setFont(numFont);
			p->setPen(QColor(130, 137, 150));
			QRect numRect(r.left() + 6, r.top(), 22, r.height());
			p->drawText(numRect, Qt::AlignLeft | Qt::AlignVCenter, numStr);

			// Song title
			QFont titleFont = option.font;
			titleFont.setBold(isSelected);
			p->setFont(titleFont);
			p->setPen(isSelected ? Qt::white : QColor(220, 224, 230));
			QRect titleRect(r.left() + 32, r.top(), r.width() - 38, r.height());
			p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);

			p->restore();
		}

		QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
		{
			return QSize(option.rect.width(), 32);
		}
	};

} // namespace

LyricsDock::LyricsDock(QWidget *parent)
	: QWidget(parent)
{
	setMinimumWidth(300);
	setupLayout();
	registerHotkeys();
	refreshSongList();
	loadConfig();
}

LyricsDock::~LyricsDock()
{
	saveConfig();
	if (hkNext != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hkNext);
	if (hkPrev != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hkPrev);
	if (hkLive != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hkLive);
	if (hkClear != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hkClear);
}

void LyricsDock::setupLayout()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	// 1. Search + Gear Row
	auto *searchRow = new QHBoxLayout();
	searchRow->setSpacing(6);

	searchEdit = new QLineEdit(this);
	searchEdit->setPlaceholderText(QStringLiteral("Search songs or lyrics"));
	searchEdit->setClearButtonEnabled(true);
	searchRow->addWidget(searchEdit, 1);

	gearBtn = new QToolButton(this);
	gearBtn->setIcon(QIcon(QStringLiteral(":/icons/gear.svg")));
	gearBtn->setToolTip(QStringLiteral("Settings & Model Options"));
	gearBtn->setFixedSize(30, 30);
	connect(gearBtn, &QToolButton::clicked, this, &LyricsDock::openGearMenu);
	searchRow->addWidget(gearBtn);

	root->addLayout(searchRow);

	// 2. Vertical scroll area for all content
	scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

	scrollContent = new QWidget(scrollArea);
	contentLayout = new QVBoxLayout(scrollContent);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->setSpacing(8);

	QFont hFont = font();
	hFont.setPointSize(8);
	hFont.setBold(true);
	hFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);

	// --- Library Section ---
	auto *libHeaderRow = new QHBoxLayout();
	libraryHeaderLabel = new QLabel(QStringLiteral("LIBRARY · 0"), scrollContent);
	libraryHeaderLabel->setFont(hFont);
	libraryHeaderLabel->setStyleSheet(QStringLiteral("color: #8C93A0;"));
	libHeaderRow->addWidget(libraryHeaderLabel, 1);

	importBtn = new QToolButton(scrollContent);
	importBtn->setText(QStringLiteral("Import ▾"));
	importBtn->setPopupMode(QToolButton::InstantPopup);
	auto *importMenu = new QMenu(importBtn);
	importMenu->addAction(QStringLiteral("Import Songs (OpenLyrics/OpenSong/JSON/Text)…"), this,
			      &LyricsDock::importSongs);
	importMenu->addAction(QStringLiteral("New Song…"), this, &LyricsDock::newSongDialog);
	importMenu->addAction(QStringLiteral("Edit Current Song…"), this, &LyricsDock::editSongDialog);
	importMenu->addSeparator();
	importMenu->addAction(QStringLiteral("Open Songs Folder"), this, &LyricsDock::openSongsFolder);
	importBtn->setMenu(importMenu);
	libHeaderRow->addWidget(importBtn);

	contentLayout->addLayout(libHeaderRow);

	songList = new QListWidget(scrollContent);
	songList->setItemDelegate(new SongListDelegate(songList));
	songList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	songList->setMinimumHeight(84);
	songList->setMaximumHeight(130);
	contentLayout->addWidget(songList);

	// --- Slides Section ---
	auto *slidesHeaderRow = new QHBoxLayout();
	slidesHeaderLabel = new QLabel(QStringLiteral("SLIDES"), scrollContent);
	slidesHeaderLabel->setFont(hFont);
	slidesHeaderLabel->setStyleSheet(QStringLiteral("color: #8C93A0;"));
	slidesHeaderRow->addWidget(slidesHeaderLabel, 1);

	linesBtn = new QToolButton(scrollContent);
	linesBtn->setText(QStringLiteral("2 lines ▾"));
	linesBtn->setPopupMode(QToolButton::InstantPopup);
	auto *linesMenu = new QMenu(linesBtn);
	auto *act1 = linesMenu->addAction(QStringLiteral("1 line"));
	auto *act2 = linesMenu->addAction(QStringLiteral("2 lines"));
	auto *act3 = linesMenu->addAction(QStringLiteral("3 lines"));
	auto *act4 = linesMenu->addAction(QStringLiteral("4 lines"));
	connect(act1, &QAction::triggered, this, [this]() { setLinesPerSlide(1); });
	connect(act2, &QAction::triggered, this, [this]() { setLinesPerSlide(2); });
	connect(act3, &QAction::triggered, this, [this]() { setLinesPerSlide(3); });
	connect(act4, &QAction::triggered, this, [this]() { setLinesPerSlide(4); });
	linesBtn->setMenu(linesMenu);
	slidesHeaderRow->addWidget(linesBtn);
	contentLayout->addLayout(slidesHeaderRow);

	// Section Chips
	sectionContainer = new QWidget(scrollContent);
	sectionLayout = new QHBoxLayout(sectionContainer);
	sectionLayout->setContentsMargins(0, 0, 0, 0);
	sectionLayout->setSpacing(4);
	contentLayout->addWidget(sectionContainer);

	// Slide Cards
	slideList = new QListWidget(scrollContent);
	slideList->setItemDelegate(new SlideCardDelegate(slideList));
	slideList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	slideList->setMinimumHeight(150);
	contentLayout->addWidget(slideList);

	// 16:9 Mini Program Thumbnail
	thumbnail = new MiniProgramThumbnail(scrollContent);
	contentLayout->addWidget(thumbnail);

	// --- Setlist Section ---
	auto *setlistHeaderRow = new QHBoxLayout();
	setlistHeaderLabel = new QLabel(QStringLiteral("SETLIST · 0"), scrollContent);
	setlistHeaderLabel->setFont(hFont);
	setlistHeaderLabel->setStyleSheet(QStringLiteral("color: #8C93A0;"));
	setlistHeaderRow->addWidget(setlistHeaderLabel, 1);

	setlistAddBtn = new QToolButton(scrollContent);
	setlistAddBtn->setIcon(QIcon(QStringLiteral(":/icons/plus.svg")));
	setlistAddBtn->setToolTip(QStringLiteral("Add current song to setlist"));
	connect(setlistAddBtn, &QToolButton::clicked, this, &LyricsDock::addToList);
	setlistHeaderRow->addWidget(setlistAddBtn);

	setlistRemBtn = new QToolButton(scrollContent);
	setlistRemBtn->setIcon(QIcon(QStringLiteral(":/icons/minus.svg")));
	setlistRemBtn->setToolTip(QStringLiteral("Remove selected song from setlist"));
	connect(setlistRemBtn, &QToolButton::clicked, this, &LyricsDock::removeFromList);
	setlistHeaderRow->addWidget(setlistRemBtn);

	setlistUpBtn = new QToolButton(scrollContent);
	setlistUpBtn->setIcon(QIcon(QStringLiteral(":/icons/prev.svg")));
	setlistUpBtn->setToolTip(QStringLiteral("Move up in setlist"));
	connect(setlistUpBtn, &QToolButton::clicked, this, &LyricsDock::moveListItemUp);
	setlistHeaderRow->addWidget(setlistUpBtn);

	setlistDownBtn = new QToolButton(scrollContent);
	setlistDownBtn->setIcon(QIcon(QStringLiteral(":/icons/next.svg")));
	setlistDownBtn->setToolTip(QStringLiteral("Move down in setlist"));
	connect(setlistDownBtn, &QToolButton::clicked, this, &LyricsDock::moveListItemDown);
	setlistHeaderRow->addWidget(setlistDownBtn);

	contentLayout->addLayout(setlistHeaderRow);

	setlist = new QListWidget(scrollContent);
	setlist->setItemDelegate(new SetlistDelegate(setlist));
	setlist->setDragDropMode(QAbstractItemView::InternalMove);
	setlist->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setlist->setMinimumHeight(80);
	setlist->setMaximumHeight(130);
	contentLayout->addWidget(setlist);

	scrollArea->setWidget(scrollContent);
	root->addWidget(scrollArea, 1);

	// 3. Sticky Bottom Control Bar (pinned to bottom of dock)
	auto *ctlRow = new QHBoxLayout();
	ctlRow->setSpacing(6);

	prevBtn = new QPushButton(QStringLiteral("◀"), this);
	prevBtn->setToolTip(QStringLiteral("Previous Slide (Shift+Space)"));
	prevBtn->setFixedHeight(34);
	prevBtn->setMaximumWidth(40);
	connect(prevBtn, &QPushButton::clicked, this, &LyricsDock::prev);
	ctlRow->addWidget(prevBtn);

	liveButton = new QPushButton(QStringLiteral("On screen"), this);
	liveButton->setObjectName(QStringLiteral("PrimaryShowBtn"));
	liveButton->setStyleSheet(QStringLiteral(
		"QPushButton#PrimaryShowBtn { background-color: #FFFFFF; color: #0F1115; font-weight: bold; border-radius: 6px; padding: 6px 14px; border: none; font-size: 13px; } "
		"QPushButton#PrimaryShowBtn:hover { background-color: #E2E8F0; } "
		"QPushButton#PrimaryShowBtn:pressed { background-color: #CBD5E1; }"));
	liveButton->setToolTip(QStringLiteral("Show selected slide on screen (Enter)"));
	liveButton->setFixedHeight(34);
	connect(liveButton, &QPushButton::clicked, this, &LyricsDock::goLive);
	ctlRow->addWidget(liveButton, 2);

	blankButton = new QPushButton(QStringLiteral("Blank"), this);
	blankButton->setCheckable(true);
	blankButton->setToolTip(QStringLiteral("Keep live state but hide card for instrumental breaks (B)"));
	blankButton->setFixedHeight(34);
	connect(blankButton, &QPushButton::clicked, this, &LyricsDock::toggleBlank);
	ctlRow->addWidget(blankButton, 1);

	clearButton = new QPushButton(QStringLiteral("Clear"), this);
	clearButton->setToolTip(QStringLiteral("Take lyrics off screen (Esc)"));
	clearButton->setFixedHeight(34);
	connect(clearButton, &QPushButton::clicked, this, &LyricsDock::clearLive);
	ctlRow->addWidget(clearButton, 1);

	nextBtn = new QPushButton(QStringLiteral("▶"), this);
	nextBtn->setToolTip(QStringLiteral("Next Slide (Space)"));
	nextBtn->setFixedHeight(34);
	nextBtn->setMaximumWidth(40);
	connect(nextBtn, &QPushButton::clicked, this, &LyricsDock::next);
	ctlRow->addWidget(nextBtn);

	root->addLayout(ctlRow);

	// Keyboard shortcut hints row
	shortcutHintLabel =
		new QLabel(QStringLiteral("Space next · ⇧Space back · Enter show · B blank · Esc clear"), this);
	QFont hintFont = font();
	hintFont.setPointSize(8);
	shortcutHintLabel->setFont(hintFont);
	shortcutHintLabel->setAlignment(Qt::AlignCenter);
	shortcutHintLabel->setStyleSheet(QStringLiteral("color: #717886;"));
	root->addWidget(shortcutHintLabel);

	// Connect signals
	connect(searchEdit, &QLineEdit::textChanged, this, &LyricsDock::searchTextChanged);
	connect(searchEdit, &QLineEdit::returnPressed, this, &LyricsDock::runSearch);
	connect(songList, &QListWidget::itemDoubleClicked, this, &LyricsDock::songSelected);
	connect(slideList, &QListWidget::currentRowChanged, this, &LyricsDock::slideSelectionChanged);
	connect(setlist, &QListWidget::itemDoubleClicked, this, &LyricsDock::openListItem);
}

void LyricsDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
}

void LyricsDock::keyPressEvent(QKeyEvent *event)
{
	switch (event->key()) {
	case Qt::Key_Space:
		if (event->modifiers() & Qt::ShiftModifier) {
			prev();
		} else {
			next();
		}
		event->accept();
		return;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		goLive();
		event->accept();
		return;
	case Qt::Key_B:
		toggleBlank();
		event->accept();
		return;
	case Qt::Key_Escape:
		clearLive();
		event->accept();
		return;
	default:
		break;
	}
	QWidget::keyPressEvent(event);
}

void LyricsDock::openGearMenu()
{
	auto *menu = new QMenu(this);

	// Lines per slide
	auto *linesSub = menu->addMenu(QStringLiteral("Lines per slide"));
	for (int l = 1; l <= 4; ++l) {
		auto *act = linesSub->addAction(QStringLiteral("%1 line%2").arg(l).arg(l > 1 ? "s" : ""));
		act->setCheckable(true);
		act->setChecked(linesPerSlide == l);
		connect(act, &QAction::triggered, this, [this, l]() { setLinesPerSlide(l); });
	}

	// Model Selection
	auto *modelSub = menu->addMenu(QStringLiteral("Speech model"));
	auto *actBase = modelSub->addAction(QStringLiteral("base.en-q5_1 (Recommended)"));
	auto *actTiny = modelSub->addAction(QStringLiteral("tiny.en (Fast / Low CPU)"));
	auto *actCustom = modelSub->addAction(QStringLiteral("Browse model file…"));

	connect(actBase, &QAction::triggered, this, []() {
		AudioEngine::instance().core().loadModel(QStringLiteral("data/models/ggml-base.en-q5_1.bin"));
	});
	connect(actTiny, &QAction::triggered, this,
		[]() { AudioEngine::instance().core().loadModel(QStringLiteral("data/models/ggml-tiny.en.bin")); });
	connect(actCustom, &QAction::triggered, this, &LyricsDock::selectCustomModel);

	// Hardware backend info
	QString backend = AudioEngine::instance().core().backendInfo();
	if (backend.isEmpty())
		backend = QStringLiteral("CPU");
	auto *actBackend = menu->addAction(QStringLiteral("Backend: %1").arg(backend));
	actBackend->setEnabled(false);

	menu->addSeparator();

	// Follow Mode Checkbox
	auto *actFollow = menu->addAction(QStringLiteral("Follow mode (smart setlist)"));
	actFollow->setCheckable(true);
	actFollow->setChecked(AudioEngine::instance().core().followMode());
	connect(actFollow, &QAction::toggled, this,
		[](bool checked) { AudioEngine::instance().core().setFollowMode(checked); });

	// Auto-Advance Checkbox
	auto *actAuto = menu->addAction(QStringLiteral("Auto-advance on speech match"));
	actAuto->setCheckable(true);
	actAuto->setChecked(AudioEngine::instance().core().autoAdvance());
	connect(actAuto, &QAction::toggled, this,
		[](bool checked) { AudioEngine::instance().core().setAutoAdvance(checked); });

	menu->exec(gearBtn->mapToGlobal(QPoint(0, gearBtn->height())));
}

void LyricsDock::selectCustomModel()
{
	QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select Whisper Model"), QString(),
						    QStringLiteral("GGML Models (*.bin);;All Files (*)"));
	if (!path.isEmpty()) {
		AudioEngine::instance().loadModelFile(path);
	}
}

void LyricsDock::setLinesPerSlide(int lines)
{
	if (lines < 1 || lines > 4)
		return;
	linesPerSlide = lines;
	linesBtn->setText(QStringLiteral("%1 lines ▾").arg(lines));
	rebuildSlides();
}

void LyricsDock::refreshSongList(const QString &filter)
{
	songList->clear();
	auto songs = SongLibrary::instance().songs();

	int count = 0;
	for (const auto &s : songs) {
		if (!filter.isEmpty() && !s.title.contains(filter, Qt::CaseInsensitive) &&
		    !s.author.contains(filter, Qt::CaseInsensitive)) {
			continue;
		}

		auto *item = new QListWidgetItem();
		item->setText(s.title);
		item->setData(RoleSongPath, s.filePath);
		item->setData(Qt::UserRole + 10, s.author);
		songList->addItem(item);
		count++;
	}

	libraryHeaderLabel->setText(QStringLiteral("LIBRARY · %1").arg(count));
}

void LyricsDock::searchTextChanged(const QString &text)
{
	refreshSongList(text.trimmed());
}

void LyricsDock::runSearch()
{
	if (songList->count() > 0) {
		songList->setCurrentRow(0);
		songSelected(songList->item(0));
	}
}

void LyricsDock::songSelected(QListWidgetItem *item)
{
	if (!item)
		return;
	QString title = item->text();
	openSong(title, false);
}

bool LyricsDock::openSong(const QString &titleOrPath, bool fromList)
{
	Song s = SongLibrary::instance().songByTitle(titleOrPath);
	if (!s.isValid()) {
		s = SongLibrary::instance().songByPath(titleOrPath);
	}
	if (!s.isValid())
		return false;

	currentSong = s;
	rebuildSlides();
	rebuildSectionButtons();

	if (slideList->count() > 0) {
		slideList->setCurrentRow(0);
	}

	// Update active song in detection core for Follow Mode
	QString nextSongTitle;
	if (fromList && listIndex >= 0 && listIndex + 1 < setlist->count()) {
		nextSongTitle = setlist->item(listIndex + 1)->text();
	}
	AudioEngine::instance().core().setActiveSong(currentSong.title, currentSlideIndex, nextSongTitle);

	return true;
}

void LyricsDock::rebuildSlides()
{
	slideList->clear();
	if (!currentSong.isValid())
		return;

	currentSlides = generateSlides(currentSong, linesPerSlide);
	QString lastSec;

	for (size_t i = 0; i < currentSlides.size(); ++i) {
		const auto &slide = currentSlides[i];
		bool isFirst = (slide.sectionName != lastSec);
		lastSec = slide.sectionName;

		auto *item = new QListWidgetItem();
		item->setText(slide.lines.join(QLatin1Char('\n')));
		item->setData(RoleSlideIndex, static_cast<int>(i));
		item->setData(RoleSectionCode, slide.sectionCode);
		item->setData(RoleSectionName, slide.sectionName);
		item->setData(RoleIsFirstOfSection, isFirst);
		item->setData(RoleIsLive, false);
		item->setData(RoleIsNext, (static_cast<int>(i) == currentSlideIndex + 1));

		slideList->addItem(item);
	}
}

void LyricsDock::rebuildSectionButtons()
{
	QLayoutItem *child;
	while ((child = sectionLayout->takeAt(0)) != nullptr) {
		delete child->widget();
		delete child;
	}

	if (!currentSong.isValid())
		return;

	QSet<QString> added;
	for (const auto &slide : currentSlides) {
		if (slide.sectionCode.isEmpty() || added.contains(slide.sectionCode))
			continue;
		added.insert(slide.sectionCode);

		auto *btn = new QPushButton(slide.sectionCode, sectionContainer);
		btn->setFixedHeight(24);
		btn->setCursor(Qt::PointingHandCursor);
		btn->setToolTip(slide.sectionName);

		QString code = slide.sectionCode;
		connect(btn, &QPushButton::clicked, this, [this, code]() { jumpToSection(code); });

		sectionLayout->addWidget(btn);
	}
	sectionLayout->addStretch(1);
}

void LyricsDock::jumpToSection(const QString &shortCode)
{
	for (int i = 0; i < slideList->count(); ++i) {
		if (slideList->item(i)->data(RoleSectionCode).toString() == shortCode) {
			slideList->setCurrentRow(i);
			if (live) {
				goLive();
			}
			break;
		}
	}
}

void LyricsDock::slideSelectionChanged()
{
	currentSlideIndex = slideList->currentRow();
	updateLiveButton();
	updatePreview();

	// Update active slide in detection core
	QString nextSongTitle;
	if (listIndex >= 0 && listIndex + 1 < setlist->count()) {
		nextSongTitle = setlist->item(listIndex + 1)->text();
	}
	AudioEngine::instance().core().setActiveSong(currentSong.title, currentSlideIndex, nextSongTitle);

	// Update next slide dashed border state
	for (int i = 0; i < slideList->count(); ++i) {
		bool isNext = (i == currentSlideIndex + 1);
		slideList->item(i)->setData(RoleIsNext, isNext);
	}
	slideList->viewport()->update();
}

void LyricsDock::updateLiveButton()
{
	if (live && !blanked) {
		liveButton->setText(QStringLiteral("● On screen"));
	} else {
		liveButton->setText(QStringLiteral("On screen"));
	}
}

void LyricsDock::updatePreview()
{
	DisplayContent content;
	if (currentSlideIndex >= 0 && currentSlideIndex < static_cast<int>(currentSlides.size())) {
		const auto &slide = currentSlides[static_cast<size_t>(currentSlideIndex)];
		content.reference = slide.kicker();
		content.visible = true;
		for (const auto &line : slide.lines) {
			DisplayEntry entry;
			entry.text = line;
			content.entries.push_back(entry);
		}
	} else {
		content.visible = false;
	}

	thumbnail->updateContent(content, live && !blanked);
}

void LyricsDock::goLive()
{
	if (currentSlideIndex < 0 && slideList->count() > 0) {
		slideList->setCurrentRow(0);
	}
	live = true;
	blanked = false;
	blankButton->setChecked(false);

	for (int i = 0; i < slideList->count(); ++i) {
		slideList->item(i)->setData(RoleIsLive, (i == currentSlideIndex));
	}
	slideList->viewport()->update();

	pushToOutput();
	updateLiveButton();
	updatePreview();
}

void LyricsDock::clearLive()
{
	live = false;
	blanked = false;
	blankButton->setChecked(false);

	for (int i = 0; i < slideList->count(); ++i) {
		slideList->item(i)->setData(RoleIsLive, false);
	}
	slideList->viewport()->update();

	DisplayContent empty;
	empty.visible = false;
	DisplayState::get(Channel::Lyrics).set(empty);
	updateLiveButton();
	updatePreview();
}

void LyricsDock::toggleBlank()
{
	blanked = blankButton->isChecked();
	if (blanked) {
		DisplayContent empty;
		empty.visible = false;
		DisplayState::get(Channel::Lyrics).set(empty);
	} else if (live) {
		pushToOutput();
	}
	updatePreview();
}

void LyricsDock::pushToOutput()
{
	if (!live || blanked)
		return;

	if (currentSlideIndex >= 0 && currentSlideIndex < static_cast<int>(currentSlides.size())) {
		const auto &slide = currentSlides[static_cast<size_t>(currentSlideIndex)];
		DisplayContent content;
		content.reference = slide.kicker();
		content.visible = true;
		for (const auto &line : slide.lines) {
			DisplayEntry entry;
			entry.text = line;
			content.entries.push_back(entry);
		}
		DisplayState::get(Channel::Lyrics).set(content);
	}
}

void LyricsDock::next()
{
	if (currentSlideIndex + 1 < slideList->count()) {
		slideList->setCurrentRow(currentSlideIndex + 1);
		if (live)
			goLive();
	} else if (listIndex >= 0 && listIndex + 1 < setlist->count()) {
		openListIndex(listIndex + 1, false);
		if (live)
			goLive();
	}
}

void LyricsDock::prev()
{
	if (currentSlideIndex > 0) {
		slideList->setCurrentRow(currentSlideIndex - 1);
		if (live)
			goLive();
	} else if (listIndex > 0) {
		openListIndex(listIndex - 1, true);
		if (live)
			goLive();
	}
}

void LyricsDock::addToList()
{
	if (currentSong.title.isEmpty())
		return;
	setlist->addItem(currentSong.title);
	setlistHeaderLabel->setText(QStringLiteral("SETLIST · %1").arg(setlist->count()));
}

void LyricsDock::removeFromList()
{
	int row = setlist->currentRow();
	if (row >= 0) {
		delete setlist->takeItem(row);
		setlistHeaderLabel->setText(QStringLiteral("SETLIST · %1").arg(setlist->count()));
	}
}

void LyricsDock::moveListItemUp()
{
	int row = setlist->currentRow();
	if (row > 0) {
		auto *item = setlist->takeItem(row);
		setlist->insertItem(row - 1, item);
		setlist->setCurrentRow(row - 1);
	}
}

void LyricsDock::moveListItemDown()
{
	int row = setlist->currentRow();
	if (row >= 0 && row < setlist->count() - 1) {
		auto *item = setlist->takeItem(row);
		setlist->insertItem(row + 1, item);
		setlist->setCurrentRow(row + 1);
	}
}

void LyricsDock::loadList() {}
void LyricsDock::saveList() {}

void LyricsDock::openListItem(QListWidgetItem *item)
{
	if (!item)
		return;
	listIndex = setlist->row(item);
	openSong(item->text(), true);
}

bool LyricsDock::openListIndex(int index, bool fromEnd)
{
	if (index < 0 || index >= setlist->count())
		return false;
	listIndex = index;
	setlist->setCurrentRow(index);
	openSong(setlist->item(index)->text(), true);
	if (fromEnd && slideList->count() > 0) {
		slideList->setCurrentRow(slideList->count() - 1);
	}
	return true;
}

void LyricsDock::newSongDialog()
{
	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("New Song"));
	dlg.setMinimumWidth(450);

	auto *layout = new QVBoxLayout(&dlg);
	layout->setSpacing(8);

	auto *form = new QFormLayout();
	auto *titleEdit = new QLineEdit(&dlg);
	titleEdit->setPlaceholderText(QStringLiteral("e.g. Amazing Grace"));
	form->addRow(QStringLiteral("Title *:"), titleEdit);

	auto *authorEdit = new QLineEdit(&dlg);
	authorEdit->setPlaceholderText(QStringLiteral("e.g. John Newton"));
	form->addRow(QStringLiteral("Author:"), authorEdit);

	auto *ccliEdit = new QLineEdit(&dlg);
	ccliEdit->setPlaceholderText(QStringLiteral("Optional CCLI #"));
	form->addRow(QStringLiteral("CCLI #:"), ccliEdit);

	auto *copyrightEdit = new QLineEdit(&dlg);
	copyrightEdit->setPlaceholderText(QStringLiteral("Optional copyright info"));
	form->addRow(QStringLiteral("Copyright:"), copyrightEdit);

	auto *orderEdit = new QLineEdit(&dlg);
	orderEdit->setPlaceholderText(QStringLiteral("e.g. V1 C V2 C B C"));
	form->addRow(QStringLiteral("Verse Order:"), orderEdit);

	layout->addLayout(form);

	auto *lyricsLabel = new QLabel(QStringLiteral("Lyrics & Sections:"), &dlg);
	layout->addWidget(lyricsLabel);

	auto *lyricsEdit = new QPlainTextEdit(&dlg);
	lyricsEdit->setPlaceholderText(QStringLiteral("[Verse 1]\n"
						      "Amazing grace! How sweet the sound\n"
						      "That saved a wretch like me!\n\n"
						      "[Chorus]\n"
						      "Praise God, praise God\n"
						      "Praise God, praise God\n\n"
						      "[Verse 2]\n"
						      "'Twas grace that taught my heart to fear\n"
						      "And grace my fears relieved"));
	lyricsEdit->setMinimumHeight(200);
	layout->addWidget(lyricsEdit);

	auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
	connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	layout->addWidget(btnBox);

	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString title = titleEdit->text().trimmed();
	if (title.isEmpty()) {
		QMessageBox::warning(this, QStringLiteral("Missing Title"),
				     QStringLiteral("Please enter a song title."));
		return;
	}

	Song song = parseNativeSong(lyricsEdit->toPlainText(), title);
	song.title = title;
	song.author = authorEdit->text().trimmed();
	song.ccli = ccliEdit->text().trimmed();
	song.copyright = copyrightEdit->text().trimmed();
	song.order = orderEdit->text().trimmed();

	QString savedPath, err;
	if (!SongLibrary::instance().saveSong(song, title, &savedPath, &err)) {
		QMessageBox::critical(this, QStringLiteral("Error Saving Song"),
				      err.isEmpty() ? QStringLiteral("Could not save song to disk.") : err);
		return;
	}

	refreshSongList(searchEdit->text().trimmed());
	openSong(title, false);
}

void LyricsDock::editSongDialog()
{
	if (!currentSong.isValid()) {
		QMessageBox::information(this, QStringLiteral("No Song Selected"),
					 QStringLiteral("Please select a song from the library to edit."));
		return;
	}

	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("Edit Song - %1").arg(currentSong.title));
	dlg.setMinimumWidth(450);

	auto *layout = new QVBoxLayout(&dlg);
	layout->setSpacing(8);

	auto *form = new QFormLayout();
	auto *titleEdit = new QLineEdit(&dlg);
	titleEdit->setText(currentSong.title);
	form->addRow(QStringLiteral("Title *:"), titleEdit);

	auto *authorEdit = new QLineEdit(&dlg);
	authorEdit->setText(currentSong.author);
	form->addRow(QStringLiteral("Author:"), authorEdit);

	auto *ccliEdit = new QLineEdit(&dlg);
	ccliEdit->setText(currentSong.ccli);
	form->addRow(QStringLiteral("CCLI #:"), ccliEdit);

	auto *copyrightEdit = new QLineEdit(&dlg);
	copyrightEdit->setText(currentSong.copyright);
	form->addRow(QStringLiteral("Copyright:"), copyrightEdit);

	auto *orderEdit = new QLineEdit(&dlg);
	orderEdit->setText(currentSong.order);
	form->addRow(QStringLiteral("Verse Order:"), orderEdit);

	layout->addLayout(form);

	auto *lyricsLabel = new QLabel(QStringLiteral("Lyrics & Sections:"), &dlg);
	layout->addWidget(lyricsLabel);

	auto *lyricsEdit = new QPlainTextEdit(&dlg);
	QString outLyrics;
	QTextStream ts(&outLyrics);
	for (size_t si = 0; si < currentSong.sections.size(); si++) {
		const auto &sec = currentSong.sections[si];
		if (!sec.name.isEmpty())
			ts << "[" << sec.name << "]\n";
		for (size_t ri = 0; ri < sec.rawSlides.size(); ri++) {
			for (const QString &line : sec.rawSlides[ri])
				ts << line << "\n";
			if (ri + 1 < sec.rawSlides.size())
				ts << "\n";
		}
		if (si + 1 < currentSong.sections.size())
			ts << "\n";
	}
	lyricsEdit->setPlainText(outLyrics.trimmed());
	lyricsEdit->setMinimumHeight(200);
	layout->addWidget(lyricsEdit);

	auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
	connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	layout->addWidget(btnBox);

	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString newTitle = titleEdit->text().trimmed();
	if (newTitle.isEmpty()) {
		QMessageBox::warning(this, QStringLiteral("Missing Title"),
				     QStringLiteral("Please enter a song title."));
		return;
	}

	if (newTitle != currentSong.title && !currentSong.builtIn && !currentSong.filePath.isEmpty()) {
		QFile::remove(currentSong.filePath);
	}

	Song updated = parseNativeSong(lyricsEdit->toPlainText(), newTitle);
	updated.title = newTitle;
	updated.author = authorEdit->text().trimmed();
	updated.ccli = ccliEdit->text().trimmed();
	updated.copyright = copyrightEdit->text().trimmed();
	updated.order = orderEdit->text().trimmed();

	QString savedPath, err;
	if (!SongLibrary::instance().saveSong(updated, newTitle, &savedPath, &err)) {
		QMessageBox::critical(this, QStringLiteral("Error Saving Song"),
				      err.isEmpty() ? QStringLiteral("Could not save song updates to disk.") : err);
		return;
	}

	refreshSongList(searchEdit->text().trimmed());
	openSong(newTitle, false);
}

void LyricsDock::importSongs()
{
	const QStringList files =
		QFileDialog::getOpenFileNames(this, QStringLiteral("Import Songs"), QString(),
					      QStringLiteral("All Supported Formats (*.xml *.json *.txt *.ccli);;"
							     "OpenLyrics / OpenSong XML (*.xml);;"
							     "JSON Songs (*.json);;"
							     "Text Songs (*.txt);;"
							     "All Files (*.*)"));
	if (files.isEmpty())
		return;

	int totalImported = 0;
	int failCount = 0;
	QString firstTitle;
	QStringList errorMessages;

	for (const QString &file : files) {
		QString title;
		QString err;
		int count = SongLibrary::instance().importSongFileCount(file, &title, &err);
		if (count > 0) {
			totalImported += count;
			if (firstTitle.isEmpty())
				firstTitle = title;
		} else {
			failCount++;
			errorMessages.push_back(QStringLiteral("%1: %2").arg(
				QFileInfo(file).fileName(), err.isEmpty() ? QStringLiteral("Unknown error") : err));
		}
	}

	refreshSongList(searchEdit->text().trimmed());

	if (!firstTitle.isEmpty()) {
		openSong(firstTitle, false);
	}

	if (failCount > 0) {
		QMessageBox::warning(
			this, QStringLiteral("Import Completed with Errors"),
			QStringLiteral("Successfully imported %1 song(s).\nFailed to import %2 file(s):\n%3")
				.arg(totalImported)
				.arg(failCount)
				.arg(errorMessages.join(QLatin1Char('\n'))));
	} else if (totalImported > 0) {
		QMessageBox::information(
			this, QStringLiteral("Import Successful"),
			QStringLiteral("Successfully imported %1 song(s) into your library!").arg(totalImported));
	}
}
void LyricsDock::openSongsFolder()
{
	QDesktopServices::openUrl(QUrl::fromLocalFile(SongLibrary::instance().userDir()));
}

void LyricsDock::registerHotkeys()
{
	hkNext = obs_hotkey_register_frontend("ld_lyrics_next", "LD Lyrics: Next Slide", hotkeyCallback<kNext>, this);
	hkPrev = obs_hotkey_register_frontend("ld_lyrics_prev", "LD Lyrics: Previous Slide", hotkeyCallback<kPrev>,
					      this);
	hkLive = obs_hotkey_register_frontend("ld_lyrics_live", "LD Lyrics: Show Slide Live", hotkeyCallback<kLive>,
					      this);
	hkClear = obs_hotkey_register_frontend("ld_lyrics_clear", "LD Lyrics: Clear Lyrics", hotkeyCallback<kClear>,
					       this);
}

void LyricsDock::loadConfig() {}
void LyricsDock::saveConfig() {}
