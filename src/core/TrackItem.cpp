#include "TrackItem.h"
#include "CueParser.h"
#include <QFileInfo>
#include <QImage>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QPainter>
#include <QDateTime>
#include <QRegularExpression>
#include <QtEndian>
#include <QHash>
#include <QBuffer>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <cstdlib>
#include <cstring>

#ifdef MUSICPLAYER_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#endif

namespace {

QImage defaultCoverImage()
{
    QImage defaultCover(64, 64, QImage::Format_ARGB32);
    defaultCover.fill(QColor(60, 60, 60));

    QPainter painter(&defaultCover);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(150, 150, 150), 2));
    painter.setFont(QFont("Segoe UI Emoji", 28));
    painter.drawText(defaultCover.rect(), Qt::AlignCenter, "🎵");

    return defaultCover;
}

bool isDefaultCoverImage(const QImage &image)
{
    if (image.isNull())
        return false;

    static const QImage defaultImage = defaultCoverImage();
    return image == defaultImage;
}

bool isSuspiciousCoverImage(const QImage &image)
{
    if (image.isNull())
        return false;

    const QImage converted = image.convertToFormat(QImage::Format_ARGB32);
    if (converted.isNull())
        return false;

    const int width = converted.width();
    const int height = converted.height();
    if (width <= 0 || height <= 0)
        return true;

    const int stepX = qMax(1, width / 16);
    const int stepY = qMax(1, height / 16);

    int samples = 0;
    int nonTransparent = 0;
    int nonBlack = 0;

    for (int y = 0; y < height; y += stepY) {
        for (int x = 0; x < width; x += stepX) {
            const QRgb px = converted.pixel(x, y);
            const int alpha = qAlpha(px);
            const int red = qRed(px);
            const int green = qGreen(px);
            const int blue = qBlue(px);

            ++samples;
            if (alpha > 8)
                ++nonTransparent;
            if (red > 8 || green > 8 || blue > 8)
                ++nonBlack;
        }
    }

    if (samples == 0)
        return true;

    if (nonTransparent == 0)
        return true;

    const bool almostOpaque = nonTransparent >= (samples * 9) / 10;
    const bool almostAllBlack = nonBlack <= qMax(1, samples / 100);
    return almostOpaque && almostAllBlack;
}

bool coverLogsEnabled()
{
    static const bool enabled = []() {
        const QByteArray raw = qgetenv("MUSICPLAYER_COVER_LOGS").trimmed();
        if (raw.isEmpty())
            return true;

        const QByteArray lowered = raw.toLower();
        return lowered != "0"
            && lowered != "false"
            && lowered != "off"
            && lowered != "no";
    }();

    return enabled;
}

QString coverLogFilter()
{
    static const QString filter = QString::fromUtf8(qgetenv("MUSICPLAYER_COVER_LOG_FILTER")).trimmed();
    return filter;
}

bool coverLogMatchesFilter(const QString &filePath)
{
    const QString filter = coverLogFilter();
    if (filter.isEmpty())
        return true;

    const QString nativePath = QDir::toNativeSeparators(filePath);
    return nativePath.contains(filter, Qt::CaseInsensitive)
        || QFileInfo(filePath).fileName().contains(filter, Qt::CaseInsensitive);
}

QString describeImageForLog(const QImage &image)
{
    if (image.isNull())
        return QStringLiteral("null");

    const int centerX = qBound(0, image.width() / 2, qMax(0, image.width() - 1));
    const int centerY = qBound(0, image.height() / 2, qMax(0, image.height() - 1));
    const QRgb center = image.pixel(centerX, centerY);

    return QStringLiteral("%1x%2 a=%3 r=%4 g=%5 b=%6")
        .arg(image.width())
        .arg(image.height())
        .arg(qAlpha(center))
        .arg(qRed(center))
        .arg(qGreen(center))
        .arg(qBlue(center));
}

void coverLog(const QString &filePath, const QString &message)
{
    if (!coverLogsEnabled() || !coverLogMatchesFilter(filePath))
        return;

    qDebug().noquote() << "[cover]" << QDir::toNativeSeparators(filePath) << "-" << message;
}

struct TrackMetadataCacheEntry {
    TrackMetadata metadata;
    qint64 fileSize = -1;
    qint64 lastModifiedMs = -1;
};

QHash<QString, TrackMetadataCacheEntry> s_trackMetadataCache;
constexpr int kTrackMetadataCacheMaxEntries = 4096;
QMutex s_trackMetadataCacheMutex;
bool s_trackMetadataCacheLoaded = false;

QString normalizeTrackPathForCache(const QString &path);

QString trackMetadataCacheDbPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dir.isEmpty())
        QDir().mkpath(dir);
    return dir + QStringLiteral("/track_metadata_cache.db");
}

QSqlDatabase openCacheDb()
{
    // Each thread needs its own connection
    const QString connName = QStringLiteral("track_metadata_cache_%1")
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));

    if (QSqlDatabase::contains(connName))
        return QSqlDatabase::database(connName);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    db.setDatabaseName(trackMetadataCacheDbPath());
    if (!db.open()) {
        qWarning() << "[cache] Failed to open SQLite db:" << db.lastError().text();
        return db;
    }

    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS track_metadata ("
        "  path TEXT PRIMARY KEY,"
        "  file_size INTEGER NOT NULL,"
        "  last_modified INTEGER NOT NULL,"
        "  title TEXT,"
        "  artist TEXT,"
        "  album TEXT,"
        "  year TEXT,"
        "  genre TEXT,"
        "  track_number INTEGER,"
        "  duration INTEGER,"
        "  bitrate INTEGER,"
        "  sample_rate INTEGER,"
        "  cover_png BLOB"
        ")"
    ));
    return db;
}

void ensureTrackMetadataCacheLoaded()
{
    {
        QMutexLocker locker(&s_trackMetadataCacheMutex);
        if (s_trackMetadataCacheLoaded)
            return;
        s_trackMetadataCacheLoaded = true;
    }

    QSqlDatabase db = openCacheDb();
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    q.exec(QStringLiteral(
        "SELECT path, file_size, last_modified, title, artist, album, year, genre,"
        "       track_number, duration, bitrate, sample_rate, cover_png"
        " FROM track_metadata"
    ));

    QMutexLocker locker(&s_trackMetadataCacheMutex);
    while (q.next()) {
        TrackMetadataCacheEntry entry;
        entry.metadata.filePath   = q.value(0).toString();
        entry.fileSize            = q.value(1).toLongLong();
        entry.lastModifiedMs      = q.value(2).toLongLong();
        entry.metadata.title      = q.value(3).toString();
        entry.metadata.artist     = q.value(4).toString();
        entry.metadata.album      = q.value(5).toString();
        entry.metadata.year       = q.value(6).toString();
        entry.metadata.genre      = q.value(7).toString();
        entry.metadata.trackNumber = q.value(8).toInt();
        entry.metadata.duration   = q.value(9).toLongLong();
        entry.metadata.bitrate    = q.value(10).toInt();
        entry.metadata.sampleRate = q.value(11).toInt();

        const QByteArray coverData = q.value(12).toByteArray();
        if (!coverData.isEmpty())
            entry.metadata.coverArt = QImage::fromData(coverData, "PNG");
        if (entry.metadata.coverArt.isNull())
            entry.metadata.coverArt = defaultCoverImage();

        const QString key = normalizeTrackPathForCache(entry.metadata.filePath);
        if (!key.isEmpty())
            s_trackMetadataCache.insert(key, entry);

        if (s_trackMetadataCache.size() >= kTrackMetadataCacheMaxEntries)
            break;
    }
}

QString normalizeTrackPathForCache(const QString &path)
{
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(QDir::cleanPath(path)).toLower();
#else
    return QDir::cleanPath(path);
#endif
}

bool tryLoadTrackMetadataFromCache(const QFileInfo &fileInfo, TrackMetadata &metadata)
{
    ensureTrackMetadataCacheLoaded();

    if (!fileInfo.exists())
        return false;

    const QString key = normalizeTrackPathForCache(fileInfo.absoluteFilePath());

    QMutexLocker locker(&s_trackMetadataCacheMutex);
    const auto it = s_trackMetadataCache.constFind(key);
    if (it == s_trackMetadataCache.cend())
        return false;

    const qint64 currentSize = fileInfo.size();
    const qint64 currentModifiedMs = fileInfo.lastModified().toMSecsSinceEpoch();
    if (it->fileSize != currentSize || it->lastModifiedMs != currentModifiedMs)
        return false;

    metadata = it->metadata;
    metadata.filePath = fileInfo.absoluteFilePath();
    return true;
}

void updateTrackMetadataCache(const QFileInfo &fileInfo, const TrackMetadata &metadata)
{
    ensureTrackMetadataCacheLoaded();

    if (!fileInfo.exists())
        return;

    const QString key = normalizeTrackPathForCache(fileInfo.absoluteFilePath());
    const qint64 fileSize = fileInfo.size();
    const qint64 lastModifiedMs = fileInfo.lastModified().toMSecsSinceEpoch();

    // Update in-memory cache
    {
        QMutexLocker locker(&s_trackMetadataCacheMutex);
        if (s_trackMetadataCache.size() >= kTrackMetadataCacheMaxEntries)
            s_trackMetadataCache.clear();

        TrackMetadataCacheEntry entry;
        entry.metadata = metadata;
        entry.metadata.filePath = fileInfo.absoluteFilePath();
        entry.fileSize = fileSize;
        entry.lastModifiedMs = lastModifiedMs;
        s_trackMetadataCache.insert(key, entry);
    }

    // Write to SQLite — cover as raw PNG bytes (no base64 overhead)
    QByteArray coverBytes;
    if (!metadata.coverArt.isNull() && !isDefaultCoverImage(metadata.coverArt)) {
        QBuffer buf(&coverBytes);
        if (buf.open(QIODevice::WriteOnly))
            metadata.coverArt.save(&buf, "PNG");
    }

    QSqlDatabase db = openCacheDb();
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO track_metadata"
        " (path, file_size, last_modified, title, artist, album, year, genre,"
        "  track_number, duration, bitrate, sample_rate, cover_png)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"
    ));
    q.addBindValue(fileInfo.absoluteFilePath());
    q.addBindValue(fileSize);
    q.addBindValue(lastModifiedMs);
    q.addBindValue(metadata.title);
    q.addBindValue(metadata.artist);
    q.addBindValue(metadata.album);
    q.addBindValue(metadata.year);
    q.addBindValue(metadata.genre);
    q.addBindValue(metadata.trackNumber);
    q.addBindValue(metadata.duration);
    q.addBindValue(metadata.bitrate);
    q.addBindValue(metadata.sampleRate);
    q.addBindValue(coverBytes.isEmpty() ? QVariant(QMetaType(QMetaType::QByteArray)) : QVariant(coverBytes));
    if (!q.exec())
        qWarning() << "[cache] SQLite insert failed:" << q.lastError().text();
}

#ifdef MUSICPLAYER_HAS_FFMPEG
QString dictValueCaseInsensitive(const AVDictionary *dict, const QStringList &wantedKeys)
{
    if (!dict)
        return {};

    for (const QString &key : wantedKeys) {
        const QByteArray k = key.toUtf8();
        const AVDictionaryEntry *entry = av_dict_get(dict, k.constData(), nullptr, 0);
        if (entry && entry->value) {
            const QString value = QString::fromUtf8(entry->value).trimmed();
            if (!value.isEmpty())
                return value;
        }
    }

    return {};
}

QString firstNonEmptyTag(const AVDictionary *primary,
                         const AVDictionary *secondary,
                         const QStringList &keys)
{
    const QString fromPrimary = dictValueCaseInsensitive(primary, keys);
    if (!fromPrimary.isEmpty())
        return fromPrimary;
    return dictValueCaseInsensitive(secondary, keys);
}

QString extractYearFromText(const QString &value)
{
    if (value.isEmpty())
        return {};

    static const QRegularExpression yearRx(QStringLiteral("(\\d{4})"));
    const QRegularExpressionMatch match = yearRx.match(value);
    if (match.hasMatch())
        return match.captured(1);
    return {};
}

int parseTrackNumberTag(const QString &value)
{
    if (value.isEmpty())
        return 0;

    bool ok = false;
    const int direct = value.toInt(&ok);
    if (ok && direct > 0)
        return direct;

    const QString firstPart = value.section('/', 0, 0).trimmed();
    const int parsed = firstPart.toInt(&ok);
    return (ok && parsed > 0) ? parsed : 0;
}

bool isLikelyCoverImageCodecId(AVCodecID codecId)
{
    switch (codecId) {
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MJPEGB:
    case AV_CODEC_ID_LJPEG:
    case AV_CODEC_ID_PNG:
    case AV_CODEC_ID_GIF:
    case AV_CODEC_ID_BMP:
    case AV_CODEC_ID_TIFF:
    case AV_CODEC_ID_WEBP:
    case AV_CODEC_ID_JPEG2000:
        return true;
    default:
        return false;
    }
}

bool shouldScanVideoPacketsForEmbeddedCover(const QString &filePath)
{
    // The packet-scan fallback is only needed for edge-case MP3 files
    // where cover is stored as a video stream without attached_pic flag.
    const QFileInfo fi(filePath);
    return fi.suffix().compare("mp3", Qt::CaseInsensitive) == 0;
}

void applyFfmpegMetadata(const QString &filePath, TrackMetadata &metadata)
{
    AVFormatContext *formatCtx = nullptr;
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const QByteArray utf8Path = nativePath.toUtf8();

    // Use minimal probing for metadata extraction - only need tags, not full decode
    AVDictionary *openOpts = nullptr;
    av_dict_set(&openOpts, "analyzeduration", "100000", 0);
    av_dict_set(&openOpts, "probesize", "32768", 0);
    int err = avformat_open_input(&formatCtx, utf8Path.constData(), nullptr, &openOpts);
    av_dict_free(&openOpts);

#ifdef Q_OS_WIN
    if (err < 0) {
        if (formatCtx)
            avformat_close_input(&formatCtx);

        const QByteArray ansiPath = QFile::encodeName(nativePath);
        if (ansiPath != utf8Path)
            err = avformat_open_input(&formatCtx, ansiPath.constData(), nullptr, nullptr);
    }
#endif
    if (err < 0 || !formatCtx)
        return;

    // Minimal stream info - just enough to find audio stream and tags
    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        avformat_close_input(&formatCtx);
        return;
    }

    const int audioStreamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const AVStream *audioStream = (audioStreamIndex >= 0) ? formatCtx->streams[audioStreamIndex] : nullptr;

    const AVDictionary *formatTags = formatCtx->metadata;
    const AVDictionary *streamTags = audioStream ? audioStream->metadata : nullptr;

    const QString title = firstNonEmptyTag(streamTags, formatTags, {"title"});
    if (!title.isEmpty())
        metadata.title = title;

    const QString artist = firstNonEmptyTag(streamTags, formatTags,
                                            {"artist", "album_artist", "albumartist", "performer", "composer"});
    if (!artist.isEmpty())
        metadata.artist = artist;

    const QString album = firstNonEmptyTag(streamTags, formatTags, {"album"});
    if (!album.isEmpty())
        metadata.album = album;

    const QString genre = firstNonEmptyTag(streamTags, formatTags, {"genre"});
    if (!genre.isEmpty())
        metadata.genre = genre;

    const QString yearRaw = firstNonEmptyTag(streamTags, formatTags, {"date", "year", "originaldate"});
    const QString year = extractYearFromText(yearRaw);
    if (!year.isEmpty())
        metadata.year = year;

    const QString trackRaw = firstNonEmptyTag(streamTags, formatTags, {"track", "tracknumber"});
    const int trackNum = parseTrackNumberTag(trackRaw);
    if (trackNum > 0)
        metadata.trackNumber = trackNum;

    qint64 durationMs = 0;
    if (audioStream && audioStream->duration != AV_NOPTS_VALUE) {
        durationMs = av_rescale_q(audioStream->duration, audioStream->time_base, AVRational{1, 1000});
    } else if (formatCtx->duration != AV_NOPTS_VALUE) {
        durationMs = formatCtx->duration / (AV_TIME_BASE / 1000);
    }
    if (durationMs > 0)
        metadata.duration = durationMs;

    if (audioStream && audioStream->codecpar) {
        if (audioStream->codecpar->sample_rate > 0)
            metadata.sampleRate = audioStream->codecpar->sample_rate;
        if (audioStream->codecpar->bit_rate > 0)
            metadata.bitrate = static_cast<int>(audioStream->codecpar->bit_rate);
    }

    if (metadata.bitrate <= 0 && formatCtx->bit_rate > 0)
        metadata.bitrate = static_cast<int>(formatCtx->bit_rate);

    avformat_close_input(&formatCtx);
}

QImage decodeCoverPacketWithFfmpeg(const AVStream *stream, const AVPacket *packet)
{
    if (!stream || !stream->codecpar || !packet || !packet->data || packet->size <= 0)
        return {};

    if (!isLikelyCoverImageCodecId(stream->codecpar->codec_id))
        return {};

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return {};

    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
        return {};

    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0) {
        avcodec_free_context(&codecCtx);
        return {};
    }

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        return {};
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        avcodec_free_context(&codecCtx);
        return {};
    }

    auto frameToImage = [](const AVFrame *decodedFrame) -> QImage {
        if (!decodedFrame || decodedFrame->width <= 0 || decodedFrame->height <= 0)
            return {};

        SwsContext *sws = sws_getContext(
            decodedFrame->width, decodedFrame->height,
            static_cast<AVPixelFormat>(decodedFrame->format),
            decodedFrame->width, decodedFrame->height,
            AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws)
            return {};

        QByteArray rgb;
        rgb.resize(decodedFrame->width * decodedFrame->height * 3);
        uint8_t *dstData[4] = { reinterpret_cast<uint8_t *>(rgb.data()), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { decodedFrame->width * 3, 0, 0, 0 };
        sws_scale(sws, decodedFrame->data, decodedFrame->linesize, 0, decodedFrame->height, dstData, dstLinesize);
        sws_freeContext(sws);

        QImage image(reinterpret_cast<const uchar *>(rgb.constData()),
                     decodedFrame->width, decodedFrame->height,
                     dstLinesize[0], QImage::Format_RGB888);
        return image.copy();
    };

    QImage result;
    if (avcodec_send_packet(codecCtx, packet) >= 0) {
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            result = frameToImage(frame);
            if (!result.isNull())
                break;
        }
    }

    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);
    return result;
}

AVCodecID detectImageCodecId(const QByteArray &bytes)
{
    if (bytes.size() >= 3
        && static_cast<uchar>(bytes[0]) == 0xFF
        && static_cast<uchar>(bytes[1]) == 0xD8
        && static_cast<uchar>(bytes[2]) == 0xFF) {
        return AV_CODEC_ID_MJPEG;
    }

    if (bytes.size() >= 8
        && bytes.startsWith(QByteArray("\x89PNG\r\n\x1A\n", 8))) {
        return AV_CODEC_ID_PNG;
    }

    if (bytes.size() >= 6
        && (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))) {
        return AV_CODEC_ID_GIF;
    }

    if (bytes.size() >= 2 && bytes.startsWith("BM"))
        return AV_CODEC_ID_BMP;

    if (bytes.size() >= 12
        && bytes.startsWith("RIFF")
        && bytes.mid(8, 4) == "WEBP") {
        return AV_CODEC_ID_WEBP;
    }

    return AV_CODEC_ID_NONE;
}

QImage decodeImageBytesWithFfmpeg(const QByteArray &bytes)
{
    const AVCodecID codecId = detectImageCodecId(bytes);
    if (codecId == AV_CODEC_ID_NONE)
        return {};

    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (!codec)
        return {};

    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
        return {};

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        return {};
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!packet || !frame) {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return {};
    }

    if (av_new_packet(packet, bytes.size()) < 0) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return {};
    }

    std::memcpy(packet->data, bytes.constData(), static_cast<size_t>(bytes.size()));

    auto frameToImage = [](const AVFrame *decodedFrame) -> QImage {
        if (!decodedFrame || decodedFrame->width <= 0 || decodedFrame->height <= 0)
            return {};

        SwsContext *sws = sws_getContext(
            decodedFrame->width, decodedFrame->height,
            static_cast<AVPixelFormat>(decodedFrame->format),
            decodedFrame->width, decodedFrame->height,
            AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws)
            return {};

        QByteArray rgb;
        rgb.resize(decodedFrame->width * decodedFrame->height * 3);
        uint8_t *dstData[4] = { reinterpret_cast<uint8_t *>(rgb.data()), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { decodedFrame->width * 3, 0, 0, 0 };
        sws_scale(sws, decodedFrame->data, decodedFrame->linesize, 0, decodedFrame->height, dstData, dstLinesize);
        sws_freeContext(sws);

        QImage image(reinterpret_cast<const uchar *>(rgb.constData()),
                     decodedFrame->width, decodedFrame->height,
                     dstLinesize[0], QImage::Format_RGB888);
        return image.copy();
    };

    QImage result;
    if (avcodec_send_packet(codecCtx, packet) >= 0) {
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            result = frameToImage(frame);
            if (!result.isNull())
                break;
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);
    return result;
}

QImage extractEmbeddedCoverArt(const QString &filePath)
{
    coverLog(filePath, QStringLiteral("extractEmbeddedCoverArt: begin"));

    AVFormatContext *formatCtx = nullptr;
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const QByteArray utf8Path = nativePath.toUtf8();

    int err = avformat_open_input(&formatCtx, utf8Path.constData(), nullptr, nullptr);
#ifdef Q_OS_WIN
    if (err < 0) {
        if (formatCtx)
            avformat_close_input(&formatCtx);
        const QByteArray ansiPath = QFile::encodeName(nativePath);
        if (ansiPath != utf8Path)
            err = avformat_open_input(&formatCtx, ansiPath.constData(), nullptr, nullptr);
    }
#endif
    if (err < 0 || !formatCtx) {
        coverLog(filePath, QStringLiteral("extractEmbeddedCoverArt: avformat_open_input failed"));
        return {};
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        coverLog(filePath, QStringLiteral("extractEmbeddedCoverArt: avformat_find_stream_info failed"));
        avformat_close_input(&formatCtx);
        return {};
    }

    const bool allowVideoPacketScan = shouldScanVideoPacketsForEmbeddedCover(filePath);
    coverLog(filePath,
             QStringLiteral("extractEmbeddedCoverArt: video packet fallback %1")
                 .arg(allowVideoPacketScan ? QStringLiteral("enabled") : QStringLiteral("disabled")));

    QImage cover;
    const auto tryPacket = [&cover, &filePath](const AVPacket *packet,
                                               int streamIndex,
                                               const QString &origin) {
        if (!packet || !packet->data || packet->size <= 0)
            return false;

        const QImage image = QImage::fromData(packet->data, packet->size);
        if (!image.isNull()) {
            cover = image;
            coverLog(filePath,
                     QStringLiteral("extractEmbeddedCoverArt: %1 stream=%2 decoded via QImage, bytes=%3, %4")
                         .arg(origin).arg(streamIndex).arg(packet->size)
                         .arg(describeImageForLog(cover)));
            return true;
        }

        const QByteArray raw(reinterpret_cast<const char *>(packet->data), packet->size);
        cover = decodeImageBytesWithFfmpeg(raw);
        if (!cover.isNull()) {
            coverLog(filePath,
                     QStringLiteral("extractEmbeddedCoverArt: %1 stream=%2 decoded via FFmpeg-image-bytes, bytes=%3, %4")
                         .arg(origin).arg(streamIndex).arg(packet->size)
                         .arg(describeImageForLog(cover)));
        }
        return !cover.isNull();
    };

    QList<int> videoCoverStreamIndices;
    for (unsigned int i = 0; i < formatCtx->nb_streams; ++i) {
        const AVStream *stream = formatCtx->streams[i];
        if (!stream)
            continue;

        const int codecType = stream->codecpar ? static_cast<int>(stream->codecpar->codec_type) : -1;
        const int codecId = stream->codecpar ? static_cast<int>(stream->codecpar->codec_id) : -1;
        coverLog(filePath,
                 QStringLiteral("extractEmbeddedCoverArt: stream=%1 codecType=%2 codecId=%3 attached_pic=%4")
                     .arg(static_cast<int>(i)).arg(codecType).arg(codecId)
                     .arg((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 ? 1 : 0));

        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
            if (tryPacket(&stream->attached_pic, static_cast<int>(i), QStringLiteral("attached_pic")))
                break;

            const QImage decoded = decodeCoverPacketWithFfmpeg(stream, &stream->attached_pic);
            if (!decoded.isNull()) {
                cover = decoded;
                coverLog(filePath,
                         QStringLiteral("extractEmbeddedCoverArt: stream=%1 decoded via FFmpeg-packet, %2")
                             .arg(static_cast<int>(i)).arg(describeImageForLog(cover)));
                break;
            }
        }

        if (cover.isNull() && allowVideoPacketScan && stream->codecpar
            && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (isLikelyCoverImageCodecId(stream->codecpar->codec_id))
                videoCoverStreamIndices.append(static_cast<int>(i));
            else
                coverLog(filePath,
                         QStringLiteral("extractEmbeddedCoverArt: stream=%1 skipped for packet scan (non-image codecId=%2)")
                             .arg(static_cast<int>(i)).arg(codecId));
        }
    }

    if (cover.isNull() && !videoCoverStreamIndices.isEmpty()) {
        av_seek_frame(formatCtx, -1, 0, AVSEEK_FLAG_BACKWARD);

        AVPacket packet;
        int packetsScanned = 0;
        constexpr int kMaxPacketsToScan = 256;
        constexpr int kMaxPacketsPerStream = 12;
        QHash<int, int> packetCountByStream;

        while (packetsScanned < kMaxPacketsToScan && av_read_frame(formatCtx, &packet) >= 0) {
            const bool isCoverStream = videoCoverStreamIndices.contains(packet.stream_index);
            if (isCoverStream) {
                const int streamPackets = packetCountByStream.value(packet.stream_index, 0);
                if (streamPackets >= kMaxPacketsPerStream) {
                    av_packet_unref(&packet);
                    ++packetsScanned;
                    continue;
                }
                packetCountByStream.insert(packet.stream_index, streamPackets + 1);

                if (tryPacket(&packet, packet.stream_index, QStringLiteral("video_packet"))) {
                    av_packet_unref(&packet);
                    break;
                }

                const AVStream *stream = formatCtx->streams[packet.stream_index];
                const QImage decoded = decodeCoverPacketWithFfmpeg(stream, &packet);
                if (!decoded.isNull()) {
                    cover = decoded;
                    coverLog(filePath,
                             QStringLiteral("extractEmbeddedCoverArt: stream=%1 video packet decoded via FFmpeg-packet, %2")
                                 .arg(packet.stream_index).arg(describeImageForLog(cover)));
                    av_packet_unref(&packet);
                    break;
                }
            }
            av_packet_unref(&packet);
            ++packetsScanned;
        }

        coverLog(filePath,
                 QStringLiteral("extractEmbeddedCoverArt: video packet scan finished, scanned=%1 candidates=%2")
                     .arg(packetsScanned).arg(videoCoverStreamIndices.size()));
    }

    coverLog(filePath,
             QStringLiteral("extractEmbeddedCoverArt: result=%1")
                 .arg(describeImageForLog(cover)));

    avformat_close_input(&formatCtx);
    return cover;
}
#endif

quint32 readSyncSafe32(const uchar *p)
{
    return (static_cast<quint32>(p[0] & 0x7F) << 21)
         | (static_cast<quint32>(p[1] & 0x7F) << 14)
         | (static_cast<quint32>(p[2] & 0x7F) << 7)
         | static_cast<quint32>(p[3] & 0x7F);
}

quint32 readBe32(const uchar *p)
{
    return (static_cast<quint32>(p[0]) << 24)
         | (static_cast<quint32>(p[1]) << 16)
         | (static_cast<quint32>(p[2]) << 8)
         | static_cast<quint32>(p[3]);
}

QByteArray decodeUnsynchronization(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const char c = in[i];
        out.append(c);
        if (static_cast<uchar>(c) == 0xFF
            && i + 1 < in.size()
            && static_cast<uchar>(in[i + 1]) == 0x00) {
            ++i;
        }
    }
    return out;
}

int findImageDataOffset(const QByteArray &data, int from)
{
    const int start = qMax(0, from);
    int best = -1;

    auto consider = [&data, start, &best](const QByteArray &sig, bool needsWebpMarker = false) {
        int pos = data.indexOf(sig, start);
        while (pos >= 0) {
            if (needsWebpMarker) {
                if (pos + 12 > data.size() || data.mid(pos + 8, 4) != "WEBP") {
                    pos = data.indexOf(sig, pos + 1);
                    continue;
                }
            }
            if (best < 0 || pos < best)
                best = pos;
            break;
        }
    };

    consider(QByteArray::fromHex("FFD8FF")); // JPEG
    consider(QByteArray("\x89PNG\r\n\x1A\n", 8)); // PNG
    consider(QByteArray("GIF87a"));
    consider(QByteArray("GIF89a"));
    consider(QByteArray("BM")); // BMP
    consider(QByteArray("RIFF"), true); // WEBP

    return best;
}

QImage decodeCoverImage(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return {};

    const QImage image = QImage::fromData(bytes);
    if (!image.isNull())
        return image;

#ifdef MUSICPLAYER_HAS_FFMPEG
    return decodeImageBytesWithFfmpeg(bytes);
#else
    return {};
#endif
}

int findTextTerminator(const QByteArray &data, int start, quint8 encoding)
{
    if (start < 0 || start >= data.size())
        return -1;

    if (encoding == 1 || encoding == 2) {
        for (int i = start; i + 1 < data.size(); ++i) {
            if (data[i] == '\0' && data[i + 1] == '\0')
                return i;
        }
        return -1;
    }

    return data.indexOf('\0', start);
}

QImage extractMp3Id3ApicCover(const QString &filePath)
{
    coverLog(filePath, QStringLiteral("extractMp3Id3ApicCover: begin"));

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        coverLog(filePath, QStringLiteral("extractMp3Id3ApicCover: open failed"));
        return {};
    }

    const QByteArray header = file.read(10);
    if (header.size() < 10 || !header.startsWith("ID3")) {
        coverLog(filePath, QStringLiteral("extractMp3Id3ApicCover: no ID3v2 header"));
        return {};
    }

    const int versionMajor = static_cast<uchar>(header[3]);
    const quint8 flags = static_cast<quint8>(header[5]);
    const quint32 tagSize = readSyncSafe32(reinterpret_cast<const uchar *>(header.constData() + 6));
    if (tagSize == 0)
        return {};

    QByteArray tag = file.read(static_cast<qint64>(tagSize));
    if (tag.isEmpty())
        return {};

    if ((flags & 0x80) != 0)
        tag = decodeUnsynchronization(tag);

    if (versionMajor == 4 && (flags & 0x10) != 0 && tag.size() >= 10) {
        const QByteArray tail = tag.right(10);
        if (tail.startsWith("3DI"))
            tag.chop(10);
    }

    int pos = 0;
    if ((flags & 0x40) != 0 && tag.size() >= 4) {
        const uchar *extPtr = reinterpret_cast<const uchar *>(tag.constData());
        if (versionMajor == 3) {
            const int extSize = static_cast<int>(readBe32(extPtr));
            const int extTotal = extSize + 4;
            if (extTotal > 0 && extTotal < tag.size())
                pos = extTotal;
        } else if (versionMajor == 4) {
            const int extTotal = static_cast<int>(readSyncSafe32(extPtr));
            if (extTotal > 0 && extTotal < tag.size())
                pos = extTotal;
        }
    }

    while (pos < tag.size()) {
        QByteArray frameId;
        int frameHeaderSize = 0;
        int frameSize = 0;

        if (versionMajor == 2) {
            if (pos + 6 > tag.size())
                break;
            frameId = tag.mid(pos, 3);
            if (frameId.trimmed().isEmpty())
                break;
            frameSize = (static_cast<uchar>(tag[pos + 3]) << 16)
                      | (static_cast<uchar>(tag[pos + 4]) << 8)
                      | static_cast<uchar>(tag[pos + 5]);
            frameHeaderSize = 6;
        } else {
            if (pos + 10 > tag.size())
                break;
            frameId = tag.mid(pos, 4);
            if (frameId.trimmed().isEmpty())
                break;
            const uchar *sizePtr = reinterpret_cast<const uchar *>(tag.constData() + pos + 4);
            frameSize = (versionMajor == 4) ? static_cast<int>(readSyncSafe32(sizePtr))
                                            : static_cast<int>(readBe32(sizePtr));
            frameHeaderSize = 10;
        }

        if (frameSize <= 0 || pos + frameHeaderSize + frameSize > tag.size())
            break;

        const QByteArray payload = tag.mid(pos + frameHeaderSize, frameSize);

        if (frameId == "APIC" && payload.size() > 4) {
            const quint8 encoding = static_cast<quint8>(payload[0]);
            int cursor = 1;
            const int mimeEnd = payload.indexOf('\0', cursor);
            int imageStart = -1;
            if (mimeEnd > cursor) {
                cursor = mimeEnd + 1;
                if (cursor < payload.size()) {
                    ++cursor;
                    const int descEnd = findTextTerminator(payload, cursor, encoding);
                    if (descEnd >= 0)
                        imageStart = descEnd + ((encoding == 1 || encoding == 2) ? 2 : 1);
                }
            }
            if (imageStart < 0 || imageStart >= payload.size())
                imageStart = findImageDataOffset(payload, qMax(1, cursor));
            if (imageStart >= 0 && imageStart < payload.size()) {
                const QImage img = decodeCoverImage(payload.mid(imageStart));
                if (!img.isNull()) {
                    coverLog(filePath, QStringLiteral("extractMp3Id3ApicCover: APIC decoded, %1")
                                 .arg(describeImageForLog(img)));
                    return img;
                }
            }
        } else if (frameId == "PIC" && payload.size() > 6) {
            const quint8 encoding = static_cast<quint8>(payload[0]);
            int cursor = 1 + 3 + 1;
            const int descEnd = findTextTerminator(payload, cursor, encoding);
            int imageStart = -1;
            if (descEnd >= 0)
                imageStart = descEnd + ((encoding == 1 || encoding == 2) ? 2 : 1);
            if (imageStart < 0 || imageStart >= payload.size())
                imageStart = findImageDataOffset(payload, cursor);
            if (imageStart >= 0 && imageStart < payload.size()) {
                const QImage img = decodeCoverImage(payload.mid(imageStart));
                if (!img.isNull()) {
                    coverLog(filePath, QStringLiteral("extractMp3Id3ApicCover: PIC decoded, %1")
                                 .arg(describeImageForLog(img)));
                    return img;
                }
            }
        }

        pos += frameHeaderSize + frameSize;
    }

    const int rawImagePos = findImageDataOffset(tag, 0);
    if (rawImagePos >= 0 && rawImagePos < tag.size()) {
        const QImage img = decodeCoverImage(tag.mid(rawImagePos));
        if (!img.isNull()) {
            coverLog(filePath, QStringLiteral("extractMp3Id3ApicCover: raw tag signature decoded, %1")
                         .arg(describeImageForLog(img)));
            return img;
        }
    }

    coverLog(filePath, QStringLiteral("extractMp3Id3ApicCover: no usable image found"));
    return {};
}

} // namespace

TrackItem::TrackItem(const QString &filePath, bool deferMetadata)
{
    m_metadata.filePath = filePath;
    if (deferMetadata)
        initializeBasicMetadata();
    else
        loadMetadata();
}

void TrackItem::ensureMetadataLoaded()
{
    if (m_metadataLoaded)
        return;

    // Fast path: try to load from cache
    QFileInfo fileInfo(m_metadata.filePath);

    // First try memory-only cache (fast)
    TrackMetadata cachedMeta;
    cachedMeta.filePath = m_metadata.filePath;
    if (tryLoadTrackMetadataFromCache(fileInfo, cachedMeta)) {
        // Refresh cover if cached one is default/suspicious
        if (isDefaultCoverImage(cachedMeta.coverArt) || isSuspiciousCoverImage(cachedMeta.coverArt)) {
            const QImage refreshedCover = extractCoverArt(m_metadata.filePath);
            if (!refreshedCover.isNull() && !isDefaultCoverImage(refreshedCover)) {
                cachedMeta.coverArt = refreshedCover;
                updateTrackMetadataCache(fileInfo, cachedMeta);
            }
        }
        m_metadata = cachedMeta;
        m_metadataLoaded = true;
        return;
    }

    // If no cache, do basic initialization only
    initializeBasicMetadata();

    // Load cover art (the main visual part)
    m_metadata.coverArt = extractCoverArt(m_metadata.filePath);
    if (m_metadata.coverArt.isNull())
        m_metadata.coverArt = defaultCoverImage();

#ifdef MUSICPLAYER_HAS_FFMPEG
    // Load FFmpeg metadata (title, artist, etc.) — always prefer real tags over filename guesses
    TrackMetadata probe;
    probe.filePath = m_metadata.filePath;
    applyFfmpegMetadata(m_metadata.filePath, probe);
    if (!probe.title.isEmpty()) m_metadata.title = probe.title;
    if (!probe.artist.isEmpty()) m_metadata.artist = probe.artist;
    if (!probe.album.isEmpty()) m_metadata.album = probe.album;
    if (!probe.genre.isEmpty()) m_metadata.genre = probe.genre;
    if (!probe.year.isEmpty()) m_metadata.year = probe.year;
    if (probe.trackNumber > 0) m_metadata.trackNumber = probe.trackNumber;
    if (probe.sampleRate > 0) m_metadata.sampleRate = probe.sampleRate;
    if (probe.bitrate > 0) m_metadata.bitrate = probe.bitrate;
    if (probe.duration > 0) m_metadata.duration = probe.duration;
#endif

    // Handle format-specific duration
    QString suffix = fileInfo.suffix().toLower();
    if (suffix == "flac" && m_metadata.duration <= 0) {
        int sr = 0; QString yr; qint64 dur = 0;
        readFlacFileInfo(m_metadata.filePath, sr, yr, dur);
        if (m_metadata.duration <= 0 && dur > 0) m_metadata.duration = dur;
        if (m_metadata.sampleRate <= 0 && sr > 0) m_metadata.sampleRate = sr;
    } else if (suffix == "mp3" && m_metadata.duration <= 0) {
        qint64 dur = readMp3Duration(m_metadata.filePath);
        if (m_metadata.duration <= 0 && dur > 0) m_metadata.duration = dur;
    }

    // Cache for future
    updateTrackMetadataCache(fileInfo, m_metadata);

    m_metadataLoaded = true;
}

void TrackItem::initializeBasicMetadata()
{
    const QFileInfo fileInfo(m_metadata.filePath);
    m_metadata.filePath = fileInfo.absoluteFilePath();
    const QString fileName = fileInfo.completeBaseName();
    const QStringList parts = fileName.split(" - ");

    if (parts.size() >= 2) {
        if (parts.size() >= 3) {
            bool ok = false;
            const int trackNum = parts[0].trimmed().toInt(&ok);
            if (ok) {
                m_metadata.trackNumber = trackNum;
                m_metadata.artist = parts[1].trimmed();
                m_metadata.title = parts[2].trimmed();
            } else {
                m_metadata.artist = parts[0].trimmed();
                m_metadata.title = parts[1].trimmed();
            }
        } else {
            m_metadata.artist = parts[0].trimmed();
            m_metadata.title = parts[1].trimmed();
        }
    } else {
        m_metadata.title = fileName;
        m_metadata.artist = "Unknown Artist";
    }

    m_metadata.album = fileInfo.dir().dirName();
    m_metadata.coverArt = defaultCoverImage();
    m_metadata.duration = 0;
}

void TrackItem::loadMetadata()
{
    // ── NEW: CUE-track guard ──────────────────────────────────────────────
    if (m_metadata.isCueTrack) {
        const QString savedTitle    = m_metadata.title;
        const QString savedArtist   = m_metadata.artist;
        const QString savedAlbum    = m_metadata.album;
        const QString savedGenre    = m_metadata.genre;
        const QString savedYear     = m_metadata.year;
        const int     savedTrackNum = m_metadata.trackNumber;
        const qint64  savedStart    = m_metadata.cueStartMs;
        const qint64  savedEnd      = m_metadata.cueEndMs;
        const QString savedCueFile  = m_metadata.cueFilePath;
        const qint64  savedDuration = m_metadata.duration;

        m_metadata.coverArt = extractCoverArt(m_metadata.filePath);
        if (m_metadata.coverArt.isNull())
            m_metadata.coverArt = defaultCoverImage();

#ifdef MUSICPLAYER_HAS_FFMPEG
        TrackMetadata probe;
        probe.filePath = m_metadata.filePath;
        applyFfmpegMetadata(m_metadata.filePath, probe);
        if (m_metadata.sampleRate <= 0) m_metadata.sampleRate = probe.sampleRate;
        if (m_metadata.bitrate    <= 0) m_metadata.bitrate    = probe.bitrate;
#endif

        m_metadata.title       = savedTitle;
        m_metadata.artist      = savedArtist;
        m_metadata.album       = savedAlbum;
        m_metadata.genre       = savedGenre;
        m_metadata.year        = savedYear;
        m_metadata.trackNumber = savedTrackNum;
        m_metadata.cueStartMs  = savedStart;
        m_metadata.cueEndMs    = savedEnd;
        m_metadata.cueFilePath = savedCueFile;
        m_metadata.isCueTrack  = true;
        m_metadata.duration    = savedDuration;

        m_metadataLoaded = true;
        return;
    }
    // ── END CUE guard ─────────────────────────────────────────────────────

    QFileInfo fileInfo(m_metadata.filePath);
    if (tryLoadTrackMetadataFromCache(fileInfo, m_metadata)) {
        coverLog(m_metadata.filePath,
                 QStringLiteral("loadMetadata: cache hit, cover=%1")
                     .arg(describeImageForLog(m_metadata.coverArt)));

        if (isDefaultCoverImage(m_metadata.coverArt) || isSuspiciousCoverImage(m_metadata.coverArt)) {
            coverLog(m_metadata.filePath,
                     QStringLiteral("loadMetadata: cache cover is default/suspicious, refreshing"));

            const QImage refreshedCover = extractCoverArt(m_metadata.filePath);
            if (!refreshedCover.isNull() && !isDefaultCoverImage(refreshedCover)) {
                m_metadata.coverArt = refreshedCover;
                updateTrackMetadataCache(fileInfo, m_metadata);
                coverLog(m_metadata.filePath,
                         QStringLiteral("loadMetadata: cache refresh applied, cover=%1")
                             .arg(describeImageForLog(m_metadata.coverArt)));
            } else {
                coverLog(m_metadata.filePath,
                         QStringLiteral("loadMetadata: cache refresh failed, keeping previous cover"));
            }
        }
        m_metadataLoaded = true;
        return;
    }

    coverLog(m_metadata.filePath, QStringLiteral("loadMetadata: cache miss"));

    initializeBasicMetadata();
    m_metadata.coverArt = extractCoverArt(m_metadata.filePath);
    coverLog(m_metadata.filePath,
             QStringLiteral("loadMetadata: after extractCoverArt cover=%1")
                 .arg(describeImageForLog(m_metadata.coverArt)));

#ifdef MUSICPLAYER_HAS_FFMPEG
    applyFfmpegMetadata(m_metadata.filePath, m_metadata);
#endif

    QString suffix = fileInfo.suffix().toLower();
    if (suffix == "flac") {
        int sr = 0; QString yr; qint64 dur = 0;
        readFlacFileInfo(m_metadata.filePath, sr, yr, dur);
        if (m_metadata.sampleRate <= 0 && sr > 0) m_metadata.sampleRate = sr;
        if (m_metadata.year.isEmpty() && !yr.isEmpty()) m_metadata.year = yr;
        if (m_metadata.duration <= 0 && dur > 0) m_metadata.duration = dur;
    } else if (suffix == "mp3") {
        qint64 dur = readMp3Duration(m_metadata.filePath);
        if (m_metadata.duration <= 0 && dur > 0) m_metadata.duration = dur;
    }

    updateTrackMetadataCache(fileInfo, m_metadata);
    coverLog(m_metadata.filePath,
             QStringLiteral("loadMetadata: completed, cover=%1")
                 .arg(describeImageForLog(m_metadata.coverArt)));
    m_metadataLoaded = true;
}

void TrackItem::loadFullMetadata(const QMediaMetaData &metaData)
{
    m_metadataLoaded = true;
    qDebug() << "--- Loading metadata for:" << m_metadata.filePath;
    for (const QMediaMetaData::Key &key : metaData.keys()) {
        qDebug() << "  " << QMediaMetaData::metaDataKeyToString(key) << ":" << metaData.value(key);
    }

    QString title = metaData.value(QMediaMetaData::Title).toString();
    if (!title.isEmpty())
        m_metadata.title = title;

    QString artist = metaData.value(QMediaMetaData::AlbumArtist).toString();
    if (artist.isEmpty())
        artist = metaData.value(QMediaMetaData::ContributingArtist).toString();
    if (!artist.isEmpty())
        m_metadata.artist = artist;

    QString album = metaData.value(QMediaMetaData::AlbumTitle).toString();
    if (!album.isEmpty())
        m_metadata.album = album;

    QVariant dateVar = metaData.value(QMediaMetaData::Date);
    if (dateVar.isValid()) {
        QDateTime dt = dateVar.toDateTime();
        if (dt.isValid()) {
            m_metadata.year = dt.toString("yyyy");
        } else {
            QDate d = dateVar.toDate();
            if (d.isValid()) {
                m_metadata.year = d.toString("yyyy");
            } else {
                QString s = dateVar.toString().trimmed();
                if (!s.isEmpty()) {
                    static QRegularExpression yearRx("(\\d{4})");
                    QRegularExpressionMatch match = yearRx.match(s);
                    if (match.hasMatch())
                        m_metadata.year = match.captured(1);
                }
            }
        }
    }

    QStringList genreList = metaData.value(QMediaMetaData::Genre).toStringList();
    if (!genreList.isEmpty())
        m_metadata.genre = genreList.join(", ");

    int trackNumber = metaData.value(QMediaMetaData::TrackNumber).toInt();
    if (trackNumber > 0)
        m_metadata.trackNumber = trackNumber;

    int bitrate = metaData.value(QMediaMetaData::AudioBitRate).toInt();
    if (bitrate > 0)
        m_metadata.bitrate = bitrate;

    QVariant coverVar = metaData.value(QMediaMetaData::ThumbnailImage);
    if (!coverVar.isValid())
        coverVar = metaData.value(QMediaMetaData::CoverArtImage);
    if (coverVar.isValid()) {
        QImage coverImage = coverVar.value<QImage>();
        if (!coverImage.isNull()) {
            if (!isSuspiciousCoverImage(coverImage)) {
                m_metadata.coverArt = coverImage;
                coverLog(m_metadata.filePath,
                         QStringLiteral("loadFullMetadata: accepted Qt cover, %1")
                             .arg(describeImageForLog(coverImage)));
            } else if (isDefaultCoverImage(m_metadata.coverArt)
                       || isSuspiciousCoverImage(m_metadata.coverArt)) {
                coverLog(m_metadata.filePath,
                         QStringLiteral("loadFullMetadata: rejected suspicious Qt cover, current=%1")
                             .arg(describeImageForLog(m_metadata.coverArt)));
                const QImage refreshedCover = extractCoverArt(m_metadata.filePath);
                if (!refreshedCover.isNull()) {
                    m_metadata.coverArt = refreshedCover;
                    coverLog(m_metadata.filePath,
                             QStringLiteral("loadFullMetadata: fallback cover applied, %1")
                                 .arg(describeImageForLog(m_metadata.coverArt)));
                }
            } else {
                coverLog(m_metadata.filePath,
                         QStringLiteral("loadFullMetadata: rejected suspicious Qt cover, preserved existing=%1")
                             .arg(describeImageForLog(m_metadata.coverArt)));
            }
        }
    }

    QFileInfo fi(m_metadata.filePath);
    if (fi.suffix().compare("flac", Qt::CaseInsensitive) == 0) {
        int flacSampleRate = 0; QString flacYear; qint64 flacDur = 0;
        readFlacFileInfo(m_metadata.filePath, flacSampleRate, flacYear, flacDur);
        if (m_metadata.sampleRate == 0 && flacSampleRate > 0)
            m_metadata.sampleRate = flacSampleRate;
        if (m_metadata.year.isEmpty() && !flacYear.isEmpty())
            m_metadata.year = flacYear;
        if (m_metadata.duration == 0 && flacDur > 0)
            m_metadata.duration = flacDur;
    }

    updateTrackMetadataCache(fi, m_metadata);
}

void TrackItem::readFlacFileInfo(const QString &filePath, int &sampleRate,
                                  QString &year, qint64 &durationMs)
{
    sampleRate = 0;
    year.clear();
    durationMs = 0;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QByteArray marker = file.read(4);
    if (marker != "fLaC")
        return;

    bool lastBlock = false;
    while (!lastBlock && !file.atEnd()) {
        char headerByte;
        if (!file.getChar(&headerByte))
            break;
        quint8 h = static_cast<quint8>(headerByte);
        lastBlock = (h & 0x80) != 0;
        quint8 blockType = h & 0x7F;

        QByteArray lenBytes = file.read(3);
        if (lenBytes.size() < 3) break;
        quint32 blockLength = (static_cast<quint8>(lenBytes[0]) << 16)
                            | (static_cast<quint8>(lenBytes[1]) << 8)
                            | static_cast<quint8>(lenBytes[2]);

        qint64 blockStart = file.pos();

        if (blockType == 0 && blockLength >= 18) {
            QByteArray data = file.read(18);
            if (data.size() >= 18) {
                // Bytes 10-13: top 20 bits = sample rate
                quint32 word = (static_cast<quint8>(data[10]) << 24)
                             | (static_cast<quint8>(data[11]) << 16)
                             | (static_cast<quint8>(data[12]) << 8)
                             | static_cast<quint8>(data[13]);
                sampleRate = static_cast<int>(word >> 12);

                // Total samples: low 4 bits of byte 13, then bytes 14-17 = 36 bits
                quint64 totalSamples =
                    (static_cast<quint64>(static_cast<quint8>(data[13]) & 0x0F) << 32)
                    | (static_cast<quint64>(static_cast<quint8>(data[14])) << 24)
                    | (static_cast<quint64>(static_cast<quint8>(data[15])) << 16)
                    | (static_cast<quint64>(static_cast<quint8>(data[16])) << 8)
                    | static_cast<quint64>(static_cast<quint8>(data[17]));

                if (sampleRate > 0 && totalSamples > 0)
                    durationMs = static_cast<qint64>(totalSamples * 1000 / sampleRate);
            }
        }

        if (blockType == 4) {
            // VORBIS_COMMENT: contains key=value pairs including DATE
            QByteArray data = file.read(qMin(blockLength, quint32(65536)));
            if (data.size() >= 8) {
                int pos = 0;
                quint32 vendorLen = qFromLittleEndian<quint32>(
                    reinterpret_cast<const uchar*>(data.constData() + pos));
                pos += 4 + vendorLen;

                if (pos + 4 <= data.size()) {
                    quint32 numComments = qFromLittleEndian<quint32>(
                        reinterpret_cast<const uchar*>(data.constData() + pos));
                    pos += 4;

                    for (quint32 i = 0; i < numComments && pos + 4 <= data.size(); ++i) {
                        quint32 commentLen = qFromLittleEndian<quint32>(
                            reinterpret_cast<const uchar*>(data.constData() + pos));
                        pos += 4;
                        if (pos + static_cast<int>(commentLen) > data.size()) break;

                        QString comment = QString::fromUtf8(data.constData() + pos, commentLen);
                        pos += commentLen;

                        if (comment.startsWith("DATE=", Qt::CaseInsensitive) ||
                            comment.startsWith("YEAR=", Qt::CaseInsensitive)) {
                            QString value = comment.mid(comment.indexOf('=') + 1).trimmed();
                            static QRegularExpression yearRx("(\\d{4})");
                            QRegularExpressionMatch match = yearRx.match(value);
                            if (match.hasMatch()) {
                                year = match.captured(1);
                                break;
                            }
                        }
                    }
                }
            }
        }

        file.seek(blockStart + blockLength);
    }
}

qint64 TrackItem::readMp3Duration(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return 0;

    QByteArray header = file.read(4096);
    if (header.size() < 128)
        return 0;

    static const int mp3SampleRates[4][4] = {
        {11025, 12000, 8000, 0},   // MPEG 2.5
        {0, 0, 0, 0},             // reserved
        {22050, 24000, 16000, 0},  // MPEG 2
        {44100, 48000, 32000, 0}   // MPEG 1
    };
    static const int mp3Bitrates[2][16] = {
        {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},  // MPEG 2/2.5 Layer III
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0} // MPEG 1 Layer III
    };

    int offset = 0;
    // Skip ID3v2 tag
    if (header.size() >= 10 && header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        quint32 tagSize = ((static_cast<quint8>(header[6]) & 0x7F) << 21)
                        | ((static_cast<quint8>(header[7]) & 0x7F) << 14)
                        | ((static_cast<quint8>(header[8]) & 0x7F) << 7)
                        | (static_cast<quint8>(header[9]) & 0x7F);
        offset = 10 + tagSize;
        file.seek(offset);
        header = file.read(4096);
        if (header.size() < 128) return 0;
    }

    // Find first sync word
    int syncPos = -1;
    for (int i = 0; i < header.size() - 4; ++i) {
        if (static_cast<quint8>(header[i]) == 0xFF
            && (static_cast<quint8>(header[i + 1]) & 0xE0) == 0xE0) {
            syncPos = i;
            break;
        }
    }
    if (syncPos < 0) return 0;

    quint8 b1 = static_cast<quint8>(header[syncPos + 1]);
    quint8 b2 = static_cast<quint8>(header[syncPos + 2]);

    int mpegVer = (b1 >> 3) & 0x03;
    int srIdx = (b2 >> 2) & 0x03;
    int brIdx = (b2 >> 4) & 0x0F;
    if (mpegVer == 1 || srIdx == 3 || brIdx == 0 || brIdx == 15) return 0;

    int sampleRate = mp3SampleRates[mpegVer][srIdx];
    if (sampleRate == 0) return 0;

    int isMpeg1 = (mpegVer == 3) ? 1 : 0;
    int samplesPerFrame = isMpeg1 ? 1152 : 576;

    // Look for Xing/Info header (accurate VBR duration)
    int xingOffset = syncPos + (isMpeg1 ? 36 : 21);
    if (xingOffset + 12 <= header.size()) {
        QByteArray tag = header.mid(xingOffset, 4);
        if (tag == "Xing" || tag == "Info") {
            quint32 flags = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(header.constData() + xingOffset + 4));
            if (flags & 0x01) {
                quint32 numFrames = qFromBigEndian<quint32>(
                    reinterpret_cast<const uchar *>(header.constData() + xingOffset + 8));
                return static_cast<qint64>(numFrames) * samplesPerFrame * 1000 / sampleRate;
            }
        }
    }

    // Fallback: estimate from file size and first frame bitrate
    int bitrate = mp3Bitrates[isMpeg1][brIdx] * 1000;
    if (bitrate <= 0) return 0;
    qint64 fileSize = file.size();
    return (fileSize * 8) / (bitrate / 1000);
}

QImage TrackItem::extractCoverArt(const QString &filePath)
{
    QFileInfo fileInfo(filePath);
    QString dirPath = fileInfo.absolutePath();
    const bool isMp3 = fileInfo.suffix().compare("mp3", Qt::CaseInsensitive) == 0;

    coverLog(filePath,
             QStringLiteral("extractCoverArt: begin, suffix=%1")
                 .arg(fileInfo.suffix().toLower()));

    QStringList coverNames = {
        "cover.jpg", "cover.png", "cover.jpeg", "folder.jpg", "folder.png",
        "album.jpg", "album.png", "artwork.jpg", "front.jpg", "Cover.jpg",
        "Folder.jpg", "Album.jpg", "Front.jpg", "COVER.JPG", "FOLDER.JPG"
    };

    QImage folderCoverFallback;
    QString folderCoverFallbackPath;

    for (const QString &coverName : coverNames) {
        QString coverPath = dirPath + "/" + coverName;
        if (QFile::exists(coverPath)) {
            QImage img(coverPath);
            if (!img.isNull()) {
                if (isSuspiciousCoverImage(img)) {
                    coverLog(filePath,
                             QStringLiteral("extractCoverArt: folder image rejected as suspicious -> %1, %2")
                                 .arg(QDir::toNativeSeparators(coverPath))
                                 .arg(describeImageForLog(img)));
                    continue;
                }

                if (isMp3) {
                    folderCoverFallback = img;
                    folderCoverFallbackPath = coverPath;
                    coverLog(filePath,
                             QStringLiteral("extractCoverArt: folder image candidate for MP3 fallback -> %1")
                                 .arg(QDir::toNativeSeparators(coverPath)));
                    break;
                }

                coverLog(filePath,
                         QStringLiteral("extractCoverArt: folder image used -> %1")
                             .arg(QDir::toNativeSeparators(coverPath)));
                return img;
            }
        }
    }

#ifdef MUSICPLAYER_HAS_FFMPEG
    QImage embeddedCover = extractEmbeddedCoverArt(filePath);
    if (!embeddedCover.isNull() && !isSuspiciousCoverImage(embeddedCover)) {
        coverLog(filePath,
                 QStringLiteral("extractCoverArt: using embedded FFmpeg cover, %1")
                     .arg(describeImageForLog(embeddedCover)));
        return embeddedCover;
    }

    if (!embeddedCover.isNull() && isSuspiciousCoverImage(embeddedCover)) {
        coverLog(filePath,
                 QStringLiteral("extractCoverArt: embedded FFmpeg cover rejected as suspicious, %1")
                     .arg(describeImageForLog(embeddedCover)));
    }
#endif

    if (isMp3) {
        QImage mp3Cover = extractMp3Id3ApicCover(filePath);
        if (!mp3Cover.isNull() && !isSuspiciousCoverImage(mp3Cover)) {
            coverLog(filePath,
                     QStringLiteral("extractCoverArt: using MP3 ID3 cover, %1")
                         .arg(describeImageForLog(mp3Cover)));
            return mp3Cover;
        }

        if (!mp3Cover.isNull() && isSuspiciousCoverImage(mp3Cover)) {
            coverLog(filePath,
                     QStringLiteral("extractCoverArt: MP3 ID3 cover rejected as suspicious, %1")
                         .arg(describeImageForLog(mp3Cover)));
        }

        if (!folderCoverFallback.isNull()) {
            coverLog(filePath,
                     QStringLiteral("extractCoverArt: using MP3 folder fallback -> %1, %2")
                         .arg(QDir::toNativeSeparators(folderCoverFallbackPath))
                         .arg(describeImageForLog(folderCoverFallback)));
            return folderCoverFallback;
        }
    }

    coverLog(filePath, QStringLiteral("extractCoverArt: fallback to default cover"));
    return defaultCoverImage();
}

QString TrackItem::formatDuration(qint64 milliseconds)
{
    if (milliseconds <= 0) return "--:--";

    int seconds = milliseconds / 1000;
    int minutes = seconds / 60;
    seconds %= 60;

    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

QImage TrackItem::coverArtFromFile(const QString &filePath)
{
    QImage cover = extractCoverArt(filePath);
    if (cover.isNull())
        cover = defaultCoverImage();
    return cover;
}

TrackItem *TrackItem::fromCueTrack(const CueTrack &ct, const QImage &sharedCover)
{
    auto *item = new TrackItem(ct.audioFilePath, true);

    item->m_metadata.filePath    = ct.audioFilePath;
    item->m_metadata.title       = ct.title.isEmpty()
                                       ? QStringLiteral("Track %1").arg(ct.trackNumber)
                                       : ct.title;
    item->m_metadata.artist      = ct.performer;
    item->m_metadata.album       = ct.album;
    item->m_metadata.genre       = ct.genre;
    item->m_metadata.year        = ct.year;
    item->m_metadata.trackNumber = ct.trackNumber;
    item->m_metadata.cueStartMs  = ct.startMs;
    item->m_metadata.cueEndMs    = ct.endMs;
    item->m_metadata.cueFilePath = ct.cueFilePath;
    item->m_metadata.isCueTrack  = true;
    item->m_metadata.duration    = (ct.endMs >= 0) ? (ct.endMs - ct.startMs) : 0;
    item->m_metadata.coverArt    = sharedCover.isNull() ? defaultCoverImage() : sharedCover;
    item->m_metadataLoaded       = true;

    return item;
}