#ifndef WASAPIAUDIOOUTPUTBACKEND_H
#define WASAPIAUDIOOUTPUTBACKEND_H

#include "AudioOutputBackend.h"

#ifdef Q_OS_WIN

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <QPair>
#include <QString>
#include <QVector>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

class WasapiAudioOutputBackend : public AudioOutputBackend
{
public:
    enum class Mode { Shared, Exclusive };

    explicit WasapiAudioOutputBackend(Mode mode = Mode::Shared);
    ~WasapiAudioOutputBackend() override;

    bool open(const QAudioFormat &format, int bufferBytes) override;
    void close() override;
    bool isOpen() const override;

    void setVolume(float volume) override;
    void suspend() override;
    void resume() override;
    int bytesFree() const override;
    qint64 bytesQueued() const override;
    qint64 write(const char *data, qint64 len) override;
    bool resetStream() override;

    bool supportsDeviceSelection() const override { return true; }
    QVector<QPair<QString, QString>> availableOutputDevices() const override;
    void setPreferredDeviceId(const QString &deviceId) override;
    QString preferredDeviceId() const override;
    QString activeDeviceId() const override;
    QString activeDeviceName() const override;

private:
    static REFERENCE_TIME bytesToReferenceTime(int bytes, const QAudioFormat &format);
    bool buildWaveFormat(const QAudioFormat &format);
    void renderLoop();
    size_t ringWriteLocked(const char *data, size_t len);
    size_t ringReadLocked(char *dst, size_t len);
    void clearRingLocked();
    void releaseWasapi();
    static QString queryDeviceName(IMMDevice *device);
    static QString queryDeviceId(IMMDevice *device);
    static QVector<QPair<QString, QString>> enumerateRenderDevices();

    QAudioFormat m_format;
    Mode m_mode = Mode::Shared;
    std::atomic<float> m_volume { 1.0f };

    QString m_preferredDeviceId;
    QString m_activeDeviceId;
    QString m_activeDeviceName = QStringLiteral("System Default");
    mutable QVector<QPair<QString, QString>> m_cachedDevices;
    mutable bool m_cachedDevicesValid = false;
    mutable std::chrono::steady_clock::time_point m_cachedDevicesUpdatedAt {};

    mutable std::mutex m_wasapiMutex;
    mutable std::mutex m_mutex;
    std::vector<char> m_ring;
    size_t m_ringReadPos = 0;
    size_t m_ringWritePos = 0;
    size_t m_ringSize = 0;

    IMMDeviceEnumerator *m_deviceEnumerator = nullptr;
    IMMDevice *m_device = nullptr;
    IAudioClient *m_audioClient = nullptr;
    IAudioRenderClient *m_renderClient = nullptr;
    WAVEFORMATEX *m_waveFormat = nullptr;
    UINT32 m_bufferFrameCount = 0;
    int m_frameBytes = 0;

    HANDLE m_eventHandle = nullptr;
    HANDLE m_shutdownEvent = nullptr;

    std::thread m_renderThread;
    std::atomic<bool> m_open { false };
    std::atomic<bool> m_paused { false };

    bool m_comInitialized = false;
};

#endif // Q_OS_WIN

#endif // WASAPIAUDIOOUTPUTBACKEND_H