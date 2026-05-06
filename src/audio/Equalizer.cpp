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

// ================================================================
// Biquad filter design (RBJ Audio EQ Cookbook)
// ================================================================

BiquadCoeffs Equalizer::makeLowShelf(double freq, double gainDb, double q) const
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * M_PI * freq / m_sampleRate;
    const double cosw = std::cos(omega);
    const double sinw = std::sin(omega);
    const double alpha = sinw / (2.0 * q);
    const double sqrtA = A;

    const double b0 =      A * ((A + 1) - (A - 1) * cosw + 2 * sqrtA * alpha);
    const double b1 =  2 * A * ((A - 1) - (A + 1) * cosw);
    const double b2 =      A * ((A + 1) - (A - 1) * cosw - 2 * sqrtA * alpha);
    const double a0_ =         (A + 1) + (A - 1) * cosw + 2 * sqrtA * alpha;
    const double a1_ =    -2 * ((A - 1) + (A + 1) * cosw);
    const double a2_ =         (A + 1) + (A - 1) * cosw - 2 * sqrtA * alpha;

    return BiquadCoeffs{
        b0 / a0_, b1 / a0_, b2 / a0_, a1_ / a0_, a2_ / a0_
    };
}

BiquadCoeffs Equalizer::makeHighShelf(double freq, double gainDb, double q) const
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * M_PI * freq / m_sampleRate;
    const double cosw = std::cos(omega);
    const double sinw = std::sin(omega);
    const double alpha = sinw / (2.0 * q);
    const double sqrtA = A;

    const double b0 =      A * ((A + 1) + (A - 1) * cosw + 2 * sqrtA * alpha);
    const double b1 = -2 * A * ((A - 1) + (A + 1) * cosw);
    const double b2 =      A * ((A + 1) + (A - 1) * cosw - 2 * sqrtA * alpha);
    const double a0_ =         (A + 1) - (A - 1) * cosw + 2 * sqrtA * alpha;
    const double a1_ =     2 * ((A - 1) - (A + 1) * cosw);
    const double a2_ =         (A + 1) - (A - 1) * cosw - 2 * sqrtA * alpha;

    return BiquadCoeffs{
        b0 / a0_, b1 / a0_, b2 / a0_, a1_ / a0_, a2_ / a0_
    };
}

BiquadCoeffs Equalizer::makePeaking(double freq, double gainDb, double q) const
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * M_PI * freq / m_sampleRate;
    const double cosw = std::cos(omega);
    const double sinw = std::sin(omega);
    const double alpha = sinw / (2.0 * q);
    const double sqrtA = A;

    const double b0 =  1 + alpha * sqrtA;
    const double b1 = -2 * cosw;
    const double b2 =  1 - alpha * sqrtA;
    const double a0_ = 1 + alpha / sqrtA;
    const double a1_ = -2 * cosw;
    const double a2_ =  1 - alpha / sqrtA;

    return BiquadCoeffs{
        b0 / a0_, b1 / a0_, b2 / a0_, a1_ / a0_, a2_ / a0_
    };
}

// ================================================================

BiquadCoeffs Equalizer::makeCoeffsForGain(int band, double gainDb) const
{
    // Band 0 = low-shelf (covers sub-bass down to 20Hz)
    if (band == 0)
        return makeLowShelf(EQ_FREQUENCIES[band], gainDb, 0.707);
    // Band 17 = high-shelf (covers up to Nyquist)
    if (band == EQ_BAND_COUNT - 1)
        return makeHighShelf(EQ_FREQUENCIES[band], gainDb, 0.707);
    return makePeaking(EQ_FREQUENCIES[band], gainDb, 1.414);
}

float Equalizer::processBiquad(BiquadState &s, const BiquadCoeffs &c, float x) const
{
    const double y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2
                   - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return static_cast<float>(y);
}

// ================================================================

void Equalizer::updateActiveBands()
{
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        m_activeBand[i] = (std::abs(m_gains[i]) > 0.05);
    }
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

    // Start a short interpolation from prior gains to avoid zipper noise.
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
    for (auto &ch : m_chState) {
        for (auto &s : ch) {
            s.x1 = s.x2 = s.y1 = s.y2 = 0;
        }
    }
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
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        if (m_gains[i] > maxGain)
            maxGain = m_gains[i];
    }
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

void Equalizer::process(char *data, qint64 bytes, int channels, int bytesPerSample)
{
    if (!m_enabled) {
        resetState();
        return;
    }
    if (bytesPerSample != 4 || channels < 1) return;

    const int frameSize = channels * bytesPerSample;
    const qint64 frames = bytes / frameSize;
    if (frames <= 0) return;

    // Quick bypass: all gains flat, no transition
    bool anyActive = false;
    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        if (std::abs(m_gains[i]) > 0.05) {
            anyActive = true;
            break;
        }
    }
    if (!anyActive && m_transFramesLeft <= 0) {
        // EQ is flat — bypass
        return;
    }

    ensureChannelState(channels);

    float *samples = reinterpret_cast<float *>(data);
    const double gainLinear = m_preampLinear * (m_autoLevelEnabled ? m_autoLevelLinear : 1.0);

    const double transTotal = static_cast<double>(kTransitionFrames);

    // Per-frame processing: interpolate gains → compute coeffs → process
    // Interpolating gains (not coeffs) guarantees filter stability
    double interpGains[EQ_BAND_COUNT];

    for (qint64 f = 0; f < frames; ++f) {
        const bool transitioning = (m_transFramesLeft > 0);

        // Compute interpolated gains for this frame
        if (transitioning) {
            double t = 1.0 - (m_transFramesLeft / transTotal);
            for (int b = 0; b < EQ_BAND_COUNT; ++b) {
                interpGains[b] = m_prevGains[static_cast<size_t>(b)]
                               + (m_gains[static_cast<size_t>(b)] - m_prevGains[static_cast<size_t>(b)]) * t;
            }
        }

        for (int c = 0; c < channels; ++c) {
            const size_t idx = static_cast<size_t>(f * channels + c);
            float v = samples[idx];

            for (int b = 0; b < EQ_BAND_COUNT; ++b) {
                const double g = transitioning ? interpGains[b] : m_gains[static_cast<size_t>(b)];
                if (std::abs(g) < 0.05) continue; // skip inactive bands

                const BiquadCoeffs coeff = makeCoeffsForGain(b, g);
                v = processBiquad(
                    m_chState[static_cast<size_t>(c)][static_cast<size_t>(b)],
                    coeff, v);
            }

            v *= static_cast<float>(gainLinear);
            samples[idx] = v;
        }

        if (transitioning) {
            --m_transFramesLeft;
            if (m_transFramesLeft <= 0) {
                m_transFramesLeft = 0;
                m_prevGains = m_gains;
            }
        }
    }
}
