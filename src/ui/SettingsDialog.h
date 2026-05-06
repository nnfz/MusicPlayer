#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QSpinBox>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <array>
#include "Equalizer.h"

class GaplessAudioEngine;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(Equalizer *eq, GaplessAudioEngine *engine,
                            QWidget *parent = nullptr);
    int rowHeight() const;
    void setRowHeight(int h);
signals:
    void rowHeightChanged(int h);
    void shuffleOptionsChanged(int mode, bool noRepeats);
    void playbackRateChanged(double rate);
    void crossfadeDurationChanged(int seconds);
private:
    void buildAppearancePage();
    void buildAudioPage();
    void buildPlaybackPage();
    void buildEqualizerPage();
    void buildAboutPage();
    void loadEqFromSettings();
    void resetEq();
    void updateAutoLevelDisplay(double dB);
    void updateFormatLabel();
    void updateDecoderStatusLabel();
    void updateBackendStatusLabel();
    void refreshOutputDeviceList();
    void doRefreshOutputDeviceList();

    void loadPresetList();
    void savePreset();
    void deletePreset();
    void loadPreset(const QString &name);

    Equalizer *m_eq;
    GaplessAudioEngine *m_engine;
    QListWidget *m_categoryList;
    QStackedWidget *m_pages;
    QSpinBox *m_rowHeightSpin;

    QComboBox *m_decoderCombo = nullptr;
    QComboBox *m_backendCombo = nullptr;
    QComboBox *m_outputDeviceCombo = nullptr;
    QSlider *m_playbackRateSlider = nullptr;
    QLabel *m_playbackRateValueLabel = nullptr;
    QSlider *m_crossfadeSlider = nullptr;
    QLabel *m_crossfadeValueLabel = nullptr;
    QComboBox *m_shuffleModeCombo = nullptr;
    QCheckBox *m_shuffleNoRepeatCheck = nullptr;
    QLabel *m_decoderStatusLabel = nullptr;
    QLabel *m_backendStatusLabel = nullptr;
    QLabel *m_outputDeviceStatusLabel = nullptr;
    QLabel *m_formatInfoLabel = nullptr;

    bool m_updatingOutputDeviceUi = false;
    bool m_outputDeviceRefreshQueued = false;

    std::array<QSlider*, EQ_BAND_COUNT> m_eqSliders;
    std::array<QLabel*, EQ_BAND_COUNT> m_eqValueLabels;
    QSlider *m_preampSlider;
    QLabel *m_preampValueLabel;
    QLabel *m_autoLevelLabel;
    QCheckBox *m_eqEnableCheck;
    QCheckBox *m_autoLevelCheck;
    QComboBox *m_presetCombo;
    QPushButton *m_savePresetBtn;
    QPushButton *m_deletePresetBtn;
};

#endif // SETTINGSDIALOG_H
