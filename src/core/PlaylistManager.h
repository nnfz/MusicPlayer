#ifndef PLAYLISTMANAGER_H
#define PLAYLISTMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QDir>

struct PlaylistInfo {
    QString id;
    QString name;
    QStringList trackPaths;
    QString autoSourceDir;
};

class PlaylistManager : public QObject
{
    Q_OBJECT
public:
    explicit PlaylistManager(QObject *parent = nullptr);

    QList<PlaylistInfo> playlists() const { return m_playlists; }
    int count() const { return m_playlists.count(); }

    QString createPlaylist(const QString &name);
    void renamePlaylist(const QString &id, const QString &newName);
    void deletePlaylist(const QString &id);

    PlaylistInfo *playlist(const QString &id);
    void setTracks(const QString &id, const QStringList &paths);
    void addTrack(const QString &id, const QString &path);
    void removeTrack(const QString &id, int index);
    void setAutoSourceDir(const QString &id, const QString &dirPath);

    QStringList importM3U(const QString &m3uPath);
    void exportM3U(const QString &id, const QString &m3uPath);

    void save();
    void load();

    QString storageDir() const { return m_storageDir; }

signals:
    void playlistsChanged();

private:
    QString generateId() const;
    QString playlistFilePath(const QString &id) const;
    void savePlaylist(const PlaylistInfo &pl);
    void deletePlaylistFile(const QString &id);

    QList<PlaylistInfo> m_playlists;
    QString m_storageDir;
};

#endif // PLAYLISTMANAGER_H
