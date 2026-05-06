#ifndef AUDIODECODERBACKEND_H
#define AUDIODECODERBACKEND_H

#include <QObject>
#include <QAudioBuffer>

class AudioDecoderBackend : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Stopped,
        Playing,
        Paused
    };
    Q_ENUM(State)

    explicit AudioDecoderBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~AudioDecoderBackend() override = default;

    virtual bool openSource(const QString &filePath) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(qint64 positionMs) = 0;
    virtual qint64 position() const = 0;
    virtual qint64 duration() const = 0;
    virtual State state() const = 0;
    virtual void setPlaybackRate(float rate) = 0;
    virtual float playbackRate() const = 0;

signals:
    void audioBufferReceived(const QAudioBuffer &buffer);
    void stateChanged(AudioDecoderBackend::State state);
    void endOfStream();
    void durationChanged(qint64 durationMs);
};

#endif // AUDIODECODERBACKEND_H