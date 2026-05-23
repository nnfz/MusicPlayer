#include "EqualizerDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFrame>
#include <QSettings>

static const double PRESET_FLAT[EQ_BAND_COUNT] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const double PRESET_ROCK[EQ_BAND_COUNT] = {4,3,2,1,0,-1,0,1,2,3,4,4,3,3,2,1,0,0};
static const double PRESET_POP[EQ_BAND_COUNT] = {-1,0,1,2,3,3,2,1,0,-1,-1,0,1,2,2,1,0,-1};
static const double PRESET_JAZZ[EQ_BAND_COUNT] = {3,2,1,2,0,-1,0,1,2,3,3,2,1,1,0,0,0,0};
static const double PRESET_CLASSICAL[EQ_BAND_COUNT] = {3,2,1,0,0,0,0,0,0,0,0,0,1,2,2,3,3,2};
static const double PRESET_BASS_BOOST[EQ_BAND_COUNT] = {8,7,6,5,4,3,2,1,0,0,0,0,0,0,0,0,0,0};
static const double PRESET_VOCAL[EQ_BAND_COUNT] = {-2,-2,-1,0,2,4,4,3,2,1,0,-1,-2,-2,-2,-2,-2,-2};
static const double PRESET_ELECTRONIC[EQ_BAND_COUNT] = {5,4,3,1,0,-1,0,1,2,1,0,1,2,3,4,4,3,2};

EqualizerDialog::EqualizerDialog(Equalizer *eq, QWidget *parent)
    : QDialog(parent, Qt::Tool | Qt::WindowStaysOnTopHint)
    , m_eq(eq)
{
    setWindowTitle("Equalizer");
    setMinimumWidth(720);
    setFixedHeight(380);
    setStyleSheet("QDialog { background: #121212; color: white; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    // Top row: enable + auto level + presets + reset
    QHBoxLayout *topRow = new QHBoxLayout();
    topRow->setSpacing(12);

    m_enableCheck = new QCheckBox("Enable EQ");
    m_enableCheck->setChecked(m_eq->isEnabled());
    m_enableCheck->setStyleSheet("QCheckBox { color: white; font-size: 13px; } QCheckBox::indicator { width: 16px; height: 16px; }");

    m_autoLevelCheck = new QCheckBox("Auto Level");
    m_autoLevelCheck->setChecked(m_eq->autoLevelEnabled());
    m_autoLevelCheck->setStyleSheet("QCheckBox { color: white; font-size: 13px; } QCheckBox::indicator { width: 16px; height: 16px; }");

    m_presetCombo = new QComboBox();
    m_presetCombo->addItems({"Flat", "Rock", "Pop", "Jazz", "Classical", "Bass Boost", "Vocal", "Electronic"});
    m_presetCombo->setStyleSheet(
        "QComboBox { background: #333; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; font-size: 12px; min-width: 100px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #1a1a1a; color: white; selection-background-color: #0078d7; border: 1px solid #555; }");

    QPushButton *resetBtn = new QPushButton("Reset");
    resetBtn->setStyleSheet(
        "QPushButton { background: #333; color: #ccc; border: 1px solid #555; border-radius: 4px; padding: 4px 12px; font-size: 12px; }"
        "QPushButton:hover { background: #444; color: white; }");
    resetBtn->setCursor(Qt::PointingHandCursor);

    topRow->addWidget(m_enableCheck);
    topRow->addWidget(m_autoLevelCheck);
    topRow->addStretch();
    topRow->addWidget(new QLabel("Preset:"));
    topRow->addWidget(m_presetCombo);
    topRow->addWidget(resetBtn);
    mainLayout->addLayout(topRow);

    // Sliders row
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

        QLabel *title = new QLabel("Pre");
        title->setStyleSheet("color: #0078d7; font-size: 10px; font-weight: bold;");
        title->setAlignment(Qt::AlignCenter);

        m_preampValueLabel = new QLabel("0.0");
        m_preampValueLabel->setStyleSheet("color: #aaa; font-size: 9px;");
        m_preampValueLabel->setAlignment(Qt::AlignCenter);
        m_preampValueLabel->setFixedWidth(30);

        m_preampSlider = new QSlider(Qt::Vertical);
        m_preampSlider->setRange(-240, 240);
        m_preampSlider->setValue(static_cast<int>(m_eq->preamp() * 10));
        m_preampSlider->setFixedHeight(200);
        m_preampSlider->setStyleSheet(sliderStyle);
        m_preampSlider->setCursor(Qt::PointingHandCursor);

        col->addWidget(title, 0, Qt::AlignCenter);
        col->addWidget(m_preampValueLabel, 0, Qt::AlignCenter);
        col->addWidget(m_preampSlider, 0, Qt::AlignCenter);
        slidersRow->addLayout(col);
    }

    // Separator
    QFrame *sep = new QFrame();
    sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet("color: #3d3d3d;");
    slidersRow->addWidget(sep);

    // Band sliders
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        QVBoxLayout *col = new QVBoxLayout();
        col->setAlignment(Qt::AlignCenter);
        col->setSpacing(2);

        m_valueLabels[i] = new QLabel("0.0");
        m_valueLabels[i]->setStyleSheet("color: #aaa; font-size: 9px;");
        m_valueLabels[i]->setAlignment(Qt::AlignCenter);
        m_valueLabels[i]->setFixedWidth(30);

        m_sliders[i] = new QSlider(Qt::Vertical);
        m_sliders[i]->setRange(-240, 240); // -24.0 to +24.0 dB in 0.1 steps
        m_sliders[i]->setValue(0);
        m_sliders[i]->setFixedHeight(200);
        m_sliders[i]->setStyleSheet(sliderStyle);
        m_sliders[i]->setCursor(Qt::PointingHandCursor);

        QLabel *freqLabel = new QLabel(eqLabel(i));
        freqLabel->setStyleSheet("color: #888; font-size: 9px;");
        freqLabel->setAlignment(Qt::AlignCenter);

        col->addWidget(m_valueLabels[i], 0, Qt::AlignCenter);
        col->addWidget(m_sliders[i], 0, Qt::AlignCenter);
        col->addWidget(freqLabel, 0, Qt::AlignCenter);

        slidersRow->addLayout(col);

        connect(m_sliders[i], &QSlider::valueChanged, this, [this, i](int val) {
            double dB = val / 10.0;
            m_eq->setBandGain(i, dB);
            m_valueLabels[i]->setText(QString::number(dB, 'f', 1));

            QSettings settings("MyCompany", "MusicPlayer");
            settings.setValue(QString("Equalizer/band_%1").arg(i), dB);
        });
    }

    mainLayout->addLayout(slidersRow, 1);

    // dB scale labels
    QHBoxLayout *scaleRow = new QHBoxLayout();
    scaleRow->addSpacing(38);
    QLabel *plus12 = new QLabel("+24 dB");
    plus12->setStyleSheet("color: #555; font-size: 9px;");
    QLabel *zero = new QLabel("0 dB");
    zero->setStyleSheet("color: #555; font-size: 9px;");
    QLabel *minus12 = new QLabel("-24 dB");
    minus12->setStyleSheet("color: #555; font-size: 9px;");
    scaleRow->addWidget(plus12);
    scaleRow->addStretch();
    scaleRow->addWidget(zero);
    scaleRow->addStretch();
    scaleRow->addWidget(minus12);
    mainLayout->addLayout(scaleRow);

    // Connections
    connect(m_preampSlider, &QSlider::valueChanged, this, [this](int val) {
        double dB = val / 10.0;
        m_eq->setPreamp(dB);
        m_preampValueLabel->setText(QString::number(dB, 'f', 1));

        QSettings settings("MyCompany", "MusicPlayer");
        settings.setValue("Equalizer/preamp", dB);
    });

    connect(m_enableCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_eq->setEnabled(on);
        QSettings settings("MyCompany", "MusicPlayer");
        settings.setValue("Equalizer/enabled", on);
    });

    connect(m_autoLevelCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_eq->setAutoLevelEnabled(on);
        QSettings settings("MyCompany", "MusicPlayer");
        settings.setValue("Equalizer/autoLevel", on);
    });

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        const double *presets[] = {
            PRESET_FLAT, PRESET_ROCK, PRESET_POP, PRESET_JAZZ,
            PRESET_CLASSICAL, PRESET_BASS_BOOST, PRESET_VOCAL, PRESET_ELECTRONIC
        };
        if (idx >= 0 && idx < 8)
            applyPreset(presets[idx]);
    });

    connect(resetBtn, &QPushButton::clicked, this, &EqualizerDialog::resetAll);

    loadFromEqualizer();
}

void EqualizerDialog::loadFromEqualizer()
{
    QSettings settings("MyCompany", "MusicPlayer");

    bool enabled = settings.value("Equalizer/enabled", true).toBool();
    m_enableCheck->setChecked(enabled);
    m_eq->setEnabled(enabled);

    bool autoLevel = settings.value("Equalizer/autoLevel", false).toBool();
    m_autoLevelCheck->setChecked(autoLevel);
    m_eq->setAutoLevelEnabled(autoLevel);

    double preamp = settings.value("Equalizer/preamp", 0.0).toDouble();
    m_preampSlider->blockSignals(true);
    m_preampSlider->setValue(static_cast<int>(preamp * 10));
    m_preampSlider->blockSignals(false);
    m_eq->setPreamp(preamp);
    m_preampValueLabel->setText(QString::number(preamp, 'f', 1));

    m_eq->beginBatch();
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        double dB = settings.value(QString("Equalizer/band_%1").arg(i), 0.0).toDouble();
        m_sliders[i]->blockSignals(true);
        m_sliders[i]->setValue(static_cast<int>(dB * 10));
        m_sliders[i]->blockSignals(false);
        m_eq->setBandGain(i, dB);
        m_valueLabels[i]->setText(QString::number(dB, 'f', 1));
    }
    m_eq->endBatch();
}

void EqualizerDialog::applyPreset(const double gains[EQ_BAND_COUNT])
{
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        m_sliders[i]->setValue(static_cast<int>(gains[i] * 10));
    }
}

void EqualizerDialog::resetAll()
{
    m_preampSlider->setValue(0);
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_sliders[i]->setValue(0);
    m_presetCombo->setCurrentIndex(0);
}
