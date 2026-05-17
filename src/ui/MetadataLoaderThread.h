#ifndef METADATALOADERTHREAD_H
#define METADATALOADERTHREAD_H

#include <QThread>
#include <QMutex>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QWaitCondition>

class MetadataLoaderThread : public QThread
{
    Q_OBJECT

public:
    explicit MetadataLoaderThread(QObject *parent = nullptr);
    ~MetadataLoaderThread();

    void queueTrack(const QString &path);
    void queueRange(const QStringList &paths);
    void queuePriorityRange(const QStringList &paths);
    void queuePathRange(const QStringList &paths);
    void clearTrackQueue();
    void clearQueue();
    void stop();
    void setPriority(QThread::Priority priority);

signals:
    void metadataLoaded(const QString &filePath);

protected:
    void run() override;

private:
    void enqueueRangeLocked(const QStringList &paths, bool prioritize);
    void enqueuePathRangeLocked(const QStringList &paths);
    QString normalizePathKey(const QString &path) const;

    QMutex m_mutex;
    QStringList m_queue;
    QSet<QString> m_queuedSet;
    QStringList m_pathQueue;
    QSet<QString> m_queuedPathSet;
    QSet<QString> m_warmedPathSet;
    QWaitCondition m_condition;
    bool m_stop;
    bool m_running;
};

#endif // METADATALOADERTHREAD_H
