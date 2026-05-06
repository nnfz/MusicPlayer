#ifndef EQUALIZER_H
#define EQUALIZER_H

#include <QObject>
#include <array>
#include <vector>
#include <cmath>

static constexpr int EQ_BAND_COUNT = 18;

static constexpr double EQ_FREQUENCIES[EQ_BAND_COUNT] = {
    65, 92, 131, 185, 262, 370, 523, 740,
    1047, 1480, 2093, 2960, 4186, 5920, 8372, 11840, 16744, 20000
};

inline const char* eqLabel(int i) {
    static const char* labels[EQ_BAND_COUNT] = {
        "65", "92", "131", "185", "262", "370", "523", "740",
        "1.0k", "1.5k", "2.1k", "3.0k", "4.2k", "5.9k", "8.4k", "12k", "17k", "20k"
    };
    return labels[i];
}

struct BiquadState {
    double x1 = 0, x2 = 0;
    double y1 = 0, y2 = 0;
};

struct BiquadCoeffs {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

class Equalizer : public QObject
{
    Q_OBJECT
public:
    explicit Equalizer(QObject *parent = nullptr);

    void setSampleRate(int sampleRate);
    void setBandGain(int band, double dBGain);
    double bandGain(int band) const;
    void setPreamp(double dB);
    double preamp() const { return m_preampDb; }
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    void setAutoLevelEnabled(bool on);
    bool autoLevelEnabled() const { return m_autoLevelEnabled; }
    double autoLevelDb() const { return m_autoLevelDb; }
    bool hasOutput() const;
    void beginBatch();
    void endBatch();

    void process(char *data, qint64 bytes, int channels, int bytesPerSample);

    void resetState();
    void prepareForSeek();

    double computePeakGainDb() const;

    static int preferredProcessBytes(int, int) { return 256; }

signals:
    void bandsChanged();
    void autoLevelChanged(double dB);

private:
    static constexpr int kTransitionFrames = 512;

    BiquadCoeffs makeLowShelf(double freq, double gainDb, double q) const;
    BiquadCoeffs makeHighShelf(double freq, double gainDb, double q) const;
    BiquadCoeffs makePeaking(double freq, double gainDb, double q) const;
    BiquadCoeffs makeCoeffsForGain(int band, double gainDb) const;
    void updateActiveBands();
    void ensureChannelState(int channels);
    float processBiquad(BiquadState &s, const BiquadCoeffs &c, float x) const;
    void recalcAutoLevel();

    int m_sampleRate = 44100;
    bool m_enabled = true;
    bool m_autoLevelEnabled = false;
    double m_preampDb = 0.0;
    double m_preampLinear = 1.0;
    double m_autoLevelDb = 0.0;
    double m_autoLevelLinear = 1.0;

    std::array<double, EQ_BAND_COUNT> m_gains{};
    std::array<double, EQ_BAND_COUNT> m_prevGains{};

    // Whether each band is active (gain != 0)
    std::array<bool, EQ_BAND_COUNT> m_activeBand{};

    // Biquad state: one per band per channel
    std::vector<std::array<BiquadState, EQ_BAND_COUNT>> m_chState;
    int m_lastChannels = 0;
    int m_transFramesLeft = 0;

    bool m_batchMode = false;
    bool m_coeffDirty = false;
};

#endif // EQUALIZER_H
