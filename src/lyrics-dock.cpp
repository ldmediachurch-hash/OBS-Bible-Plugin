/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "lyrics-dock.hpp"
#include "display-state.hpp"

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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextStream>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Roles { RoleSlideIndex = Qt::UserRole + 1, RoleSongPath, RoleSectionCode, RoleSectionName };

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
const char kBlank[] = "toggleBlank";

class SearchKeys : public QObject {
public:
	SearchKeys(QListWidget *list, QObject *parent) : QObject(parent), slideList(list) {}

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (ev->type() == QEvent::KeyPress) {
			auto *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up) {
				const int delta = (ke->key() == Qt::Key_Down) ? 1 : -1;
				int row = slideList->currentRow() + delta;
				while (row >= 0 && row < slideList->count()) {
					if (slideList->item(row)->data(RoleSlideIndex).toInt() >= 0) {
						slideList->setCurrentRow(row);
						break;
					}
					row += delta;
				}
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}

private:
	QListWidget *slideList;
};

} // namespace

LyricsDock::LyricsDock(QWidget *parent) : QWidget(parent)
{
	setMinimumWidth(280);
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	// Top search & songs menu row
	auto *topRow = new QHBoxLayout();
	searchEdit = new QLineEdit(this);
	searchEdit->setPlaceholderText(QStringLiteral("Search title or lyrics…"));
	searchEdit->setClearButtonEnabled(true);
	topRow->addWidget(searchEdit, 1);

	auto *songsMenuBtn = new QToolButton(this);
	songsMenuBtn->setText(QStringLiteral("Songs"));
	songsMenuBtn->setPopupMode(QToolButton::InstantPopup);
	auto *sMenu = new QMenu(songsMenuBtn);
	sMenu->addAction(QStringLiteral("New song…"), this, &LyricsDock::newSongDialog);
	sMenu->addAction(QStringLiteral("Edit current song…"), this, &LyricsDock::editSongDialog);
	sMenu->addAction(QStringLiteral("Import songs (Text or OpenLyrics XML)…"), this, &LyricsDock::importSongs);
	sMenu->addSeparator();
	sMenu->addAction(QStringLiteral("Open songs folder"), this, &LyricsDock::openSongsFolder);
	songsMenuBtn->setMenu(sMenu);
	topRow->addWidget(songsMenuBtn);

	linesBox = new QComboBox(this);
	linesBox->setToolTip(QStringLiteral("Lines per slide"));
	linesBox->addItem(QStringLiteral("1 line"), 1);
	linesBox->addItem(QStringLiteral("2 lines"), 2);
	linesBox->addItem(QStringLiteral("3 lines"), 3);
	linesBox->addItem(QStringLiteral("4 lines"), 4);
	linesBox->setCurrentIndex(1); // default 2 lines
	topRow->addWidget(linesBox);

	root->addLayout(topRow);

	// Splitter containing Song Library list (top) and Current Song Slides (bottom)
	auto *splitter = new QSplitter(Qt::Vertical, this);

	songList = new QListWidget(splitter);
	songList->setUniformItemSizes(true);
	songList->setTextElideMode(Qt::ElideRight);
	songList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	songList->setWordWrap(false);
	songList->setToolTip(QStringLiteral("Library songs. Double-click or press Enter to open."));

	slideList = new QListWidget(splitter);
	slideList->setUniformItemSizes(true);
	slideList->setTextElideMode(Qt::ElideRight);
	slideList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	slideList->setWordWrap(false);

	splitter->addWidget(songList);
	splitter->addWidget(slideList);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 3);
	root->addWidget(splitter, 4);

	// Section jump buttons container
	sectionContainer = new QWidget(this);
	auto *secLayout = new QHBoxLayout(sectionContainer);
	secLayout->setContentsMargins(0, 0, 0, 0);
	secLayout->setSpacing(4);
	root->addWidget(sectionContainer);

	// Preview frame
	preview = new QLabel(this);
	preview->setWordWrap(true);
	preview->setFrameShape(QFrame::StyledPanel);
	preview->setMargin(8);
	preview->setTextFormat(Qt::RichText);
	preview->setMinimumHeight(70);
	preview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	root->addWidget(preview, 2);

	// Controls: ◀ Show Blank Clear ▶
	auto *ctl = new QHBoxLayout();
	auto *prevBtn = new QPushButton(QStringLiteral("◀"), this);
	prevBtn->setToolTip(QStringLiteral("Previous slide"));
	liveButton = new QPushButton(QStringLiteral("Show"), this);
	liveButton->setToolTip(QStringLiteral("Put the selected slide on screen"));
	blankButton = new QPushButton(QStringLiteral("Blank"), this);
	blankButton->setToolTip(QStringLiteral("Keep live state but hide card for instrumental breaks"));
	blankButton->setCheckable(true);
	clearButton = new QPushButton(QStringLiteral("Clear"), this);
	clearButton->setToolTip(QStringLiteral("Take the lyrics off screen"));
	auto *nextBtn = new QPushButton(QStringLiteral("▶"), this);
	nextBtn->setToolTip(QStringLiteral("Next slide"));
	prevBtn->setMaximumWidth(40);
	nextBtn->setMaximumWidth(40);
	ctl->addWidget(prevBtn);
	ctl->addWidget(liveButton, 2);
	ctl->addWidget(blankButton, 1);
	ctl->addWidget(clearButton, 2);
	ctl->addWidget(nextBtn);
	root->addLayout(ctl);

	// Status & Live badge row
	auto *stRow = new QHBoxLayout();
	liveBadge = new QLabel(this);
	status = new QLabel(this);
	status->setWordWrap(true);
	QFont small = status->font();
	small.setPointSizeF(small.pointSizeF() * 0.9);
	status->setFont(small);
	stRow->addWidget(liveBadge);
	stRow->addWidget(status, 1);
	root->addLayout(stRow);

	// Setlist header and list
	auto *listHeader = new QLabel(QStringLiteral("<b>Setlist</b>"), this);
	root->addWidget(listHeader);
	setlist = new QListWidget(this);
	setlist->setDragDropMode(QAbstractItemView::InternalMove);
	root->addWidget(setlist, 2);

	auto *lRow = new QHBoxLayout();
	auto *addBtn = new QPushButton(QStringLiteral("Add"), this);
	addBtn->setToolTip(QStringLiteral("Add current song to setlist"));
	auto *remBtn = new QPushButton(QStringLiteral("Remove"), this);
	auto *loadBtn = new QPushButton(QStringLiteral("Open…"), this);
	auto *saveBtn = new QPushButton(QStringLiteral("Save…"), this);
	lRow->addWidget(addBtn);
	lRow->addWidget(remBtn);
	lRow->addWidget(loadBtn);
	lRow->addWidget(saveBtn);
	root->addLayout(lRow);

	searchEdit->installEventFilter(new SearchKeys(slideList, this));

	connect(searchEdit, &QLineEdit::textChanged, this, &LyricsDock::searchTextChanged);
	connect(searchEdit, &QLineEdit::returnPressed, this, &LyricsDock::runSearch);
	connect(songList, &QListWidget::itemDoubleClicked, this, &LyricsDock::songSelected);
	connect(slideList, &QListWidget::currentRowChanged, this, &LyricsDock::slideSelectionChanged);
	connect(slideList, &QListWidget::itemActivated, this, [this](QListWidgetItem *) { goLive(); });
	connect(linesBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LyricsDock::linesPerSlideChanged);

	connect(prevBtn, &QPushButton::clicked, this, &LyricsDock::prev);
	connect(nextBtn, &QPushButton::clicked, this, &LyricsDock::next);
	connect(liveButton, &QPushButton::clicked, this, &LyricsDock::goLive);
	connect(blankButton, &QPushButton::clicked, this, &LyricsDock::toggleBlank);
	connect(clearButton, &QPushButton::clicked, this, &LyricsDock::clearLive);

	connect(addBtn, &QPushButton::clicked, this, &LyricsDock::addToList);
	connect(remBtn, &QPushButton::clicked, this, &LyricsDock::removeFromList);
	connect(loadBtn, &QPushButton::clicked, this, &LyricsDock::loadList);
	connect(saveBtn, &QPushButton::clicked, this, &LyricsDock::saveList);
	connect(setlist, &QListWidget::itemActivated, this, &LyricsDock::openListItem);

	registerHotkeys();
	loadConfig();
	refreshSongList();
	updatePreview();
}

LyricsDock::~LyricsDock()
{
	if (obs_initialized()) {
		for (obs_hotkey_id id : {hkNext, hkPrev, hkLive, hkClear, hkBlank}) {
			if (id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(id);
		}
	}
}

void LyricsDock::registerHotkeys()
{
	hkNext = obs_hotkey_register_frontend("ld_lyrics.next", "LD Lyrics: Next slide", hotkeyCallback<kNext>, this);
	hkPrev = obs_hotkey_register_frontend("ld_lyrics.prev", "LD Lyrics: Previous slide", hotkeyCallback<kPrev>,
					      this);
	hkLive = obs_hotkey_register_frontend("ld_lyrics.show", "LD Lyrics: Show", hotkeyCallback<kLive>, this);
	hkClear = obs_hotkey_register_frontend("ld_lyrics.clear", "LD Lyrics: Clear", hotkeyCallback<kClear>, this);
	hkBlank = obs_hotkey_register_frontend("ld_lyrics.blank", "LD Lyrics: Blank", hotkeyCallback<kBlank>, this);
}

/* ------------------------------------------------------------------ config */

void LyricsDock::loadConfig()
{
	OBSDataAutoRelease cfg = obs_data_create_from_json_file_safe(configPath().toUtf8().constData(), "bak");
	QString lastSong;
	if (cfg) {
		linesPerSlide = static_cast<int>(obs_data_get_int(cfg, "lines_per_slide"));
		if (linesPerSlide < 1 || linesPerSlide > 4)
			linesPerSlide = 2;
		for (int i = 0; i < linesBox->count(); i++) {
			if (linesBox->itemData(i).toInt() == linesPerSlide) {
				linesBox->setCurrentIndex(i);
				break;
			}
		}

		lastSong = QString::fromUtf8(obs_data_get_string(cfg, "current_song"));

		OBSDataArrayAutoRelease list = obs_data_get_array(cfg, "setlist");
		const size_t count = obs_data_array_count(list);
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease item = obs_data_array_item(list, i);
			setlist->addItem(QString::fromUtf8(obs_data_get_string(item, "song")));
		}

		const std::pair<obs_hotkey_id, const char *> keys[] = {{hkNext, "hk_next"},
								       {hkPrev, "hk_prev"},
								       {hkLive, "hk_show"},
								       {hkClear, "hk_clear"},
								       {hkBlank, "hk_blank"}};
		for (const auto &k : keys) {
			OBSDataArrayAutoRelease arr = obs_data_get_array(cfg, k.second);
			if (arr)
				obs_hotkey_load(k.first, arr);
		}
	}

	if (!lastSong.isEmpty())
		openSong(lastSong, false);
	else {
		const auto all = SongLibrary::instance().songs();
		if (!all.empty())
			openSong(all.front().title, false);
	}
	status->setText(QStringLiteral("Select a song and press Show."));
}

void LyricsDock::saveConfig()
{
	ensureConfigDir();
	OBSDataAutoRelease cfg = obs_data_create();
	obs_data_set_int(cfg, "lines_per_slide", linesPerSlide);
	if (!currentSong.title.isEmpty())
		obs_data_set_string(cfg, "current_song", currentSong.title.toUtf8().constData());

	OBSDataArrayAutoRelease list = obs_data_array_create();
	for (int i = 0; i < setlist->count(); i++) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "song", setlist->item(i)->text().toUtf8().constData());
		obs_data_array_push_back(list, item);
	}
	obs_data_set_array(cfg, "setlist", list);

	const std::pair<obs_hotkey_id, const char *> keys[] = {{hkNext, "hk_next"},
							       {hkPrev, "hk_prev"},
							       {hkLive, "hk_show"},
							       {hkClear, "hk_clear"},
							       {hkBlank, "hk_blank"}};
	for (const auto &k : keys) {
		OBSDataArrayAutoRelease arr = obs_hotkey_save(k.first);
		obs_data_set_array(cfg, k.second, arr);
	}
	obs_data_save_json_safe(cfg, configPath().toUtf8().constData(), "tmp", "bak");
}

/* ------------------------------------------------------------------ library & search */

void LyricsDock::refreshSongList(const QString &filter)
{
	QSignalBlocker b(songList);
	songList->clear();
	const auto results = SongLibrary::instance().search(filter);
	for (const auto &s : results) {
		QString label = s.title;
		if (!s.author.isEmpty())
			label += QStringLiteral(" — %1").arg(s.author);
		auto *item = new QListWidgetItem(label, songList);
		item->setData(RoleSongPath, s.title);
		item->setToolTip(label);
	}
	if (songList->count() > 0)
		songList->setCurrentRow(0);
}

void LyricsDock::searchTextChanged(const QString &text)
{
	refreshSongList(text);
}

void LyricsDock::runSearch()
{
	// A new search sets follow = false so screen does not change until Show this is pressed
	follow = false;
	listIndex = -1;
	setlist->clearSelection();

	if (songList->count() > 0) {
		auto *item = songList->item(0);
		openSong(item->data(RoleSongPath).toString(), false);
	}
	updateLiveButton();
}

void LyricsDock::songSelected(QListWidgetItem *item)
{
	if (!item)
		return;
	follow = false; // opening a song sets follow = false
	listIndex = -1;
	setlist->clearSelection();
	openSong(item->data(RoleSongPath).toString(), false);
	updateLiveButton();
}

bool LyricsDock::openSong(const QString &titleOrPath, bool fromList)
{
	Q_UNUSED(fromList);
	Song s = SongLibrary::instance().songByTitle(titleOrPath);
	if (!s.isValid())
		s = SongLibrary::instance().songByPath(titleOrPath);
	if (!s.isValid())
		return false;

	currentSong = s;
	rebuildSlides();
	rebuildSectionButtons();
	updatePreview();
	saveConfig();
	status->setText(QStringLiteral("Opened “%1”.").arg(currentSong.title));
	return true;
}

void LyricsDock::linesPerSlideChanged()
{
	linesPerSlide = linesBox->currentData().toInt();
	rebuildSlides();
	updatePreview();
	if (live && follow)
		pushToOutput();
	saveConfig();
}

void LyricsDock::rebuildSlides()
{
	QSignalBlocker b(slideList);
	slideList->clear();
	currentSlides = generateSlides(currentSong, linesPerSlide);

	QString lastSec;
	for (size_t i = 0; i < currentSlides.size(); i++) {
		const auto &sl = currentSlides[i];
		if (!sl.sectionName.isEmpty() && sl.sectionName != lastSec) {
			auto *hdr = new QListWidgetItem(QStringLiteral("—— %1 ——").arg(sl.sectionName.toUpper()),
							slideList);
			hdr->setFlags(Qt::NoItemFlags);
			hdr->setTextAlignment(Qt::AlignCenter);
			hdr->setForeground(QBrush(QColor(106, 112, 122))); // slate kicker color
			QFont f = hdr->font();
			f.setBold(true);
			hdr->setFont(f);
			hdr->setData(RoleSlideIndex, -1);
			lastSec = sl.sectionName;
		}

		auto *item = new QListWidgetItem(sl.listLabel(), slideList);
		item->setData(RoleSlideIndex, static_cast<int>(i));
		item->setData(RoleSectionCode, sl.sectionCode);
		item->setData(RoleSectionName, sl.sectionName);
		item->setToolTip(sl.text());
	}

	b.unblock();

	currentSlideIndex = currentSlides.empty() ? -1 : 0;
	for (int row = 0; row < slideList->count(); row++) {
		if (slideList->item(row)->data(RoleSlideIndex).toInt() == 0) {
			slideList->setCurrentRow(row);
			break;
		}
	}
}

void LyricsDock::rebuildSectionButtons()
{
	QLayout *layout = sectionContainer->layout();
	QLayoutItem *child = nullptr;
	while ((child = layout->takeAt(0)) != nullptr) {
		delete child->widget();
		delete child;
	}

	std::vector<std::pair<QString, QString>> uniqueSections;
	for (const auto &sl : currentSlides) {
		if (sl.sectionCode.isEmpty())
			continue;
		bool exists = false;
		for (const auto &p : uniqueSections) {
			if (p.first == sl.sectionCode) {
				exists = true;
				break;
			}
		}
		if (!exists)
			uniqueSections.push_back({sl.sectionCode, sl.sectionName});
	}

	for (const auto &sec : uniqueSections) {
		const QString code = sec.first;
		const QString name = sec.second;
		auto *btn = new QPushButton(code, sectionContainer);
		btn->setToolTip(name);
		btn->setMaximumWidth(48);
		connect(btn, &QPushButton::clicked, this, [this, code]() { jumpToSection(code); });
		layout->addWidget(btn);
	}
}

void LyricsDock::jumpToSection(const QString &shortCode)
{
	const QString target = shortCode.trimmed().toUpper();
	if (target.isEmpty())
		return;

	for (int row = 0; row < slideList->count(); row++) {
		auto *item = slideList->item(row);
		if (item->data(RoleSlideIndex).toInt() >= 0) {
			const QString code = item->data(RoleSectionCode).toString().toUpper();
			const QString name = item->data(RoleSectionName).toString().toUpper();
			if (code == target || name == target || name.contains(target) || target.contains(name)) {
				slideList->setCurrentRow(row);
				slideList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
				if (live && follow)
					pushToOutput();
				break;
			}
		}
	}
}

void LyricsDock::slideSelectionChanged()
{
	auto *item = slideList->currentItem();
	if (!item)
		return;
	const int idx = item->data(RoleSlideIndex).toInt();
	if (idx >= 0) {
		currentSlideIndex = idx;
		updatePreview();
		if (live && follow)
			pushToOutput();
		updateLiveButton();
	}
}

/* ------------------------------------------------------------------ preview & live */

void LyricsDock::updatePreview()
{
	if (currentSlideIndex < 0 || currentSlideIndex >= static_cast<int>(currentSlides.size())) {
		preview->setText(QStringLiteral("<span style='color:gray'>The selected lyric appears here.</span>"));
		return;
	}

	const SongSlide &sl = currentSlides[currentSlideIndex];
	QString html = QStringLiteral("<b>%1</b>").arg(sl.kicker().toHtmlEscaped());
	for (const QString &ln : sl.lines)
		html += QStringLiteral("<br>%1").arg(ln.toHtmlEscaped());
	if (!sl.ccli.isEmpty())
		html += QStringLiteral("<br><span style='color:gray'>CCLI %1</span>").arg(sl.ccli.toHtmlEscaped());
	preview->setText(html);
}

void LyricsDock::updateLiveButton()
{
	if (live && blanked) {
		liveBadge->setText(QStringLiteral("<span style='color:#FFA500'>●</span> <b>BLANK</b>"));
		liveButton->setText(QStringLiteral("On screen"));
	} else if (live && follow) {
		liveBadge->setText(QStringLiteral("<span style='color:#FF2D2D'>●</span> <b>LIVE</b>"));
		liveButton->setText(QStringLiteral("On screen"));
	} else if (live) {
		liveBadge->setText(QStringLiteral("<span style='color:#FF2D2D'>●</span> <b>LIVE</b>"));
		liveButton->setText(QStringLiteral("Show this"));
	} else {
		liveBadge->clear();
		liveButton->setText(QStringLiteral("Show"));
	}
	blankButton->setChecked(blanked);
}

void LyricsDock::pushToOutput()
{
	if (currentSlideIndex < 0 || currentSlideIndex >= static_cast<int>(currentSlides.size()))
		return;
	const SongSlide &sl = currentSlides[currentSlideIndex];
	DisplayContent c;
	c.visible = live && !blanked;
	c.reference = sl.kicker();
	c.ccli = sl.ccli;
	c.entries.push_back({QString(), sl.text()});
	DisplayState::get(Channel::Lyrics).set(c);
}

void LyricsDock::goLive()
{
	if (currentSlides.empty()) {
		status->setText(QStringLiteral("Select a song first."));
		return;
	}
	blanked = false;
	blankButton->setChecked(false);
	live = true;
	follow = true;
	pushToOutput();
	updateLiveButton();
}

void LyricsDock::clearLive()
{
	live = false;
	follow = false;
	blanked = false;
	blankButton->setChecked(false);
	DisplayState::get(Channel::Lyrics).setVisible(false);
	updateLiveButton();
}

void LyricsDock::toggleBlank()
{
	blanked = !blanked;
	blankButton->setChecked(blanked);
	if (blanked) {
		DisplayState::get(Channel::Lyrics).setVisible(false);
	} else if (live) {
		pushToOutput();
	}
	updateLiveButton();
}

void LyricsDock::next()
{
	// Step to next slide or rollover to next song in setlist
	if (currentSlideIndex + 1 < static_cast<int>(currentSlides.size())) {
		const int targetIdx = currentSlideIndex + 1;
		for (int row = 0; row < slideList->count(); row++) {
			if (slideList->item(row)->data(RoleSlideIndex).toInt() == targetIdx) {
				slideList->setCurrentRow(row);
				slideList->scrollToItem(slideList->item(row));
				return;
			}
		}
	} else if (listIndex >= 0 && listIndex + 1 < setlist->count()) {
		openListIndex(listIndex + 1, false);
		if (live && follow)
			pushToOutput();
	}
}

void LyricsDock::prev()
{
	if (currentSlideIndex > 0) {
		const int targetIdx = currentSlideIndex - 1;
		for (int row = 0; row < slideList->count(); row++) {
			if (slideList->item(row)->data(RoleSlideIndex).toInt() == targetIdx) {
				slideList->setCurrentRow(row);
				slideList->scrollToItem(slideList->item(row));
				return;
			}
		}
	} else if (listIndex > 0) {
		openListIndex(listIndex - 1, true);
		if (live && follow)
			pushToOutput();
	}
}

/* ------------------------------------------------------------------ setlist */

bool LyricsDock::openListIndex(int index, bool fromEnd)
{
	if (index < 0 || index >= setlist->count())
		return false;
	listIndex = index;
	setlist->clearSelection();
	setlist->setCurrentRow(index);
	const QString songTitle = setlist->item(index)->text();
	const bool ok = openSong(songTitle, true);
	if (ok && fromEnd && !currentSlides.empty()) {
		const int lastIdx = static_cast<int>(currentSlides.size()) - 1;
		for (int row = slideList->count() - 1; row >= 0; row--) {
			if (slideList->item(row)->data(RoleSlideIndex).toInt() == lastIdx) {
				slideList->setCurrentRow(row);
				break;
			}
		}
	}
	return ok;
}

void LyricsDock::openListItem(QListWidgetItem *item)
{
	follow = false; // opening a setlist item prepares it; Show puts it on screen
	openListIndex(setlist->row(item), false);
	updateLiveButton();
}

void LyricsDock::addToList()
{
	if (currentSong.title.isEmpty()) {
		status->setText(QStringLiteral("Open a song first."));
		return;
	}
	setlist->addItem(currentSong.title);
	saveConfig();
}

void LyricsDock::removeFromList()
{
	delete setlist->currentItem();
	listIndex = -1;
	saveConfig();
}

void LyricsDock::loadList()
{
	const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Open setlist"), QString(),
							  QStringLiteral("Text files (*.txt);;All files (*)"));
	if (file.isEmpty())
		return;
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;

	setlist->clear();
	QTextStream in(&f);
	int loaded = 0;
	while (!in.atEnd()) {
		const QString line = in.readLine().trimmed();
		if (!line.isEmpty()) {
			setlist->addItem(line);
			loaded++;
		}
	}
	listIndex = -1;
	status->setText(
		QStringLiteral("Loaded %1 song%2.").arg(loaded).arg(loaded == 1 ? QString() : QStringLiteral("s")));
	saveConfig();
}

void LyricsDock::saveList()
{
	const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Save setlist"),
							  QStringLiteral("Setlist.txt"),
							  QStringLiteral("Text files (*.txt)"));
	if (file.isEmpty())
		return;
	QFile f(file);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
		return;
	QTextStream out(&f);
	for (int i = 0; i < setlist->count(); i++)
		out << setlist->item(i)->text() << "\n";
}

/* ------------------------------------------------------------------ dialogs */

void LyricsDock::newSongDialog()
{
	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("New Song"));
	auto *form = new QFormLayout(&dlg);

	auto *titleEdit = new QLineEdit(&dlg);
	auto *authorEdit = new QLineEdit(&dlg);
	auto *ccliEdit = new QLineEdit(&dlg);
	auto *orderEdit = new QLineEdit(&dlg);
	orderEdit->setPlaceholderText(QStringLiteral("e.g. V1 C V2 C B C"));

	auto *hint = new QLabel(QStringLiteral("Tip: Use [Verse 1], [Chorus], [Bridge] to label sections.<br>"
					       "Leave blank lines between slides."),
				&dlg);
	hint->setWordWrap(true);

	auto *lyricsEdit = new QPlainTextEdit(&dlg);
	lyricsEdit->setPlaceholderText(QStringLiteral("[Verse 1]\nFirst line\nSecond line\n\n[Chorus]\nChorus line\n"));
	lyricsEdit->setMinimumHeight(180);

	form->addRow(hint);
	form->addRow(QStringLiteral("Title"), titleEdit);
	form->addRow(QStringLiteral("Author"), authorEdit);
	form->addRow(QStringLiteral("CCLI #"), ccliEdit);
	form->addRow(QStringLiteral("Order (optional)"), orderEdit);
	form->addRow(QStringLiteral("Lyrics"), lyricsEdit);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	form->addRow(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString title = titleEdit->text().trimmed();
	if (title.isEmpty()) {
		QMessageBox::warning(this, QStringLiteral("New Song"), QStringLiteral("Please enter a song title."));
		return;
	}

	Song parsed = parseNativeSong(lyricsEdit->toPlainText(), title);
	parsed.title = title;
	parsed.author = authorEdit->text().trimmed();
	parsed.ccli = ccliEdit->text().trimmed();
	parsed.order = orderEdit->text().trimmed();

	QString savedPath, err;
	if (!SongLibrary::instance().saveSong(parsed, title, &savedPath, &err)) {
		QMessageBox::warning(this, QStringLiteral("New Song"), err);
		return;
	}

	refreshSongList();
	openSong(title, false);
	status->setText(QStringLiteral("Created song “%1”.").arg(title));
}

void LyricsDock::editSongDialog()
{
	if (currentSong.title.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Edit Song"),
					 QStringLiteral("Select a song to edit first."));
		return;
	}

	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("Edit Song"));
	auto *form = new QFormLayout(&dlg);

	auto *titleEdit = new QLineEdit(currentSong.title, &dlg);
	auto *authorEdit = new QLineEdit(currentSong.author, &dlg);
	auto *ccliEdit = new QLineEdit(currentSong.ccli, &dlg);
	auto *orderEdit = new QLineEdit(currentSong.order, &dlg);

	auto *hint = new QLabel(QStringLiteral("Tip: Use [Verse 1], [Chorus], [Bridge] to label sections.<br>"
					       "Leave blank lines between slides."),
				&dlg);
	hint->setWordWrap(true);

	// Generate lyrics content body for editing
	QString bodyContent;
	QTextStream ts(&bodyContent);
	for (size_t si = 0; si < currentSong.sections.size(); si++) {
		const auto &sec = currentSong.sections[si];
		if (!sec.name.isEmpty())
			ts << "[" << sec.name << "]\n";
		for (size_t ri = 0; ri < sec.rawSlides.size(); ri++) {
			for (const QString &ln : sec.rawSlides[ri])
				ts << ln << "\n";
			if (ri + 1 < sec.rawSlides.size())
				ts << "\n";
		}
		if (si + 1 < currentSong.sections.size())
			ts << "\n";
	}

	auto *lyricsEdit = new QPlainTextEdit(&dlg);
	lyricsEdit->setPlainText(bodyContent);
	lyricsEdit->setMinimumHeight(180);

	form->addRow(hint);
	form->addRow(QStringLiteral("Title"), titleEdit);
	form->addRow(QStringLiteral("Author"), authorEdit);
	form->addRow(QStringLiteral("CCLI #"), ccliEdit);
	form->addRow(QStringLiteral("Order (optional)"), orderEdit);
	form->addRow(QStringLiteral("Lyrics"), lyricsEdit);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	form->addRow(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString title = titleEdit->text().trimmed();
	if (title.isEmpty()) {
		QMessageBox::warning(this, QStringLiteral("Edit Song"), QStringLiteral("Please enter a song title."));
		return;
	}

	Song parsed = parseNativeSong(lyricsEdit->toPlainText(), title);
	parsed.title = title;
	parsed.author = authorEdit->text().trimmed();
	parsed.ccli = ccliEdit->text().trimmed();
	parsed.order = orderEdit->text().trimmed();
	parsed.filePath = currentSong.filePath;

	QString savedPath, err;
	const QFileInfo fi(currentSong.filePath);
	if (!SongLibrary::instance().saveSong(parsed, fi.fileName(), &savedPath, &err)) {
		QMessageBox::warning(this, QStringLiteral("Edit Song"), err);
		return;
	}

	refreshSongList();
	openSong(title, false);
	status->setText(QStringLiteral("Updated song “%1”.").arg(title));
}

void LyricsDock::importSongs()
{
	const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import Songs"), QString(),
								QStringLiteral("Songs (*.txt *.xml);;All files (*)"));
	if (files.isEmpty())
		return;

	int imported = 0, skipped = 0;
	QString firstTitle;

	QApplication::setOverrideCursor(Qt::WaitCursor);
	for (const QString &f : files) {
		QString title, err;
		if (SongLibrary::instance().importSongFile(f, &title, &err)) {
			imported++;
			if (firstTitle.isEmpty())
				firstTitle = title;
		} else {
			skipped++;
		}
	}
	QApplication::restoreOverrideCursor();

	refreshSongList();
	if (!firstTitle.isEmpty())
		openSong(firstTitle, false);

	const QString msg = QStringLiteral("Imported %1 song%2.%3")
				    .arg(imported)
				    .arg(imported == 1 ? QString() : QStringLiteral("s"))
				    .arg(skipped > 0 ? QStringLiteral(" (%1 skipped)").arg(skipped) : QString());
	status->setText(msg);
	QMessageBox::information(this, QStringLiteral("Import Songs"), msg);
}

void LyricsDock::openSongsFolder()
{
	QDesktopServices::openUrl(QUrl::fromLocalFile(SongLibrary::instance().userDir()));
}
