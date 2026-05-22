#ifndef GAPLESSAUDIOENGINE_H
#define GAPLESSAUDIOENGINE_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QPair>
#include <QMap>

#include "Equalizer.h"

#ifdef MUSICPLAYER_HAS_BASS
#include <bass.h>
#include <bass_fx.h>
#include <bassflac.h>
#include <bassmix.h>
#else
typedef unsigned long HSTREAM;
typedef unsigned long HFX;
typedef unsigned long HSYNC;
#endif

class GaplessAudioEngine : public QObject
{
    Q_OBJECT
public:
    enum State { Stopped, Playing, Paused };

    explicit GaplessAudioEngine(QObject *parent = nullptr);
    ~GaplessAudioEngine();

    void play(const QString &filePath);
    void pause();
    void resume();
    void stop();
    void seek(qint64 positionMs);
    void setVolume(float vol);
    float volume() const { return m_volume; }
    void setPlaybackRate(float rate);
    float playbackRate() const { return m_playbackRate; }
    void setCrossfadeDurationMs(int durationMs);
    int crossfadeDurationMs() const { return m_crossfadeDurationMs; }
    void setEqualizer(Equalizer *eq);

    // Stubs for API compatibility
    static QString backendQtId() { return "bass"; }
    static QString backendWasapiSharedId() { return "bass"; }
    static QString backendWasapiExclusiveId() { return "bass"; }
    static QString backendWasapiCustomId() { return "bass"; }
    void setBackendPreferenceId(const QString &) {}
    QString backendPreferenceId() const { return "bass"; }
    QString activeBackendId() const { return "bass"; }
    QString activeBackendDisplayName() const { return "BASS Engine"; }

    static QString decoderQtId() { return "bass"; }
    static QString decoderFfmpegId() { return "bass"; }
    void setDecoderPreferenceId(const QString &) {}
    QString decoderPreferenceId() const { return "bass"; }
    QString activeDecoderId() const { return "bass"; }
    QString activeDecoderDisplayName() const { return "BASS Decoder"; }

    void setOutputDevicePreferenceId(const QString &deviceId);
    QString outputDevicePreferenceId() const { return m_outputDevicePreferenceId; }
    QString activeOutputDeviceId() const { return m_activeOutputDeviceId; }
    QString activeOutputDeviceName() const;
    bool canSelectOutputDevice() const { return true; }
    QVector<QPair<QString, QString>> availableOutputDevices() const;

    void prepareNext(const QString &filePath);

    State state() const { return m_state; }
    qint64 position() const;
    qint64 duration() const;
    QString currentFilePath() const { return m_currentFilePath; }

signals:
    void positionChanged(qint64 positionMs);
    void durationChanged(qint64 durationMs);
    void trackTransitioned();
    void playbackFinished();
    void stateChanged(GaplessAudioEngine::State newState);
    void backendChanged();
    void decoderChanged();
    void outputDeviceChanged();
    void playbackRateChanged(float rate);
    void currentAudioLevel(float level);
    void bassLevel(float level);

private slots:
    void onPositionTick();
    void processPendingTransitions();
    void applyEqToAll();

private:
    void initBass();
    void freeBass();
    bool selectBassDevice(int deviceIndex);
    HSTREAM createStream(const QString &filePath);
    void setupStream(HSTREAM stream);
    void applyVolume(HSTREAM stream);
    void applyPlaybackRate(HSTREAM stream);
    void setupEq(HSTREAM stream);
    void updateEq(HSTREAM stream);
    void updateState(State newState);

    static void CALLBACK AudioEndSyncProc(HSYNC handle, unsigned long channel, unsigned long data, void *user);
    static void CALLBACK CrossfadeSyncProc(HSYNC handle, unsigned long channel, unsigned long data, void *user);

    int getBassDeviceIndex(const QString &deviceId) const;

    State m_state = Stopped;
    float m_volume = 0.7f;
    float m_playbackRate = 1.0f;
    int m_crossfadeDurationMs = 3000;
    QString m_currentFilePath;
    QString m_preparedNextPath;
    QString m_outputDevicePreferenceId;
    QString m_activeOutputDeviceId;
    QString m_activeOutputDeviceName;

    Equalizer *m_equalizer = nullptr;
    QTimer *m_positionTimer = nullptr;

    HSTREAM m_mixer = 0;
    HSTREAM m_activeStream = 0;
    HSTREAM m_nextStream = 0;
    HSYNC m_endSync = 0;
    HSYNC m_crossfadeSync = 0;
    
    struct FxState {
        HFX eq = 0;
        HFX vol = 0;
    };
    QMap<HSTREAM, FxState> m_streamFx;
    
    bool m_transitionPending = false;
};

#endif // GAPLESSAUDIOENGINE_H