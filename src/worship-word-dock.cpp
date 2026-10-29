/*
LD Worship & Word for OBS
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
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
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

// Custom Confidence Bar Widget
class ConfidenceBarWidget : public QWidget {
public:
	explicit ConfidenceBarWidget(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFixedHeight(2);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void setConfidence(int conf)
	{
		m_confidence = std::clamp(conf, 0, 100);
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);

		p.fillRect(rect(), QColor(255, 255, 255, 30));

		const int fillW = (width() * m_confidence) / 100;
		if (fillW > 0) {
			p.fillRect(QRect(0, 0, fillW, height()), QColor(255, 255, 255, 230));
		}
	}

private:
	int m_confidence = 95;
};

} // namespace

WorshipWordDock::WorshipWordDock(QWidget *parent) : QWidget(parent)
{
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
	m_audioCombo->setToolTip(QStringLiteral("Select audio input device"));
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
	connect(&AudioEngine::instance(), &AudioEngine::statusChanged, this,
		[this](bool, const QString &) { updateAudioStatusPill(); });
	connect(&AudioEngine::instance(), &AudioEngine::modelLoadedChanged, this,
		[this](bool, const QString &) { updateAudioStatusPill(); });

	refreshAudioSources();
	updateTabStyles();
	updateAudioStatusPill();
	loadConfig();
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
	if (!AudioEngine::instance().isModelLoaded()) {
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

	menu->exec(m_statusPill->mapToGlobal(QPoint(0, m_statusPill->height())));
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

	auto *bar = qobject_cast<ConfidenceBarWidget *>(m_confidenceBar);
	if (bar) {
		bar->setConfidence(confidence);
	}

	m_suggestionCard->setVisible(true);

	// Auto-dismiss after 15 seconds
	QTimer::singleShot(15000, this, [this]() {
		if (m_suggestionCard->isVisible())
			m_suggestionCard->setVisible(false);
	});
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
		m_scripture->showQuery(m_suggestedRef, false);
		if (goLive) {
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

void WorshipWordDock::loadConfig() {}
void WorshipWordDock::saveConfig() {}
