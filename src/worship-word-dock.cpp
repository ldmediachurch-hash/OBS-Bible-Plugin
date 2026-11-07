/*
LD Worship & Word for OBS
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "worship-word-dock.hpp"
#include "scripture/scripture_bible_structure.hpp"

#include <obs-module.h>

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
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

ConfidenceBarWidget::ConfidenceBarWidget(QWidget *parent) : QWidget(parent)
{
	setFixedHeight(2);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ConfidenceBarWidget::setConfidence(int conf)
{
	m_confidence = std::clamp(conf, 0, 100);
	update();
}

void ConfidenceBarWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	p.fillRect(rect(), QColor(255, 255, 255, 30));

	const int fillW = (width() * m_confidence) / 100;
	if (fillW > 0) {
		p.fillRect(QRect(0, 0, fillW, height()), QColor(255, 255, 255, 230));
	}
}

WorshipWordDock::WorshipWordDock(QWidget *parent) : QWidget(parent)
{
	s_instance = this;
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	// 1. Top Title & Segmented Control (Worship | Word)
	m_segmentedContainer = new QWidget(this);
	m_segmentedContainer->setStyleSheet(QStringLiteral(
		"QWidget { background-color: rgba(255, 255, 255, 0.06); border-radius: 8px; padding: 2px; }"));
	auto *modeLayout = new QHBoxLayout(m_segmentedContainer);
	modeLayout->setContentsMargins(2, 2, 2, 2);
	modeLayout->setSpacing(2);

	m_worshipBtn = new QPushButton(QStringLiteral("Worship"), m_segmentedContainer);
	m_worshipBtn->setFixedHeight(30);
	m_worshipBtn->setCursor(Qt::PointingHandCursor);
	m_worshipBtn->setToolTip(QStringLiteral("Lyrics & Worship Mode (Tab to toggle)"));

	m_wordBtn = new QPushButton(QStringLiteral("Word"), m_segmentedContainer);
	m_wordBtn->setFixedHeight(30);
	m_wordBtn->setCursor(Qt::PointingHandCursor);
	m_wordBtn->setToolTip(QStringLiteral("Scripture & Bible Mode (Tab to toggle)"));

	connect(m_worshipBtn, &QPushButton::clicked, this, [this]() { setMode(0); });
	connect(m_wordBtn, &QPushButton::clicked, this, [this]() { setMode(1); });

	modeLayout->addWidget(m_worshipBtn, 1);
	modeLayout->addWidget(m_wordBtn, 1);
	root->addWidget(m_segmentedContainer);

	// 2. Audio Row (One compact line)
	m_audioRow = new QWidget(this);
	m_audioRow->setStyleSheet(QStringLiteral(
		"QWidget#AudioRow { background-color: rgba(255, 255, 255, 0.04); border: 1px solid rgba(255, 255, 255, 0.06); border-radius: 6px; }"));
	m_audioRow->setObjectName(QStringLiteral("AudioRow"));

	auto *audioLayout = new QHBoxLayout(m_audioRow);
	audioLayout->setContentsMargins(6, 4, 6, 4);
	audioLayout->setSpacing(6);

	m_micIcon = new QLabel(m_audioRow);
	m_micIcon->setPixmap(QIcon(QStringLiteral(":/icons/mic.svg")).pixmap(16, 16));
	m_micIcon->setToolTip(QStringLiteral("Audio Listening & Live Assist"));
	audioLayout->addWidget(m_micIcon);

	m_audioCombo = new QComboBox(m_audioRow);
	m_audioCombo->setToolTip(QStringLiteral("Select audio input device.\nNote: Use a dry pastor-mic feed (aux send, pre-fader), not the program mix with music."));
	m_audioCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(m_audioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&WorshipWordDock::audioSourceChanged);
	audioLayout->addWidget(m_audioCombo, 1);

	m_meter = new AudioMeterWidget(m_audioRow);
	audioLayout->addWidget(m_meter);

	m_statusPill = new QPushButton(m_audioRow);
	m_statusPill->setFixedHeight(24);
	m_statusPill->setCursor(Qt::PointingHandCursor);
	m_statusPill->setStyleSheet(QStringLiteral(
		"QPushButton { background-color: rgba(255, 255, 255, 0.07); color: #C9CDD4; border-radius: 12px; padding: 2px 10px; border: 1px solid rgba(255, 255, 255, 0.1); font-size: 11px; font-weight: 500; } "
		"QPushButton:hover { background-color: rgba(255, 255, 255, 0.12); }"));
	connect(m_statusPill, &QPushButton::clicked, this, &WorshipWordDock::openAudioStatusMenu);
	audioLayout->addWidget(m_statusPill);

	root->addWidget(m_audioRow);

	// 3. Suggestion Card (Mockup fidelity)
	m_suggestionCard = new QWidget(this);
	m_suggestionCard->setObjectName(QStringLiteral("SuggestionCard"));
	m_suggestionCard->setStyleSheet(QStringLiteral(
		"QWidget#SuggestionCard { background-color: #171A21; border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 8px; }"));
	m_suggestionCard->setVisible(false);

	auto *cardLayout = new QHBoxLayout(m_suggestionCard);
	cardLayout->setContentsMargins(10, 8, 8, 8);
	cardLayout->setSpacing(8);

	// Left: Kicker + Title + Confidence Bar
	auto *textCol = new QVBoxLayout();
	textCol->setContentsMargins(0, 0, 0, 0);
	textCol->setSpacing(2);

	m_suggestionKicker = new QLabel(QStringLiteral("HEARD · 95%"), m_suggestionCard);
	QFont kickerFont = font();
	kickerFont.setPointSize(7);
	kickerFont.setBold(true);
	kickerFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
	m_suggestionKicker->setFont(kickerFont);
	m_suggestionKicker->setStyleSheet(QStringLiteral("color: #8C93A0; text-transform: uppercase;"));
	textCol->addWidget(m_suggestionKicker);

	m_suggestionTitle = new QLabel(QStringLiteral("Song · Verse 1"), m_suggestionCard);
	QFont titleFont = font();
	titleFont.setPointSize(10);
	titleFont.setBold(true);
	m_suggestionTitle->setFont(titleFont);
	m_suggestionTitle->setStyleSheet(QStringLiteral("color: #FFFFFF;"));
	textCol->addWidget(m_suggestionTitle);

	m_confidenceBar = new ConfidenceBarWidget(m_suggestionCard);
	textCol->addWidget(m_confidenceBar);

	cardLayout->addLayout(textCol, 1);

	// Right: Preview, Show, Close
	m_previewBtn = new QPushButton(QStringLiteral("Preview"), m_suggestionCard);
	m_previewBtn->setFixedHeight(26);
	m_previewBtn->setStyleSheet(QStringLiteral(
		"QPushButton { background-color: rgba(255, 255, 255, 0.1); color: #E2E8F0; border-radius: 5px; padding: 2px 10px; border: none; font-size: 11px; } "
		"QPushButton:hover { background-color: rgba(255, 255, 255, 0.16); }"));
	connect(m_previewBtn, &QPushButton::clicked, this, [this]() { acceptSuggestion(false); });
	cardLayout->addWidget(m_previewBtn);

	m_showBtn = new QPushButton(QStringLiteral("Show"), m_suggestionCard);
	m_showBtn->setFixedHeight(26);
	m_showBtn->setStyleSheet(QStringLiteral(
		"QPushButton { background-color: #FFFFFF; color: #0F1115; font-weight: bold; border-radius: 5px; padding: 2px 12px; border: none; font-size: 11px; } "
		"QPushButton:hover { background-color: #E2E8F0; } "
		"QPushButton:pressed { background-color: #CBD5E1; }"));
	connect(m_showBtn, &QPushButton::clicked, this, [this]() { acceptSuggestion(true); });
	cardLayout->addWidget(m_showBtn);

	m_dismissBtn = new QToolButton(m_suggestionCard);
	m_dismissBtn->setIcon(QIcon(QStringLiteral(":/icons/close.svg")));
	m_dismissBtn->setFixedSize(22, 22);
	m_dismissBtn->setStyleSheet(QStringLiteral("border: none; color: #8C93A0;"));
	connect(m_dismissBtn, &QToolButton::clicked, this, &WorshipWordDock::dismissSuggestion);
	cardLayout->addWidget(m_dismissBtn);

	root->addWidget(m_suggestionCard);

	// Backlog Warning Guard Bar
	m_backlogBar = new QWidget(this);
	m_backlogBar->setStyleSheet(QStringLiteral(
		"background-color: rgba(220, 38, 38, 0.2); border: 1px solid rgba(220, 38, 38, 0.6); "
		"border-radius: 6px; padding: 4px 8px; margin: 2px 0px;"));
	auto *blLayout = new QHBoxLayout(m_backlogBar);
	blLayout->setContentsMargins(6, 2, 6, 2);
	blLayout->setSpacing(8);

	m_backlogLabel = new QLabel(QStringLiteral("⚠️ Falling behind (> 3s backlog)"), m_backlogBar);
	m_backlogLabel->setStyleSheet(QStringLiteral("color: #FCA5A5; font-size: 11px; font-weight: bold;"));
	blLayout->addWidget(m_backlogLabel, 1);

	auto *switchBtn = new QPushButton(QStringLiteral("Switch model"), m_backlogBar);
	switchBtn->setStyleSheet(QStringLiteral(
		"QPushButton { background-color: #DC2626; color: #FFF; border: none; border-radius: 4px; "
		"padding: 2px 8px; font-size: 10px; font-weight: bold; } "
		"QPushButton:hover { background-color: #EF4444; }"));
	connect(switchBtn, &QPushButton::clicked, this, &WorshipWordDock::switchToFasterModel);
	blLayout->addWidget(switchBtn);

	auto *blDismiss = new QToolButton(m_backlogBar);
	blDismiss->setText(QStringLiteral("✕"));
	blDismiss->setStyleSheet(QStringLiteral("color: #FCA5A5; border: none; background: transparent; font-size: 11px;"));
	connect(blDismiss, &QToolButton::clicked, this, [this]() {
		m_backlogBar->setVisible(false);
	});
	blLayout->addWidget(blDismiss);

	m_backlogBar->setVisible(false);
	root->addWidget(m_backlogBar);

	// 4. Main Stacked Widget (Worship Page & Word Page)
	m_stack = new QStackedWidget(this);

	m_lyrics = new LyricsDock(m_stack);
	m_stack->addWidget(m_lyrics); // Index 0

	m_scripture = new ScriptureDock(m_stack);
	m_stack->addWidget(m_scripture); // Index 1

	root->addWidget(m_stack, 1);

	// 5. Collapsible Transcript Debug Panel
	m_transcriptToggleBtn = new QPushButton(QStringLiteral("▶  Transcript Debug"), this);
	m_transcriptToggleBtn->setCheckable(true);
	m_transcriptToggleBtn->setChecked(false);
	m_transcriptToggleBtn->setStyleSheet(QStringLiteral(
		"QPushButton { background: transparent; color: #8C93A0; text-align: left; padding: 4px 6px; "
		"font-size: 11px; font-weight: 600; border: none; } "
		"QPushButton:hover { color: #FFFFFF; }"));
	root->addWidget(m_transcriptToggleBtn);

	m_transcriptContainer = new QWidget(this);
	auto *transLayout = new QVBoxLayout(m_transcriptContainer);
	transLayout->setContentsMargins(0, 0, 0, 0);
	transLayout->setSpacing(4);

	auto *transHeader = new QHBoxLayout();
	auto *transLabel = new QLabel(QStringLiteral("SPEECH & DETECTION LOG"), m_transcriptContainer);
	QFont headerFont = font();
	headerFont.setPointSize(8);
	headerFont.setBold(true);
	transLabel->setFont(headerFont);
	transLabel->setStyleSheet(QStringLiteral("color: #64748B; letter-spacing: 0.5px;"));
	transHeader->addWidget(transLabel);

	auto *clearBtn = new QPushButton(QStringLiteral("Clear"), m_transcriptContainer);
	clearBtn->setStyleSheet(QStringLiteral(
		"QPushButton { background: rgba(255,255,255,0.06); color: #8C93A0; border: none; border-radius: 3px; "
		"padding: 1px 8px; font-size: 10px; } QPushButton:hover { background: rgba(255,255,255,0.12); color: #FFF; }"));
	connect(clearBtn, &QPushButton::clicked, this, [this]() {
		if (m_transcriptView)
			m_transcriptView->clear();
	});
	transHeader->addWidget(clearBtn);

	auto *saveLogBtn = new QPushButton(QStringLiteral("Save log…"), m_transcriptContainer);
	saveLogBtn->setStyleSheet(QStringLiteral(
		"QPushButton { background: rgba(255,255,255,0.06); color: #8C93A0; border: none; border-radius: 3px; "
		"padding: 1px 8px; font-size: 10px; } QPushButton:hover { background: rgba(255,255,255,0.12); color: #FFF; }"));
	connect(saveLogBtn, &QPushButton::clicked, this, &WorshipWordDock::saveTranscriptLog);
	transHeader->addWidget(saveLogBtn);
	transLayout->addLayout(transHeader);

	m_transcriptView = new QPlainTextEdit(m_transcriptContainer);
	m_transcriptView->setReadOnly(true);
	m_transcriptView->setMaximumBlockCount(1000);
	m_transcriptView->setFixedHeight(120);
	m_transcriptView->setStyleSheet(QStringLiteral(
		"QPlainTextEdit { background-color: #0B0D11; color: #94A3B8; "
		"font-family: Menlo, Monaco, 'Courier New', monospace; font-size: 11px; "
		"border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 6px; padding: 4px; }"));
	transLayout->addWidget(m_transcriptView);

	m_transcriptContainer->setVisible(false);
	root->addWidget(m_transcriptContainer);

	connect(m_transcriptToggleBtn, &QPushButton::toggled, this, [this](bool checked) {
		m_transcriptContainer->setVisible(checked);
		m_transcriptToggleBtn->setText(checked ? QStringLiteral("▼  Transcript Debug")
						       : QStringLiteral("▶  Transcript Debug"));
	});

	// Connect AudioEngine signals
	connect(&AudioEngine::instance(), &AudioEngine::suggestionDetected, this,
		&WorshipWordDock::onSuggestionDetected);
	connect(&AudioEngine::instance(), &AudioEngine::rawTranscriptPass, this,
		&WorshipWordDock::onRawTranscriptPass);
	connect(&AudioEngine::instance(), &AudioEngine::scriptureRefDetected, this,
		&WorshipWordDock::onScriptureDetected);
	connect(&AudioEngine::instance(), &AudioEngine::scriptureSuggestion, this,
		&WorshipWordDock::showSuggestion);
	connect(&AudioEngine::instance(), &AudioEngine::statusChanged, this,
		[this](bool, const QString &) { updateAudioStatusPill(); });
	connect(&AudioEngine::instance(), &AudioEngine::modelLoadedChanged, this,
		[this](bool, const QString &) { updateAudioStatusPill(); });
	connect(&AudioEngine::instance(), &AudioEngine::modelDownloadProgress, this,
		[this](int) { updateAudioStatusPill(); });
	connect(&AudioEngine::instance().core(), &DetectionCore::backlogWarningChanged, this,
		&WorshipWordDock::onBacklogWarning);

	refreshAudioSources();
	updateTabStyles();
	updateAudioStatusPill();
	loadConfig();
}

WorshipWordDock::~WorshipWordDock()
{
	if (s_instance == this)
		s_instance = nullptr;
	saveConfig();
}

void WorshipWordDock::refreshAudioSources()
{
	const QString current = m_audioCombo->currentText();
	m_audioCombo->blockSignals(true);
	m_audioCombo->clear();
	m_audioCombo->addItem(QStringLiteral("Off"), QStringLiteral(""));

	QList<QString> sources = AudioEngine::availableAudioSources();
	for (const QString &s : sources) {
		m_audioCombo->addItem(s, s);
	}

	int idx = m_audioCombo->findData(current);
	if (idx >= 0)
		m_audioCombo->setCurrentIndex(idx);
	else
		m_audioCombo->setCurrentIndex(0);

	m_audioCombo->blockSignals(false);
	updateAudioStatusPill();
}

void WorshipWordDock::updateAudioStatusPill()
{
	if (AudioEngine::instance().isDownloading()) {
		m_statusPill->setText(
			QStringLiteral("● Downloading (%1%)").arg(AudioEngine::instance().downloadPercent()));
		m_statusPill->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: rgba(59, 130, 246, 0.15); color: #60A5FA; border-radius: 12px; padding: 2px 10px; border: 1px solid rgba(59, 130, 246, 0.3); font-size: 11px; font-weight: 500; }"));
	} else if (!AudioEngine::instance().isModelLoaded()) {
		m_statusPill->setText(QStringLiteral("● Loading model"));
		m_statusPill->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: rgba(245, 158, 11, 0.15); color: #F59E0B; border-radius: 12px; padding: 2px 10px; border: 1px solid rgba(245, 158, 11, 0.3); font-size: 11px; font-weight: 500; }"));
	} else if (AudioEngine::instance().isListening()) {
		m_statusPill->setText(QStringLiteral("● Listening"));
		m_statusPill->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: rgba(16, 185, 129, 0.15); color: #10B981; border-radius: 12px; padding: 2px 10px; border: 1px solid rgba(16, 185, 129, 0.3); font-size: 11px; font-weight: 500; }"));
	} else {
		m_statusPill->setText(QStringLiteral("● Off"));
		m_statusPill->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: rgba(255, 255, 255, 0.07); color: #8C93A0; border-radius: 12px; padding: 2px 10px; border: 1px solid rgba(255, 255, 255, 0.1); font-size: 11px; font-weight: 500; }"));
	}
}

void WorshipWordDock::openAudioStatusMenu()
{
	auto *menu = new QMenu(this);
	menu->addAction(QStringLiteral("Refresh Audio Devices"), this, &WorshipWordDock::refreshAudioSources);

	menu->addSeparator();
	QString modelInfo = AudioEngine::instance().modelStatusText();
	auto *actModel = menu->addAction(modelInfo);
	actModel->setEnabled(false);

	QString backend = AudioEngine::instance().core().backendInfo();
	if (!backend.isEmpty()) {
		auto *actBackend = menu->addAction(QStringLiteral("Backend: %1").arg(backend));
		actBackend->setEnabled(false);
	}

	menu->addSeparator();
	auto *modelMenu = menu->addMenu(QStringLiteral("Whisper Model"));
	const QString cur = AudioEngine::instance().currentModelChoice();

	auto *actTurbo = modelMenu->addAction(QStringLiteral("Large v3 Turbo (Q5_0) [Apple Silicon]"));
	actTurbo->setCheckable(true);
	actTurbo->setChecked(cur == QStringLiteral("large-v3-turbo-q5_0"));
	connect(actTurbo, &QAction::triggered, [this]() {
		AudioEngine::instance().setModelChoice(QStringLiteral("large-v3-turbo-q5_0"));
		saveConfig();
	});

	auto *actSmall = modelMenu->addAction(QStringLiteral("Small.en (Q5_1) [Standard]"));
	actSmall->setCheckable(true);
	actSmall->setChecked(cur == QStringLiteral("small.en-q5_1"));
	connect(actSmall, &QAction::triggered, [this]() {
		AudioEngine::instance().setModelChoice(QStringLiteral("small.en-q5_1"));
		saveConfig();
	});

	auto *actBase = modelMenu->addAction(QStringLiteral("Base.en [Weak PCs]"));
	actBase->setCheckable(true);
	actBase->setChecked(cur == QStringLiteral("base.en"));
	connect(actBase, &QAction::triggered, [this]() {
		AudioEngine::instance().setModelChoice(QStringLiteral("base.en"));
		saveConfig();
	});

	auto *actBrowse = modelMenu->addAction(QStringLiteral("Browse Custom Model…"));
	connect(actBrowse, &QAction::triggered, [this]() {
		QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select Whisper GGML Model File"),
							    QString(),
							    QStringLiteral("GGML Model (*.bin);;All Files (*.*)"));
		if (!path.isEmpty()) {
			AudioEngine::instance().loadCustomModelFile(path);
			saveConfig();
		}
	});

	menu->addSeparator();
	menu->addAction(QStringLiteral("Test with audio file…"), this, &WorshipWordDock::testWithAudioFile);

	menu->addSeparator();
	auto *actFeedNote = menu->addAction(QStringLiteral("Tip: Use a dry pastor-mic feed (aux send, pre-fader), not the program mix with music."));
	actFeedNote->setEnabled(false);

	menu->exec(m_statusPill->mapToGlobal(QPoint(0, m_statusPill->height())));
}

void WorshipWordDock::testWithAudioFile()
{
	QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select WAV Audio File to Test"),
						    QString(),
						    QStringLiteral("WAV Files (*.wav);;All Files (*.*)"));
	if (path.isEmpty())
		return;

	if (m_transcriptContainer && !m_transcriptContainer->isVisible()) {
		m_transcriptContainer->setVisible(true);
		m_transcriptToggleBtn->setText(QStringLiteral("▼  Transcript Debug"));
		m_transcriptToggleBtn->setChecked(true);
	}

	if (m_transcriptView) {
		m_transcriptView->appendPlainText(QStringLiteral("=== Starting Audio File Test (Real-Time): %1 ===").arg(QFileInfo(path).fileName()));
		m_transcriptView->ensureCursorVisible();
	}

	std::thread([path]() {
		AudioEngine::instance().runAudioFileTest(path, true);
	}).detach();
}

void WorshipWordDock::audioSourceChanged(int index)
{
	QString sourceName = m_audioCombo->itemData(index).toString();
	AudioEngine::instance().setAudioSource(sourceName);
	updateAudioStatusPill();
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
		"QPushButton { background-color: rgba(255, 255, 255, 0.18); color: #FFFFFF; font-weight: bold; border-radius: 6px; border: none; font-size: 12px; }");
	const QString inactiveStyle = QStringLiteral(
		"QPushButton { background-color: transparent; color: rgba(255, 255, 255, 0.55); font-weight: 500; border-radius: 6px; border: none; font-size: 12px; } "
		"QPushButton:hover { background-color: rgba(255, 255, 255, 0.08); color: rgba(255, 255, 255, 0.85); }");

	m_worshipBtn->setStyleSheet(idx == 0 ? activeStyle : inactiveStyle);
	m_wordBtn->setStyleSheet(idx == 1 ? activeStyle : inactiveStyle);
}

void WorshipWordDock::onSuggestionDetected(Channel ch, const QString &ref, const QString &text, int confidence,
					   bool tentative, bool isNextSlide)
{
	m_suggestedChannel = ch;
	m_suggestedRef = ref;
	m_suggestedText = text;
	m_confidence = confidence;
	m_isTentative = tentative;
	m_isNext = isNextSlide;

	QString kicker = tentative ? QStringLiteral("TENTATIVE · %1%").arg(confidence)
				   : QStringLiteral("HEARD · %1%").arg(confidence);
	if (isNextSlide) {
		kicker = QStringLiteral("NEXT SLIDE · %1%").arg(confidence);
	}
	m_suggestionKicker->setText(kicker);
	m_suggestionTitle->setText(ref);

	if (m_confidenceBar) {
		m_confidenceBar->setConfidence(confidence);
	}

	m_suggestionCard->setVisible(true);

	// Auto-dismiss after 15 seconds
	QTimer::singleShot(15000, this, [this]() {
		if (m_suggestionCard->isVisible())
			m_suggestionCard->setVisible(false);
	});
}

void WorshipWordDock::showSuggestion(const ldsd::Ref &ref)
{
	m_suggestedChannel = Channel::Scripture;
	m_currentScriptureRef = ref;
	m_suggestedRef = QString::fromStdString(ref.label);
	m_suggestedText = QString::fromStdString(ref.text);
	m_confidence = static_cast<int>(std::round(ref.confidence * 100.0));
	m_isTentative = (ref.status == "preview");
	m_isNext = false;

	QString kicker;
	if (ref.status == "preview") {
		kicker = QStringLiteral("PREVIEW · %1%").arg(m_confidence);
	} else if (ref.status == "refined") {
		kicker = QStringLiteral("REFINED · %1%").arg(m_confidence);
	} else {
		kicker = QStringLiteral("HEARD · %1%").arg(m_confidence);
	}

	if (ref.kind == "quote") {
		kicker = QStringLiteral("QUOTE · ") + kicker;
	}

	m_suggestionKicker->setText(kicker);
	m_suggestionTitle->setText(QStringLiteral("%1 %2%").arg(QString::fromStdString(ref.label)).arg(m_confidence));

	if (m_confidenceBar) {
		m_confidenceBar->setConfidence(m_confidence);
	}

	if (m_showBtn) {
		m_showBtn->setEnabled(ref.status != "preview");
	}

	m_suggestionCard->setVisible(true);

	if (m_suggestionTimer) {
		m_suggestionTimer->stop();
	} else {
		m_suggestionTimer = new QTimer(this);
		m_suggestionTimer->setSingleShot(true);
		connect(m_suggestionTimer, &QTimer::timeout, this, [this]() {
			if (m_suggestionCard->isVisible())
				m_suggestionCard->setVisible(false);
		});
	}
	m_suggestionTimer->start(15000);
}

void WorshipWordDock::acceptSuggestion(bool goLive)
{
	m_suggestionCard->setVisible(false);

	if (m_suggestedChannel == Channel::Lyrics) {
		setMode(0);
		m_lyrics->openSong(m_suggestedRef, false);
		if (goLive) {
			m_lyrics->goLive();
		}
	} else {
		setMode(1);
		int bookNum = 0;
		if (!m_currentScriptureRef.book.empty()) {
			const auto &bStruct = ldsd::bibleStructure();
			for (size_t i = 0; i < bStruct.size(); ++i) {
				if (bStruct[i].id == m_currentScriptureRef.book) {
					bookNum = static_cast<int>(i + 1);
					break;
				}
			}
		}
		if (bookNum > 0) {
			m_scripture->openReference(bookNum, m_currentScriptureRef.chapter,
						   m_currentScriptureRef.verse, m_currentScriptureRef.verseEnd);
		} else {
			m_scripture->showQuery(m_suggestedRef, true);
		}

		if (goLive && m_currentScriptureRef.status != "preview") {
			m_scripture->goLive();
		}
	}
}

void WorshipWordDock::dismissSuggestion()
{
	m_suggestionCard->setVisible(false);
}

void WorshipWordDock::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Tab) {
		toggleMode();
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}

void WorshipWordDock::onRawTranscriptPass(const QString &text, double timestampSec)
{
	if (!m_transcriptView || text.trimmed().isEmpty())
		return;

	QString timeStr = timestampSec > 0.0
		? QStringLiteral("%1s").arg(QString::number(timestampSec, 'f', 1))
		: QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"));

	m_transcriptView->appendPlainText(
		QStringLiteral("[%1] [Whisper] \"%2\"").arg(timeStr, text.trimmed()));
	m_transcriptView->ensureCursorVisible();
}

void WorshipWordDock::onScriptureDetected(const QString &label, int confidence, const QString &status,
					  const QString &kind, double timestampSec)
{
	if (!m_transcriptView)
		return;

	QString timeStr = timestampSec > 0.0
		? QStringLiteral("%1s").arg(QString::number(timestampSec, 'f', 1))
		: QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"));

	QString logMsg = QStringLiteral("[%1] ==> DETECT: %2 (%3%) [status=%4, kind=%5]")
		.arg(timeStr, label, QString::number(confidence), status, kind);

	m_transcriptView->appendPlainText(logMsg);
	m_transcriptView->ensureCursorVisible();
}

void WorshipWordDock::loadConfig()
{
	QFile file(dockConfigPath());
	if (!file.open(QIODevice::ReadOnly))
		return;

	QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
	if (obj.contains(QStringLiteral("model"))) {
		QString modelId = obj.value(QStringLiteral("model")).toString();
		if (!modelId.isEmpty()) {
			AudioEngine::instance().setModelChoice(modelId);
		}
	}
}

void WorshipWordDock::saveConfig()
{
	QDir().mkpath(QFileInfo(dockConfigPath()).dir().absolutePath());
	QFile file(dockConfigPath());
	if (!file.open(QIODevice::WriteOnly))
		return;

	QJsonObject obj;
	obj[QStringLiteral("model")] = AudioEngine::instance().currentModelChoice();
	file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

void WorshipWordDock::saveTranscriptLog()
{
	if (!m_transcriptView)
		return;
	QString defaultName = QStringLiteral("ld-transcript-log-%1.txt")
		.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
	QString path = QFileDialog::getSaveFileName(
		this, QStringLiteral("Save Transcript Log"),
		defaultName,
		QStringLiteral("Text Files (*.txt);;All Files (*.*)"));
	if (path.isEmpty())
		return;
	QFile f(path);
	if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QTextStream out(&f);
		out << m_transcriptView->toPlainText();
		f.close();
	}
}

void WorshipWordDock::onBacklogWarning(bool fallingBehind, double backlogSec)
{
	if (m_backlogBar) {
		m_backlogBar->setVisible(fallingBehind);
		if (fallingBehind && m_backlogLabel) {
			m_backlogLabel->setText(QStringLiteral("⚠️ Falling behind (%1s backlog)")
						.arg(QString::number(backlogSec, 'f', 1)));
		}
	}
	if (m_statusPill) {
		if (fallingBehind) {
			m_statusPill->setText(QStringLiteral("Falling behind"));
			m_statusPill->setStyleSheet(QStringLiteral(
				"QPushButton { background: #DC2626; color: #FFF; border: none; "
				"border-radius: 10px; padding: 2px 8px; font-size: 10px; font-weight: bold; }"));
		} else {
			updateAudioStatusPill();
		}
	}
}

void WorshipWordDock::switchToFasterModel()
{
	QString current = AudioEngine::instance().currentModelChoice();
	if (current.contains(QStringLiteral("large"))) {
		AudioEngine::instance().setModelChoice(QStringLiteral("small.en-q5_1"));
	} else {
		AudioEngine::instance().setModelChoice(QStringLiteral("base.en"));
	}
	saveConfig();
	if (m_backlogBar)
		m_backlogBar->setVisible(false);
}
