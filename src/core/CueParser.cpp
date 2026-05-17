#include "CueParser.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>

namespace {

qint64 cueTimestampToMs(const QString &ts)
{
    static QRegularExpression re(QStringLiteral("(\\d+):(\\d+):(\\d+)"));
    const auto m = re.match(ts.trimmed());
    if (!m.hasMatch())
        return 0;
    const int minutes = m.captured(1).toInt();
    const int seconds = m.captured(2).toInt();
    const int frames  = m.captured(3).toInt();
    return static_cast<qint64>(minutes) * 60000LL
         + static_cast<qint64>(seconds) * 1000LL
         + (static_cast<qint64>(frames) * 1000LL + 37LL) / 75LL;
}

QString unquote(const QString &s)
{
    QString t = s.trimmed();
    if (t.size() >= 2 && t.startsWith('"') && t.endsWith('"'))
        return t.mid(1, t.size() - 2);
    return t;
}

QString decodeBytes(const QByteArray &raw)
{
    if (raw.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(raw.constData() + 3, raw.size() - 3);

    if (raw.size() >= 2) {
        const auto b0 = static_cast<uchar>(raw[0]);
        const auto b1 = static_cast<uchar>(raw[1]);
        if (b0 == 0xFF && b1 == 0xFE)
            return QString::fromUtf16(
                reinterpret_cast<const char16_t *>(raw.constData() + 2),
                (raw.size() - 2) / 2);
        if (b0 == 0xFE && b1 == 0xFF) {
            QByteArray sw = raw.mid(2);
            for (int i = 0; i + 1 < sw.size(); i += 2)
                std::swap(sw[i], sw[i + 1]);
            return QString::fromUtf16(
                reinterpret_cast<const char16_t *>(sw.constData()),
                sw.size() / 2);
        }
    }

    const QString asUtf8 = QString::fromUtf8(raw);
    if (!asUtf8.contains(QChar::ReplacementCharacter))
        return asUtf8;

    const QString asLocal = QString::fromLocal8Bit(raw);
    if (!asLocal.isEmpty())
        return asLocal;

    return asUtf8;
}

QString resolveAudioFile(const QString &cueDir, const QString &entry)
{
    QFileInfo fi(entry);
    if (fi.isRelative())
        fi.setFile(QDir(cueDir).absoluteFilePath(entry));
    if (fi.exists())
        return fi.absoluteFilePath();

    const QString base = QFileInfo(entry).completeBaseName();
    static const QStringList kExts = {
        QStringLiteral("flac"), QStringLiteral("wav"),  QStringLiteral("ape"),
        QStringLiteral("mp3"),  QStringLiteral("ogg"),  QStringLiteral("m4a"),
        QStringLiteral("aac"),  QStringLiteral("wv"),   QStringLiteral("wma"),
        QStringLiteral("opus"), QStringLiteral("tak"),  QStringLiteral("tta"),
    };
    for (const QString &ext : kExts) {
        QFileInfo c(QDir(cueDir).absoluteFilePath(base + '.' + ext));
        if (c.exists())
            return c.absoluteFilePath();
    }
    return fi.absoluteFilePath();
}

} // namespace

QList<CueTrack> CueParser::parse(const QString &cuePath)
{
    QFile file(cuePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    const QString content = decodeBytes(file.readAll());
    const QString cueDir  = QFileInfo(cuePath).absolutePath();
    const QString absPath = QFileInfo(cuePath).absoluteFilePath();

    static QRegularExpression reFile(
        QStringLiteral("^\\s*FILE\\s+(.+?)\\s*(?:WAVE|MP3|AIFF|BINARY|MOTOROLA|FLAC|OGG|APE|WV|WMA|M4A|AAC|OPUS|TAK|TTA)?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression reTrack(
        QStringLiteral("^\\s*TRACK\\s+(\\d+)\\s+AUDIO"),
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression reIndex(
        QStringLiteral("^\\s*INDEX\\s+(\\d+)\\s+([\\d:]+)"));
    static QRegularExpression reTitle(
        QStringLiteral("^\\s*TITLE\\s+(.+)$"));
    static QRegularExpression rePerformer(
        QStringLiteral("^\\s*PERFORMER\\s+(.+)$"));
    static QRegularExpression reGenre(
        QStringLiteral("^\\s*REM\\s+GENRE\\s+(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression reDate(
        QStringLiteral("^\\s*REM\\s+DATE\\s+(\\S+)"),
        QRegularExpression::CaseInsensitiveOption);

    QList<CueTrack> tracks;

    QString globalPerformer;
    QString globalTitle;
    QString globalGenre;
    QString globalYear;
    QString currentAudioPath;

    CueTrack current;
    bool inTrack = false;

    auto finishTrack = [&]() {
        if (inTrack && current.trackNumber > 0
                && !current.audioFilePath.isEmpty()
                && current.startMs >= 0) {
            if (current.performer.isEmpty()) current.performer = globalPerformer;
            if (current.album.isEmpty())     current.album     = globalTitle;
            if (current.genre.isEmpty())     current.genre     = globalGenre;
            if (current.year.isEmpty())      current.year      = globalYear;
            current.cueFilePath = absPath;
            tracks.append(current);
        }
        current = CueTrack();
        current.audioFilePath = currentAudioPath;
        inTrack = false;
    };

    static QRegularExpression reSplit(QStringLiteral("\\r\\n|\\n|\\r"));
    for (const QString &rawLine : content.split(reSplit)) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        if (const auto mf = reFile.match(line); mf.hasMatch()) {
            finishTrack();
            currentAudioPath = resolveAudioFile(cueDir, unquote(mf.captured(1)));
            current.audioFilePath = currentAudioPath;
            continue;
        }

        if (const auto mt = reTrack.match(line); mt.hasMatch()) {
            finishTrack();
            inTrack = true;
            current.audioFilePath = currentAudioPath;
            current.trackNumber   = mt.captured(1).toInt();
            continue;
        }

        if (const auto m = reTitle.match(line); m.hasMatch()) {
            const QString val = unquote(m.captured(1));
            if (inTrack) current.title = val;
            else         globalTitle   = val;
            continue;
        }

        if (const auto m = rePerformer.match(line); m.hasMatch()) {
            const QString val = unquote(m.captured(1));
            if (inTrack) current.performer  = val;
            else         globalPerformer    = val;
            continue;
        }

        if (const auto m = reGenre.match(line); m.hasMatch()) {
            const QString val = unquote(m.captured(1));
            if (inTrack) current.genre = val;
            else         globalGenre   = val;
            continue;
        }

        if (const auto m = reDate.match(line); m.hasMatch()) {
            const QString val = unquote(m.captured(1));
            if (inTrack) current.year = val;
            else         globalYear   = val;
            continue;
        }

        if (const auto m = reIndex.match(line); m.hasMatch()) {
            if (m.captured(1).toInt() == 1)
                current.startMs = cueTimestampToMs(m.captured(2));
            continue;
        }
    }
    finishTrack();

    for (int i = 0; i < tracks.size(); ++i) {
        const bool sameFile = (i + 1 < tracks.size())
            && (tracks[i].audioFilePath == tracks[i + 1].audioFilePath);
        tracks[i].endMs = sameFile ? tracks[i + 1].startMs : -1;
    }

    return tracks;
}
