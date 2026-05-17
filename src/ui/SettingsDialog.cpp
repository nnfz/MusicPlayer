#include "SettingsDialog.h"
#include "GaplessAudioEngine.h"
#include "TrackItem.h"
#include <QAudioFormat>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QSettings>
#include <QInputDialog>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>
#include <QPushButton>
#include <utility>

SettingsDialog::SettingsDialog(Equalizer *eq, GaplessAudioEngine *engine,
                               QWidget *parent)
    : QDialog(parent)
    , m_eq(eq)
    , m_engine(engine)
{
    setWindowTitle("Settings");
    setMinimumSize(700, 480);
    resize(720, 500);
    setStyleSheet("QDialog { background: #1e1e1e; color: white; }");

    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    m_categoryList = new QListWidget();
    m_categoryList->setFixedWidth(160);
    m_categoryList->setStyleSheet(
        "QListWidget { background: #252525; border: none; color: white; font-size: 13px; outline: none; }"
        "QListWidget::item { padding: 10px 14px; }"
        "QListWidget::item:selected { background: #333; font-weight: bold; }"
        "QListWidget::item:hover { background: #2e2e2e; }");
    m_categoryList->addItem(QString::fromUtf8("\xF0\x9F\x8E\xA8  Appearance"));
    m_categoryList->addItem(QString::fromUtf8("\xF0\x9F\x94\x8A  Audio"));
    m_categoryList->addItem(QString::fromUtf8("\xE2\x8F\xAF  Playback"));
    m_categoryList->addItem(QString::fromUtf8("\xF0\x9F\x8E\x9B  Equalizer"));
    m_categoryList->addItem(QString::fromUtf8("\xE2\x84\xB9\xEF\xB8\x8F  About"));

    m_pages = new QStackedWidget();

    buildAppearancePage();
    buildAudioPage();
    buildPlaybackPage();
    buildEqualizerPage();
    buildAboutPage();

    mainLayout->addWidget(m_categoryList);
    mainLayout->addWidget(m_pages, 1);

    connect(m_categoryList, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_categoryList->setCurrentRow(0);

    connect(m_rowHeightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::rowHeightChanged);

    if (m_engine) {
        connect(m_engine, &GaplessAudioEngine::backendChanged,
                this, &SettingsDialog::updateBackendStatusLabel);
        connect(m_engine, &GaplessAudioEngine::decoderChanged,
            this, &SettingsDialog::updateDecoderStatusLabel);
        connect(m_engine, &GaplessAudioEngine::backendChanged,
            this, &SettingsDialog::refreshOutputDeviceList);
        connect(m_engine, &GaplessAudioEngine::outputDeviceChanged,
            this, &SettingsDialog::refreshOutputDeviceList);
    }
}

void SettingsDialog::buildAppearancePage()
{
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    QLabel *title = new QLabel("Appearance");
    title->setStyleSheet("font-size: 18px; font-weight: bold; color: white;");
    layout->addWidget(title);

    QFrame *sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #3d3d3d;");
    layout->addWidget(sep);

    QHBoxLayout *rowHeightRow = new QHBoxLayout();
    QLabel *label = new QLabel("Playlist row height");
    label->setStyleSheet("font-size: 13px; color: #ccc;");
    m_rowHeightSpin = new QSpinBox();
    m_rowHeightSpin->setRange(30, 150);
    m_rowHeightSpin->setSuffix(" px");
    m_rowHeightSpin->setStyleSheet(
        "QSpinBox { background: #333; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; font-size: 13px; min-width: 80px; }"
        "QSpinBox::up-button, QSpinBox::down-button { background: #444; border: none; width: 16px; }"
        "QSpinBox::up-arrow { image: none; border-left: 4px solid transparent; border-right: 4px solid transparent; border-bottom: 5px solid #ccc; }"
        "QSpinBox::down-arrow { image: none; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top: 5px solid #ccc; }");
    rowHeightRow->addWidget(label);
    rowHeightRow->addStretch();
    rowHeightRow->addWidget(m_rowHeightSpin);
    layout->addLayout(rowHeightRow);

    QFrame *sep2 = new QFrame();
    sep2->setFrameShape(QFrame::HLine);
    sep2->setStyleSheet("color: #3d3d3d;");
    layout->addWidget(sep2);

    QHBoxLayout *cacheRow = new QHBoxLayout();
    QLabel *cacheLabel = new QLabel("Clear Metadata Cache");
    cacheLabel->setStyleSheet("font-size: 13px; color: #ccc;");
    QPushButton *btnCache = new QPushButton("Clear Cache");
    btnCache->setStyleSheet("QPushButton { background: #333; border: 1px solid #555; color: #ccc; font-size: 13px; padding: 6px 12px; border-radius: 4px; }"
                            "QPushButton:hover { background: #444; color: white; }");
    cacheRow->addWidget(cacheLabel);
    cacheRow->addStretch();
    cacheRow->addWidget(btnCache);
    layout->addLayout(cacheRow);

    connect(btnCache, &QPushButton::clicked, this, [this, btnCache]() {
        TrackItem::clearTrackMetadataCache();
        btnCache->setText("Cleared!");
        btnCache->setEnabled(false);
        QTimer::singleShot(2000, this, [btnCache]() {
            btnCache->setText("Clear Cache");
            btnCache->setEnabled(true);
        });
    });

    layout->addStretch();
    m_pages->addWidget(page);
}

void SettingsDialog::buildAudioPage()
{
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    QLabel *title = new QLabel("Audio Output");
    title->setStyleSheet("font-size: 18px; font-weight: bold; color: white;");
    layout->addWidget(title);

    QFrame *sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #3d3d3d;");
    layout->addWidget(sep);

    QHBoxLayout *decoderRow = new QHBoxLayout();
    QLabel *decoderTitle = new QLabel("Decoder");
    decoderTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_decoderCombo = new QComboBox();
    m_decoderCombo->setMinimumWidth(260);
    m_decoderCombo->setStyleSheet(
        "QComboBox { background: #333; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #2b2b2b; color: white; selection-background-color: #0078d7; border: 1px solid #555; }");
    m_decoderCombo->addItem("FFmpeg Decoder (custom)", GaplessAudioEngine::decoderFfmpegId());
    decoderRow->addWidget(decoderTitle);
    decoderRow->addStretch();
    decoderRow->addWidget(m_decoderCombo);
    layout->addLayout(decoderRow);

    QHBoxLayout *backendRow = new QHBoxLayout();
    QLabel *backendTitle = new QLabel("Backend");
    backendTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_backendCombo = new QComboBox();
    m_backendCombo->setMinimumWidth(260);
    m_backendCombo->setStyleSheet(
        "QComboBox { background: #333; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #2b2b2b; color: white; selection-background-color: #0078d7; border: 1px solid #555; }");
    m_backendCombo->addItem("Custom WASAPI Shared", GaplessAudioEngine::backendWasapiSharedId());
    m_backendCombo->addItem("Custom WASAPI Exclusive", GaplessAudioEngine::backendWasapiExclusiveId());

    backendRow->addWidget(backendTitle);
    backendRow->addStretch();
    backendRow->addWidget(m_backendCombo);
    layout->addLayout(backendRow);

    QHBoxLayout *deviceRow = new QHBoxLayout();
    QLabel *deviceTitle = new QLabel("Output device");
    deviceTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_outputDeviceCombo = new QComboBox();
    m_outputDeviceCombo->setMinimumWidth(260);
    m_outputDeviceCombo->setStyleSheet(
        "QComboBox { background: #333; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #2b2b2b; color: white; selection-background-color: #0078d7; border: 1px solid #555; }");
    deviceRow->addWidget(deviceTitle);
    deviceRow->addStretch();
    deviceRow->addWidget(m_outputDeviceCombo);
    layout->addLayout(deviceRow);

    connect(m_outputDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_updatingOutputDeviceUi || index < 0)
            return;
        if (!m_engine)
            return;

        const QString deviceId = m_outputDeviceCombo->itemData(index).toString();
        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Audio/outputDeviceId", deviceId);
        m_engine->setOutputDevicePreferenceId(deviceId);
        refreshOutputDeviceList();
    });

    connect(m_decoderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0)
            return;

        const QString decoderId = m_decoderCombo->itemData(index).toString();
        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Audio/decoder", decoderId);

        if (m_engine) {
            m_engine->setDecoderPreferenceId(decoderId);
            updateDecoderStatusLabel();
            updateFormatLabel();
        }
    });

    connect(m_backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0)
            return;

        const QString backendId = m_backendCombo->itemData(index).toString();
        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Audio/backend", backendId);

        if (m_engine) {
            m_engine->setBackendPreferenceId(backendId);
            updateBackendStatusLabel();
            updateFormatLabel();
        }
    });

    QLabel *infoNote = new QLabel(
        "Strict custom mode is enabled: playback uses FFmpeg decoder with custom WASAPI output only.");
    infoNote->setStyleSheet("font-size: 12px; color: #aaa;");
    infoNote->setWordWrap(true);
    layout->addWidget(infoNote);

    QFrame *sep2 = new QFrame();
    sep2->setFrameShape(QFrame::HLine);
    sep2->setStyleSheet("color: #3d3d3d;");
    layout->addWidget(sep2);

    QHBoxLayout *activeBackendRow = new QHBoxLayout();
    QLabel *activeBackendTitle = new QLabel("Active backend");
    activeBackendTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_backendStatusLabel = new QLabel("—");
    m_backendStatusLabel->setStyleSheet("font-size: 13px; color: #1db954;");
    activeBackendRow->addWidget(activeBackendTitle);
    activeBackendRow->addStretch();
    activeBackendRow->addWidget(m_backendStatusLabel);
    layout->addLayout(activeBackendRow);

    QHBoxLayout *activeDecoderRow = new QHBoxLayout();
    QLabel *activeDecoderTitle = new QLabel("Active decoder");
    activeDecoderTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_decoderStatusLabel = new QLabel("FFmpeg Decoder");
    m_decoderStatusLabel->setStyleSheet("font-size: 13px; color: #1db954;");
    activeDecoderRow->addWidget(activeDecoderTitle);
    activeDecoderRow->addStretch();
    activeDecoderRow->addWidget(m_decoderStatusLabel);
    layout->addLayout(activeDecoderRow);

    QHBoxLayout *activeDeviceRow = new QHBoxLayout();
    QLabel *activeDeviceTitle = new QLabel("Active device");
    activeDeviceTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_outputDeviceStatusLabel = new QLabel("System Default");
    m_outputDeviceStatusLabel->setStyleSheet("font-size: 13px; color: #1db954;");
    activeDeviceRow->addWidget(activeDeviceTitle);
    activeDeviceRow->addStretch();
    activeDeviceRow->addWidget(m_outputDeviceStatusLabel);
    layout->addLayout(activeDeviceRow);

    QHBoxLayout *infoRow = new QHBoxLayout();
    QLabel *infoTitle = new QLabel("Current format");
    infoTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_formatInfoLabel = new QLabel("—");
    m_formatInfoLabel->setStyleSheet("font-size: 13px; color: #0078d7;");
    infoRow->addWidget(infoTitle);
    infoRow->addStretch();
    infoRow->addWidget(m_formatInfoLabel);
    layout->addLayout(infoRow);

    layout->addStretch();

    QSettings settings("MyCompany", "MusicPlayer");
    QString savedDecoder = settings.value("Audio/decoder", GaplessAudioEngine::decoderFfmpegId()).toString();
    if (savedDecoder == QStringLiteral("ffmpeg")
        || savedDecoder == QStringLiteral("qt")
        || savedDecoder == QStringLiteral("qt-decoder")) {
        savedDecoder = GaplessAudioEngine::decoderFfmpegId();
    }
    QString savedBackend = settings.value("Audio/backend", GaplessAudioEngine::backendWasapiSharedId()).toString();
    if (savedBackend == QStringLiteral("wasapi-custom")
        || savedBackend == QStringLiteral("qt")
        || savedBackend == QStringLiteral("qtmultimedia")
        || savedBackend == QStringLiteral("qt multimedia")) {
        savedBackend = GaplessAudioEngine::backendWasapiSharedId();
    }
    const QString savedDeviceId = settings.value("Audio/outputDeviceId", QString()).toString();

    if (m_engine) {
        if (m_engine->decoderPreferenceId() != savedDecoder)
            m_engine->setDecoderPreferenceId(savedDecoder);
        if (m_engine->backendPreferenceId() != savedBackend)
            m_engine->setBackendPreferenceId(savedBackend);
        if (m_engine->outputDevicePreferenceId() != savedDeviceId)
            m_engine->setOutputDevicePreferenceId(savedDeviceId);
    }

    for (int i = 0; i < m_decoderCombo->count(); ++i) {
        if (m_decoderCombo->itemData(i).toString() == savedDecoder) {
            m_decoderCombo->setCurrentIndex(i);
            break;
        }
    }

    for (int i = 0; i < m_backendCombo->count(); ++i) {
        if (m_backendCombo->itemData(i).toString() == savedBackend) {
            m_backendCombo->setCurrentIndex(i);
            break;
        }
    }

    updateDecoderStatusLabel();
    updateBackendStatusLabel();
    refreshOutputDeviceList();
    updateFormatLabel();

    m_pages->addWidget(page);
}

void SettingsDialog::buildPlaybackPage()
{
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    QLabel *title = new QLabel("Playback");
    title->setStyleSheet("font-size: 18px; font-weight: bold; color: white;");
    layout->addWidget(title);

    QFrame *sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #3d3d3d;");
    layout->addWidget(sep);

    QLabel *speedTitle = new QLabel("Speed");
    speedTitle->setStyleSheet("font-size: 14px; font-weight: bold; color: white;");
    layout->addWidget(speedTitle);

    QHBoxLayout *playbackRateHeaderRow = new QHBoxLayout();
    QLabel *playbackRateTitle = new QLabel("Playback rate");
    playbackRateTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_playbackRateValueLabel = new QLabel("1.00 x");
    m_playbackRateValueLabel->setStyleSheet("font-size: 13px; color: #1db954; font-weight: bold;");
    playbackRateHeaderRow->addWidget(playbackRateTitle);
    playbackRateHeaderRow->addStretch();
    playbackRateHeaderRow->addWidget(m_playbackRateValueLabel);
    layout->addLayout(playbackRateHeaderRow);

    QHBoxLayout *ratePresetsRow = new QHBoxLayout();
    ratePresetsRow->setSpacing(6);
    const QString presetBtnStyle =
        "QPushButton { background: #2a2a2a; color: #ccc; border: 1px solid #444; border-radius: 4px;"
        " font-size: 12px; padding: 3px 0; }"
        "QPushButton:hover { background: #333; border-color: #666; color: white; }"
        "QPushButton:pressed { background: #1db954; border-color: #1db954; color: white; }";
    for (auto [label, value] : std::initializer_list<std::pair<const char*, int>>{
             {"0.85x", 85}, {"1x", 100}, {"1.25x", 125}}) {
        QPushButton *btn = new QPushButton(label);
        btn->setStyleSheet(presetBtnStyle);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, value] {
            m_playbackRateSlider->setValue(value);
        });
        ratePresetsRow->addWidget(btn);
    }
    layout->addLayout(ratePresetsRow);

    m_playbackRateSlider = new QSlider(Qt::Horizontal);
    m_playbackRateSlider->setRange(50, 200);
    m_playbackRateSlider->setSingleStep(1);
    m_playbackRateSlider->setPageStep(5);
    m_playbackRateSlider->setTickInterval(10);
    m_playbackRateSlider->setTickPosition(QSlider::TicksBelow);
    m_playbackRateSlider->setStyleSheet(
        "QSlider::groove:horizontal { height: 6px; background: #3a3a3a; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: #1db954; border-radius: 3px; }"
        "QSlider::add-page:horizontal { background: #555; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #e0e0e0; width: 14px; margin: -5px 0; border-radius: 7px; }"
        "QSlider::handle:horizontal:hover { background: white; }");
    layout->addWidget(m_playbackRateSlider);

    QHBoxLayout *rateHintsRow = new QHBoxLayout();
    QLabel *minRateHint = new QLabel("0.50 x");
    minRateHint->setStyleSheet("font-size: 11px; color: #888;");
    QLabel *maxRateHint = new QLabel("2.00 x");
    maxRateHint->setStyleSheet("font-size: 11px; color: #888;");
    rateHintsRow->addWidget(minRateHint);
    rateHintsRow->addStretch();
    rateHintsRow->addWidget(maxRateHint);
    layout->addLayout(rateHintsRow);

    if (m_engine) {
        connect(m_engine, &GaplessAudioEngine::playbackRateChanged, this, [this](float rate) {
            const int percent = qRound(rate * 100);
            if (m_playbackRateSlider->value() != percent) {
                QSignalBlocker blocker(m_playbackRateSlider);
                m_playbackRateSlider->setValue(percent);
            }
            if (m_playbackRateValueLabel)
                m_playbackRateValueLabel->setText(QString::number(rate, 'f', 2) + " x");
        });
    }

    QHBoxLayout *crossfadeHeaderRow = new QHBoxLayout();
    QLabel *crossfadeTitle = new QLabel("Crossfade");
    crossfadeTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_crossfadeValueLabel = new QLabel("3 s");
    m_crossfadeValueLabel->setStyleSheet("font-size: 13px; color: #1db954; font-weight: bold;");
    crossfadeHeaderRow->addWidget(crossfadeTitle);
    crossfadeHeaderRow->addStretch();
    crossfadeHeaderRow->addWidget(m_crossfadeValueLabel);
    layout->addLayout(crossfadeHeaderRow);

    m_crossfadeSlider = new QSlider(Qt::Horizontal);
    m_crossfadeSlider->setRange(0, 8);
    m_crossfadeSlider->setSingleStep(1);
    m_crossfadeSlider->setPageStep(1);
    m_crossfadeSlider->setTickInterval(1);
    m_crossfadeSlider->setTickPosition(QSlider::TicksBelow);
    m_crossfadeSlider->setStyleSheet(
        "QSlider::groove:horizontal { height: 6px; background: #3a3a3a; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: #1db954; border-radius: 3px; }"
        "QSlider::add-page:horizontal { background: #555; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #e0e0e0; width: 14px; margin: -5px 0; border-radius: 7px; }"
        "QSlider::handle:horizontal:hover { background: white; }");
    layout->addWidget(m_crossfadeSlider);

    QHBoxLayout *crossfadeHintsRow = new QHBoxLayout();
    QLabel *minCrossfadeHint = new QLabel("0 s");
    minCrossfadeHint->setStyleSheet("font-size: 11px; color: #888;");
    QLabel *maxCrossfadeHint = new QLabel("8 s");
    maxCrossfadeHint->setStyleSheet("font-size: 11px; color: #888;");
    crossfadeHintsRow->addWidget(minCrossfadeHint);
    crossfadeHintsRow->addStretch();
    crossfadeHintsRow->addWidget(maxCrossfadeHint);
    layout->addLayout(crossfadeHintsRow);

    connect(m_playbackRateSlider, &QSlider::valueChanged, this, [this](int ratePercent) {
        const int normalizedPercent = qBound(50, ratePercent, 200);
        const double normalizedRate = normalizedPercent / 100.0;

        if (m_playbackRateValueLabel)
            m_playbackRateValueLabel->setText(QString::number(normalizedRate, 'f', 2) + " x");

        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Playback/playbackRate", normalizedRate);

        if (m_engine) {
            m_engine->setPlaybackRate(normalizedRate);
        }
    });

    connect(m_crossfadeSlider, &QSlider::valueChanged, this, [this](int seconds) {
        const int normalizedSeconds = qBound(0, seconds, 8);

        if (m_crossfadeValueLabel)
            m_crossfadeValueLabel->setText(QString::number(normalizedSeconds) + " s");

        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Playback/crossfadeSeconds", normalizedSeconds);
        emit crossfadeDurationChanged(normalizedSeconds);
    });

    {
        QSettings settings("MyCompany", "MusicPlayer");
        const double savedPlaybackRate = qBound(0.5, settings.value("Playback/playbackRate", 1.0).toDouble(), 2.0);
        const int savedRatePercent = qBound(50, qRound(savedPlaybackRate * 100.0), 200);
        const int savedCrossfadeSeconds = qBound(0, settings.value("Playback/crossfadeSeconds", 3).toInt(), 8);

        {
            QSignalBlocker rateBlocker(m_playbackRateSlider);
            m_playbackRateSlider->setValue(savedRatePercent);
        }

        {
            QSignalBlocker crossfadeBlocker(m_crossfadeSlider);
            m_crossfadeSlider->setValue(savedCrossfadeSeconds);
        }

        m_playbackRateValueLabel->setText(QString::number(savedRatePercent / 100.0, 'f', 2) + " x");
        m_crossfadeValueLabel->setText(QString::number(savedCrossfadeSeconds) + " s");
    }

    QFrame *speedSep = new QFrame();
    speedSep->setFrameShape(QFrame::HLine);
    speedSep->setStyleSheet("color: #3d3d3d;");
    layout->addWidget(speedSep);

    QLabel *shuffleTitle = new QLabel("Shuffle");
    shuffleTitle->setStyleSheet("font-size: 14px; font-weight: bold; color: white;");
    layout->addWidget(shuffleTitle);

    QHBoxLayout *shuffleModeRow = new QHBoxLayout();
    QLabel *shuffleModeTitle = new QLabel("Style");
    shuffleModeTitle->setStyleSheet("font-size: 13px; color: #ccc;");
    m_shuffleModeCombo = new QComboBox();
    m_shuffleModeCombo->setMinimumWidth(280);
    m_shuffleModeCombo->setStyleSheet(
        "QComboBox { background: #333; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #2b2b2b; color: white; selection-background-color: #0078d7; border: 1px solid #555; }");
    m_shuffleModeCombo->addItem("Random (next/previous)", 0);
    m_shuffleModeCombo->addItem("Shuffle (history-aware)", 1);
    shuffleModeRow->addWidget(shuffleModeTitle);
    shuffleModeRow->addStretch();
    shuffleModeRow->addWidget(m_shuffleModeCombo);
    layout->addLayout(shuffleModeRow);

    m_shuffleNoRepeatCheck = new QCheckBox("No repeats until all tracks are played");
    m_shuffleNoRepeatCheck->setStyleSheet("QCheckBox { color: #ccc; font-size: 12px; } QCheckBox::indicator { width: 14px; height: 14px; }");
    layout->addWidget(m_shuffleNoRepeatCheck);

    QLabel *hint = new QLabel(
        "Random picks a fresh random track for both next and previous. "
        "History-aware shuffle keeps play history so previous moves back naturally.");
    hint->setStyleSheet("font-size: 12px; color: #999;");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto saveShuffleOptions = [this]() {
        if (!m_shuffleModeCombo || !m_shuffleNoRepeatCheck)
            return;

        const int mode = qBound(0, m_shuffleModeCombo->currentData().toInt(), 1);
        const bool noRepeats = m_shuffleNoRepeatCheck->isChecked();

        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Playback/shuffleMode", mode);
        s.setValue("Playback/shuffleNoRepeats", noRepeats);

        emit shuffleOptionsChanged(mode, noRepeats);
    };

    connect(m_shuffleModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [saveShuffleOptions](int) { saveShuffleOptions(); });
    connect(m_shuffleNoRepeatCheck, &QCheckBox::toggled,
            this, [saveShuffleOptions](bool) { saveShuffleOptions(); });

    {
        QSettings settings("MyCompany", "MusicPlayer");
        const int savedShuffleMode = qBound(0, settings.value("Playback/shuffleMode", 1).toInt(), 1);
        const bool savedShuffleNoRepeats = settings.value("Playback/shuffleNoRepeats", false).toBool();

        QSignalBlocker modeBlocker(m_shuffleModeCombo);
        QSignalBlocker noRepeatBlocker(m_shuffleNoRepeatCheck);
        for (int i = 0; i < m_shuffleModeCombo->count(); ++i) {
            if (m_shuffleModeCombo->itemData(i).toInt() == savedShuffleMode) {
                m_shuffleModeCombo->setCurrentIndex(i);
                break;
            }
        }
        m_shuffleNoRepeatCheck->setChecked(savedShuffleNoRepeats);
    }

    layout->addStretch();
    m_pages->addWidget(page);
}

void SettingsDialog::updateFormatLabel()
{
    if (!m_formatInfoLabel) return;
    if (m_engine) {
        const QAudioFormat f = m_engine->outputFormat();
        m_formatInfoLabel->setText(QStringLiteral("%1 Hz / Float32 / %2 ch")
            .arg(f.sampleRate())
            .arg(f.channelCount()));
    } else {
        m_formatInfoLabel->setText(QStringLiteral("44100 Hz / Float32 / 2 ch"));
    }
}

void SettingsDialog::updateBackendStatusLabel()
{
    if (!m_backendStatusLabel)
        return;

    if (!m_engine) {
        m_backendStatusLabel->setText("Custom WASAPI Shared");
        return;
    }

    const QString requested = m_engine->backendPreferenceId();
    const QString active = m_engine->activeBackendId();
    QString text = m_engine->activeBackendDisplayName();

    if (requested != active)
        text += QStringLiteral(" (fallback)");

    m_backendStatusLabel->setText(text);
}

void SettingsDialog::updateDecoderStatusLabel()
{
    if (!m_decoderStatusLabel)
        return;

    if (!m_engine) {
        m_decoderStatusLabel->setText("FFmpeg Decoder");
        return;
    }

    const QString requested = m_engine->decoderPreferenceId();
    const QString active = m_engine->activeDecoderId();
    QString text = m_engine->activeDecoderDisplayName();
    if (requested != active)
        text += QStringLiteral(" (fallback)");
    m_decoderStatusLabel->setText(text);
}

void SettingsDialog::refreshOutputDeviceList()
{
    if (m_outputDeviceRefreshQueued)
        return;

    m_outputDeviceRefreshQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_outputDeviceRefreshQueued = false;
        doRefreshOutputDeviceList();
    });
}

void SettingsDialog::doRefreshOutputDeviceList()
{
    if (!m_outputDeviceCombo)
        return;

    QSignalBlocker blocker(m_outputDeviceCombo);
    m_updatingOutputDeviceUi = true;

    m_outputDeviceCombo->clear();

    bool selectable = false;
    QString preferredId;
    QString activeName = QStringLiteral("System Default");

    if (m_engine) {
        selectable = m_engine->canSelectOutputDevice();
        preferredId = m_engine->outputDevicePreferenceId();
        activeName = m_engine->activeOutputDeviceName();
    }

    if (!selectable || !m_engine) {
        m_outputDeviceCombo->addItem("System default (backend managed)", QString());
        m_outputDeviceCombo->setEnabled(false);
    } else {
        m_outputDeviceCombo->setEnabled(true);
        m_outputDeviceCombo->addItem("Default system device", QString());

        const auto devices = m_engine->availableOutputDevices();
        for (const auto &device : devices)
            m_outputDeviceCombo->addItem(device.second, device.first);

        int selectedIndex = 0;
        for (int i = 0; i < m_outputDeviceCombo->count(); ++i) {
            if (m_outputDeviceCombo->itemData(i).toString() == preferredId) {
                selectedIndex = i;
                break;
            }
        }
        m_outputDeviceCombo->setCurrentIndex(selectedIndex);
    }

    if (m_outputDeviceStatusLabel)
        m_outputDeviceStatusLabel->setText(activeName.isEmpty() ? QStringLiteral("System Default") : activeName);

    m_updatingOutputDeviceUi = false;
}

void SettingsDialog::buildEqualizerPage()
{
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(8);

    QString btnStyle =
        "QPushButton { background: #333; color: #ccc; border: 1px solid #555; border-radius: 4px; padding: 4px 10px; font-size: 11px; }"
        "QPushButton:hover { background: #444; color: white; }";

    // Top row: enable + auto-level
    QHBoxLayout *topRow = new QHBoxLayout();
    topRow->setSpacing(12);

    m_eqEnableCheck = new QCheckBox("Enable EQ");
    m_eqEnableCheck->setStyleSheet("QCheckBox { color: white; font-size: 13px; } QCheckBox::indicator { width: 16px; height: 16px; }");

    m_autoLevelCheck = new QCheckBox("Prevent clipping");
    m_autoLevelCheck->setStyleSheet("QCheckBox { color: #aaa; font-size: 12px; } QCheckBox::indicator { width: 14px; height: 14px; }");
    m_autoLevelCheck->setToolTip("Automatically reduce gain to prevent clipping (like foobar2000)");

    QPushButton *resetBtn = new QPushButton("Reset");
    resetBtn->setStyleSheet(btnStyle);
    resetBtn->setCursor(Qt::PointingHandCursor);

    topRow->addWidget(m_eqEnableCheck);
    topRow->addWidget(m_autoLevelCheck);
    topRow->addStretch();
    topRow->addWidget(resetBtn);
    layout->addLayout(topRow);

    // Preset row
    QHBoxLayout *presetRow = new QHBoxLayout();
    presetRow->setSpacing(6);

    QLabel *presetLabel = new QLabel("Preset:");
    presetLabel->setStyleSheet("color: #ccc; font-size: 12px;");

    m_presetCombo = new QComboBox();
    m_presetCombo->setMinimumWidth(140);
    m_presetCombo->setStyleSheet(
        "QComboBox { background: #333; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #2b2b2b; color: white; selection-background-color: #0078d7; border: 1px solid #555; }");

    m_savePresetBtn = new QPushButton("Save");
    m_savePresetBtn->setStyleSheet(btnStyle);
    m_savePresetBtn->setCursor(Qt::PointingHandCursor);

    m_deletePresetBtn = new QPushButton("Delete");
    m_deletePresetBtn->setStyleSheet(btnStyle);
    m_deletePresetBtn->setCursor(Qt::PointingHandCursor);

    presetRow->addWidget(presetLabel);
    presetRow->addWidget(m_presetCombo, 1);
    presetRow->addWidget(m_savePresetBtn);
    presetRow->addWidget(m_deletePresetBtn);
    layout->addLayout(presetRow);

    // Sliders
    QHBoxLayout *slidersRow = new QHBoxLayout();
    slidersRow->setSpacing(2);

    QString sliderStyle =
        "QSlider::groove:vertical { width: 4px; background: #4d4d4d; border-radius: 2px; }"
        "QSlider::handle:vertical { background: #b3b3b3; height: 10px; width: 14px; margin: 0 -5px; border-radius: 5px; }"
        "QSlider::handle:vertical:hover { background: #0078d7; }"
        "QSlider::sub-page:vertical { background: #4d4d4d; border-radius: 2px; }"
        "QSlider::add-page:vertical { background: #0078d7; border-radius: 2px; }";

    // Preamp column
    {
        QVBoxLayout *col = new QVBoxLayout();
        col->setAlignment(Qt::AlignCenter);
        col->setSpacing(2);

        QLabel *t = new QLabel("Pre");
        t->setStyleSheet("color: #0078d7; font-size: 10px; font-weight: bold;");
        t->setAlignment(Qt::AlignCenter);

        m_preampValueLabel = new QLabel("0.0");
        m_preampValueLabel->setStyleSheet("color: #aaa; font-size: 9px;");
        m_preampValueLabel->setAlignment(Qt::AlignCenter);
        m_preampValueLabel->setFixedWidth(36);

        m_preampSlider = new QSlider(Qt::Vertical);
        m_preampSlider->setRange(-240, 240);
        m_preampSlider->setFixedHeight(160);
        m_preampSlider->setStyleSheet(sliderStyle);
        m_preampSlider->setCursor(Qt::PointingHandCursor);

        m_autoLevelLabel = new QLabel("");
        m_autoLevelLabel->setStyleSheet("color: #0078d7; font-size: 9px;");
        m_autoLevelLabel->setAlignment(Qt::AlignCenter);
        m_autoLevelLabel->setFixedWidth(42);

        col->addWidget(t, 0, Qt::AlignCenter);
        col->addWidget(m_preampValueLabel, 0, Qt::AlignCenter);
        col->addWidget(m_preampSlider, 0, Qt::AlignCenter);
        col->addWidget(m_autoLevelLabel, 0, Qt::AlignCenter);
        slidersRow->addLayout(col);
    }

    QFrame *vSep = new QFrame();
    vSep->setFrameShape(QFrame::VLine);
    vSep->setStyleSheet("color: #3d3d3d;");
    slidersRow->addWidget(vSep);

    // Band sliders
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        QVBoxLayout *col = new QVBoxLayout();
        col->setAlignment(Qt::AlignCenter);
        col->setSpacing(2);

        m_eqValueLabels[i] = new QLabel("0.0");
        m_eqValueLabels[i]->setStyleSheet("color: #aaa; font-size: 9px;");
        m_eqValueLabels[i]->setAlignment(Qt::AlignCenter);
        m_eqValueLabels[i]->setFixedWidth(30);

        m_eqSliders[i] = new QSlider(Qt::Vertical);
        m_eqSliders[i]->setRange(-240, 240);
        m_eqSliders[i]->setFixedHeight(160);
        m_eqSliders[i]->setStyleSheet(sliderStyle);
        m_eqSliders[i]->setCursor(Qt::PointingHandCursor);

        QLabel *freqLabel = new QLabel(eqLabel(i));
        freqLabel->setStyleSheet("color: #888; font-size: 9px;");
        freqLabel->setAlignment(Qt::AlignCenter);

        col->addWidget(m_eqValueLabels[i], 0, Qt::AlignCenter);
        col->addWidget(m_eqSliders[i], 0, Qt::AlignCenter);
        col->addWidget(freqLabel, 0, Qt::AlignCenter);

        slidersRow->addLayout(col);

        connect(m_eqSliders[i], &QSlider::valueChanged, this, [this, i](int val) {
            double dB = val / 10.0;
            m_eq->setBandGain(i, dB);
            m_eqValueLabels[i]->setText(QString::number(dB, 'f', 1));
            QSettings s("MyCompany", "MusicPlayer");
            s.setValue(QString("Equalizer/band_%1").arg(i), dB);
        });
    }

    layout->addLayout(slidersRow, 1);

    // Connections
    connect(m_preampSlider, &QSlider::valueChanged, this, [this](int val) {
        double dB = val / 10.0;
        m_eq->setPreamp(dB);
        m_preampValueLabel->setText(QString::number(dB, 'f', 1));
        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Equalizer/preamp", dB);
    });

    connect(m_eqEnableCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_eq->setEnabled(on);
        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Equalizer/enabled", on);
    });

    connect(m_autoLevelCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_eq->setAutoLevelEnabled(on);
        QSettings s("MyCompany", "MusicPlayer");
        s.setValue("Equalizer/autoLevel", on);
        if (!on)
            m_autoLevelLabel->setText("");
    });

    connect(m_eq, &Equalizer::autoLevelChanged, this, &SettingsDialog::updateAutoLevelDisplay);

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (idx < 0) return;
        QString name = m_presetCombo->itemText(idx);
        loadPreset(name);
    });

    connect(m_savePresetBtn, &QPushButton::clicked, this, &SettingsDialog::savePreset);
    connect(m_deletePresetBtn, &QPushButton::clicked, this, &SettingsDialog::deletePreset);
    connect(resetBtn, &QPushButton::clicked, this, &SettingsDialog::resetEq);

    m_pages->addWidget(page);

    loadPresetList();
    loadEqFromSettings();
}

void SettingsDialog::buildAboutPage()
{
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    QLabel *title = new QLabel("About");
    title->setStyleSheet("font-size: 18px; font-weight: bold; color: white;");
    layout->addWidget(title);

    QFrame *sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #3d3d3d;");
    layout->addWidget(sep);

    QLabel *appName = new QLabel("MusicPlayer");
    appName->setStyleSheet("font-size: 22px; font-weight: bold; color: #0078d7;");
    layout->addWidget(appName);

    QLabel *version = new QLabel("Version 0.1.0");
    version->setStyleSheet("font-size: 13px; color: #aaa;");
    layout->addWidget(version);

    QLabel *desc = new QLabel("A lightweight music player with true gapless playback.\nBuilt with Qt 6 and C++.");
    desc->setStyleSheet("font-size: 13px; color: #ccc;");
    desc->setWordWrap(true);
    layout->addWidget(desc);

    layout->addStretch();

    QLabel *copyright = new QLabel(QString::fromUtf8("\xC2\xA9 2026"));
    copyright->setStyleSheet("font-size: 11px; color: #666;");
    layout->addWidget(copyright);

    m_pages->addWidget(page);
}

void SettingsDialog::loadEqFromSettings()
{
    QSettings s("MyCompany", "MusicPlayer");

    m_eqEnableCheck->blockSignals(true);
    m_autoLevelCheck->blockSignals(true);
    m_preampSlider->blockSignals(true);
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_eqSliders[i]->blockSignals(true);

    bool enabled = s.value("Equalizer/enabled", true).toBool();
    m_eqEnableCheck->setChecked(enabled);
    m_eq->setEnabled(enabled);

    bool autoLevel = s.value("Equalizer/autoLevel", false).toBool();
    m_autoLevelCheck->setChecked(autoLevel);
    m_eq->setAutoLevelEnabled(autoLevel);

    double preamp = s.value("Equalizer/preamp", 0.0).toDouble();
    m_preampSlider->setValue(static_cast<int>(preamp * 10));
    m_eq->setPreamp(preamp);
    m_preampValueLabel->setText(QString::number(preamp, 'f', 1));

    m_eq->beginBatch();
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        double dB = s.value(QString("Equalizer/band_%1").arg(i), 0.0).toDouble();
        m_eqSliders[i]->setValue(static_cast<int>(dB * 10));
        m_eq->setBandGain(i, dB);
        m_eqValueLabels[i]->setText(QString::number(dB, 'f', 1));
    }
    m_eq->endBatch();

    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_eqSliders[i]->blockSignals(false);
    m_preampSlider->blockSignals(false);
    m_autoLevelCheck->blockSignals(false);
    m_eqEnableCheck->blockSignals(false);

    updateAutoLevelDisplay(m_eq->autoLevelDb());
}

// ========== Preset CRUD ==========

void SettingsDialog::loadPresetList()
{
    m_presetCombo->clear();
    m_presetCombo->addItem("Flat");

    QSettings s("MyCompany", "MusicPlayer");
    s.beginGroup("Equalizer/presets");
    QStringList presets = s.childGroups();
    s.endGroup();

    presets.sort(Qt::CaseInsensitive);
    for (const QString &name : presets)
        m_presetCombo->addItem(name);
}

void SettingsDialog::savePreset()
{
    bool ok = false;
    QString name = QInputDialog::getText(
        this, "Save Preset", "Preset name:",
        QLineEdit::Normal, QString(), &ok);

    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    if (name.compare("Flat", Qt::CaseInsensitive) == 0) {
        QMessageBox::warning(this, "Save Preset", "Cannot overwrite the Flat preset.");
        return;
    }

    QSettings s("MyCompany", "MusicPlayer");
    QString prefix = QString("Equalizer/presets/%1").arg(name);

    s.setValue(prefix + "/preamp", m_eq->preamp());
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        s.setValue(prefix + QString("/band_%1").arg(i), m_eq->bandGain(i));

    loadPresetList();

    for (int i = 0; i < m_presetCombo->count(); ++i) {
        if (m_presetCombo->itemText(i) == name) {
            m_presetCombo->setCurrentIndex(i);
            break;
        }
    }
}

void SettingsDialog::deletePreset()
{
    int idx = m_presetCombo->currentIndex();
    if (idx <= 0) return; // can't delete "Flat"

    QString name = m_presetCombo->currentText();

    QSettings s("MyCompany", "MusicPlayer");
    s.beginGroup(QString("Equalizer/presets/%1").arg(name));
    s.remove("");
    s.endGroup();

    loadPresetList();
}

void SettingsDialog::loadPreset(const QString &name)
{
    if (name == "Flat") {
        resetEq();
        return;
    }

    QSettings s("MyCompany", "MusicPlayer");
    QString prefix = QString("Equalizer/presets/%1").arg(name);

    if (!s.contains(prefix + "/band_0")) return;

    m_preampSlider->blockSignals(true);
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_eqSliders[i]->blockSignals(true);

    double preamp = s.value(prefix + "/preamp", 0.0).toDouble();
    m_preampSlider->setValue(static_cast<int>(preamp * 10));
    m_preampValueLabel->setText(QString::number(preamp, 'f', 1));
    m_eq->setPreamp(preamp);
    s.setValue("Equalizer/preamp", preamp);

    m_eq->beginBatch();
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        double dB = s.value(prefix + QString("/band_%1").arg(i), 0.0).toDouble();
        m_eqSliders[i]->setValue(static_cast<int>(dB * 10));
        m_eq->setBandGain(i, dB);
        m_eqValueLabels[i]->setText(QString::number(dB, 'f', 1));
        s.setValue(QString("Equalizer/band_%1").arg(i), dB);
    }
    m_eq->endBatch();

    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_eqSliders[i]->blockSignals(false);
    m_preampSlider->blockSignals(false);

    updateAutoLevelDisplay(m_eq->autoLevelDb());
}

void SettingsDialog::resetEq()
{
    m_autoLevelCheck->blockSignals(true);
    m_preampSlider->blockSignals(true);
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_eqSliders[i]->blockSignals(true);

    m_autoLevelCheck->setChecked(false);
    m_eq->setAutoLevelEnabled(false);

    m_preampSlider->setValue(0);
    m_preampValueLabel->setText(QStringLiteral("0.0"));

    m_eq->setPreamp(0.0);
    m_eq->beginBatch();
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        m_eqSliders[i]->setValue(0);
        m_eqValueLabels[i]->setText(QStringLiteral("0.0"));
        m_eq->setBandGain(i, 0.0);
    }
    m_eq->endBatch();

    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_eqSliders[i]->blockSignals(false);
    m_preampSlider->blockSignals(false);
    m_autoLevelCheck->blockSignals(false);

    QSettings s("MyCompany", "MusicPlayer");
    s.setValue("Equalizer/autoLevel", false);
    s.setValue("Equalizer/preamp", 0.0);
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        s.setValue(QString("Equalizer/band_%1").arg(i), 0.0);

    updateAutoLevelDisplay(m_eq->autoLevelDb());
    m_presetCombo->setCurrentIndex(0);
}

void SettingsDialog::updateAutoLevelDisplay(double dB)
{
    if (m_autoLevelCheck->isChecked() && dB < -0.05)
        m_autoLevelLabel->setText(QString("%1 dB").arg(dB, 0, 'f', 1));
    else
        m_autoLevelLabel->setText("");
}

int SettingsDialog::rowHeight() const { return m_rowHeightSpin->value(); }
void SettingsDialog::setRowHeight(int h) { m_rowHeightSpin->setValue(h); }
