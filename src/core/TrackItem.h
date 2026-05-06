#ifndef TRACKITEM_H
#define TRACKITEM_H

#include <QString>
#include <QPixmap>
#include <QFileInfo>
#include <QPainter>
#include <QMediaPlayer>
#include <QMediaMetaData>
#include "CueParser.h"

struct TrackMetadata {
    QString title;
    QString artist;
    QString album;
    QString year;
    QString genre;
    int trackNumber;
    qint64 duration;
    int bitrate;
    int sampleRate;
    QPixmap coverArt;
    QString filePath;
    qint64  cueStartMs  = -1;
    qint64  cueEndMs    = -1;
    bool    isCueTrack  = false;
    QString cueFilePath;

    TrackMetadata() : trackNumber(0), duration(0), bitrate(0), sampleRate(0) {}
};

class TrackItem
{
public:
    explicit TrackItem(const QString &filePath, bool deferMetadata = false);

    const TrackMetadata& metadata() const { return m_metadata; }
    TrackMetadata& metadata() { return m_metadata; }
    QString filePath() const { return m_metadata.filePath; }
    bool isMetadataLoaded() const { return m_metadataLoaded; }
    void ensureMetadataLoaded();

    void loadFullMetadata(const QMediaMetaData &metaData);

    static TrackItem *fromCueTrack(const CueTrack &ct, const QPixmap &sharedCover = {});
    static QPixmap    coverArtFromFile(const QString &filePath);

    static QPixmap extractCoverArt(const QString &filePath);
    static QString formatDuration(qint64 milliseconds);
    static void readFlacFileInfo(const QString &filePath, int &sampleRate, QString &year,
                                 qint64 &durationMs);
    static qint64 readMp3Duration(const QString &filePath);

private:
    void initializeBasicMetadata();
    void loadMetadata();
    TrackMetadata m_metadata;
    bool m_metadataLoaded = false;
};

#endif // TRACKITEM_H