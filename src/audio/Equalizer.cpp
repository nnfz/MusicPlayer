#include "Equalizer.h"
#include <algorithm>
#include <cmath>

Equalizer::Equalizer(QObject *parent) : QObject(parent)
{
    m_gains.fill(0.0);
}

void Equalizer::setBandGain(int band, double dBGain)
{
    if (band < 0 || band >= EQ_BAND_COUNT) return;
    dBGain = std::clamp(dBGain, -24.0, 24.0);
    if (std::abs(m_gains[band] - dBGain) < 0.001) return;

    m_gains[band] = dBGain;
    if (m_batchMode) {
        m_dirty = true;
    } else {
        recalcAutoLevel();
        emit bandsChanged();
    }
}

double Equalizer::bandGain(int band) const
{
    if (band < 0 || band >= EQ_BAND_COUNT) return 0.0;
    return m_gains[band];
}

void Equalizer::setPreamp(double dB)
{
    m_preampDb = std::clamp(dB, -20.0, 20.0);
    emit bandsChanged();
}

void Equalizer::setEnabled(bool on)
{
    if (m_enabled == on) return;
    m_enabled = on;
    emit bandsChanged();
}

void Equalizer::setAutoLevelEnabled(bool on)
{
    if (m_autoLevelEnabled == on) return;
    m_autoLevelEnabled = on;
    recalcAutoLevel();
    emit bandsChanged();
}

void Equalizer::endBatch()
{
    m_batchMode = false;
    if (m_dirty) {
        recalcAutoLevel();
        emit bandsChanged();
        m_dirty = false;
    }
}

double Equalizer::computePeakGainDb() const
{
    double maxBoost = 0.0;
    for (double g : m_gains) if (g > maxBoost) maxBoost = g;
    return maxBoost;
}

void Equalizer::recalcAutoLevel()
{
    if (!m_autoLevelEnabled) {
        m_autoLevelDb = 0.0;
    } else {
        double peak = computePeakGainDb();
        m_autoLevelDb = (peak > 0.0) ? -peak : 0.0;
    }
    emit autoLevelChanged(m_autoLevelDb);
}
