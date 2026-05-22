#ifndef EQUALIZER_H
#define EQUALIZER_H

#include <QObject>
#include <array>

static constexpr int EQ_BAND_COUNT = 18;

// Exact foobar2000 standard 18-band frequencies
static constexpr double EQ_FREQUENCIES[EQ_BAND_COUNT] = {
    55, 77, 110, 156, 220, 311, 440, 622, 880,
    1200, 1800, 2500, 3500, 5000, 7000, 10000, 14000, 20000
};

inline const char* eqLabel(int i) {
    static const char* labels[EQ_BAND_COUNT] = {
        "55", "77", "110", "156", "220", "311", "440", "622", "880",
        "1.2k", "1.8k", "2.5k", "3.5k", "5k", "7k", "10k", "14k", "20k"
    };
    return labels[i];
}

class Equalizer : public QObject
{
    Q_OBJECT
public:
    explicit Equalizer(QObject *parent = nullptr);

    void setBandGain(int band, double dBGain);
    double bandGain(int band) const;
    
    void setPreamp(double dB);
    double preamp() const { return m_preampDb; }
    
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    void setAutoLevelEnabled(bool on);
    bool autoLevelEnabled() const { return m_autoLevelEnabled; }
    double autoLevelDb() const { return m_autoLevelDb; }

    void beginBatch() { m_batchMode = true; }
    void endBatch();

    double computePeakGainDb() const;

signals:
    void bandsChanged();
    void autoLevelChanged(double dB);

private:
    void recalcAutoLevel();

    bool m_enabled = true;
    bool m_autoLevelEnabled = false;
    double m_preampDb = 0.0;
    double m_autoLevelDb = 0.0;

    std::array<double, EQ_BAND_COUNT> m_gains{};
    bool m_batchMode = false;
    bool m_dirty = false;
};

#endif