#include "Equalizer.h"
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

Equalizer::Equalizer(QObject *parent)
    : QObject(parent)
{
    m_gains.fill(0.0);
    m_prevGains.fill(0.0);
    m_transFramesLeft = 0;
    updateActiveBands();
    recalcAutoLevel();
}

BiquadCoeffs Equalizer::makeLowShelf(double freq, double gainDb, double q) const
{
    const double A     = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * M_PI * freq / m_sampleRate;
    const double cosw  = std::cos(omega);
    const double sinw  = std::sin(omega);
    const double alpha = sinw / (2.0 * q);

    const double b0 =  A * ((A+1) - (A-1)*cosw + 2*A*alpha);
    const double b1 =  2*A * ((A-1) - (A+1)*cosw);
    const double b2 =  A * ((A+1) - (A-1)*cosw - 2*A*alpha);
    const double a0 =       (A+1) + (A-1)*cosw + 2*A*alpha;
    const double a1 = -2 * ((A-1) + (A+1)*cosw);
    const double a2 =       (A+1) + (A-1)*cosw - 2*A*alpha;

    return { float(b0/a0), float(b1/a0), float(b2/a0), float(a1/a0), float(a2/a0) };
}

BiquadCoeffs Equalizer::makeHighShelf(double freq, double gainDb, double q) const
{
    const double A     = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * M_PI * freq / m_sampleRate;
    const double cosw  = std::cos(omega);
    const double sinw  = std::sin(omega);
    const double alpha = sinw / (2.0 * q);

    const double b0 =  A * ((A+1) + (A-1)*cosw + 2*A*alpha);
    const double b1 = -2*A * ((A-1) + (A+1)*cosw);
    const double b2 =  A * ((A+1) + (A-1)*cosw - 2*A*alpha);
    const double a0 =       (A+1) - (A-1)*cosw + 2*A*alpha;
    const double a1 =  2 * ((A-1) - (A+1)*cosw);
    const double a2 =       (A+1) - (A-1)*cosw - 2*A*alpha;

    return { float(b0/a0), float(b1/a0), float(b2/a0), float(a1/a0), float(a2/a0) };
}

BiquadCoeffs Equalizer::makePeaking(double freq, double gainDb, double q) const
{
    const double A     = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * M_PI * freq / m_sampleRate;
    const double cosw  = std::cos(omega);
    const double sinw  = std::sin(omega);
    const double alpha = sinw / (2.0 * q);

    const double b0 =  1 + alpha * A;
    const double b1 = -2 * cosw;
    const double b2 =  1 - alpha * A;
    const double a0 =  1 + alpha / A;
    const double a1 = -2 * cosw;
    const double a2 =  1 - alpha / A;

    return { float(b0/a0), float(b1/a0), float(b2/a0), float(a1/a0), float(a2/a0) };
}

BiquadCoeffs Equalizer::makeCoeffsForGain(int band, double gainDb) const
{
    if (band == 0)
        return makeLowShelf(EQ_FREQUENCIES[band], gainDb, 0.707);
    if (band == EQ_BAND_COUNT - 1)
        return makeHighShelf(EQ_FREQUENCIES[band], gainDb, 0.707);
    return makePeaking(EQ_FREQUENCIES[band], gainDb, 1.414);
}

inline float Equalizer::processBiquad(BiquadState &s, const BiquadCoeffs &c, float x) const
{
    const float y = c.b0*x + c.b1*s.x1 + c.b2*s.x2 - c.a1*s.y1 - c.a2*s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}

void Equalizer::updateActiveBands()
{
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        m_activeBand[i] = (std::abs(m_gains[i]) > 0.05);
}

void Equalizer::setSampleRate(int sampleRate)
{
    if (sampleRate <= 0 || m_sampleRate == sampleRate) return;
    m_sampleRate = sampleRate;
    resetState();
    m_prevGains = m_gains;
    m_transFramesLeft = 0;
    recalcAutoLevel();
}

void Equalizer::setBandGain(int band, double dBGain)
{
    if (band < 0 || band >= EQ_BAND_COUNT) return;
    dBGain = std::clamp(dBGain, -24.0, 24.0);
    const double oldGain = m_gains[band];
    if (std::abs(oldGain - dBGain) < 0.001) return;

    m_gains[band] = dBGain;

    if (m_batchMode) {
        m_coeffDirty = true;
        return;
    }

    m_prevGains = m_gains;
    m_prevGains[band] = oldGain;
    m_transFramesLeft = kTransitionFrames;
    updateActiveBands();
    recalcAutoLevel();
    emit bandsChanged();
}

double Equalizer::bandGain(int band) const
{
    if (band < 0 || band >= EQ_BAND_COUNT) return 0.0;
    return m_gains[band];
}

void Equalizer::setPreamp(double dB)
{
    dB = std::clamp(dB, -20.0, 20.0);
    m_preampDb = dB;
    m_preampLinear = std::pow(10.0, dB / 20.0);
    emit bandsChanged();
}

void Equalizer::setEnabled(bool on)
{
    m_enabled = on;
    if (!on) resetState();
    emit bandsChanged();
}

void Equalizer::setAutoLevelEnabled(bool on)
{
    m_autoLevelEnabled = on;
    if (on)
        recalcAutoLevel();
    else {
        m_autoLevelDb = 0.0;
        m_autoLevelLinear = 1.0;
    }
    emit bandsChanged();
}

void Equalizer::resetState()
{
    for (auto &ch : m_chState)
        for (auto &s : ch)
            s.x1 = s.x2 = s.y1 = s.y2 = 0;
    m_transFramesLeft = 0;
    m_prevGains = m_gains;
}

void Equalizer::prepareForSeek()
{
    resetState();
}

bool Equalizer::hasOutput() const
{
    return m_enabled;
}

void Equalizer::beginBatch()
{
    if (!m_batchMode)
        m_prevGains = m_gains;
    m_batchMode = true;
}

void Equalizer::endBatch()
{
    m_batchMode = false;
    if (m_coeffDirty) {
        m_transFramesLeft = kTransitionFrames;
        updateActiveBands();
        recalcAutoLevel();
        m_coeffDirty = false;
        emit bandsChanged();
    }
}

double Equalizer::computePeakGainDb() const
{
    double maxGain = 0.0;
    for (int i = 0; i < EQ_BAND_COUNT; ++i)
        if (m_gains[i] > maxGain)
            maxGain = m_gains[i];
    return maxGain;
}

void Equalizer::recalcAutoLevel()
{
    if (!m_autoLevelEnabled) return;
    double maxGain = computePeakGainDb();
    m_autoLevelDb = (maxGain > 0.0) ? -maxGain : 0.0;
    m_autoLevelLinear = std::pow(10.0, m_autoLevelDb / 20.0);
    emit autoLevelChanged(m_autoLevelDb);
}

void Equalizer::ensureChannelState(int channels)
{
    if (channels != m_lastChannels) {
        m_lastChannels = channels;
        m_chState.assign(static_cast<size_t>(channels), {});
    }
}

void Equalizer::process(float *samples, qint64 sampleCount, int channels)
{
    if (!m_enabled) {
        resetState();
        return;
    }
    if (channels < 1 || sampleCount <= 0) return;

    ensureChannelState(channels);

    BiquadCoeffs activeCoeffs[EQ_BAND_COUNT];
    bool bandActive[EQ_BAND_COUNT];
    for (int b = 0; b < EQ_BAND_COUNT; ++b) {
        bandActive[b] = (std::abs(m_gains[b]) > 0.05);
        if (bandActive[b])
            activeCoeffs[b] = makeCoeffsForGain(b, m_gains[b]);
    }

    const float gainLinear = static_cast<float>(
        m_preampLinear * (m_autoLevelEnabled ? m_autoLevelLinear : 1.0));
    const qint64 frames = sampleCount / channels;

    for (qint64 f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            float v = samples[f * channels + c];
            for (int b = 0; b < EQ_BAND_COUNT; ++b) {
                if (bandActive[b])
                    v = processBiquad(m_chState[static_cast<size_t>(c)][static_cast<size_t>(b)], activeCoeffs[b], v);
            }
            samples[f * channels + c] = v * gainLinear;
        }
    }

    if (m_transFramesLeft > 0) {
        m_transFramesLeft -= static_cast<int>(frames);
        if (m_transFramesLeft <= 0) {
            m_transFramesLeft = 0;
            m_prevGains = m_gains;
        }
    }
}