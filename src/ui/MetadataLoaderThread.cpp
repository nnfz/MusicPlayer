#include "MetadataLoaderThread.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include "TrackItem.h"

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

void MetadataLoaderThread::queueTrack(const QString &path)
{
    if (path.isEmpty())
        return;

    const QString key = normalizePathKey(path);
    QMutexLocker locker(&m_mutex);
    if (m_queuedSet.contains(key))
        return;

    m_queue.append(path);
    m_queuedSet.insert(key);
    m_condition.wakeOne();
}

void MetadataLoaderThread::queueRange(const QStringList &paths)
{
    QMutexLocker locker(&m_mutex);
    enqueueRangeLocked(paths, false);
}

void MetadataLoaderThread::queuePriorityRange(const QStringList &paths)
{
    QMutexLocker locker(&m_mutex);
    enqueueRangeLocked(paths, true);
}

void MetadataLoaderThread::queuePathRange(const QStringList &paths)
{
    QMutexLocker locker(&m_mutex);
    enqueuePathRangeLocked(paths);
}

void MetadataLoaderThread::enqueueRangeLocked(const QStringList &paths, bool prioritize)
{
    if (paths.isEmpty())
        return;

    bool added = false;
    if (prioritize) {
        for (int i = paths.size() - 1; i >= 0; --i) {
            const QString &path = paths[i];
            if (path.isEmpty())
                continue;
            
            const QString key = normalizePathKey(path);
            if (m_queuedSet.contains(key))
                continue;

            m_queue.prepend(path);
            m_queuedSet.insert(key);
            added = true;
        }
    } else {
        for (const QString &path : paths) {
            if (path.isEmpty())
                continue;
            
            const QString key = normalizePathKey(path);
            if (m_queuedSet.contains(key))
                continue;

            m_queue.append(path);
            m_queuedSet.insert(key);
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
        QStringList pathsToProcess;
        QStringList backgroundPathsToProcess;

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
                const QString path = m_queue.takeFirst();
                m_queuedSet.remove(normalizePathKey(path));
                pathsToProcess.append(path);
            }

            const int pathCount = qMin(96, m_pathQueue.size());
            for (int i = 0; i < pathCount; ++i) {
                const QString path = m_pathQueue.takeFirst();
                m_queuedPathSet.remove(normalizePathKey(path));
                backgroundPathsToProcess.append(path);
            }
        }

        // Process queued track paths first so active playlist updates stay responsive.
        for (const QString &path : pathsToProcess) {
            if (!m_running)
                break;
            if (!path.isEmpty()) {
                TrackItem track(path, false);
                track.ensureMetadataLoaded();
                QMutexLocker locker(&m_mutex);
                const QString key = normalizePathKey(path);
                if (!key.isEmpty()) {
                    m_warmedPathSet.insert(key);
                    if (m_warmedPathSet.size() > 12000)
                        m_warmedPathSet.clear();
                }
                locker.unlock(); // Unlock before emitting signal
                emit metadataLoaded(path);
            }
        }

        // Process background path warmup without forcing UI row updates.
        for (const QString &path : backgroundPathsToProcess) {
            if (!m_running)
                break;
            if (path.isEmpty())
                continue;

            TrackItem warmup(path, false);
            warmup.ensureMetadataLoaded();

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