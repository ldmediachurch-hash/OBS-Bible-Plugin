/*
LD Scripture for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "worship-word-dock.hpp"

#include <obs-module.h>

#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString dockConfigPath()
{
	char *p = obs_module_config_path("worship-word-dock.json");
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

} // namespace

WorshipWordDock::WorshipWordDock(QWidget *parent)
	: QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(6);

	// 1. Top Mode Selector Bar (Worship vs Word)
	auto *modeBar = new QHBoxLayout();
	modeBar->setSpacing(4);

	m_worshipBtn = new QPushButton(QStringLiteral("🎵 Worship"), this);
	m_worshipBtn->setFixedHeight(32);
	m_worshipBtn->setCursor(Qt::PointingHandCursor);
	m_worshipBtn->setToolTip(QStringLiteral("Worship & Lyrics Mode (Tab to switch)"));

	m_wordBtn = new QPushButton(QStringLiteral("📖 Word"), this);
	m_wordBtn->setFixedHeight(32);
	m_wordBtn->setCursor(Qt::PointingHandCursor);
	m_wordBtn->setToolTip(QStringLiteral("Scripture & Bible Mode (Tab to switch)"));

	connect(m_worshipBtn, &QPushButton::clicked, this, [this]() { setMode(0); });
	connect(m_wordBtn, &QPushButton::clicked, this, [this]() { setMode(1); });

	modeBar->addWidget(m_worshipBtn, 1);
	modeBar->addWidget(m_wordBtn, 1);
	root->addLayout(modeBar);

	// 2. Audio Assist Bar
	m_audioBar = new QWidget(this);
	m_audioBar->setStyleSheet(QStringLiteral(
		"QWidget#AudioBar { background-color: rgba(255, 255, 255, 0.05); border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 6px; }"));
	m_audioBar->setObjectName(QStringLiteral("AudioBar"));

	auto *audioLayout = new QHBoxLayout(m_audioBar);
	audioLayout->setContentsMargins(6, 4, 6, 4);
	audioLayout->setSpacing(6);

	m_micIcon = new QLabel(QStringLiteral("🎙️"), m_audioBar);
	m_micIcon->setToolTip(QStringLiteral("Audio Listening & Live Assist"));
	audioLayout->addWidget(m_micIcon);

	m_audioCombo = new QComboBox(m_audioBar);
	m_audioCombo->setToolTip(QStringLiteral("Select audio input device for smart suggestions"));
	m_audioCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(m_audioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&WorshipWordDock::audioSourceChanged);
	audioLayout->addWidget(m_audioCombo, 1);

	m_meter = new AudioMeterWidget(m_audioBar);
	audioLayout->addWidget(m_meter);

	m_modelStatusBtn = new QToolButton(m_audioBar);
	m_modelStatusBtn->setText(AudioEngine::instance().isModelLoaded() ? QStringLiteral("🟢 AI") : QStringLiteral("📥 Model"));
	m_modelStatusBtn->setToolTip(AudioEngine::instance().modelStatusText());
	connect(m_modelStatusBtn, &QToolButton::clicked, this, [this]() {
		if (!AudioEngine::instance().isModelLoaded()) {
			m_modelStatusBtn->setText(QStringLiteral("⏳ Loading..."));
			AudioEngine::instance().loadModel();
		}
	});
	connect(&AudioEngine::instance(), &AudioEngine::modelLoadedChanged, this,
		[this](bool loaded, const QString &text) {
			m_modelStatusBtn->setText(loaded ? QStringLiteral("🟢 AI") : QStringLiteral("📥 Model"));
			m_modelStatusBtn->setToolTip(text);
		});
	audioLayout->addWidget(m_modelStatusBtn);

	m_refreshAudioBtn = new QToolButton(m_audioBar);
	m_refreshAudioBtn->setText(QStringLiteral("🔄"));
	m_refreshAudioBtn->setToolTip(QStringLiteral("Refresh Audio Devices"));
	connect(m_refreshAudioBtn, &QToolButton::clicked, this, &WorshipWordDock::refreshAudioSources);
	audioLayout->addWidget(m_refreshAudioBtn);

	root->addWidget(m_audioBar);

	// 3. Smart Suggestion Pill (Hidden until detected)
	m_suggestionPill = new QWidget(this);
	m_suggestionPill->setObjectName(QStringLiteral("SuggestionPill"));
	m_suggestionPill->setStyleSheet(QStringLiteral(
		"QWidget#SuggestionPill { background-color: rgba(16, 185, 129, 0.15); border: 1px solid rgba(16, 185, 129, 0.4); border-radius: 6px; padding: 2px; }"));
	m_suggestionPill->setVisible(false);

	auto *suggLayout = new QHBoxLayout(m_suggestionPill);
	suggLayout->setContentsMargins(8, 4, 6, 4);
	suggLayout->setSpacing(6);

	m_suggestionLabel = new QLabel(m_suggestionPill);
	m_suggestionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #10B981;"));
	suggLayout->addWidget(m_suggestionLabel, 1);

	m_previewBtn = new QPushButton(QStringLiteral("Preview"), m_suggestionPill);
	m_previewBtn->setFixedHeight(24);
	connect(m_previewBtn, &QPushButton::clicked, this, [this]() { acceptSuggestion(false); });
	suggLayout->addWidget(m_previewBtn);

	m_liveBtn = new QPushButton(QStringLiteral("Show"), m_suggestionPill);
	m_liveBtn->setFixedHeight(24);
	m_liveBtn->setStyleSheet(QStringLiteral(
		"QPushButton { background-color: #10B981; color: white; font-weight: bold; border-radius: 4px; padding: 2px 8px; }"));
	connect(m_liveBtn, &QPushButton::clicked, this, [this]() { acceptSuggestion(true); });
	suggLayout->addWidget(m_liveBtn);

	m_dismissBtn = new QToolButton(m_suggestionPill);
	m_dismissBtn->setText(QStringLiteral("✕"));
	m_dismissBtn->setFixedHeight(22);
	connect(m_dismissBtn, &QToolButton::clicked, this, &WorshipWordDock::dismissSuggestion);
	suggLayout->addWidget(m_dismissBtn);

	root->addWidget(m_suggestionPill);

	// 4. Main Stacked Widget (Worship Page & Word Page)
	m_stack = new QStackedWidget(this);

	m_lyrics = new LyricsDock(m_stack);
	m_stack->addWidget(m_lyrics); // Index 0

	m_scripture = new ScriptureDock(m_stack);
	m_stack->addWidget(m_scripture); // Index 1

	root->addWidget(m_stack, 1);

	// Connect AudioEngine signals
	connect(&AudioEngine::instance(), &AudioEngine::suggestionDetected, this,
		&WorshipWordDock::onSuggestionDetected);

	refreshAudioSources();
	loadConfig();
	updateTabStyles();
}

WorshipWordDock::~WorshipWordDock()
{
	saveConfig();
}

void WorshipWordDock::refreshAudioSources()
{
	const QString current = m_audioCombo->currentText();
	m_audioCombo->blockSignals(true);
	m_audioCombo->clear();
	m_audioCombo->addItem(QStringLiteral("🚫 Audio Off"), QStringLiteral(""));

	QList<QString> sources = AudioEngine::availableAudioSources();
	for (const QString &s : sources) {
		m_audioCombo->addItem(QStringLiteral("🎙️ %1").arg(s), s);
	}

	int idx = m_audioCombo->findData(current);
	if (idx >= 0)
		m_audioCombo->setCurrentIndex(idx);
	else
		m_audioCombo->setCurrentIndex(0);

	m_audioCombo->blockSignals(false);
}

void WorshipWordDock::audioSourceChanged(int index)
{
	QString sourceName = m_audioCombo->itemData(index).toString();
	AudioEngine::instance().setAudioSource(sourceName);
}

void WorshipWordDock::setMode(int mode)
{
	if (mode < 0 || mode > 1)
		mode = 0;
	m_stack->setCurrentIndex(mode);
	AudioEngine::instance().setChannelMode(mode == 0 ? Channel::Lyrics : Channel::Scripture);
	updateTabStyles();
}

void WorshipWordDock::toggleMode()
{
	setMode(m_stack->currentIndex() == 0 ? 1 : 0);
}

void WorshipWordDock::updateTabStyles()
{
	const int idx = m_stack->currentIndex();
	const QString activeStyle = QStringLiteral(
		"QPushButton { background-color: rgba(255, 255, 255, 0.16); color: #ffffff; font-weight: bold; border-radius: 6px; border: 1px solid rgba(255, 255, 255, 0.25); }");
	const QString inactiveStyle = QStringLiteral(
		"QPushButton { background-color: transparent; color: rgba(255, 255, 255, 0.55); font-weight: 500; border-radius: 6px; border: 1px solid transparent; } "
		"QPushButton:hover { background-color: rgba(255, 255, 255, 0.08); color: rgba(255, 255, 255, 0.85); }");

	m_worshipBtn->setStyleSheet(idx == 0 ? activeStyle : inactiveStyle);
	m_wordBtn->setStyleSheet(idx == 1 ? activeStyle : inactiveStyle);
}

void WorshipWordDock::onSuggestionDetected(Channel ch, const QString &ref, const QString &text, int confidence)
{
	m_suggestedChannel = ch;
	m_suggestedRef = ref;
	m_suggestedText = text;

	QString label = QStringLiteral("💡 %1 (%2%)").arg(ref).arg(confidence);
	m_suggestionLabel->setText(label);
	m_suggestionPill->setVisible(true);

	// Auto-dismiss after 14 seconds
	QTimer::singleShot(14000, this, [this]() {
		if (m_suggestionPill->isVisible())
			m_suggestionPill->setVisible(false);
	});
}

void WorshipWordDock::acceptSuggestion(bool goLive)
{
	m_suggestionPill->setVisible(false);
	if (m_suggestedChannel == Channel::Scripture) {
		setMode(1);
		m_scripture->showQuery(m_suggestedRef, false);
		if (goLive)
			m_scripture->goLive();
	} else {
		setMode(0);
		m_lyrics->openSong(m_suggestedRef, false);
		if (goLive)
			m_lyrics->goLive();
	}
}

void WorshipWordDock::dismissSuggestion()
{
	m_suggestionPill->setVisible(false);
}

void WorshipWordDock::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Tab && (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
		toggleMode();
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}

void WorshipWordDock::saveConfig()
{
	QJsonObject root;
	root[QStringLiteral("mode")] = m_stack->currentIndex();
	root[QStringLiteral("audioSource")] = AudioEngine::instance().currentAudioSource();

	QFile f(dockConfigPath());
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	}

	if (m_scripture)
		m_scripture->saveConfig();
	if (m_lyrics)
		m_lyrics->saveConfig();
}

void WorshipWordDock::loadConfig()
{
	QFile f(dockConfigPath());
	if (!f.open(QIODevice::ReadOnly))
		return;

	QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	if (root.contains(QStringLiteral("mode"))) {
		setMode(root.value(QStringLiteral("mode")).toInt(0));
	}
	if (root.contains(QStringLiteral("audioSource"))) {
		QString src = root.value(QStringLiteral("audioSource")).toString();
		int idx = m_audioCombo->findData(src);
		if (idx >= 0)
			m_audioCombo->setCurrentIndex(idx);
	}
}

