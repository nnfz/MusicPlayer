#ifndef FFMPEGDECODERBACKEND_H
#define FFMPEGDECODERBACKEND_H

#include "AudioDecoderBackend.h"

#include <QAudioFormat>
#include <QString>
#include <QTimer>
#include <QByteArray>

class FfmpegDecoderBackend : public AudioDecoderBackend
{
    Q_OBJECT
public:
    explicit FfmpegDecoderBackend(const QAudioFormat &format, QObject *parent = nullptr);
    ~FfmpegDecoderBackend() override;

    bool openSource(const QString &filePath) override;
    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 positionMs) override;
    qint64 position() const override;
    qint64 duration() const override;
    State state() const override;
    void setPlaybackRate(float rate) override;
    float playbackRate() const override;

private slots:
    void onPumpTimeout();

private:
    void closeSource();
    bool decodeSomePcm();
    bool convertFrameToPcm();
    void parsePacketGaplessMetadata();
    void applyQueuedLeadingTrim();
    void applyQueuedTrailingTrimIfReady();
    void updateDurationForGaplessTrim();
    qint64 inputSamplesToOutputFrames(qint64 inputSamples) const;
    QByteArray applyPlaybackRate(const QByteArray &chunk, int frameBytes);
    void resetPlaybackRateResampler();
    void emitQueuedChunk();
    static int avToQtSampleFormatBytes(int avSampleFmt);
    void preloadInitialPackets();

#ifdef MUSICPLAYER_HAS_FFMPEG
    bool ensurePlaybackRateResampler();
#endif

    QAudioFormat m_format;
    QString m_currentSource;
    State m_state = State::Stopped;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    qint64 m_rawDurationMs = 0;
    qint64 m_emittedFrames = 0;
    qint64 m_leadingTrimFrames = 0;
    qint64 m_pendingLeadingTrimFrames = 0;
    qint64 m_trailingTrimFrames = 0;
    qint64 m_pendingTrailingTrimFrames = 0;
    qint64 m_seekTargetMs = 0;
    bool m_seekDiscardActive = false;
    int m_seekAnchorFallbackAttempts = 0;
    qint64 m_seekDiscardOutputFrames = 0;
    qint64 m_lastPacketTsMs = -1;
    int m_seekUnknownFramesToDrop = 0;
    bool m_reachedEnd = false;
    bool m_endOfStreamEmitted = false;
    bool m_allowGaplessLeadingTrimAdoption = false;
    float m_playbackRate = 1.0f;
    QByteArray m_pcmQueue;
    QTimer *m_pumpTimer = nullptr;

#ifdef MUSICPLAYER_HAS_FFMPEG
    struct AVFormatContext *m_formatCtx = nullptr;
    struct AVCodecContext *m_codecCtx = nullptr;
    struct AVPacket *m_packet = nullptr;
    struct AVFrame *m_frame = nullptr;
    struct SwrContext *m_swrCtx = nullptr;
    struct SwrContext *m_rateSwrCtx = nullptr;
    int m_streamIndex = -1;
    int m_rateSwrInRate = 0;
    int m_rateSwrOutRate = 0;
    float m_rateSwrAppliedRate = 1.0f;
    bool m_decoderFlushing = false;
    bool m_packetPending = false;
#endif
};

#endif // FFMPEGDECODERBACKEND_H