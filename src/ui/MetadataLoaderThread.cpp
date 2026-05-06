#include "MetadataLoaderThread.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>

MetadataLoaderThread::MetadataLoaderThread(QObject *parent)
    : QThread(parent)
    , m_stop(false)
    , m_running(true)
{
}

MetadataLoaderThread::~MetadataLoaderThread()
{
    stop();
    wait();
}

QString MetadataLoaderThread::normalizePathKey(const QString &path) const
{
    if (path.isEmpty())
        return {};

#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(QDir::cleanPath(path)).toLower();
#else
    return QDir::cleanPath(path);
#endif
}

void MetadataLoaderThread::queueTrack(TrackItem *track)
{
    if (!track || track->isMetadataLoaded())
        return;

    QMutexLocker locker(&m_mutex);
    if (m_queuedSet.contains(track))
        return;

    m_queue.append(track);
    m_queuedSet.insert(track);
    m_condition.wakeOne();
}

void MetadataLoaderThread::queueRange(const QList<TrackItem*> &tracks)
{
    QMutexLocker locker(&m_mutex);
    enqueueRangeLocked(tracks, false);
}

void MetadataLoaderThread::queuePriorityRange(const QList<TrackItem*> &tracks)
{
    QMutexLocker locker(&m_mutex);
    enqueueRangeLocked(tracks, true);
}

void MetadataLoaderThread::queuePathRange(const QStringList &paths)
{
    QMutexLocker locker(&m_mutex);
    enqueuePathRangeLocked(paths);
}

void MetadataLoaderThread::enqueueRangeLocked(const QList<TrackItem*> &tracks, bool prioritize)
{
    if (tracks.isEmpty())
        return;

    bool added = false;
    if (prioritize) {
        for (int i = tracks.size() - 1; i >= 0; --i) {
            TrackItem *track = tracks[i];
            if (!track || track->isMetadataLoaded() || m_queuedSet.contains(track))
                continue;

            m_queue.prepend(track);
            m_queuedSet.insert(track);
            added = true;
        }
    } else {
        for (TrackItem *track : tracks) {
            if (!track || track->isMetadataLoaded() || m_queuedSet.contains(track))
                continue;

            m_queue.append(track);
            m_queuedSet.insert(track);
            added = true;
        }
    }

    if (added)
        m_condition.wakeOne();
}

void MetadataLoaderThread::enqueuePathRangeLocked(const QStringList &paths)
{
    if (paths.isEmpty())
        return;

    bool added = false;
    for (const QString &path : paths) {
        QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile())
            continue;

        const QString absolutePath = fi.absoluteFilePath();
        const QString key = normalizePathKey(absolutePath);
        if (key.isEmpty()
            || m_queuedPathSet.contains(key)
            || m_warmedPathSet.contains(key)) {
            continue;
        }

        m_pathQueue.append(absolutePath);
        m_queuedPathSet.insert(key);
        added = true;
    }

    if (added)
        m_condition.wakeOne();
}

void MetadataLoaderThread::clearTrackQueue()
{
    QMutexLocker locker(&m_mutex);
    m_queue.clear();
    m_queuedSet.clear();
}

void MetadataLoaderThread::clearQueue()
{
    QMutexLocker locker(&m_mutex);
    m_queue.clear();
    m_queuedSet.clear();
    m_pathQueue.clear();
    m_queuedPathSet.clear();
}

void MetadataLoaderThread::stop()
{
    {
        QMutexLocker locker(&m_mutex);
        m_stop = true;
        m_running = false;
    }
    m_condition.wakeAll();
}

void MetadataLoaderThread::setPriority(QThread::Priority priority)
{
    QThread::setPriority(priority);
}

void MetadataLoaderThread::run()
{
    setPriority(QThread::HighPriority);

    while (true) {
        QList<TrackItem*> tracksToProcess;
        QStringList pathsToProcess;

        {
            QMutexLocker locker(&m_mutex);
            while (m_queue.isEmpty() && m_pathQueue.isEmpty() && !m_stop)
                m_condition.wait(&m_mutex);

            if (m_stop) {
                m_running = false;
                break;
            }

            // Pull a larger chunk to reduce mutex churn and increase throughput.
            const int count = qMin(24, m_queue.size());
            for (int i = 0; i < count; ++i) {
                TrackItem *track = m_queue.takeFirst();
                m_queuedSet.remove(track);
                tracksToProcess.append(track);
            }

            const int pathCount = qMin(96, m_pathQueue.size());
            for (int i = 0; i < pathCount; ++i) {
                const QString path = m_pathQueue.takeFirst();
                m_queuedPathSet.remove(normalizePathKey(path));
                pathsToProcess.append(path);
            }
        }

        // Process queued track objects first so active playlist updates stay responsive.
        for (TrackItem *track : tracksToProcess) {
            if (!m_running)
                break;
            if (track) {
                track->ensureMetadataLoaded();
                const QString loadedPath = track->filePath();
                if (!loadedPath.isEmpty()) {
                    QMutexLocker locker(&m_mutex);
                    const QString key = normalizePathKey(loadedPath);
                    if (!key.isEmpty()) {
                        m_warmedPathSet.insert(key);
                        if (m_warmedPathSet.size() > 12000)
                            m_warmedPathSet.clear();
                    }
                }
                emit metadataLoaded(loadedPath);
            }
        }

        // Process background path warmup without forcing UI row updates.
        for (const QString &path : pathsToProcess) {
            if (!m_running)
                break;
            if (path.isEmpty())
                continue;

            TrackItem warmup(path, false);

            QMutexLocker locker(&m_mutex);
            const QString key = normalizePathKey(path);
            if (!key.isEmpty()) {
                m_warmedPathSet.insert(key);
                if (m_warmedPathSet.size() > 12000)
                    m_warmedPathSet.clear();
            }
        }
    }
}