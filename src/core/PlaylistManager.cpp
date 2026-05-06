#include "PlaylistManager.h"
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QDebug>
#include <QTextStream>
#include <QUrl>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#endif

namespace {

QString normalizeM3uEntry(QString line);

#ifdef Q_OS_WIN
QString decodeWithCodePage(const QByteArray &bytes, UINT codePage)
{
    if (bytes.isEmpty())
        return {};

    const int wcharCount = MultiByteToWideChar(codePage,
                                               0,
                                               bytes.constData(),
                                               bytes.size(),
                                               nullptr,
                                               0);
    if (wcharCount <= 0)
        return {};

    std::wstring wideText(static_cast<size_t>(wcharCount), L'\0');
    const int converted = MultiByteToWideChar(codePage,
                                              0,
                                              bytes.constData(),
                                              bytes.size(),
                                              wideText.data(),
                                              wcharCount);
    if (converted <= 0)
        return {};

    return QString::fromWCharArray(wideText.data(), converted);
}

QByteArray encodeWithCodePage(const QString &text, UINT codePage)
{
    if (text.isEmpty())
        return {};

    const std::wstring wideText = text.toStdWString();
    const int byteCount = WideCharToMultiByte(codePage,
                                              0,
                                              wideText.c_str(),
                                              static_cast<int>(wideText.size()),
                                              nullptr,
                                              0,
                                              nullptr,
                                              nullptr);
    if (byteCount <= 0)
        return {};

    QByteArray out(byteCount, Qt::Uninitialized);
    const int converted = WideCharToMultiByte(codePage,
                                              0,
                                              wideText.c_str(),
                                              static_cast<int>(wideText.size()),
                                              out.data(),
                                              byteCount,
                                              nullptr,
                                              nullptr);
    if (converted <= 0)
        return {};
    if (converted != out.size())
        out.truncate(converted);

    return out;
}
#endif

QString absoluteExistingPath(const QString &path)
{
    if (path.isEmpty())
        return {};

    QFileInfo fi(path);
    if (!fi.exists())
        return {};

    return fi.absoluteFilePath();
}

QString repairStoredTrackPath(const QString &storedPath)
{
    if (storedPath.isEmpty())
        return {};

    if (storedPath.startsWith(QLatin1String("CUE|")))
        return storedPath;

    QStringList candidates;
    const auto addCandidate = [&candidates](QString value) {
        value = value.trimmed();
        if (value.isEmpty())
            return;
        if (!candidates.contains(value))
            candidates.append(value);
    };

    addCandidate(storedPath);
    addCandidate(normalizeM3uEntry(storedPath));

    const QString percentDecoded = QUrl::fromPercentEncoding(storedPath.toUtf8());
    if (percentDecoded != storedPath) {
        addCandidate(percentDecoded);
        addCandidate(normalizeM3uEntry(percentDecoded));
    }

#ifdef Q_OS_WIN
    const QByteArray cp1251Bytes = encodeWithCodePage(storedPath, 1251);
    if (!cp1251Bytes.isEmpty()) {
        const QString utf8FromCp1251 = QString::fromUtf8(cp1251Bytes.constData(), cp1251Bytes.size());
        addCandidate(utf8FromCp1251);
        addCandidate(normalizeM3uEntry(utf8FromCp1251));

        const QString cp1251PercentDecoded = QUrl::fromPercentEncoding(utf8FromCp1251.toUtf8());
        if (cp1251PercentDecoded != utf8FromCp1251) {
            addCandidate(cp1251PercentDecoded);
            addCandidate(normalizeM3uEntry(cp1251PercentDecoded));
        }
    }
#endif

    for (const QString &candidate : std::as_const(candidates)) {
        const QString resolved = absoluteExistingPath(candidate);
        if (!resolved.isEmpty())
            return resolved;
    }

    // Keep unresolved path in a cleaned form; existing logic will decide whether to keep/drop it.
    return normalizeM3uEntry(storedPath).trimmed();
}

QString decodeUtf16WithBom(const QByteArray &bytes)
{
    if (bytes.size() < 2)
        return {};

    const uchar b0 = static_cast<uchar>(bytes[0]);
    const uchar b1 = static_cast<uchar>(bytes[1]);
    if (b0 == 0xFF && b1 == 0xFE) {
        const char16_t *ptr = reinterpret_cast<const char16_t *>(bytes.constData() + 2);
        const qsizetype len = (bytes.size() - 2) / 2;
        return QString::fromUtf16(ptr, len);
    }
    if (b0 == 0xFE && b1 == 0xFF) {
        QByteArray swapped = bytes.mid(2);
        for (int i = 0; i + 1 < swapped.size(); i += 2)
            std::swap(swapped[i], swapped[i + 1]);
        const char16_t *ptr = reinterpret_cast<const char16_t *>(swapped.constData());
        const qsizetype len = swapped.size() / 2;
        return QString::fromUtf16(ptr, len);
    }

    return {};
}

QString decodeUtf16WithoutBom(const QByteArray &bytes)
{
    if (bytes.size() < 4 || (bytes.size() % 2) != 0)
        return {};

    int evenZeros = 0;
    int oddZeros = 0;
    for (int i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\0') {
            if ((i % 2) == 0)
                ++evenZeros;
            else
                ++oddZeros;
        }
    }

    const bool likelyLe = oddZeros > evenZeros * 2;
    const bool likelyBe = evenZeros > oddZeros * 2;
    if (!likelyLe && !likelyBe)
        return {};

    QByteArray data = bytes;
    if (likelyBe) {
        for (int i = 0; i + 1 < data.size(); i += 2)
            std::swap(data[i], data[i + 1]);
    }

    const char16_t *ptr = reinterpret_cast<const char16_t *>(data.constData());
    const qsizetype len = data.size() / 2;
    return QString::fromUtf16(ptr, len);
}

QString normalizeM3uEntry(QString line)
{
    line = line.trimmed();
    line.remove(QChar::ByteOrderMark);
    if (line.startsWith('"') && line.endsWith('"') && line.size() >= 2)
        line = line.mid(1, line.size() - 2).trimmed();
    if (line.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        const QUrl url(line);
        if (url.isValid() && url.isLocalFile())
            line = url.toLocalFile();
    }
    return line;
}

QStringList parseM3uContent(const QString &content, const QString &baseDir)
{
    QStringList result;
    const QStringList lines = content.split(QRegularExpression("\\r\\n|\\n|\\r"), Qt::SkipEmptyParts);

    for (QString line : lines) {
        line = normalizeM3uEntry(line);
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        QString filePath = line;
        QFileInfo fi(filePath);
        if (fi.isRelative())
            filePath = QDir(baseDir).absoluteFilePath(filePath);

        fi.setFile(filePath);
        if (fi.exists())
            result.append(fi.absoluteFilePath());
    }

    return result;
}

QStringList decodeCandidates(const QByteArray &raw)
{
    QStringList candidates;

    if (raw.startsWith("\xEF\xBB\xBF")) {
        candidates.append(QString::fromUtf8(raw.constData() + 3, raw.size() - 3));
        return candidates;
    }

    const QString utf16Bom = decodeUtf16WithBom(raw);
    if (!utf16Bom.isEmpty()) {
        candidates.append(utf16Bom);
        return candidates;
    }

    candidates.append(QString::fromUtf8(raw));
#ifdef Q_OS_WIN
    const QString cp1251Decoded = decodeWithCodePage(raw, 1251);
    if (!cp1251Decoded.isEmpty())
        candidates.append(cp1251Decoded);
#endif
    candidates.append(QString::fromLocal8Bit(raw));

    const QString utf16NoBom = decodeUtf16WithoutBom(raw);
    if (!utf16NoBom.isEmpty())
        candidates.prepend(utf16NoBom);

    candidates.removeDuplicates();
    return candidates;
}

} // namespace

PlaylistManager::PlaylistManager(QObject *parent)
    : QObject(parent)
{
    m_storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + "/playlists";
    QDir().mkpath(m_storageDir);
    load();
}

QString PlaylistManager::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString PlaylistManager::playlistFilePath(const QString &id) const
{
    return m_storageDir + "/" + id + ".json";
}

QString PlaylistManager::createPlaylist(const QString &name)
{
    PlaylistInfo pl;
    pl.id = generateId();
    pl.name = name;
    m_playlists.append(pl);
    savePlaylist(pl);
    emit playlistsChanged();
    return pl.id;
}

void PlaylistManager::renamePlaylist(const QString &id, const QString &newName)
{
    for (auto &pl : m_playlists) {
        if (pl.id == id) {
            pl.name = newName;
            savePlaylist(pl);
            emit playlistsChanged();
            return;
        }
    }
}

void PlaylistManager::deletePlaylist(const QString &id)
{
    for (int i = 0; i < m_playlists.count(); ++i) {
        if (m_playlists[i].id == id) {
            m_playlists.removeAt(i);
            deletePlaylistFile(id);
            emit playlistsChanged();
            return;
        }
    }
}

PlaylistInfo *PlaylistManager::playlist(const QString &id)
{
    for (auto &pl : m_playlists) {
        if (pl.id == id) return &pl;
    }
    return nullptr;
}

void PlaylistManager::setTracks(const QString &id, const QStringList &paths)
{
    for (auto &pl : m_playlists) {
        if (pl.id == id) {
            pl.trackPaths = paths;
            savePlaylist(pl);
            return;
        }
    }
}

void PlaylistManager::addTrack(const QString &id, const QString &path)
{
    for (auto &pl : m_playlists) {
        if (pl.id == id) {
            pl.trackPaths.append(path);
            savePlaylist(pl);
            return;
        }
    }
}

void PlaylistManager::removeTrack(const QString &id, int index)
{
    for (auto &pl : m_playlists) {
        if (pl.id == id && index >= 0 && index < pl.trackPaths.count()) {
            pl.trackPaths.removeAt(index);
            savePlaylist(pl);
            return;
        }
    }
}

void PlaylistManager::setAutoSourceDir(const QString &id, const QString &dirPath)
{
    for (auto &pl : m_playlists) {
        if (pl.id == id) {
            pl.autoSourceDir = dirPath;
            savePlaylist(pl);
            emit playlistsChanged();
            return;
        }
    }
}

QStringList PlaylistManager::importM3U(const QString &m3uPath)
{
    QStringList paths;
    QFile file(m3uPath);
    if (!file.open(QIODevice::ReadOnly))
        return paths;

    const QByteArray raw = file.readAll();
    const QString baseDir = QFileInfo(m3uPath).absolutePath();

    QStringList best;
    for (const QString &decoded : decodeCandidates(raw)) {
        const QStringList parsed = parseM3uContent(decoded, baseDir);
        if (parsed.size() > best.size())
            best = parsed;
        if (!best.isEmpty() && best.size() >= 4)
            break;
    }

    return best;
}

void PlaylistManager::exportM3U(const QString &id, const QString &m3uPath)
{
    PlaylistInfo *pl = playlist(id);
    if (!pl) return;

    QFile file(m3uPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QTextStream out(&file);
    out << "#EXTM3U\n";
    for (const QString &path : pl->trackPaths)
        out << path << "\n";
}

void PlaylistManager::savePlaylist(const PlaylistInfo &pl)
{
    QJsonObject obj;
    obj["id"] = pl.id;
    obj["name"] = pl.name;
    obj["autoSourceDir"] = pl.autoSourceDir;
    QJsonArray arr;
    for (const QString &p : pl.trackPaths)
        arr.append(p);
    obj["tracks"] = arr;

    QFile file(playlistFilePath(pl.id));
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void PlaylistManager::deletePlaylistFile(const QString &id)
{
    QFile::remove(playlistFilePath(id));
}

void PlaylistManager::save()
{
    for (const auto &pl : m_playlists)
        savePlaylist(pl);
}

void PlaylistManager::load()
{
    m_playlists.clear();
    QDir dir(m_storageDir);
    QStringList files = dir.entryList({"*.json"}, QDir::Files, QDir::Name);
    for (const QString &f : files) {
        QFile file(dir.filePath(f));
        if (!file.open(QIODevice::ReadOnly)) continue;

        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) continue;
        QJsonObject obj = doc.object();

        PlaylistInfo pl;
        pl.id = obj["id"].toString();
        pl.name = obj["name"].toString();
        pl.autoSourceDir = obj["autoSourceDir"].toString();
        QJsonArray arr = obj["tracks"].toArray();
        bool repairedTrackPaths = false;
        for (const auto &v : arr) {
            const QString storedPath = v.toString();
            const QString repairedPath = repairStoredTrackPath(storedPath);
            if (!repairedPath.isEmpty()) {
                pl.trackPaths.append(repairedPath);
                if (repairedPath != storedPath)
                    repairedTrackPaths = true;
            } else if (!storedPath.isEmpty()) {
                pl.trackPaths.append(storedPath);
            }
        }

        if (!pl.id.isEmpty()) {
            m_playlists.append(pl);
            if (repairedTrackPaths)
                savePlaylist(pl);
        }
    }
}
