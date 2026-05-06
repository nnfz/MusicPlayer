#ifndef EQUALIZERDIALOG_H
#define EQUALIZERDIALOG_H

#include <QDialog>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <array>
#include "Equalizer.h"

class EqualizerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit EqualizerDialog(Equalizer *eq, QWidget *parent = nullptr);

private:
    void loadFromEqualizer();
    void applyPreset(const double gains[EQ_BAND_COUNT]);
    void resetAll();

    Equalizer *m_eq;
    std::array<QSlider*, EQ_BAND_COUNT> m_sliders;
    std::array<QLabel*, EQ_BAND_COUNT> m_valueLabels;
    QSlider *m_preampSlider;
    QLabel *m_preampValueLabel;
    QCheckBox *m_enableCheck;
    QComboBox *m_presetCombo;
};

#endif // EQUALIZERDIALOG_H
