#ifndef GAPLESSAUDIOENGINE_H
#define GAPLESSAUDIOENGINE_H

#include <QObject>
#include <QIODevice>
#include <QAudioFormat>
#include <QAudioBuffer>
#include <QByteArray>
#include <QMutex>
#include <QTimer>
#include <QElapsedTimer>
#include <QPair>
#include <QVector>
#include <memory>

#include "AudioDecoderBackend.h"
#include "AudioOutputBackend.h"

class Equalizer;

class AudioBuffer
{
public:
    void write(const char *data, qint64 len);
    qint64 read(char *data, qint64 maxLen);
    qint64 size() const;
    void clear();
    void swap(AudioBuffer &other);
private:
    QByteArray m_buf;
    qint64 m_readPos = 0;
    mutable QMutex m_mutex;
};

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
    void setEqualizer(Equalizer *eq) { m_equalizer = eq; }

    static QString backendQtId();
    static QString backendWasapiSharedId();
    static QString backendWasapiExclusiveId();
    static QString backendWasapiCustomId();
    void setBackendPreferenceId(const QString &backendId);
    QString backendPreferenceId() const { return m_backendPreferenceId; }
    QString activeBackendId() const { return m_activeBackendId; }
    QString activeBackendDisplayName() const;

    static QString decoderQtId();
    static QString decoderFfmpegId();
    void setDecoderPreferenceId(const QString &decoderId);
    QString decoderPreferenceId() const { return m_decoderPreferenceId; }
    QString activeDecoderId() const { return m_activeDecoderId; }
    QString activeDecoderDisplayName() const;

    void setOutputDevicePreferenceId(const QString &deviceId);
    QString outputDevicePreferenceId() const { return m_outputDevicePreferenceId; }
    QString activeOutputDeviceId() const { return m_activeOutputDeviceId; }
    QString activeOutputDeviceName() const { return m_activeOutputDeviceName; }
    bool canSelectOutputDevice() const;
    QVector<QPair<QString, QString>> availableOutputDevices() const;

    void prepareNext(const QString &filePath);

    State state() const { return m_state; }
    qint64 position() const;
    qint64 duration() const { return m_duration; }
    QString currentFilePath() const { return m_currentFilePath; }

    QAudioFormat outputFormat() const { return m_format; }

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

private slots:
    void onDecoderBufferReady(const QAudioBuffer &buffer);
    void onDecoderStateChanged(AudioDecoderBackend::State state);
    void onDecoderEndOfStream();
    void onDecoderDurationChanged(qint64 durationMs);
    void onNextDecoderBufferReady(const QAudioBuffer &buffer);
    void onNextDecoderEndOfStream();
    void onNextDecoderDurationChanged(qint64 durationMs);
    void onPositionTick();

private:
    AudioDecoderBackend *createDecoderBackend();
    void ensureSinkRunning();
    void destroyOutput();
    void rebuildOutputBackend();
    void rebuildDecoderBackend();
    void clearPreparedNext();
    void beginPrepareNextDecoder();
    void promotePreparedNextDecoder();
    QString normalizeDecoderId(const QString &decoderId) const;
    void connectDecoderSignals();
    void applyOutputFade(char *data, qint64 bytes);
    bool attemptSeekBackoffRecovery(const char *reason);
    void feedSink();
    void resetStreamState();
    void updateState(State newState);
    bool formatsMatch(const QAudioFormat &a, const QAudioFormat &b) const;
    QString normalizeBackendId(const QString &backendId) const;
    void syncActiveOutputDeviceInfo();
    void markAudibleTransition(const QString &fromPath, const QString &toPath, const char *reason);
    void updateAudibleTransitionTrace();
    void finalizePendingTrackTransition();

    QAudioFormat m_format;
    std::unique_ptr<AudioOutputBackend> m_output;

    AudioDecoderBackend *m_decoder = nullptr;
    AudioDecoderBackend *m_nextDecoder = nullptr;

    AudioBuffer m_currentBuf;
    AudioBuffer m_nextBuf;
    QByteArray m_pendingPcm;
    QByteArray m_chunkScratch;

    QString m_currentFilePath;
    QString m_nextFilePath;

    qint64 m_duration = 0;
    qint64 m_decoderPositionBaseMs = 0;
    float m_volume = 0.7f;
    float m_playbackRate = 1.0f;
    int m_crossfadeDurationMs = 3000;

    bool m_sourceEnded = false;
    bool m_nextSourceEnded = false;
    bool m_playbackFinishedEmitted = false;
    bool m_needEqPrebuffer = true;
    bool m_sinkEqMode = false;
    bool m_waitingForNextBuffer = false;
    bool m_crossfadeActive = false;
    bool m_crossfadePendingPromotion = false;
    int m_outputFadeFramesRemaining = 0;
    qint64 m_crossfadeFramesTotal = 0;
    qint64 m_crossfadeFramesDone = 0;
    qint64 m_nextDuration = 0;
    QString m_preparedNextPath;
    QElapsedTimer m_nextBufferWaitTimer;
    QByteArray m_crossfadeScratch;

    QString m_backendPreferenceId = QStringLiteral("wasapi-shared");
    QString m_activeBackendId = QStringLiteral("wasapi-shared");
    QString m_decoderPreferenceId = QStringLiteral("ffmpeg-decoder");
    QString m_activeDecoderId = QStringLiteral("ffmpeg-decoder");
    QString m_outputDevicePreferenceId;
    QString m_activeOutputDeviceId;
    QString m_activeOutputDeviceName = QStringLiteral("System Default");

    quint64 m_seekTraceId = 0;
    qint64 m_seekTraceTargetMs = -1;
    qint64 m_seekTraceDecoderPositionBaseMs = 0;
    bool m_seekTraceWaitingForFirstBuffer = false;
    bool m_seekTraceWaitingForFirstWrite = false;
    bool m_seekTraceWarnedNoWrite = false;
    bool m_seekTraceRecoveryAttempted = false;
    bool m_seekTraceReopenAttempted = false;
    bool m_seekTraceWrongStartRecoveryAttempted = false;
    int m_seekTraceBackoffAttempts = 0;
    int m_seekTraceImmediateEosCount = 0;
    QElapsedTimer m_seekTraceTimer;

    bool m_audibleTransitionPending = false;
    qint64 m_audibleTransitionMarkedAtMs = 0;
    qint64 m_audibleTransitionEstimatedDelayMs = 0;
    QString m_audibleTransitionFromPath;
    QString m_audibleTransitionToPath;

    // For gapless transition: track pre-buffered audio position
    bool m_inGaplessTransition = false;
    qint64 m_gaplessTailBytes = -1;
    qint64 m_gaplessTailPrev = -1;
    bool m_pendingTrackTransitioned = false;
    qint64 m_pendingNextDuration = 0;
    qint64 m_gaplessTransitionInitialBufMs = 0;
    qint64 m_gaplessTransitionDecoderOffsetMs = 0;

    State m_state = Stopped;
    QTimer *m_positionTimer = nullptr;
    QTimer *m_feedTimer = nullptr;
    Equalizer *m_equalizer = nullptr;
};

#endif // GAPLESSAUDIOENGINE_H