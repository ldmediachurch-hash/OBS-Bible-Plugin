/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "dock.hpp"
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
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Roles { RoleBook = Qt::UserRole + 1, RoleChapter, RoleVerse, RoleIsLive, RoleIsNext };

QString configPath()
{
	char *p = obs_module_config_path("dock.json");
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
	QMetaObject::invokeMethod(static_cast<ScriptureDock *>(data), Slot, Qt::QueuedConnection);
}

const char kNext[] = "next";
const char kPrev[] = "prev";
const char kLive[] = "goLive";
const char kClear[] = "clearLive";

// Delegate for Scripture Verse Cards (Mockup fidelity)
class VerseCardDelegate : public QStyledItemDelegate {
public:
	explicit VerseCardDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

	void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		p->save();
		p->setRenderHint(QPainter::Antialiasing, true);

		QRect r = option.rect.adjusted(4, 3, -4, -3);
		bool isSelected = option.state & QStyle::State_Selected;
		bool isLive = index.data(RoleIsLive).toBool();
		bool isNext = index.data(RoleIsNext).toBool();
		int verseNum = index.data(RoleVerse).toInt();
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

		// Verse number in small-caps slate
		QFont numFont = option.font;
		numFont.setPointSize(8);
		numFont.setBold(true);
		p->setFont(numFont);
		p->setPen(QColor(140, 147, 160));
		QString numStr = QStringLiteral("VERSE %1").arg(verseNum);
		p->drawText(r.left() + 10, contentTop + 10, numStr);

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

		// Verse text
		QFont textFont = option.font;
		textFont.setPointSize(9);
		p->setFont(textFont);
		p->setPen(QColor(240, 243, 246));

		int textTop = contentTop + 18;
		QRect textRect(r.left() + 10, textTop, r.width() - 20, r.bottom() - textTop - 4);
		p->drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);

		p->restore();
	}

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
	{
		return QSize(option.rect.width(), 62);
	}
};

// Delegate for Service List (numbered rows)
class ServiceListDelegate : public QStyledItemDelegate {
public:
	explicit ServiceListDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

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

		// Number in muted slate
		QFont numFont = option.font;
		numFont.setPointSize(numFont.pointSize() - 1);
		p->setFont(numFont);
		p->setPen(QColor(130, 137, 150));
		QRect numRect(r.left() + 6, r.top(), 22, r.height());
		p->drawText(numRect, Qt::AlignLeft | Qt::AlignVCenter, numStr);

		// Reference
		QFont titleFont = option.font;
		titleFont.setBold(isSelected);
		p->setFont(titleFont);
		p->setPen(isSelected ? Qt::white : QColor(220, 224, 230));
		QFontMetrics fmTitle(titleFont);
		QString elidedTitle = fmTitle.elidedText(title, Qt::ElideRight, r.width() - 38);
		QRect titleRect(r.left() + 32, r.top(), r.width() - 38, r.height());
		p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

		p->restore();
	}

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
	{
		return QSize(option.rect.width(), 32);
	}
};

} // namespace

ScriptureDock::ScriptureDock(QWidget *parent) : QWidget(parent)
{
	setMinimumWidth(300);
	setupLayout();
	registerHotkeys();
	refreshTranslations(QStringLiteral("KJV"), QString());
	loadConfig();
}

ScriptureDock::~ScriptureDock()
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

void ScriptureDock::setupLayout()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	// 1. Search + Gear Row
	auto *searchRow = new QHBoxLayout();
	searchRow->setSpacing(6);

	searchEdit = new QLineEdit(this);
	searchEdit->setPlaceholderText(QStringLiteral("Rom 8:28, Psalm 23, or words"));
	searchEdit->setClearButtonEnabled(true);
	searchRow->addWidget(searchEdit, 1);

	gearBtn = new QToolButton(this);
	gearBtn->setIcon(QIcon(QStringLiteral(":/icons/gear.svg")));
	gearBtn->setToolTip(QStringLiteral("Settings & Bible Options"));
	gearBtn->setFixedSize(30, 30);
	connect(gearBtn, &QToolButton::clicked, this, &ScriptureDock::openGearMenu);
	searchRow->addWidget(gearBtn);

	root->addLayout(searchRow);

	// Translations Row (Primary + Secondary)
	auto *transRow = new QHBoxLayout();
	transRow->setSpacing(6);

	primaryBox = new QComboBox(this);
	primaryBox->setToolTip(QStringLiteral("Primary Translation"));
	primaryBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	transRow->addWidget(primaryBox, 1);

	secondaryBox = new QComboBox(this);
	secondaryBox->setToolTip(QStringLiteral("Secondary Translation (Optional)"));
	secondaryBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	transRow->addWidget(secondaryBox, 1);

	root->addLayout(transRow);

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

	// --- Verses Section ---
	auto *versesHeaderRow = new QHBoxLayout();
	versesHeaderLabel = new QLabel(QStringLiteral("VERSES"), scrollContent);
	versesHeaderLabel->setFont(hFont);
	versesHeaderLabel->setStyleSheet(QStringLiteral("color: #8C93A0;"));
	versesHeaderRow->addWidget(versesHeaderLabel, 1);
	contentLayout->addLayout(versesHeaderRow);

	results = new QListWidget(scrollContent);
	results->setItemDelegate(new VerseCardDelegate(results));
	results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	results->setMinimumHeight(150);
	contentLayout->addWidget(results);

	// 16:9 Mini Program Thumbnail
	thumbnail = new MiniProgramThumbnail(scrollContent);
	contentLayout->addWidget(thumbnail);

	// --- Service List Section ---
	auto *serviceHeaderRow = new QHBoxLayout();
	serviceHeaderLabel = new QLabel(QStringLiteral("SERVICE LIST · 0"), scrollContent);
	serviceHeaderLabel->setFont(hFont);
	serviceHeaderLabel->setStyleSheet(QStringLiteral("color: #8C93A0;"));
	serviceHeaderRow->addWidget(serviceHeaderLabel, 1);

	serviceAddBtn = new QToolButton(scrollContent);
	serviceAddBtn->setIcon(QIcon(QStringLiteral(":/icons/plus.svg")));
	serviceAddBtn->setToolTip(QStringLiteral("Add current scripture to service list"));
	connect(serviceAddBtn, &QToolButton::clicked, this, &ScriptureDock::addToList);
	serviceHeaderRow->addWidget(serviceAddBtn);

	serviceRemBtn = new QToolButton(scrollContent);
	serviceRemBtn->setIcon(QIcon(QStringLiteral(":/icons/minus.svg")));
	serviceRemBtn->setToolTip(QStringLiteral("Remove selected scripture from service list"));
	connect(serviceRemBtn, &QToolButton::clicked, this, &ScriptureDock::removeFromList);
	serviceHeaderRow->addWidget(serviceRemBtn);

	serviceUpBtn = new QToolButton(scrollContent);
	serviceUpBtn->setIcon(QIcon(QStringLiteral(":/icons/prev.svg")));
	serviceUpBtn->setToolTip(QStringLiteral("Move up in service list"));
	connect(serviceUpBtn, &QToolButton::clicked, this, &ScriptureDock::moveListItemUp);
	serviceHeaderRow->addWidget(serviceUpBtn);

	serviceDownBtn = new QToolButton(scrollContent);
	serviceDownBtn->setIcon(QIcon(QStringLiteral(":/icons/next.svg")));
	serviceDownBtn->setToolTip(QStringLiteral("Move down in service list"));
	connect(serviceDownBtn, &QToolButton::clicked, this, &ScriptureDock::moveListItemDown);
	serviceHeaderRow->addWidget(serviceDownBtn);

	contentLayout->addLayout(serviceHeaderRow);

	serviceList = new QListWidget(scrollContent);
	serviceList->setItemDelegate(new ServiceListDelegate(serviceList));
	serviceList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	serviceList->setMinimumHeight(80);
	serviceList->setMaximumHeight(130);
	contentLayout->addWidget(serviceList);

	scrollArea->setWidget(scrollContent);
	root->addWidget(scrollArea, 1);

	// 3. Sticky Bottom Control Bar (pinned to bottom of dock)
	auto *ctlRow = new QHBoxLayout();
	ctlRow->setSpacing(6);

	prevBtn = new QPushButton(QStringLiteral("◀"), this);
	prevBtn->setToolTip(QStringLiteral("Previous Verse (Shift+Space)"));
	prevBtn->setFixedHeight(34);
	prevBtn->setMaximumWidth(40);
	connect(prevBtn, &QPushButton::clicked, this, &ScriptureDock::prev);
	ctlRow->addWidget(prevBtn);

	liveButton = new QPushButton(QStringLiteral("On screen"), this);
	liveButton->setObjectName(QStringLiteral("PrimaryShowBtn"));
	liveButton->setStyleSheet(QStringLiteral(
		"QPushButton#PrimaryShowBtn { background-color: #FFFFFF; color: #0F1115; font-weight: bold; border-radius: 6px; padding: 6px 14px; border: none; font-size: 13px; } "
		"QPushButton#PrimaryShowBtn:hover { background-color: #E2E8F0; } "
		"QPushButton#PrimaryShowBtn:pressed { background-color: #CBD5E1; }"));
	liveButton->setToolTip(QStringLiteral("Show selected verse on screen (Enter)"));
	liveButton->setFixedHeight(34);
	connect(liveButton, &QPushButton::clicked, this, &ScriptureDock::goLive);
	ctlRow->addWidget(liveButton, 2);

	clearButton = new QPushButton(QStringLiteral("Clear"), this);
	clearButton->setToolTip(QStringLiteral("Take scripture off screen (Esc)"));
	clearButton->setFixedHeight(34);
	connect(clearButton, &QPushButton::clicked, this, &ScriptureDock::clearLive);
	ctlRow->addWidget(clearButton, 1);

	nextBtn = new QPushButton(QStringLiteral("▶"), this);
	nextBtn->setToolTip(QStringLiteral("Next Verse (Space)"));
	nextBtn->setFixedHeight(34);
	nextBtn->setMaximumWidth(40);
	connect(nextBtn, &QPushButton::clicked, this, &ScriptureDock::next);
	ctlRow->addWidget(nextBtn);

	root->addLayout(ctlRow);

	// Shortcut hint
	shortcutHintLabel = new QLabel(QStringLiteral("Space next · ⇧Space back · Enter show · Esc clear"), this);
	QFont hintFont = font();
	hintFont.setPointSize(8);
	shortcutHintLabel->setFont(hintFont);
	shortcutHintLabel->setAlignment(Qt::AlignCenter);
	shortcutHintLabel->setStyleSheet(QStringLiteral("color: #717886;"));
	root->addWidget(shortcutHintLabel);

	// Connect signals
	connect(searchEdit, &QLineEdit::returnPressed, this, &ScriptureDock::runSearch);
	connect(primaryBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ScriptureDock::translationChanged);
	connect(secondaryBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ScriptureDock::translationChanged);
	connect(results, &QListWidget::currentRowChanged, this, &ScriptureDock::selectionChanged);
	connect(serviceList, &QListWidget::itemDoubleClicked, this, &ScriptureDock::openListItem);
}

void ScriptureDock::keyPressEvent(QKeyEvent *event)
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
	case Qt::Key_Escape:
		clearLive();
		event->accept();
		return;
	default:
		break;
	}
	QWidget::keyPressEvent(event);
}

void ScriptureDock::openGearMenu()
{
	auto *menu = new QMenu(this);
	menu->addAction(QStringLiteral("Import Bible (XML or JSON)…"), this, &ScriptureDock::importBible);
	menu->addAction(QStringLiteral("Remove Custom Bible…"), this, &ScriptureDock::removeBible);
	menu->addSeparator();
	menu->addAction(QStringLiteral("Open Bibles Folder"), this, &ScriptureDock::openBiblesFolder);
	menu->exec(gearBtn->mapToGlobal(QPoint(0, gearBtn->height())));
}

void ScriptureDock::refreshTranslations(const QString &primaryAbbrev, const QString &secondaryAbbrev)
{
	primaryBox->blockSignals(true);
	secondaryBox->blockSignals(true);

	primaryBox->clear();
	secondaryBox->clear();
	secondaryBox->addItem(QStringLiteral("(None)"), QStringLiteral(""));

	auto bibles = BibleLibrary::instance().bibles();
	for (const auto &b : bibles) {
		QString label = QStringLiteral("%1 (%2)").arg(b->name, b->abbrev);
		primaryBox->addItem(label, b->abbrev);
		secondaryBox->addItem(label, b->abbrev);
	}

	int pIdx = primaryBox->findData(primaryAbbrev);
	primaryBox->setCurrentIndex(pIdx >= 0 ? pIdx : 0);

	int sIdx = secondaryBox->findData(secondaryAbbrev);
	secondaryBox->setCurrentIndex(sIdx >= 0 ? sIdx : 0);

	primaryBox->blockSignals(false);
	secondaryBox->blockSignals(false);
}

BiblePtr ScriptureDock::primary() const
{
	QString abbrev = primaryBox->currentData().toString();
	return BibleLibrary::instance().byAbbrev(abbrev);
}

BiblePtr ScriptureDock::secondary() const
{
	QString abbrev = secondaryBox->currentData().toString();
	return BibleLibrary::instance().byAbbrev(abbrev);
}

void ScriptureDock::translationChanged()
{
	updatePreview();
	if (live)
		pushToOutput();
}

void ScriptureDock::runSearch()
{
	QString query = searchEdit->text().trimmed();
	if (query.isEmpty())
		return;
	showQuery(query, false);
}

bool ScriptureDock::showQuery(const QString &query, bool fromList)
{
	auto b = primary();
	if (!b)
		return false;

	ParsedRef parsed = BibleLibrary::instance().parse(query, b.get());
	if (parsed.valid) {
		searchIsKeyword = false;
		setChapter(parsed.book, parsed.chapter, parsed.v1 > 0 ? parsed.v1 : 1);
		if (fromList && parsed.v2 > parsed.v1) {
			rangeEnd = parsed.v2;
		} else {
			rangeEnd = 0;
		}
		return true;
	}

	// Keyword search
	auto hits = BibleLibrary::instance().search(b.get(), query, 50);
	if (hits.empty())
		return false;

	searchIsKeyword = true;
	results->clear();
	for (const auto &ref : hits) {
		auto *item = new QListWidgetItem();
		QString t = b->verse(ref.book, ref.chapter, ref.verse);
		item->setText(t);
		item->setData(RoleBook, ref.book);
		item->setData(RoleChapter, ref.chapter);
		item->setData(RoleVerse, ref.verse);
		item->setData(RoleIsLive, false);
		item->setData(RoleIsNext, false);
		results->addItem(item);
	}

	if (results->count() > 0) {
		results->setCurrentRow(0);
	}
	return true;
}

void ScriptureDock::setChapter(int book, int chapter, int selectVerse)
{
	auto b = primary();
	if (!b)
		return;
	b->ensureLoaded();

	results->clear();
	int count = b->verseCount(book, chapter);
	int selectRow = 0;

	for (int v = 1; v <= count; ++v) {
		auto *item = new QListWidgetItem();
		item->setText(b->verse(book, chapter, v));
		item->setData(RoleBook, book);
		item->setData(RoleChapter, chapter);
		item->setData(RoleVerse, v);
		item->setData(RoleIsLive, false);
		item->setData(RoleIsNext, false);

		if (v == selectVerse) {
			selectRow = v - 1;
		}
		results->addItem(item);
	}

	if (results->count() > 0) {
		results->setCurrentRow(selectRow);
	}
}

VerseRef ScriptureDock::currentVerse() const
{
	VerseRef ref;
	auto *item = results->currentItem();
	if (item) {
		ref.book = item->data(RoleBook).toInt();
		ref.chapter = item->data(RoleChapter).toInt();
		ref.verse = item->data(RoleVerse).toInt();
	}
	return ref;
}

void ScriptureDock::selectionChanged()
{
	int curRow = results->currentRow();
	for (int i = 0; i < results->count(); ++i) {
		results->item(i)->setData(RoleIsNext, (i == curRow + 1));
	}
	results->viewport()->update();

	updateLiveButton();
	updatePreview();

	if (live && follow) {
		pushToOutput();
	}
}

void ScriptureDock::updateLiveButton()
{
	if (live) {
		liveButton->setText(QStringLiteral("● On screen"));
	} else {
		liveButton->setText(QStringLiteral("On screen"));
	}
}

void ScriptureDock::updatePreview()
{
	VerseRef ref = currentVerse();
	auto b1 = primary();
	auto b2 = secondary();

	DisplayContent content;
	if (ref.book > 0 && b1) {
		content.reference = BibleLibrary::instance().formatRef(b1.get(), ref.book, ref.chapter, ref.verse);
		content.visible = true;

		DisplayEntry e1;
		e1.text = b1->verse(ref.book, ref.chapter, ref.verse);
		e1.label = b1->abbrev;
		content.entries.push_back(e1);

		if (b2) {
			b2->ensureLoaded();
			DisplayEntry e2;
			e2.text = b2->verse(ref.book, ref.chapter, ref.verse);
			e2.label = b2->abbrev;
			content.entries.push_back(e2);
		}
	} else {
		content.visible = false;
	}

	thumbnail->updateContent(content, live);
}

void ScriptureDock::goLive()
{
	if (results->currentRow() < 0 && results->count() > 0) {
		results->setCurrentRow(0);
	}

	live = true;
	int cur = results->currentRow();
	for (int i = 0; i < results->count(); ++i) {
		results->item(i)->setData(RoleIsLive, (i == cur));
	}
	results->viewport()->update();

	pushToOutput();
	updateLiveButton();
	updatePreview();
}

void ScriptureDock::clearLive()
{
	live = false;
	for (int i = 0; i < results->count(); ++i) {
		results->item(i)->setData(RoleIsLive, false);
	}
	results->viewport()->update();

	DisplayContent empty;
	empty.visible = false;
	DisplayState::get(Channel::Scripture).set(empty);
	updateLiveButton();
	updatePreview();
}

void ScriptureDock::pushToOutput()
{
	if (!live)
		return;

	VerseRef ref = currentVerse();
	auto b1 = primary();
	auto b2 = secondary();

	if (ref.book > 0 && b1) {
		DisplayContent content;
		content.reference = BibleLibrary::instance().formatRef(b1.get(), ref.book, ref.chapter, ref.verse);
		content.visible = true;

		DisplayEntry e1;
		e1.text = b1->verse(ref.book, ref.chapter, ref.verse);
		e1.label = b1->abbrev;
		content.entries.push_back(e1);

		if (b2) {
			b2->ensureLoaded();
			DisplayEntry e2;
			e2.text = b2->verse(ref.book, ref.chapter, ref.verse);
			e2.label = b2->abbrev;
			content.entries.push_back(e2);
		}

		DisplayState::get(Channel::Scripture).set(content);
	}
}

void ScriptureDock::next()
{
	int cur = results->currentRow();
	if (cur + 1 < results->count()) {
		results->setCurrentRow(cur + 1);
		if (live)
			goLive();
	} else if (listIndex >= 0 && listIndex + 1 < serviceList->count()) {
		openListIndex(listIndex + 1, false);
		if (live)
			goLive();
	}
}

void ScriptureDock::prev()
{
	int cur = results->currentRow();
	if (cur > 0) {
		results->setCurrentRow(cur - 1);
		if (live)
			goLive();
	} else if (listIndex > 0) {
		openListIndex(listIndex - 1, true);
		if (live)
			goLive();
	}
}

void ScriptureDock::addToList()
{
	VerseRef ref = currentVerse();
	auto b = primary();
	if (ref.book > 0 && b) {
		QString rStr = BibleLibrary::instance().formatRef(b.get(), ref.book, ref.chapter, ref.verse);
		serviceList->addItem(rStr);
		serviceHeaderLabel->setText(QStringLiteral("SERVICE LIST · %1").arg(serviceList->count()));
	}
}

void ScriptureDock::removeFromList()
{
	int row = serviceList->currentRow();
	if (row >= 0) {
		delete serviceList->takeItem(row);
		serviceHeaderLabel->setText(QStringLiteral("SERVICE LIST · %1").arg(serviceList->count()));
	}
}

void ScriptureDock::moveListItemUp()
{
	int row = serviceList->currentRow();
	if (row > 0) {
		auto *item = serviceList->takeItem(row);
		serviceList->insertItem(row - 1, item);
		serviceList->setCurrentRow(row - 1);
	}
}

void ScriptureDock::moveListItemDown()
{
	int row = serviceList->currentRow();
	if (row >= 0 && row < serviceList->count() - 1) {
		auto *item = serviceList->takeItem(row);
		serviceList->insertItem(row + 1, item);
		serviceList->setCurrentRow(row + 1);
	}
}

void ScriptureDock::loadList() {}
void ScriptureDock::saveList() {}

void ScriptureDock::openListItem(QListWidgetItem *item)
{
	if (!item)
		return;
	listIndex = serviceList->row(item);
	showQuery(item->text(), true);
}

bool ScriptureDock::openListIndex(int index, bool fromEnd)
{
	if (index < 0 || index >= serviceList->count())
		return false;
	listIndex = index;
	serviceList->setCurrentRow(index);
	showQuery(serviceList->item(index)->text(), true);
	if (fromEnd && results->count() > 0) {
		results->setCurrentRow(results->count() - 1);
	}
	return true;
}

void ScriptureDock::importBible() {}
void ScriptureDock::removeBible() {}
void ScriptureDock::openBiblesFolder()
{
	QDesktopServices::openUrl(QUrl::fromLocalFile(BibleLibrary::instance().userDir()));
}

void ScriptureDock::registerHotkeys()
{
	hkNext = obs_hotkey_register_frontend("ld_bible_next", "LD Scripture: Next Verse", hotkeyCallback<kNext>, this);
	hkPrev = obs_hotkey_register_frontend("ld_bible_prev", "LD Scripture: Previous Verse", hotkeyCallback<kPrev>,
					      this);
	hkLive = obs_hotkey_register_frontend("ld_bible_live", "LD Scripture: Show Verse Live", hotkeyCallback<kLive>,
					      this);
	hkClear = obs_hotkey_register_frontend("ld_bible_clear", "LD Scripture: Clear Scripture",
					       hotkeyCallback<kClear>, this);
}

void ScriptureDock::loadConfig() {}
void ScriptureDock::saveConfig() {}
