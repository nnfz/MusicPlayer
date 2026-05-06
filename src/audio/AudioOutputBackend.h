#ifndef AUDIOOUTPUTBACKEND_H
#define AUDIOOUTPUTBACKEND_H

#include <QAudioFormat>
#include <QPair>
#include <QString>
#include <QVector>

class AudioOutputBackend
{
public:
    virtual ~AudioOutputBackend() = default;

    virtual bool open(const QAudioFormat &format, int bufferBytes) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual void setVolume(float volume) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual int bytesFree() const = 0;
    virtual qint64 bytesQueued() const { return 0; }
    virtual qint64 write(const char *data, qint64 len) = 0;
    virtual bool resetStream() = 0;

    virtual bool supportsDeviceSelection() const { return false; }
    virtual QVector<QPair<QString, QString>> availableOutputDevices() const { return {}; }
    virtual void setPreferredDeviceId(const QString &) {}
    virtual QString preferredDeviceId() const { return {}; }
    virtual QString activeDeviceId() const { return {}; }
    virtual QString activeDeviceName() const { return QStringLiteral("System Default"); }
};

#endif // AUDIOOUTPUTBACKEND_H