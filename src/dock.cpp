/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "dock.hpp"
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
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Roles { RoleBook = Qt::UserRole + 1, RoleChapter, RoleVerse };

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

// Arrow keys / Enter in the search box drive the results list.
class SearchKeys : public QObject {
public:
	SearchKeys(QListWidget *list, QObject *parent) : QObject(parent), results(list) {}

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (ev->type() == QEvent::KeyPress) {
			auto *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up) {
				int row = results->currentRow() + (ke->key() == Qt::Key_Down ? 1 : -1);
				if (row >= 0 && row < results->count())
					results->setCurrentRow(row);
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}

private:
	QListWidget *results;
};

} // namespace

ScriptureDock::ScriptureDock(QWidget *parent) : QWidget(parent)
{
	setMinimumWidth(280);
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	searchEdit = new QLineEdit(this);
	searchEdit->setPlaceholderText(QStringLiteral("Rom 8:28, Psalm 23, or words…"));
	searchEdit->setClearButtonEnabled(true);
	root->addWidget(searchEdit);

	auto *tRow = new QHBoxLayout();
	primaryBox = new QComboBox(this);
	primaryBox->setToolTip(QStringLiteral("Translation"));
	secondaryBox = new QComboBox(this);
	secondaryBox->setToolTip(QStringLiteral("Second translation shown alongside (optional)"));
	auto *more = new QToolButton(this);
	more->setText(QStringLiteral("Bibles"));
	more->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(more);
	menu->addAction(QStringLiteral("Import Bible (XML or JSON)…"), this, &ScriptureDock::importBible);
	menu->addAction(QStringLiteral("Remove an imported Bible…"), this, &ScriptureDock::removeBible);
	menu->addSeparator();
	menu->addAction(QStringLiteral("Open Bibles folder"), this, &ScriptureDock::openBiblesFolder);
	more->setMenu(menu);
	tRow->addWidget(primaryBox, 1);
	tRow->addWidget(new QLabel(QStringLiteral("+"), this));
	tRow->addWidget(secondaryBox, 1);
	tRow->addWidget(more);
	root->addLayout(tRow);

	results = new QListWidget(this);
	results->setUniformItemSizes(true);
	results->setTextElideMode(Qt::ElideRight);
	results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	results->setWordWrap(false);
	root->addWidget(results, 3);

	preview = new QLabel(this);
	preview->setWordWrap(true);
	preview->setFrameShape(QFrame::StyledPanel);
	preview->setMargin(8);
	preview->setTextFormat(Qt::RichText);
	preview->setMinimumHeight(70);
	preview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	root->addWidget(preview, 2);

	auto *ctl = new QHBoxLayout();
	auto *prevBtn = new QPushButton(QStringLiteral("◀"), this);
	prevBtn->setToolTip(QStringLiteral("Previous verse"));
	liveButton = new QPushButton(QStringLiteral("Show"), this);
	liveButton->setToolTip(QStringLiteral("Put the selected verse on screen"));
	clearButton = new QPushButton(QStringLiteral("Clear"), this);
	clearButton->setToolTip(QStringLiteral("Take the lower third off screen"));
	auto *nextBtn = new QPushButton(QStringLiteral("▶"), this);
	nextBtn->setToolTip(QStringLiteral("Next verse"));
	prevBtn->setMaximumWidth(44);
	nextBtn->setMaximumWidth(44);
	ctl->addWidget(prevBtn);
	ctl->addWidget(liveButton, 1);
	ctl->addWidget(clearButton, 1);
	ctl->addWidget(nextBtn);
	root->addLayout(ctl);

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

	auto *listHeader = new QLabel(QStringLiteral("<b>Service list</b>"), this);
	root->addWidget(listHeader);
	serviceList = new QListWidget(this);
	serviceList->setDragDropMode(QAbstractItemView::InternalMove);
	root->addWidget(serviceList, 2);
	auto *lRow = new QHBoxLayout();
	auto *addBtn = new QPushButton(QStringLiteral("Add"), this);
	addBtn->setToolTip(QStringLiteral("Add the current passage to the service list"));
	auto *remBtn = new QPushButton(QStringLiteral("Remove"), this);
	auto *loadBtn = new QPushButton(QStringLiteral("Open…"), this);
	auto *saveBtn = new QPushButton(QStringLiteral("Save…"), this);
	lRow->addWidget(addBtn);
	lRow->addWidget(remBtn);
	lRow->addWidget(loadBtn);
	lRow->addWidget(saveBtn);
	root->addLayout(lRow);

	searchEdit->installEventFilter(new SearchKeys(results, this));

	connect(searchEdit, &QLineEdit::returnPressed, this, &ScriptureDock::runSearch);
	connect(results, &QListWidget::currentRowChanged, this, &ScriptureDock::selectionChanged);
	connect(results, &QListWidget::itemActivated, this, [this](QListWidgetItem *) { goLive(); });
	connect(primaryBox, &QComboBox::currentIndexChanged, this, &ScriptureDock::translationChanged);
	connect(secondaryBox, &QComboBox::currentIndexChanged, this, &ScriptureDock::translationChanged);
	connect(prevBtn, &QPushButton::clicked, this, &ScriptureDock::prev);
	connect(nextBtn, &QPushButton::clicked, this, &ScriptureDock::next);
	connect(liveButton, &QPushButton::clicked, this, &ScriptureDock::goLive);
	connect(clearButton, &QPushButton::clicked, this, &ScriptureDock::clearLive);
	connect(addBtn, &QPushButton::clicked, this, &ScriptureDock::addToList);
	connect(remBtn, &QPushButton::clicked, this, &ScriptureDock::removeFromList);
	connect(loadBtn, &QPushButton::clicked, this, &ScriptureDock::loadList);
	connect(saveBtn, &QPushButton::clicked, this, &ScriptureDock::saveList);
	connect(serviceList, &QListWidget::itemActivated, this, &ScriptureDock::openListItem);

	registerHotkeys();
	loadConfig();
	updatePreview();
}

ScriptureDock::~ScriptureDock()
{
	if (obs_initialized()) {
		for (obs_hotkey_id id : {hkNext, hkPrev, hkLive, hkClear}) {
			if (id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(id);
		}
	}
}

void ScriptureDock::registerHotkeys()
{
	hkNext = obs_hotkey_register_frontend("ld_scripture.next", "LD Scripture: Next verse", hotkeyCallback<kNext>,
					      this);
	hkPrev = obs_hotkey_register_frontend("ld_scripture.prev", "LD Scripture: Previous verse",
					      hotkeyCallback<kPrev>, this);
	hkLive = obs_hotkey_register_frontend("ld_scripture.show", "LD Scripture: Show", hotkeyCallback<kLive>, this);
	hkClear =
		obs_hotkey_register_frontend("ld_scripture.clear", "LD Scripture: Clear", hotkeyCallback<kClear>, this);
}

/* ------------------------------------------------------------------ config */

void ScriptureDock::loadConfig()
{
	OBSDataAutoRelease cfg = obs_data_create_from_json_file_safe(configPath().toUtf8().constData(), "bak");
	QString p, s;
	if (cfg) {
		p = QString::fromUtf8(obs_data_get_string(cfg, "primary"));
		s = QString::fromUtf8(obs_data_get_string(cfg, "secondary"));
		OBSDataArrayAutoRelease list = obs_data_get_array(cfg, "service_list");
		for (size_t i = 0; i < obs_data_array_count(list); i++) {
			OBSDataAutoRelease item = obs_data_array_item(list, i);
			serviceList->addItem(QString::fromUtf8(obs_data_get_string(item, "ref")));
		}
		const std::pair<obs_hotkey_id, const char *> keys[] = {{hkNext, "hk_next"},
								       {hkPrev, "hk_prev"},
								       {hkLive, "hk_show"},
								       {hkClear, "hk_clear"}};
		for (const auto &k : keys) {
			OBSDataArrayAutoRelease arr = obs_data_get_array(cfg, k.second);
			if (arr)
				obs_hotkey_load(k.first, arr);
		}
	}
	refreshTranslations(p, s);
	status->setText(QStringLiteral("Type a reference and press Enter."));
}

void ScriptureDock::saveConfig()
{
	ensureConfigDir();
	OBSDataAutoRelease cfg = obs_data_create();
	if (auto b = primary())
		obs_data_set_string(cfg, "primary", b->abbrev.toUtf8().constData());
	if (auto b = secondary())
		obs_data_set_string(cfg, "secondary", b->abbrev.toUtf8().constData());
	OBSDataArrayAutoRelease list = obs_data_array_create();
	for (int i = 0; i < serviceList->count(); i++) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "ref", serviceList->item(i)->text().toUtf8().constData());
		obs_data_array_push_back(list, item);
	}
	obs_data_set_array(cfg, "service_list", list);
	const std::pair<obs_hotkey_id, const char *> keys[] = {{hkNext, "hk_next"},
							       {hkPrev, "hk_prev"},
							       {hkLive, "hk_show"},
							       {hkClear, "hk_clear"}};
	for (const auto &k : keys) {
		OBSDataArrayAutoRelease arr = obs_hotkey_save(k.first);
		obs_data_set_array(cfg, k.second, arr);
	}
	obs_data_save_json_safe(cfg, configPath().toUtf8().constData(), "tmp", "bak");
}

/* ------------------------------------------------------------------ translations */

BiblePtr ScriptureDock::primary() const
{
	return BibleLibrary::instance().byAbbrev(primaryBox->currentData().toString());
}

BiblePtr ScriptureDock::secondary() const
{
	const QString a = secondaryBox->currentData().toString();
	return a.isEmpty() ? nullptr : BibleLibrary::instance().byAbbrev(a);
}

void ScriptureDock::refreshTranslations(const QString &primaryAbbrev, const QString &secondaryAbbrev)
{
	QSignalBlocker b1(primaryBox), b2(secondaryBox);
	primaryBox->clear();
	secondaryBox->clear();
	secondaryBox->addItem(QStringLiteral("None"), QString());
	for (const auto &b : BibleLibrary::instance().bibles()) {
		primaryBox->addItem(b->abbrev, b->abbrev);
		primaryBox->setItemData(primaryBox->count() - 1, b->name, Qt::ToolTipRole);
		secondaryBox->addItem(b->abbrev, b->abbrev);
		secondaryBox->setItemData(secondaryBox->count() - 1, b->name, Qt::ToolTipRole);
	}
	int pi = primaryBox->findData(primaryAbbrev);
	primaryBox->setCurrentIndex(pi >= 0 ? pi : 0);
	int si = secondaryBox->findData(secondaryAbbrev);
	secondaryBox->setCurrentIndex(si >= 0 ? si : 0);
}

void ScriptureDock::translationChanged()
{
	VerseRef v = currentVerse();
	if (v.book > 0 && !searchIsKeyword)
		setChapter(v.book, v.chapter, v.verse);
	updatePreview();
	if (live && follow)
		pushToOutput();
	saveConfig();
}

/* ------------------------------------------------------------------ search */

void ScriptureDock::runSearch()
{
	// a new search never changes what's on screen until Show is pressed
	follow = false;
	listIndex = -1;
	serviceList->clearSelection();
	showQuery(searchEdit->text(), false);
}

bool ScriptureDock::showQuery(const QString &query, bool fromList)
{
	auto bible = primary();
	if (!bible)
		return false;
	QApplication::setOverrideCursor(Qt::WaitCursor);
	QString err;
	const bool ok = bible->ensureLoaded(&err);
	QApplication::restoreOverrideCursor();
	if (!ok) {
		status->setText(err);
		return false;
	}
	auto &lib = BibleLibrary::instance();
	ParsedRef r = lib.parse(query, bible.get());
	if (r.valid) {
		if (!bible->book(r.book)) {
			status->setText(
				QStringLiteral("%1 isn't in %2.").arg(Canon::englishName(r.book), bible->abbrev));
			return false;
		}
		if (r.chapter < 1 || r.chapter > bible->chapterCount(r.book)) {
			status->setText(QStringLiteral("%1 has %2 chapters.")
						.arg(lib.bookName(bible.get(), r.book))
						.arg(bible->chapterCount(r.book)));
			return false;
		}
		const int count = bible->verseCount(r.book, r.chapter);
		if (r.v1 > count) {
			status->setText(QStringLiteral("%1 has %2 verses.")
						.arg(lib.formatRef(bible.get(), r.book, r.chapter, 0))
						.arg(count));
			return false;
		}
		searchIsKeyword = false;
		rangeEnd = (fromList && r.v2 > r.v1) ? r.v2 : 0;
		setChapter(r.book, r.chapter, r.v1 > 0 ? r.v1 : 1);
		status->setText(r.v2 > r.v1 && r.v1 > 0
					? QStringLiteral("Showing %1. Use ▶ to step through.")
						  .arg(lib.formatRef(bible.get(), r.book, r.chapter, r.v1, r.v2))
					: QString());
		results->setFocus();
		return true;
	}
	if (query.trimmed().size() < 3) {
		status->setText(QStringLiteral("Couldn't read that reference."));
		return false;
	}
	QApplication::setOverrideCursor(Qt::WaitCursor);
	const auto found = lib.search(bible.get(), query, 300);
	QApplication::restoreOverrideCursor();
	searchIsKeyword = true;
	rangeEnd = 0;
	results->clear();
	for (const auto &v : found) {
		QString t = bible->verse(v.book, v.chapter, v.verse);
		auto *item = new QListWidgetItem(
			QStringLiteral("%1  %2").arg(lib.formatRef(bible.get(), v.book, v.chapter, v.verse), t),
			results);
		item->setData(RoleBook, v.book);
		item->setData(RoleChapter, v.chapter);
		item->setData(RoleVerse, v.verse);
		item->setToolTip(t);
	}
	status->setText(found.empty() ? QStringLiteral("No verses found for “%1”.").arg(query.trimmed())
				      : QStringLiteral("%1 verse%2 found%3.")
						.arg(found.size())
						.arg(found.size() == 1 ? QString() : QStringLiteral("s"))
						.arg(found.size() >= 300 ? QStringLiteral(" (first 300)") : QString()));
	if (!found.empty()) {
		results->setCurrentRow(0);
		results->setFocus();
	}
	return !found.empty();
}

void ScriptureDock::setChapter(int book, int chapter, int selectVerse)
{
	auto bible = primary();
	if (!bible || !bible->ensureLoaded())
		return;
	QSignalBlocker block(results);
	results->clear();
	const int count = bible->verseCount(book, chapter);
	for (int v = 1; v <= count; v++) {
		QString t = bible->verse(book, chapter, v);
		auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(v).arg(t), results);
		item->setData(RoleBook, book);
		item->setData(RoleChapter, chapter);
		item->setData(RoleVerse, v);
		item->setToolTip(t);
	}
	if (count == 0) {
		status->setText(QStringLiteral("%1 doesn't have that chapter.").arg(bible->abbrev));
	}
	block.unblock();
	results->setCurrentRow(std::max(0, std::min(selectVerse, count) - 1));
	results->scrollToItem(results->currentItem(), QAbstractItemView::PositionAtCenter);
}

VerseRef ScriptureDock::currentVerse() const
{
	VerseRef v;
	if (auto *item = results->currentItem()) {
		v.book = item->data(RoleBook).toInt();
		v.chapter = item->data(RoleChapter).toInt();
		v.verse = item->data(RoleVerse).toInt();
	}
	return v;
}

void ScriptureDock::selectionChanged()
{
	updatePreview();
	if (live && follow)
		pushToOutput();
	updateLiveButton();
}

void ScriptureDock::updateLiveButton()
{
	if (live && follow) {
		liveBadge->setText(QStringLiteral("<span style='color:#FF2D2D'>●</span> <b>LIVE</b>"));
		liveButton->setText(QStringLiteral("On screen"));
	} else if (live) {
		liveBadge->setText(QStringLiteral("<span style='color:#FF2D2D'>●</span> <b>LIVE</b>"));
		liveButton->setText(QStringLiteral("Show this"));
	} else {
		liveBadge->clear();
		liveButton->setText(QStringLiteral("Show"));
	}
}

void ScriptureDock::updatePreview()
{
	const VerseRef v = currentVerse();
	auto p = primary();
	if (v.book == 0 || !p) {
		preview->setText(QStringLiteral("<span style='color:gray'>The selected verse appears here.</span>"));
		return;
	}
	auto &lib = BibleLibrary::instance();
	QString html =
		QStringLiteral("<b>%1</b>").arg(lib.formatRef(p.get(), v.book, v.chapter, v.verse).toHtmlEscaped());
	auto add = [&](const BiblePtr &b) {
		if (!b)
			return;
		b->ensureLoaded();
		QString t = b->verse(v.book, v.chapter, v.verse);
		if (t.isEmpty())
			t = QStringLiteral("(not in %1)").arg(b->abbrev);
		html += QStringLiteral("<br><span style='color:gray'>%1</span> %2")
				.arg(b->abbrev.toHtmlEscaped(), t.toHtmlEscaped());
	};
	add(p);
	add(secondary());
	preview->setText(html);
}

/* ------------------------------------------------------------------ output */

void ScriptureDock::pushToOutput()
{
	const VerseRef v = currentVerse();
	auto p = primary();
	if (v.book == 0 || !p)
		return;
	auto &lib = BibleLibrary::instance();
	DisplayContent c;
	c.visible = live;
	c.reference = lib.formatRef(p.get(), v.book, v.chapter, v.verse);
	c.entries.push_back({p->abbrev, p->verse(v.book, v.chapter, v.verse)});
	if (auto s = secondary()) {
		s->ensureLoaded();
		c.entries.push_back({s->abbrev, s->verse(v.book, v.chapter, v.verse)});
	}
	DisplayState::get().set(c);
}

void ScriptureDock::goLive()
{
	if (currentVerse().book == 0) {
		status->setText(QStringLiteral("Search for a verse first."));
		return;
	}
	live = true;
	follow = true;
	pushToOutput();
	updateLiveButton();
}

void ScriptureDock::clearLive()
{
	live = false;
	follow = false;
	DisplayState::get().setVisible(false);
	updateLiveButton();
}

void ScriptureDock::next()
{
	int row = results->currentRow();
	// end of a service-list range ("1 Corinthians 13:4-7") -> next item on the list
	if (listIndex >= 0 && rangeEnd > 0 && currentVerse().verse >= rangeEnd &&
	    listIndex + 1 < serviceList->count()) {
		openListIndex(listIndex + 1, false);
		return;
	}
	if (row + 1 < results->count()) {
		results->setCurrentRow(row + 1);
		results->scrollToItem(results->currentItem());
		return;
	}
	if (listIndex >= 0)
		openListIndex(listIndex + 1, false);
}

void ScriptureDock::prev()
{
	int row = results->currentRow();
	if (row > 0) {
		results->setCurrentRow(row - 1);
		results->scrollToItem(results->currentItem());
		return;
	}
	if (listIndex > 0)
		openListIndex(listIndex - 1, true);
}

/* ------------------------------------------------------------------ service list */

bool ScriptureDock::openListIndex(int index, bool fromEnd)
{
	Q_UNUSED(fromEnd);
	if (index < 0 || index >= serviceList->count())
		return false;
	listIndex = index;
	serviceList->clearSelection();
	serviceList->setCurrentRow(index);
	searchEdit->setText(serviceList->item(index)->text());
	return showQuery(serviceList->item(index)->text(), true);
}

void ScriptureDock::openListItem(QListWidgetItem *item)
{
	follow = false; // opening a list item prepares it; Show puts it on screen
	openListIndex(serviceList->row(item), false);
	updateLiveButton();
}

void ScriptureDock::addToList()
{
	const VerseRef v = currentVerse();
	if (v.book == 0) {
		status->setText(QStringLiteral("Search for a verse first."));
		return;
	}
	auto &lib = BibleLibrary::instance();
	// keep a typed range ("Romans 8:28-30"), otherwise the selected verse; English names so any translation reads it
	QString ref = lib.formatRef(nullptr, v.book, v.chapter, v.verse);
	ParsedRef r = lib.parse(searchEdit->text(), primary().get());
	if (!searchIsKeyword && r.valid && r.book == v.book && r.chapter == v.chapter && r.v2 > r.v1 && r.v1 == v.verse)
		ref = lib.formatRef(nullptr, r.book, r.chapter, r.v1, r.v2);
	serviceList->addItem(ref);
	saveConfig();
}

void ScriptureDock::removeFromList()
{
	delete serviceList->currentItem();
	listIndex = -1;
	saveConfig();
}

void ScriptureDock::loadList()
{
	const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Open service list"), QString(),
							  QStringLiteral("Text files (*.txt);;All files (*)"));
	if (file.isEmpty())
		return;
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;
	auto &lib = BibleLibrary::instance();
	serviceList->clear();
	int skipped = 0;
	QTextStream in(&f);
	while (!in.atEnd()) {
		const QString line = in.readLine().trimmed();
		if (line.isEmpty())
			continue;
		ParsedRef r = lib.parse(line, primary().get());
		if (r.valid)
			serviceList->addItem(lib.formatRef(nullptr, r.book, r.chapter, r.v1, r.v2));
		else
			skipped++;
	}
	listIndex = -1;
	status->setText(QStringLiteral("Loaded %1 passage%2%3.")
				.arg(serviceList->count())
				.arg(serviceList->count() == 1 ? QString() : QStringLiteral("s"))
				.arg(skipped ? QStringLiteral(" (%1 lines skipped)").arg(skipped) : QString()));
	saveConfig();
}

void ScriptureDock::saveList()
{
	const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Save service list"),
							  QStringLiteral("Service list.txt"),
							  QStringLiteral("Text files (*.txt)"));
	if (file.isEmpty())
		return;
	QFile f(file);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
		return;
	QTextStream out(&f);
	for (int i = 0; i < serviceList->count(); i++)
		out << serviceList->item(i)->text() << "\n";
}

/* ------------------------------------------------------------------ Bibles */

void ScriptureDock::importBible()
{
	const QString file =
		QFileDialog::getOpenFileName(this, QStringLiteral("Import a Bible"), QString(),
					     QStringLiteral("Bibles (*.xml *.json *.zef *.osis *.xmm);;All files (*)"));
	if (file.isEmpty())
		return;
	QString err, name, abbrev;
	QApplication::setOverrideCursor(Qt::WaitCursor);
	BiblePtr parsed = parseBibleFile(file, &err, &name, &abbrev);
	QApplication::restoreOverrideCursor();
	if (!parsed) {
		QMessageBox::warning(this, QStringLiteral("Import Bible"), err);
		return;
	}
	int verses = 0;
	for (const auto &b : parsed->books)
		for (const auto &c : b.chapters)
			verses += static_cast<int>(c.size());
	const BibleBook *first = parsed->books.empty() ? nullptr : &parsed->books.front();
	QString sample;
	if (first && !first->chapters.empty() && !first->chapters.front().isEmpty())
		sample = QStringLiteral("%1 1:1 — %2").arg(first->name, first->chapters.front().first().left(90));

	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("Import Bible"));
	auto *form = new QFormLayout(&dlg);
	auto *nameEdit = new QLineEdit(name, &dlg);
	auto *abbrevEdit = new QLineEdit(abbrev, &dlg);
	abbrevEdit->setMaxLength(12);
	auto *info = new QLabel(QStringLiteral("Found %1 books and %2 verses.<br><i>%3</i>")
					.arg(parsed->books.size())
					.arg(verses)
					.arg(sample.toHtmlEscaped()),
				&dlg);
	info->setWordWrap(true);
	form->addRow(info);
	form->addRow(QStringLiteral("Name"), nameEdit);
	form->addRow(QStringLiteral("Short name"), abbrevEdit);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	form->addRow(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	if (dlg.exec() != QDialog::Accepted)
		return;

	auto &lib = BibleLibrary::instance();
	const QString a = abbrevEdit->text().trimmed();
	if (auto existing = lib.byAbbrev(a)) {
		const QString q =
			existing->builtIn
				? QStringLiteral("%1 comes with LD Scripture. Use your imported copy instead?").arg(a)
				: QStringLiteral("You already have %1. Replace it?").arg(a);
		if (QMessageBox::question(this, QStringLiteral("Import Bible"), q) != QMessageBox::Yes)
			return;
	}
	BiblePtr installed = lib.install(parsed, nameEdit->text(), a, &err);
	if (!installed) {
		QMessageBox::warning(this, QStringLiteral("Import Bible"), err);
		return;
	}
	refreshTranslations(installed->abbrev, secondaryBox->currentData().toString());
	translationChanged();
	status->setText(QStringLiteral("Imported %1 (%2).").arg(installed->name, installed->abbrev));
}

void ScriptureDock::removeBible()
{
	QStringList names;
	for (const auto &b : BibleLibrary::instance().bibles()) {
		if (!b->builtIn)
			names << b->abbrev;
	}
	if (names.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Remove Bible"),
					 QStringLiteral("You haven't imported any Bibles yet."));
		return;
	}
	bool ok = false;
	const QString a = QInputDialog::getItem(this, QStringLiteral("Remove Bible"),
						QStringLiteral("Remove which imported Bible?"), names, 0, false, &ok);
	if (!ok || a.isEmpty())
		return;
	QString err;
	if (!BibleLibrary::instance().remove(a, &err)) {
		QMessageBox::warning(this, QStringLiteral("Remove Bible"), err);
		return;
	}
	// a built-in Bible with the same short name comes back after a reload
	char *builtIn = obs_module_file("bibles");
	BibleLibrary::instance().load(QString::fromUtf8(builtIn), BibleLibrary::instance().userDir());
	bfree(builtIn);
	refreshTranslations(primaryBox->currentData().toString(), secondaryBox->currentData().toString());
	translationChanged();
	status->setText(QStringLiteral("Removed %1.").arg(a));
}

void ScriptureDock::openBiblesFolder()
{
	QDesktopServices::openUrl(QUrl::fromLocalFile(BibleLibrary::instance().userDir()));
}
