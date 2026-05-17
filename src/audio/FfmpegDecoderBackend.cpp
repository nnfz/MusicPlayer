#include "FfmpegDecoderBackend.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QVector>
#include <QtGlobal>
#include <limits>
#include <cmath>
#include <QElapsedTimer>
#include <QThread>

#ifdef MUSICPLAYER_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace {
constexpr int kPumpIntervalMs = 10;
constexpr int kPumpChunkMs = 64;
constexpr qint64 kMaxReasonableGaplessLeadingTrimMs = 500;
constexpr qint64 kMaxReasonableGaplessTrailingTrimMs = 500;
constexpr qint64 kMaxReasonableGaplessTotalTrimMs = 1000;
constexpr int kSeekAnchorFallbackMaxAttempts = 6;
constexpr qint64 kSeekAnchorStepMs = 20000;
constexpr int kPlaybackRateMinOutRate = 2000;
constexpr int kPlaybackRateMaxOutRate = 384000;

const char *decoderStateToString(AudioDecoderBackend::State state)
{
    switch (state) {
    case AudioDecoderBackend::State::Stopped:
        return "Stopped";
    case AudioDecoderBackend::State::Playing:
        return "Playing";
    case AudioDecoderBackend::State::Paused:
        return "Paused";
    }
    return "Unknown";
}

quint64 nextDecoderSeekTraceId()
{
    static quint64 s_seekTraceId = 0;
    return ++s_seekTraceId;
}

int playbackRateOutRate(int inRate, float rate)
{
    const int safeInRate = qMax(1, inRate);
    const float safeRate = qBound(0.5f, rate, 2.0f);
    const int outRate = static_cast<int>(std::lround(static_cast<double>(safeInRate)
                                                     / static_cast<double>(safeRate)));
    return qBound(kPlaybackRateMinOutRate, outRate, kPlaybackRateMaxOutRate);
}

#ifdef MUSICPLAYER_HAS_FFMPEG
QString avErrorToString(int err)
{
    char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, errBuf, sizeof(errBuf));
    return QString::fromUtf8(errBuf);
}

AVSampleFormat toAvSampleFormat(QAudioFormat::SampleFormat sampleFormat)
{
    switch (sampleFormat) {
    case QAudioFormat::UInt8:
        return AV_SAMPLE_FMT_U8;
    case QAudioFormat::Int16:
        return AV_SAMPLE_FMT_S16;
    case QAudioFormat::Int32:
        return AV_SAMPLE_FMT_S32;
    case QAudioFormat::Float:
        return AV_SAMPLE_FMT_FLT;
    case QAudioFormat::Unknown:
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}

struct DurationCacheEntry
{
    qint64 durationMs = 0;
    qint64 fileSize = -1;
    QDateTime lastModifiedUtc;
};

struct SeekPoint
{
    qint64 ms = 0;
    int64_t pos = 0;
};

struct SeekMapCacheEntry
{
    QVector<SeekPoint> points;
    qint64 fileSize = -1;
    QDateTime lastModifiedUtc;
};

QHash<QString, DurationCacheEntry> &durationCache()
{
    static QHash<QString, DurationCacheEntry> s_cache;
    return s_cache;
}

QHash<QString, SeekMapCacheEntry> &seekMapCache()
{
    static QHash<QString, SeekMapCacheEntry> s_cache;
    return s_cache;
}

QString durationCacheKey(const QString &filePath)
{
    return QDir::toNativeSeparators(filePath).toLower();
}

int seekByByteOffset(AVFormatContext *formatCtx, int64_t byteTarget)
{
    if (!formatCtx)
        return AVERROR(EINVAL);

    int err = av_seek_frame(formatCtx,
                            -1,
                            byteTarget,
                            AVSEEK_FLAG_BYTE | AVSEEK_FLAG_BACKWARD);
    if (err < 0) {
        err = avformat_seek_file(formatCtx,
                                 -1,
                                 INT64_MIN,
                                 byteTarget,
                                 INT64_MAX,
                                 AVSEEK_FLAG_BYTE | AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    }
    return err;
}

QVector<SeekPoint> probeAudioSeekPointsByPacketScan(const QString &filePath)
{
    QVector<SeekPoint> points;
    AVFormatContext *scanCtx = nullptr;
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const QByteArray utf8Path = nativePath.toUtf8();

    auto tryOpen = [&scanCtx](const QByteArray &path) {
        return avformat_open_input(&scanCtx, path.constData(), nullptr, nullptr);
    };

    int err = tryOpen(utf8Path);
#ifdef Q_OS_WIN
    if (err < 0) {
        if (scanCtx)
            avformat_close_input(&scanCtx);

        const QByteArray ansiPath = QFile::encodeName(nativePath);
        if (ansiPath != utf8Path)
            err = tryOpen(ansiPath);
    }
#endif
    if (err < 0 || !scanCtx)
        return points;

    if (avformat_find_stream_info(scanCtx, nullptr) < 0) {
        avformat_close_input(&scanCtx);
        return points;
    }

    const int streamIndex = av_find_best_stream(scanCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        avformat_close_input(&scanCtx);
        return points;
    }

    AVStream *stream = scanCtx->streams[streamIndex];
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        avformat_close_input(&scanCtx);
        return points;
    }

    qint64 originMs = -1;
    qint64 lastKeptMs = std::numeric_limits<qint64>::min() / 4;
    int64_t lastKeptPos = -1;

    while (av_read_frame(scanCtx, packet) >= 0) {
        if (packet->stream_index == streamIndex && packet->pos >= 0) {
            const int64_t ts = (packet->dts != AV_NOPTS_VALUE)
                ? packet->dts
                : packet->pts;
            if (ts != AV_NOPTS_VALUE) {
                const qint64 packetMsRaw =
                    av_rescale_q(ts, stream->time_base, AVRational{1, 1000});
                if (originMs < 0)
                    originMs = packetMsRaw;

                const qint64 packetMs = qMax<qint64>(0, packetMsRaw - originMs);
                const bool shouldKeep = points.isEmpty()
                    || (packetMs - lastKeptMs >= 250)
                    || (lastKeptPos >= 0 && packet->pos - lastKeptPos >= 32 * 1024);

                if (shouldKeep) {
                    SeekPoint point;
                    point.ms = packetMs;
                    point.pos = packet->pos;
                    points.push_back(point);
                    lastKeptMs = packetMs;
                    lastKeptPos = packet->pos;
                }
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    avformat_close_input(&scanCtx);
    return points;
}

bool resolvePacketAlignedByteTarget(const QString &filePath,
                                    qint64 targetMs,
                                    int64_t fileSize,
                                    int64_t *outByteTarget,
                                    qint64 *outMappedMs)
{
    if (!outByteTarget)
        return false;

    QFileInfo info(filePath);
    if (!info.exists())
        return false;

    const QString key = durationCacheKey(info.absoluteFilePath());
    const qint64 infoSize = info.size();
    const QDateTime infoMtimeUtc = info.lastModified().toUTC();

    auto it = seekMapCache().find(key);
    const bool cacheValid = (it != seekMapCache().end()
        && it->fileSize == infoSize
        && it->lastModifiedUtc == infoMtimeUtc);

    if (!cacheValid) {
        SeekMapCacheEntry entry;
        entry.points = probeAudioSeekPointsByPacketScan(info.absoluteFilePath());
        entry.fileSize = infoSize;
        entry.lastModifiedUtc = infoMtimeUtc;
        it = seekMapCache().insert(key, entry);
    }

    if (it == seekMapCache().end() || it->points.isEmpty())
        return false;

    int64_t bestPos = it->points.first().pos;
    qint64 bestMs = it->points.first().ms;
    for (const SeekPoint &point : it->points) {
        if (point.ms > targetMs)
            break;
        bestPos = point.pos;
        bestMs = point.ms;
    }

    if (fileSize > 0)
        bestPos = qBound<int64_t>(0, bestPos, fileSize - 1);

    *outByteTarget = bestPos;
    if (outMappedMs)
        *outMappedMs = bestMs;

    return true;
}

qint64 probeAudioDurationMsByPacketScan(const QString &filePath)
{
    AVFormatContext *scanCtx = nullptr;
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const QByteArray utf8Path = nativePath.toUtf8();

    auto tryOpen = [&scanCtx](const QByteArray &path) {
        return avformat_open_input(&scanCtx, path.constData(), nullptr, nullptr);
    };

    int err = tryOpen(utf8Path);
#ifdef Q_OS_WIN
    if (err < 0) {
        if (scanCtx)
            avformat_close_input(&scanCtx);

        const QByteArray ansiPath = QFile::encodeName(nativePath);
        if (ansiPath != utf8Path)
            err = tryOpen(ansiPath);
    }
#endif
    if (err < 0 || !scanCtx)
        return 0;

    if (avformat_find_stream_info(scanCtx, nullptr) < 0) {
        avformat_close_input(&scanCtx);
        return 0;
    }

    const int streamIndex = av_find_best_stream(scanCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        avformat_close_input(&scanCtx);
        return 0;
    }

    AVStream *stream = scanCtx->streams[streamIndex];
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        avformat_close_input(&scanCtx);
        return 0;
    }

    int64_t firstTs = AV_NOPTS_VALUE;
    int64_t lastTs = AV_NOPTS_VALUE;
    int64_t lastDuration = 0;

    while (av_read_frame(scanCtx, packet) >= 0) {
        if (packet->stream_index == streamIndex) {
            const int64_t ts = (packet->dts != AV_NOPTS_VALUE)
                ? packet->dts
                : packet->pts;
            if (ts != AV_NOPTS_VALUE) {
                if (firstTs == AV_NOPTS_VALUE)
                    firstTs = ts;
                lastTs = ts;
                if (packet->duration > 0)
                    lastDuration = packet->duration;
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    qint64 scannedMs = 0;
    if (lastTs != AV_NOPTS_VALUE) {
        const int64_t endTs = lastTs + qMax<int64_t>(0, lastDuration);
        const qint64 endMs = av_rescale_q(endTs, stream->time_base, AVRational{1, 1000});
        qint64 startMs = 0;
        if (firstTs != AV_NOPTS_VALUE)
            startMs = av_rescale_q(firstTs, stream->time_base, AVRational{1, 1000});
        scannedMs = qMax<qint64>(0, endMs - startMs);
    } else if (stream->duration != AV_NOPTS_VALUE) {
        scannedMs = qMax<qint64>(0, av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000}));
    } else if (scanCtx->duration != AV_NOPTS_VALUE) {
        scannedMs = qMax<qint64>(0, scanCtx->duration / (AV_TIME_BASE / 1000));
    }

    avformat_close_input(&scanCtx);
    return scannedMs;
}

qint64 resolvedDurationMsWithCache(const QString &filePath, qint64 fallbackMs)
{
    QFileInfo info(filePath);
    if (!info.exists())
        return fallbackMs;

    const QString key = durationCacheKey(info.absoluteFilePath());
    const qint64 fileSize = info.size();
    const QDateTime lastModifiedUtc = info.lastModified().toUTC();

    const auto cacheIt = durationCache().constFind(key);
    if (cacheIt != durationCache().constEnd()
        && cacheIt->fileSize == fileSize
        && cacheIt->lastModifiedUtc == lastModifiedUtc) {
        return cacheIt->durationMs > 0 ? cacheIt->durationMs : fallbackMs;
    }

    const qint64 scannedMs = probeAudioDurationMsByPacketScan(info.absoluteFilePath());
    DurationCacheEntry entry;
    entry.durationMs = scannedMs;
    entry.fileSize = fileSize;
    entry.lastModifiedUtc = lastModifiedUtc;
    durationCache().insert(key, entry);

    return scannedMs > 0 ? scannedMs : fallbackMs;
}
#endif
}

FfmpegDecoderBackend::FfmpegDecoderBackend(const QAudioFormat &format, QObject *parent)
    : AudioDecoderBackend(parent)
    , m_format(format)
{
    m_pumpTimer = new QTimer(this);
    m_pumpTimer->setTimerType(Qt::PreciseTimer);
    m_pumpTimer->setInterval(kPumpIntervalMs);
    connect(m_pumpTimer, &QTimer::timeout,
            this, &FfmpegDecoderBackend::onPumpTimeout);
}

FfmpegDecoderBackend::~FfmpegDecoderBackend()
{
    closeSource();
}

bool FfmpegDecoderBackend::openSource(const QString &filePath)
{
    closeSource();

    if (filePath.isEmpty())
        return false;

    // Fast validation before expensive FFmpeg initialization
    QFileInfo info(filePath);
    if (!info.exists()) {
        qWarning() << "FFmpeg: file does not exist:" << filePath;
        return false;
    }
    if (!info.isFile()) {
        qWarning() << "FFmpeg: not a regular file:" << filePath;
        return false;
    }
    if (!info.isReadable()) {
        qWarning() << "FFmpeg: file not readable:" << filePath;
        return false;
    }

    // Quick format sniff using file extension to reject obviously unsupported formats
    const QString extensionCheck = filePath.toLower();
    const bool isKnownAudio = extensionCheck.endsWith(".mp3") || extensionCheck.endsWith(".flac")
        || extensionCheck.endsWith(".ogg") || extensionCheck.endsWith(".opus") || extensionCheck.endsWith(".m4a")
        || extensionCheck.endsWith(".aac") || extensionCheck.endsWith(".wav") || extensionCheck.endsWith(".wv")
        || extensionCheck.endsWith(".ape") || extensionCheck.endsWith(".tta") || extensionCheck.endsWith(".tak")
        || extensionCheck.endsWith(".mpc") || extensionCheck.endsWith(".wma") || extensionCheck.endsWith(".aiff")
        || extensionCheck.endsWith(".aif") || extensionCheck.endsWith(".pcm") || extensionCheck.endsWith(".raw")
        || extensionCheck.endsWith(".ac3") || extensionCheck.endsWith(".dts") || extensionCheck.endsWith(".spx");
    if (!isKnownAudio) {
        qWarning() << "FFmpeg: unknown audio extension for file:" << filePath;
        // Don't reject - let FFmpeg try to handle it
    }

    m_currentSource = filePath;
    m_state = State::Stopped;
    m_positionMs = 0;
    m_durationMs = 0;
    m_rawDurationMs = m_durationMs;
    m_emittedFrames = 0;
    m_leadingTrimFrames = 0;
    m_pendingLeadingTrimFrames = 0;
    m_trailingTrimFrames = 0;
    m_pendingTrailingTrimFrames = 0;
    m_seekTargetMs = 0;
    m_seekDiscardActive = false;
    m_seekAnchorFallbackAttempts = 0;
    m_seekDiscardOutputFrames = 0;
    m_lastPacketTsMs = -1;
    m_seekUnknownFramesToDrop = 0;
    m_reachedEnd = false;
    m_endOfStreamEmitted = false;
    m_allowGaplessLeadingTrimAdoption = true;
    m_pcmQueue.clear();

#ifndef MUSICPLAYER_HAS_FFMPEG
    qWarning() << "FFmpeg headers/libs are not enabled at build time. Source open rejected:" << filePath;
    return false;
#else
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const QByteArray utf8Path = nativePath.toUtf8();

    auto openWithFastProbe = [this](const QByteArray &path, bool largeProbe) {
        AVDictionary *openOpts = nullptr;
        if (largeProbe) {
            av_dict_set(&openOpts, "analyzeduration", "10000000", 0);
            av_dict_set(&openOpts, "probesize", "10000000", 0);
            av_dict_set(&openOpts, "timeout", "8000000", 0);
        } else {
            // For well-known formats, use minimal probing
            av_dict_set(&openOpts, "analyzeduration", "50000", 0);
            av_dict_set(&openOpts, "probesize", "32768", 0);
            av_dict_set(&openOpts, "timeout", "1000000", 0);
        }
        const int openErr = avformat_open_input(&m_formatCtx, path.constData(), nullptr, &openOpts);
        av_dict_free(&openOpts);
        return openErr;
    };

    // Only APE/WV/TTA/TAK need larger probes; everything else is fast
    const bool needsLargeProbe = extensionCheck.endsWith(QLatin1String(".ape"))
        || extensionCheck.endsWith(QLatin1String(".wv"))
        || extensionCheck.endsWith(QLatin1String(".tta"))
        || extensionCheck.endsWith(QLatin1String(".tak"));

    int err = openWithFastProbe(utf8Path, needsLargeProbe);
#ifdef Q_OS_WIN
    if (err < 0) {
        if (m_formatCtx)
            avformat_close_input(&m_formatCtx);

        // Some Windows FFmpeg builds still expect ANSI file paths.
        const QByteArray ansiPath = QFile::encodeName(nativePath);
        if (ansiPath != utf8Path)
            err = openWithFastProbe(ansiPath, needsLargeProbe);
    }
#endif
    if (err < 0) {
        qWarning() << "FFmpeg: avformat_open_input failed for" << filePath << avErrorToString(err);
        closeSource();
        return false;
    }

    int infoErr = avformat_find_stream_info(m_formatCtx, nullptr);
    if (infoErr < 0) {
        qWarning() << "FFmpeg: avformat_find_stream_info failed for" << filePath << avErrorToString(infoErr);
        closeSource();
        return false;
    }

    m_streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // For well-known formats, skip expensive probing - format is already known
    if (m_streamIndex < 0) {
        qWarning() << "FFmpeg: no audio stream found for" << filePath;
        closeSource();
        return false;
    }

    AVStream *stream = m_formatCtx->streams[m_streamIndex];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        qWarning() << "FFmpeg: decoder not found for codec id" << stream->codecpar->codec_id;
        closeSource();
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qWarning() << "FFmpeg: avcodec_alloc_context3 failed";
        closeSource();
        return false;
    }

    err = avcodec_parameters_to_context(m_codecCtx, stream->codecpar);
    if (err < 0) {
        qWarning() << "FFmpeg: avcodec_parameters_to_context failed" << avErrorToString(err);
        closeSource();
        return false;
    }

#ifdef AV_CODEC_FLAG2_SKIP_MANUAL
    // Handle gapless skip/discard ourselves to avoid decoder timestamp-update
    // issues on some MP3 seek paths.
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
#endif

    err = avcodec_open2(m_codecCtx, codec, nullptr);
    if (err < 0) {
        qWarning() << "FFmpeg: avcodec_open2 failed" << avErrorToString(err);
        closeSource();
        return false;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (!m_packet || !m_frame) {
        qWarning() << "FFmpeg: packet/frame allocation failed";
        closeSource();
        return false;
    }

    const int outChannels = qMax(1, m_format.channelCount());
    const int outSampleRate = qMax(1, m_format.sampleRate());
    AVSampleFormat outSampleFmt = toAvSampleFormat(m_format.sampleFormat());
    if (outSampleFmt == AV_SAMPLE_FMT_NONE)
        outSampleFmt = AV_SAMPLE_FMT_FLT;

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outChannels);
    err = swr_alloc_set_opts2(&m_swrCtx,
                              &outLayout,
                              outSampleFmt,
                              outSampleRate,
                              &m_codecCtx->ch_layout,
                              m_codecCtx->sample_fmt,
                              m_codecCtx->sample_rate,
                              0,
                              nullptr);
    av_channel_layout_uninit(&outLayout);
    if (err < 0 || !m_swrCtx) {
        qWarning() << "FFmpeg: swr_alloc_set_opts2 failed" << avErrorToString(err);
        closeSource();
        return false;
    }

    err = swr_init(m_swrCtx);
    if (err < 0) {
        qWarning() << "FFmpeg: swr_init failed" << avErrorToString(err);
        closeSource();
        return false;
    }

    qint64 durMs = 0;
    const bool hasStreamDuration = (stream->duration != AV_NOPTS_VALUE);
    if (hasStreamDuration) {
        durMs = av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000});
    } else if (m_formatCtx->duration != AV_NOPTS_VALUE) {
        durMs = m_formatCtx->duration / (AV_TIME_BASE / 1000);
    }

    if (stream->codecpar && stream->codecpar->codec_id == AV_CODEC_ID_MP3) {
        const qint64 refinedDurMs = resolvedDurationMsWithCache(filePath, durMs);
        const bool refinedValid = (refinedDurMs > 0);
        const bool durationEstimatedFromBitrate =
            (m_formatCtx->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE);
        const bool fallbackLikelyUnreliable = durationEstimatedFromBitrate || !hasStreamDuration;
        const bool refinementNotTooSmall = (!hasStreamDuration || durMs <= 0)
            ? true
            : (refinedDurMs * 10 >= durMs * 6);
        if (refinedValid && (fallbackLikelyUnreliable || refinementNotTooSmall)) {
            durMs = refinedDurMs;
        } else if (refinedValid && hasStreamDuration && durMs > 0) {
            qWarning() << "FFmpeg: ignoring suspicious refined MP3 duration"
                       << "fallbackMs=" << durMs
                       << "refinedMs=" << refinedDurMs
                       << "durationEstimationMethod=" << static_cast<int>(m_formatCtx->duration_estimation_method)
                       << "source=" << filePath;
        }
    }

    if (stream->codecpar && stream->codecpar->codec_id == AV_CODEC_ID_APE) {
        if (!hasStreamDuration || durMs <= 0
            || m_formatCtx->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE) {
            const qint64 refinedDurMs = resolvedDurationMsWithCache(filePath, durMs);
            if (refinedDurMs > 0) {
                durMs = refinedDurMs;
                qInfo() << "FFmpeg: APE duration refined by packet scan"
                        << "refinedMs=" << refinedDurMs
                        << "source=" << filePath;
            }
        }
    }

    m_rawDurationMs = qMax<qint64>(0, durMs);
    m_durationMs = m_rawDurationMs;
    emit durationChanged(m_durationMs);

    // Pre-decode initial packets in background to reduce first-play latency
    preloadInitialPackets();

    return true;
#endif
}

void FfmpegDecoderBackend::play()
{
    if (m_currentSource.isEmpty())
        return;

    if (m_state == State::Playing)
        return;

    // Resuming from pause must continue from current buffered timeline.
    // Auto-restart from zero is only valid for explicit replay from Stopped.
    const bool resumingFromPause = (m_state == State::Paused);

    if (!resumingFromPause && m_reachedEnd)
        seek(0);

    m_state = State::Playing;
    emit stateChanged(m_state);
    if (m_pumpTimer && !m_pumpTimer->isActive())
        m_pumpTimer->start();
}

void FfmpegDecoderBackend::pause()
{
    if (m_state != State::Playing)
        return;

    if (m_state == State::Paused)
        return;

    m_state = State::Paused;
    emit stateChanged(m_state);
    if (m_pumpTimer)
        m_pumpTimer->stop();
}

void FfmpegDecoderBackend::stop()
{
    if (m_pumpTimer)
        m_pumpTimer->stop();

#ifdef MUSICPLAYER_HAS_FFMPEG
    if (m_formatCtx && m_codecCtx && m_streamIndex >= 0) {
        av_seek_frame(m_formatCtx, m_streamIndex, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m_codecCtx);
        if (m_swrCtx) {
            swr_close(m_swrCtx);
            swr_init(m_swrCtx);
        }
    }
    m_decoderFlushing = false;
    m_packetPending = false;
#endif

    resetPlaybackRateResampler();

    m_pcmQueue.clear();
    m_reachedEnd = false;
    m_endOfStreamEmitted = false;
    m_pendingLeadingTrimFrames = m_leadingTrimFrames;
    m_pendingTrailingTrimFrames = m_trailingTrimFrames;
    m_seekTargetMs = 0;
    m_seekDiscardActive = false;
    m_seekAnchorFallbackAttempts = 0;
    m_seekDiscardOutputFrames = 0;
    m_lastPacketTsMs = -1;
    m_seekUnknownFramesToDrop = 0;
    m_emittedFrames = 0;
    m_positionMs = 0;

    if (m_state == State::Stopped)
        return;

    m_state = State::Stopped;
    emit stateChanged(m_state);
}

void FfmpegDecoderBackend::seek(qint64 positionMs)
{
    qint64 targetMs = qMax<qint64>(0, positionMs);
    const qint64 clampDurationMs = qMax<qint64>(m_rawDurationMs, m_durationMs);
    if (clampDurationMs > 0)
        targetMs = qBound<qint64>(0, targetMs, qMax<qint64>(0, clampDurationMs - 1));
    const bool targetIsStreamStart = (targetMs <= 0);

    const quint64 seekId = nextDecoderSeekTraceId();
    m_allowGaplessLeadingTrimAdoption = false;
    qInfo() << "[seek-decoder] start"
            << "id=" << seekId
            << "requestedMs=" << positionMs
            << "targetMs=" << targetMs
            << "durationMs=" << m_durationMs
            << "state=" << decoderStateToString(m_state)
            << "queueBytesBefore=" << m_pcmQueue.size()
            << "source=" << m_currentSource;

#ifdef MUSICPLAYER_HAS_FFMPEG
    if (!m_formatCtx || !m_codecCtx || m_streamIndex < 0) {
        m_positionMs = targetMs;
        qInfo() << "[seek-decoder] no-active-context"
                << "id=" << seekId
                << "positionMs=" << m_positionMs;
        return;
    }

    AVStream *stream = m_formatCtx->streams[m_streamIndex];
    const int64_t targetUs = static_cast<int64_t>(targetMs) * 1000;
    const int64_t streamTargetTsNoOffset =
        av_rescale_q(targetMs, AVRational{1, 1000}, stream->time_base);
    const int64_t streamStartTsRaw =
        (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : AV_NOPTS_VALUE;
    const int64_t formatStartUsRaw =
        (m_formatCtx->start_time != AV_NOPTS_VALUE) ? m_formatCtx->start_time : AV_NOPTS_VALUE;

    const qint64 streamStartMs = (streamStartTsRaw != AV_NOPTS_VALUE)
        ? av_rescale_q(streamStartTsRaw, stream->time_base, AVRational{1, 1000})
        : 0;
    const qint64 formatStartMs = (formatStartUsRaw != AV_NOPTS_VALUE)
        ? (formatStartUsRaw / 1000)
        : 0;

    const bool useStreamOffsetFallback =
        !targetIsStreamStart
        && (streamStartTsRaw != AV_NOPTS_VALUE)
        && (qAbs(streamStartMs) <= 3000);
    const bool useGlobalOffsetFallback =
        !targetIsStreamStart
        && (formatStartUsRaw != AV_NOPTS_VALUE)
        && (qAbs(formatStartMs) <= 3000);

    const bool isApe = stream->codecpar
        && stream->codecpar->codec_id == AV_CODEC_ID_APE;

    const bool useApeStreamOffset =
        isApe
        && !targetIsStreamStart
        && (streamStartTsRaw != AV_NOPTS_VALUE);

    if (!useStreamOffsetFallback && streamStartTsRaw != AV_NOPTS_VALUE) {
        qWarning() << "[seek-decoder] ignoring large stream start offset"
                   << "id=" << seekId
                   << "streamStartMs=" << streamStartMs
                   << "targetMs=" << targetMs
                   << "source=" << m_currentSource;
    }
    if (!useGlobalOffsetFallback && formatStartUsRaw != AV_NOPTS_VALUE) {
        qWarning() << "[seek-decoder] ignoring large format start offset"
                   << "id=" << seekId
                   << "formatStartMs=" << formatStartMs
                   << "targetMs=" << targetMs
                   << "source=" << m_currentSource;
    }

    const int64_t streamTargetTsWithOffset = useStreamOffsetFallback
        ? (streamTargetTsNoOffset + streamStartTsRaw)
        : (useApeStreamOffset ? (streamTargetTsNoOffset + streamStartTsRaw) : streamTargetTsNoOffset);
    const int64_t globalTargetUsNoOffset = targetUs;
    const int64_t globalTargetUsWithOffset = useGlobalOffsetFallback
        ? (targetUs + formatStartUsRaw)
        : targetUs;

    avformat_flush(m_formatCtx);
    avcodec_flush_buffers(m_codecCtx);

    int err = AVERROR(EINVAL);
    qint64 packetAlignedMappedMs = -1;

    // Packet-aligned byte seeking (AVSEEK_FLAG_BACKWARD) is only safe for MP3 where
    // frame boundaries are deterministic. APE uses variable-length frames and
    // byte-seeking can land on a packet far before the target, causing audio
    // to "teleport" backward. Let APE use the timestamp-based seek paths below.
    const bool canUsePacketAlignedPrimarySeek =
        !targetIsStreamStart
        && stream
        && stream->codecpar
        && stream->codecpar->codec_id == AV_CODEC_ID_MP3
        && m_formatCtx->pb;

    if (canUsePacketAlignedPrimarySeek) {
        const int64_t fileSize = avio_size(m_formatCtx->pb);
        if (fileSize > 0) {
            int64_t byteTarget = -1;
            if (resolvePacketAlignedByteTarget(m_currentSource,
                                               targetMs,
                                               fileSize,
                                               &byteTarget,
                                               &packetAlignedMappedMs)
                && byteTarget >= 0) {
                err = seekByByteOffset(m_formatCtx, byteTarget);
                if (err >= 0) {
                    qInfo() << "[seek-decoder] packet-aligned primary seek"
                            << "id=" << seekId
                            << "targetMs=" << targetMs
                            << "mappedMs=" << packetAlignedMappedMs
                            << "byteTarget=" << byteTarget
                            << "fileSize=" << fileSize
                            << "source=" << m_currentSource;
                }
            }
        }
    }

    if (targetIsStreamStart && m_formatCtx->pb) {
        err = seekByByteOffset(m_formatCtx, 0);
        if (err >= 0) {
            qInfo() << "[seek-decoder] start byte-seek"
                    << "id=" << seekId
                    << "byteTarget=0"
                    << "source=" << m_currentSource;
        }
    }

    if (err < 0)
        err = avformat_seek_file(m_formatCtx,
                                 m_streamIndex,
                                 INT64_MIN,
                                 streamTargetTsNoOffset,
                                 streamTargetTsNoOffset,
                                 AVSEEK_FLAG_BACKWARD);
    if (err < 0)
        err = av_seek_frame(m_formatCtx, m_streamIndex, streamTargetTsNoOffset, AVSEEK_FLAG_BACKWARD);

    if (err < 0 && useStreamOffsetFallback)
        err = avformat_seek_file(m_formatCtx,
                                 m_streamIndex,
                                 INT64_MIN,
                                 streamTargetTsWithOffset,
                                 streamTargetTsWithOffset,
                                 AVSEEK_FLAG_BACKWARD);
    if (err < 0 && useStreamOffsetFallback)
        err = av_seek_frame(m_formatCtx, m_streamIndex, streamTargetTsWithOffset, AVSEEK_FLAG_BACKWARD);

    if (err < 0 && useApeStreamOffset && !useStreamOffsetFallback)
        err = avformat_seek_file(m_formatCtx,
                                 m_streamIndex,
                                 INT64_MIN,
                                 streamTargetTsWithOffset,
                                 streamTargetTsWithOffset,
                                 AVSEEK_FLAG_BACKWARD);
    if (err < 0 && useApeStreamOffset && !useStreamOffsetFallback)
        err = av_seek_frame(m_formatCtx, m_streamIndex, streamTargetTsWithOffset, AVSEEK_FLAG_BACKWARD);

    if (err < 0)
        err = avformat_seek_file(m_formatCtx,
                                 -1,
                                 INT64_MIN,
                                 globalTargetUsNoOffset,
                                 globalTargetUsNoOffset,
                                 AVSEEK_FLAG_BACKWARD);
    if (err < 0)
        err = av_seek_frame(m_formatCtx, -1, globalTargetUsNoOffset, AVSEEK_FLAG_BACKWARD);

    if (err < 0 && useGlobalOffsetFallback)
        err = avformat_seek_file(m_formatCtx,
                                 -1,
                                 INT64_MIN,
                                 globalTargetUsWithOffset,
                                 globalTargetUsWithOffset,
                                 AVSEEK_FLAG_BACKWARD);
    if (err < 0 && useGlobalOffsetFallback)
        err = av_seek_frame(m_formatCtx, -1, globalTargetUsWithOffset, AVSEEK_FLAG_BACKWARD);

    if (err < 0 && !targetIsStreamStart)
        err = avformat_seek_file(m_formatCtx,
                                 m_streamIndex,
                                 INT64_MIN,
                                 streamTargetTsNoOffset,
                                 INT64_MAX,
                                 AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    if (err < 0 && useStreamOffsetFallback)
        err = avformat_seek_file(m_formatCtx,
                                 m_streamIndex,
                                 INT64_MIN,
                                 streamTargetTsWithOffset,
                                 INT64_MAX,
                                 AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    if (err < 0 && !targetIsStreamStart)
        err = avformat_seek_file(m_formatCtx,
                                 -1,
                                 INT64_MIN,
                                 globalTargetUsNoOffset,
                                 INT64_MAX,
                                 AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    if (err < 0 && useGlobalOffsetFallback)
        err = avformat_seek_file(m_formatCtx,
                                 -1,
                                 INT64_MIN,
                                 globalTargetUsWithOffset,
                                 INT64_MAX,
                                 AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);

    if (err >= 0 && m_rawDurationMs > 0 && m_formatCtx->pb) {
        const int64_t fileSize = avio_size(m_formatCtx->pb);
        const int64_t bytePos = avio_tell(m_formatCtx->pb);
        if (fileSize > 0 && bytePos >= 0) {
            const int64_t eofGuardBytes = qMax<int64_t>(4096, fileSize / 200);
            const bool targetNotNearEnd = (targetMs + 2000 < m_rawDurationMs);
            const bool landedNearEof = (bytePos + eofGuardBytes >= fileSize);
            if (targetNotNearEnd && landedNearEof) {
                qWarning() << "[seek-decoder] suspicious seek landing near EOF"
                           << "id=" << seekId
                           << "targetMs=" << targetMs
                           << "durationMs=" << m_rawDurationMs
                           << "bytePos=" << bytePos
                           << "fileSize=" << fileSize
                           << "forcingFallback=true";
                err = AVERROR(EINVAL);
            }
        }
    }

    if (err < 0 && m_formatCtx->pb) {
        const int64_t fileSize = avio_size(m_formatCtx->pb);
        if (fileSize > 0) {
            int64_t byteTarget = -1;
            qint64 mappedMs = -1;
            bool hasPacketAlignedTarget = false;

            if (targetIsStreamStart) {
                byteTarget = 0;
            } else {
                hasPacketAlignedTarget = resolvePacketAlignedByteTarget(
                    m_currentSource,
                    targetMs,
                    fileSize,
                    &byteTarget,
                    &mappedMs);

                if (!hasPacketAlignedTarget && m_rawDurationMs > 0) {
                    const int64_t byteTargetRaw = (fileSize * targetMs) / qMax<qint64>(1, m_rawDurationMs);
                    byteTarget = qBound<int64_t>(0, byteTargetRaw, fileSize - 1);
                }
            }

            if (byteTarget >= 0) {
                err = seekByByteOffset(m_formatCtx, byteTarget);
                if (err >= 0) {
                    qInfo() << "[seek-decoder] byte-fallback"
                            << "id=" << seekId
                            << "targetMs=" << targetMs
                            << "mappedMs=" << mappedMs
                            << "byteTarget=" << byteTarget
                            << "fileSize=" << fileSize
                            << "packetAligned=" << hasPacketAlignedTarget;
                }
            }
        }
    }
    if (err < 0) {
        qWarning() << "[seek-decoder] seek failed"
                   << "id=" << seekId
                   << avErrorToString(err)
                   << "targetMs=" << targetMs
                   << "source=" << m_currentSource;
        return;
    }

    if (m_formatCtx->pb) {
        m_formatCtx->pb->eof_reached = 0;
        m_formatCtx->pb->error = 0;
    }

    avformat_flush(m_formatCtx);
    avcodec_flush_buffers(m_codecCtx);
    if (m_swrCtx) {
        swr_close(m_swrCtx);
        swr_init(m_swrCtx);
    }
    resetPlaybackRateResampler();

    m_pcmQueue.clear();
    m_reachedEnd = false;
    m_endOfStreamEmitted = false;
    m_decoderFlushing = false;
    m_packetPending = false;
    m_pendingLeadingTrimFrames = 0;
    m_pendingTrailingTrimFrames = m_trailingTrimFrames;
    m_seekTargetMs = targetMs;
    m_seekDiscardActive = (targetMs > 0);
    m_seekAnchorFallbackAttempts = 0;
    m_seekDiscardOutputFrames = 0;
    m_lastPacketTsMs = -1;
    m_seekUnknownFramesToDrop = (targetMs > 0) ? 1 : 0;

    m_emittedFrames = (targetMs * qMax(1, m_format.sampleRate())) / 1000;
    m_positionMs = targetMs;

    if (m_state == State::Playing && m_pumpTimer && !m_pumpTimer->isActive()) {
        m_pumpTimer->start();
    }

    qInfo() << "[seek-decoder] complete"
        << "id=" << seekId
        << "positionMs=" << m_positionMs
        << "seekDiscardActive=" << m_seekDiscardActive
        << "seekDiscardFrames=" << m_seekDiscardOutputFrames
        << "unknownFramesToDrop=" << m_seekUnknownFramesToDrop
        << "queueBytesAfter=" << m_pcmQueue.size();
#else
    m_positionMs = targetMs;
    qInfo() << "[seek-decoder] complete-no-ffmpeg"
        << "id=" << seekId
        << "positionMs=" << m_positionMs;
#endif
}

qint64 FfmpegDecoderBackend::position() const
{
    return m_positionMs;
}

qint64 FfmpegDecoderBackend::duration() const
{
    return m_durationMs;
}

AudioDecoderBackend::State FfmpegDecoderBackend::state() const
{
    return m_state;
}

void FfmpegDecoderBackend::setPlaybackRate(float rate)
{
    const float normalized = qBound(0.5f, rate, 2.0f);
    if (qAbs(m_playbackRate - normalized) < 0.0001f)
        return;

    m_playbackRate = normalized;
    resetPlaybackRateResampler();
}

float FfmpegDecoderBackend::playbackRate() const
{
    return m_playbackRate;
}

void FfmpegDecoderBackend::resetPlaybackRateResampler()
{
#ifdef MUSICPLAYER_HAS_FFMPEG
    if (m_rateSwrCtx)
        swr_free(&m_rateSwrCtx);
    m_rateSwrInRate = 0;
    m_rateSwrOutRate = 0;
    m_rateSwrAppliedRate = 1.0f;
#endif
}

#ifdef MUSICPLAYER_HAS_FFMPEG
bool FfmpegDecoderBackend::ensurePlaybackRateResampler()
{
    const float rate = qBound(0.5f, m_playbackRate, 2.0f);
    if (qAbs(rate - 1.0f) < 0.0001f)
        return false;

    const int inRate = qMax(1, m_format.sampleRate());
    const int outRate = playbackRateOutRate(inRate, rate);
    if (m_rateSwrCtx
        && m_rateSwrInRate == inRate
        && m_rateSwrOutRate == outRate
        && qAbs(m_rateSwrAppliedRate - rate) < 0.0001f) {
        return true;
    }

    resetPlaybackRateResampler();

    AVSampleFormat sampleFmt = toAvSampleFormat(m_format.sampleFormat());
    if (sampleFmt == AV_SAMPLE_FMT_NONE)
        sampleFmt = AV_SAMPLE_FMT_FLT;

    const int channels = qMax(1, m_format.channelCount());
    AVChannelLayout inLayout;
    AVChannelLayout outLayout;
    av_channel_layout_default(&inLayout, channels);
    av_channel_layout_default(&outLayout, channels);

    int err = swr_alloc_set_opts2(&m_rateSwrCtx,
                                  &outLayout,
                                  sampleFmt,
                                  outRate,
                                  &inLayout,
                                  sampleFmt,
                                  inRate,
                                  0,
                                  nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);

    if (err < 0 || !m_rateSwrCtx) {
        qWarning() << "FFmpeg: rate swr_alloc_set_opts2 failed" << avErrorToString(err)
                   << "rate=" << rate
                   << "inRate=" << inRate
                   << "outRate=" << outRate;
        resetPlaybackRateResampler();
        return false;
    }

    err = swr_init(m_rateSwrCtx);
    if (err < 0) {
        qWarning() << "FFmpeg: rate swr_init failed" << avErrorToString(err)
                   << "rate=" << rate
                   << "inRate=" << inRate
                   << "outRate=" << outRate;
        resetPlaybackRateResampler();
        return false;
    }

    m_rateSwrInRate = inRate;
    m_rateSwrOutRate = outRate;
    m_rateSwrAppliedRate = rate;
    return true;
}
#endif

void FfmpegDecoderBackend::onPumpTimeout()
{
    if (m_state != State::Playing)
        return;

    emitQueuedChunk();

    const float rate = qBound(0.5f, m_playbackRate, 2.0f);
    // Increase buffer size to 1 second for high-bitrate formats like APE
    const qint64 targetQueueUs = static_cast<qint64>(1000000.0 * static_cast<double>(rate));
    const qint64 targetQueueBytes = qMax<qint64>(
            m_format.bytesForDuration(targetQueueUs),
        static_cast<qint64>(m_format.bytesPerFrame()) * 1024);

    int attempts = 0;
    while (m_pcmQueue.size() < targetQueueBytes && !m_reachedEnd && attempts < 24) {
        if (!decodeSomePcm())
            break;
        ++attempts;
    }

    if (m_reachedEnd)
        applyQueuedTrailingTrimIfReady();

    emitQueuedChunk();

    if (m_reachedEnd && m_pcmQueue.isEmpty() && !m_endOfStreamEmitted) {
        m_endOfStreamEmitted = true;
        emit endOfStream();

        const bool stillAtEnd = m_reachedEnd && m_pcmQueue.isEmpty();
        if (m_pumpTimer && (m_state != State::Playing || stillAtEnd))
            m_pumpTimer->stop();
    }
}

void FfmpegDecoderBackend::closeSource()
{
    if (m_pumpTimer)
        m_pumpTimer->stop();

    resetPlaybackRateResampler();

#ifdef MUSICPLAYER_HAS_FFMPEG
    if (m_swrCtx)
        swr_free(&m_swrCtx);
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_codecCtx)
        avcodec_free_context(&m_codecCtx);
    if (m_formatCtx)
        avformat_close_input(&m_formatCtx);

    m_streamIndex = -1;
    m_decoderFlushing = false;
    m_packetPending = false;
#endif

    m_pcmQueue.clear();
    m_currentSource.clear();
    m_rawDurationMs = 0;
    m_durationMs = 0;
    m_positionMs = 0;
    m_emittedFrames = 0;
    m_leadingTrimFrames = 0;
    m_pendingLeadingTrimFrames = 0;
    m_trailingTrimFrames = 0;
    m_pendingTrailingTrimFrames = 0;
    m_seekTargetMs = 0;
    m_seekDiscardActive = false;
    m_seekAnchorFallbackAttempts = 0;
    m_seekDiscardOutputFrames = 0;
    m_lastPacketTsMs = -1;
    m_seekUnknownFramesToDrop = 0;
    m_reachedEnd = false;
    m_endOfStreamEmitted = false;
    m_allowGaplessLeadingTrimAdoption = false;
}

bool FfmpegDecoderBackend::decodeSomePcm()
{
#ifndef MUSICPLAYER_HAS_FFMPEG
    return false;
#else
    if (!m_formatCtx || !m_codecCtx || !m_packet || !m_frame || !m_swrCtx || m_streamIndex < 0)
        return false;

    for (;;) {
        if (!m_decoderFlushing) {
            if (m_packetPending) {
                const int sendRet = avcodec_send_packet(m_codecCtx, m_packet);
                if (sendRet == AVERROR(EAGAIN)) {
                    // Decoder still full; must drain frames
                } else {
                    m_packetPending = false;
                    av_packet_unref(m_packet);
                    if (sendRet < 0) {
                        qWarning() << "FFmpeg: avcodec_send_packet (pending) failed" << avErrorToString(sendRet);
                    }
                }
            }

            if (!m_packetPending) {
                const int readRet = av_read_frame(m_formatCtx, m_packet);
                if (readRet == AVERROR_EOF) {
                    if (m_seekDiscardActive
                        && m_seekTargetMs > 0
                        && m_seekAnchorFallbackAttempts < kSeekAnchorFallbackMaxAttempts) {
                        const int attempt = ++m_seekAnchorFallbackAttempts;
                        const qint64 anchorBackoffMs = static_cast<qint64>(attempt) * kSeekAnchorStepMs;
                        const qint64 anchorMs = qMax<qint64>(0, m_seekTargetMs - anchorBackoffMs);
                        const int64_t anchorUs = static_cast<int64_t>(anchorMs) * 1000;

                        qWarning() << "[seek-decoder] immediate EOF after seek, retrying from earlier anchor"
                                   << "attempt=" << attempt
                                   << "targetMs=" << m_seekTargetMs
                                   << "anchorMs=" << anchorMs
                                   << "source=" << m_currentSource;

                        int retryErr = AVERROR(EINVAL);
                        qint64 discardReferenceMs = anchorMs;
                        if (m_formatCtx->pb) {
                            const int64_t fileSize = avio_size(m_formatCtx->pb);
                            if (fileSize > 0) {
                                int64_t byteTarget = -1;
                                qint64 mappedAnchorMs = -1;
                                const bool hasPacketAlignedTarget = resolvePacketAlignedByteTarget(
                                    m_currentSource,
                                    anchorMs,
                                    fileSize,
                                    &byteTarget,
                                    &mappedAnchorMs);

                                if (!hasPacketAlignedTarget && m_rawDurationMs > 0) {
                                    const int64_t byteTargetRaw =
                                        (fileSize * anchorMs) / qMax<qint64>(1, m_rawDurationMs);
                                    byteTarget = qBound<int64_t>(0, byteTargetRaw, fileSize - 1);
                                }

                                if (byteTarget >= 0) {
                                    retryErr = seekByByteOffset(m_formatCtx, byteTarget);
                                    if (retryErr >= 0) {
                                        if (mappedAnchorMs >= 0)
                                            discardReferenceMs = mappedAnchorMs;

                                        qInfo() << "[seek-decoder] anchor byte-seek fallback"
                                                << "targetMs=" << m_seekTargetMs
                                                << "anchorMs=" << anchorMs
                                                << "mappedAnchorMs=" << mappedAnchorMs
                                                << "byteTarget=" << byteTarget
                                                << "fileSize=" << fileSize
                                                << "packetAligned=" << hasPacketAlignedTarget
                                                << "source=" << m_currentSource;
                                    }
                                }
                            }
                        }

                        AVStream *seekStream = (m_streamIndex >= 0
                            && m_streamIndex < static_cast<int>(m_formatCtx->nb_streams))
                            ? m_formatCtx->streams[m_streamIndex]
                            : nullptr;
                        if (retryErr < 0 && seekStream) {
                            const int64_t anchorStreamTs =
                                av_rescale_q(anchorMs, AVRational{1, 1000}, seekStream->time_base);
                            retryErr = avformat_seek_file(m_formatCtx,
                                                          m_streamIndex,
                                                          INT64_MIN,
                                                          anchorStreamTs,
                                                          anchorStreamTs,
                                                          AVSEEK_FLAG_BACKWARD);
                            if (retryErr < 0)
                                retryErr = av_seek_frame(m_formatCtx,
                                                         m_streamIndex,
                                                         anchorStreamTs,
                                                         AVSEEK_FLAG_BACKWARD);
                        }
                        if (retryErr < 0)
                            retryErr = av_seek_frame(m_formatCtx, -1, anchorUs, AVSEEK_FLAG_BACKWARD);

                        if (retryErr >= 0) {
                            if (m_formatCtx->pb) {
                                m_formatCtx->pb->eof_reached = 0;
                                m_formatCtx->pb->error = 0;
                            }

                            avformat_flush(m_formatCtx);
                            avcodec_flush_buffers(m_codecCtx);
                            if (m_swrCtx) {
                                swr_close(m_swrCtx);
                                swr_init(m_swrCtx);
                            }

                            m_pcmQueue.clear();
                            m_reachedEnd = false;
                            m_endOfStreamEmitted = false;
                            m_decoderFlushing = false;
                            const qint64 discardMs = qMax<qint64>(0, m_seekTargetMs - discardReferenceMs);
                            const int outRate = qMax(1, m_format.sampleRate());
                            m_seekDiscardOutputFrames = (discardMs * outRate + 999) / 1000LL;
                            m_seekDiscardActive = false;
                            m_lastPacketTsMs = -1;
                            m_seekUnknownFramesToDrop = 0;
                            continue;
                        }
                    }

                    m_decoderFlushing = true;
                    const int flushRet = avcodec_send_packet(m_codecCtx, nullptr);
                    if (flushRet < 0 && flushRet != AVERROR(EAGAIN)) {
                        m_reachedEnd = true;
                        return false;
                    }
                } else if (readRet < 0) {
                    m_reachedEnd = true;
                    return false;
                } else {
                    if (m_packet->stream_index != m_streamIndex) {
                        av_packet_unref(m_packet);
                        continue;
                    }

                    m_lastPacketTsMs = -1;
                    const int64_t packetTs = (m_packet->dts != AV_NOPTS_VALUE) ? m_packet->dts : m_packet->pts;
                    if (packetTs != AV_NOPTS_VALUE) {
                        AVStream *stream = m_formatCtx->streams[m_streamIndex];
                        const int64_t streamStartOffset = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
                        m_lastPacketTsMs = qMax<qint64>(0, av_rescale_q(packetTs - streamStartOffset, stream->time_base, AVRational{1, 1000}));
                    }

                    parsePacketGaplessMetadata();

                    const int sendRet = avcodec_send_packet(m_codecCtx, m_packet);
                    if (sendRet == AVERROR(EAGAIN)) {
                        m_packetPending = true;
                    } else {
                        m_packetPending = false;
                        av_packet_unref(m_packet);
                        if (sendRet < 0) continue;
                    }
                }
            }
        }

        const int recvRet = avcodec_receive_frame(m_codecCtx, m_frame);
        if (recvRet == AVERROR(EAGAIN)) {
            if (m_decoderFlushing) {
                m_reachedEnd = true;
                return false;
            }
            if (m_packetPending) return false; // Yield to avoid tight loop
            continue;
        }
        if (recvRet == AVERROR_EOF) {
            m_reachedEnd = true;
            return false;
        }
        if (recvRet < 0) {
            m_reachedEnd = true;
            return false;
        }

        if (m_seekDiscardActive) {
            AVStream *stream = m_formatCtx->streams[m_streamIndex];
            const qint64 bestTs = (m_frame->best_effort_timestamp != AV_NOPTS_VALUE) ? m_frame->best_effort_timestamp : m_frame->pts;
            const int64_t streamStartOffset = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
            qint64 frameStartMs = -1;
            if (bestTs != AV_NOPTS_VALUE) {
                frameStartMs = qMax<qint64>(0, av_rescale_q(bestTs - streamStartOffset, stream->time_base, AVRational{1, 1000}));
            } else if (m_lastPacketTsMs >= 0) {
                frameStartMs = m_lastPacketTsMs;
            }

            if (frameStartMs >= 0) {
                const int inRate = qMax(1, m_codecCtx->sample_rate);
                const qint64 frameDurMs = qMax<qint64>(1, (static_cast<qint64>(m_frame->nb_samples) * 1000LL) / inRate);
                if (frameStartMs + frameDurMs <= m_seekTargetMs) {
                    av_frame_unref(m_frame);
                    continue;
                }
                if (frameStartMs < m_seekTargetMs) {
                    const qint64 dropMs = m_seekTargetMs - frameStartMs;
                    m_seekDiscardOutputFrames = qMax<qint64>(0, (dropMs * qMax(1, m_format.sampleRate()) + 999) / 1000LL);
                }
                m_seekDiscardActive = false;
            } else if (m_seekUnknownFramesToDrop > 0) {
                --m_seekUnknownFramesToDrop;
                av_frame_unref(m_frame);
                continue;
            } else {
                m_seekDiscardActive = false;
            }
        }

        const bool converted = convertFrameToPcm();
        av_frame_unref(m_frame);
        if (converted) return true;
    }
#endif
}

bool FfmpegDecoderBackend::convertFrameToPcm()
{
#ifndef MUSICPLAYER_HAS_FFMPEG
    return false;
#else
    if (!m_frame || !m_codecCtx || !m_swrCtx)
        return false;

    AVSampleFormat outSampleFmt = toAvSampleFormat(m_format.sampleFormat());
    if (outSampleFmt == AV_SAMPLE_FMT_NONE)
        outSampleFmt = AV_SAMPLE_FMT_FLT;

    const int outChannels = qMax(1, m_format.channelCount());
    const int outRate = qMax(1, m_format.sampleRate());
    const int64_t delay = swr_get_delay(m_swrCtx, m_codecCtx->sample_rate);
    const int dstSamples = static_cast<int>(av_rescale_rnd(delay + m_frame->nb_samples,
                                                            outRate,
                                                            m_codecCtx->sample_rate,
                                                            AV_ROUND_UP));
    if (dstSamples <= 0)
        return false;

    uint8_t *dstData = nullptr;
    int dstLineSize = 0;
    const int allocRet = av_samples_alloc(&dstData,
                                          &dstLineSize,
                                          outChannels,
                                          dstSamples,
                                          outSampleFmt,
                                          0);
    if (allocRet < 0) return false;

    const int converted = swr_convert(m_swrCtx,
                                      &dstData,
                                      dstSamples,
                                      const_cast<const uint8_t **>(m_frame->extended_data),
                                      m_frame->nb_samples);
    if (converted < 0) {
        av_freep(&dstData);
        return false;
    }

    const int bytesPerSample = avToQtSampleFormatBytes(outSampleFmt);
    const int frameBytes = qMax(1, outChannels * bytesPerSample);
    const int totalBytes = qMax(0, converted * frameBytes);
    qint64 dropFrames = 0;
    if (m_seekDiscardOutputFrames > 0) {
        dropFrames = qMin<qint64>(m_seekDiscardOutputFrames, converted);
        m_seekDiscardOutputFrames -= dropFrames;
    }

    const int dropBytes = static_cast<int>(dropFrames * frameBytes);
    const int keptBytes = qMax(0, totalBytes - dropBytes);
    if (keptBytes > 0) {
        m_pcmQueue.append(reinterpret_cast<const char *>(dstData) + dropBytes, keptBytes);
        applyQueuedLeadingTrim();
    }

    av_freep(&dstData);
    return keptBytes > 0;
#endif
}

void FfmpegDecoderBackend::emitQueuedChunk()
{
    if (m_pcmQueue.isEmpty())
        return;

    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const int targetFrames = qMax(64, (m_format.sampleRate() * kPumpChunkMs) / 1000);
    const qint64 queuedFrames = m_pcmQueue.size() / frameBytes;
    qint64 emittableFrames = queuedFrames;
    if (!m_reachedEnd && m_pendingTrailingTrimFrames > 0)
        emittableFrames = qMax<qint64>(0, emittableFrames - m_pendingTrailingTrimFrames);

    if (emittableFrames <= 0)
        return;

    const qint64 chunkFrames = qMin<qint64>(emittableFrames, targetFrames);
    const int chunkBytes = static_cast<int>(chunkFrames * frameBytes);
    if (chunkBytes <= 0)
        return;

    const QByteArray sourceChunk = m_pcmQueue.left(chunkBytes);
    m_pcmQueue.remove(0, chunkBytes);

    const QByteArray chunk = applyPlaybackRate(sourceChunk, frameBytes);
    if (chunk.isEmpty())
        return;

    const qint64 mediaFrameCount = chunkBytes / frameBytes;
    const int sampleRate = qMax(1, m_format.sampleRate());
    const qint64 startUs = (m_emittedFrames * 1000000LL) / sampleRate;
    QAudioBuffer buffer(chunk, m_format, startUs);
    m_emittedFrames += mediaFrameCount;
    m_positionMs = (m_emittedFrames * 1000LL) / sampleRate;

    emit audioBufferReceived(buffer);
}

QByteArray FfmpegDecoderBackend::applyPlaybackRate(const QByteArray &chunk, int frameBytes)
{
    if (chunk.isEmpty() || frameBytes <= 0)
        return {};

    const float rate = qBound(0.5f, m_playbackRate, 2.0f);
    if (qAbs(rate - 1.0f) < 0.0001f)
        return chunk;

#ifndef MUSICPLAYER_HAS_FFMPEG
    return chunk;
#else
    if (!ensurePlaybackRateResampler())
        return chunk;

    const qint64 inputFrames = chunk.size() / frameBytes;
    if (inputFrames <= 0)
        return {};

    const int inRate = qMax(1, m_rateSwrInRate);
    const int outRate = qMax(1, m_rateSwrOutRate);
    const int channels = qMax(1, m_format.channelCount());
    AVSampleFormat sampleFmt = toAvSampleFormat(m_format.sampleFormat());
    if (sampleFmt == AV_SAMPLE_FMT_NONE)
        sampleFmt = AV_SAMPLE_FMT_FLT;

    const int64_t delay = swr_get_delay(m_rateSwrCtx, inRate);
    const int dstSamples = static_cast<int>(av_rescale_rnd(delay + inputFrames,
                                                            outRate,
                                                            inRate,
                                                            AV_ROUND_UP));
    if (dstSamples <= 0)
        return chunk;

    uint8_t *dstData = nullptr;
    int dstLineSize = 0;
    const int allocRet = av_samples_alloc(&dstData,
                                          &dstLineSize,
                                          channels,
                                          dstSamples,
                                          sampleFmt,
                                          0);
    if (allocRet < 0) return chunk;

    const uint8_t *srcData = reinterpret_cast<const uint8_t *>(chunk.constData());
    const int converted = swr_convert(m_rateSwrCtx,
                                      &dstData,
                                      dstSamples,
                                      &srcData,
                                      static_cast<int>(inputFrames));
    if (converted < 0) {
        av_freep(&dstData);
        resetPlaybackRateResampler();
        return chunk;
    }

    const int totalBytes = qMax(0, converted * frameBytes);
    QByteArray transformed;
    if (totalBytes > 0)
        transformed = QByteArray(reinterpret_cast<const char *>(dstData), totalBytes);

    av_freep(&dstData);
    return transformed.isEmpty() ? chunk : transformed;
#endif
}

void FfmpegDecoderBackend::parsePacketGaplessMetadata()
{
#ifndef MUSICPLAYER_HAS_FFMPEG
    return;
#else
    if (!m_packet)
        return;

    size_t sideDataSize = 0;
    const uint8_t *sideData = av_packet_get_side_data(m_packet, AV_PKT_DATA_SKIP_SAMPLES, &sideDataSize);
    if (!sideData || sideDataSize < 8)
        return;

    const qint64 skipInputSamples = static_cast<qint64>(AV_RL32(sideData));
    const qint64 discardInputSamples = static_cast<qint64>(AV_RL32(sideData + 4));

    bool changed = false;
    if (skipInputSamples > 0 && m_leadingTrimFrames == 0) {
        m_leadingTrimFrames = inputSamplesToOutputFrames(skipInputSamples);
        m_pendingLeadingTrimFrames = m_leadingTrimFrames;
        changed = true;
    }
    if (discardInputSamples > 0 && m_trailingTrimFrames == 0) {
        m_trailingTrimFrames = inputSamplesToOutputFrames(discardInputSamples);
        m_pendingTrailingTrimFrames = m_trailingTrimFrames;
        changed = true;
    }

    if (changed) updateDurationForGaplessTrim();
#endif
}

void FfmpegDecoderBackend::applyQueuedLeadingTrim()
{
    if (m_pendingLeadingTrimFrames <= 0 || m_pcmQueue.isEmpty()) return;
    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const qint64 dropFrames = qMin(m_pendingLeadingTrimFrames, static_cast<qint64>(m_pcmQueue.size() / frameBytes));
    m_pcmQueue.remove(0, static_cast<int>(dropFrames * frameBytes));
    m_pendingLeadingTrimFrames -= dropFrames;
}

void FfmpegDecoderBackend::applyQueuedTrailingTrimIfReady()
{
    if (!m_reachedEnd || m_pendingTrailingTrimFrames <= 0 || m_pcmQueue.isEmpty()) return;
    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const qint64 dropFrames = qMin(m_pendingTrailingTrimFrames, static_cast<qint64>(m_pcmQueue.size() / frameBytes));
    m_pcmQueue.chop(static_cast<int>(dropFrames * frameBytes));
    m_pendingTrailingTrimFrames -= dropFrames;
}

void FfmpegDecoderBackend::updateDurationForGaplessTrim()
{
    const int sampleRate = qMax(1, m_format.sampleRate());
    const qint64 trimMs = ((m_leadingTrimFrames + m_trailingTrimFrames) * 1000LL) / sampleRate;
    if (m_rawDurationMs > trimMs) {
        m_durationMs = m_rawDurationMs - trimMs;
        emit durationChanged(m_durationMs);
    }
}

qint64 FfmpegDecoderBackend::inputSamplesToOutputFrames(qint64 inputSamples) const
{
    if (inputSamples <= 0) return 0;
#ifndef MUSICPLAYER_HAS_FFMPEG
    return inputSamples;
#else
    const int inRate = (m_codecCtx && m_codecCtx->sample_rate > 0) ? m_codecCtx->sample_rate : qMax(1, m_format.sampleRate());
    return av_rescale_rnd(inputSamples, qMax(1, m_format.sampleRate()), inRate, AV_ROUND_UP);
#endif
}

int FfmpegDecoderBackend::avToQtSampleFormatBytes(int avSampleFmt)
{
#ifdef MUSICPLAYER_HAS_FFMPEG
    return qMax(0, av_get_bytes_per_sample(static_cast<AVSampleFormat>(avSampleFmt)));
#else
    Q_UNUSED(avSampleFmt); return 0;
#endif
}

void FfmpegDecoderBackend::preloadInitialPackets()
{
#ifndef MUSICPLAYER_HAS_FFMPEG
    return;
#else
    if (!m_formatCtx || !m_codecCtx || !m_packet || !m_swrCtx || m_streamIndex < 0) return;
    int decodedFrames = 0;
    for (int i = 0; i < 50 && decodedFrames < 128; ++i) {
        if (av_read_frame(m_formatCtx, m_packet) < 0) break;
        if (m_packet->stream_index != m_streamIndex) { av_packet_unref(m_packet); continue; }
        if (avcodec_send_packet(m_codecCtx, m_packet) == AVERROR(EAGAIN)) break;
        av_packet_unref(m_packet);
        while (avcodec_receive_frame(m_codecCtx, m_frame) >= 0) {
            convertFrameToPcm();
            av_frame_unref(m_frame);
            decodedFrames++;
        }
    }
    av_seek_frame(m_formatCtx, m_streamIndex, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codecCtx);
    if (m_swrCtx) { swr_close(m_swrCtx); swr_init(m_swrCtx); }
    m_pcmQueue.clear();
#endif
}
