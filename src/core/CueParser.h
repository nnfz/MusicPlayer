#ifndef CUEPARSER_H
#define CUEPARSER_H

#include <QString>
#include <QList>

struct CueTrack {
    QString audioFilePath;
    QString title;
    QString performer;
    QString album;
    QString genre;
    QString year;
    QString cueFilePath;
    int trackNumber = 0;
    qint64 startMs  = 0;
    qint64 endMs    = -1;
};

class CueParser {
public:
    static QList<CueTrack> parse(const QString &cuePath);
};

#endif
