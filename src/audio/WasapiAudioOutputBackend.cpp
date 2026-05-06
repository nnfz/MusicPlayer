#include "WasapiAudioOutputBackend.h"

#ifdef Q_OS_WIN

#include <QDebug>

#include <algorithm>
#include <cstring>

#include <avrt.h>
#include <propvarutil.h>

namespace {

void safeRelease(IUnknown *&obj)
{
    if (obj) {
        obj->Release();
        obj = nullptr;
    }
}

QString toHexHr(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

bool isRecoverableDeviceLoss(HRESULT hr)
{
    return hr == AUDCLNT_E_DEVICE_INVALIDATED
        || hr == AUDCLNT_E_RESOURCES_INVALIDATED
        || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

quint64 nextResetTraceId()
{
    static quint64 s_resetTraceId = 0;
    return ++s_resetTraceId;
}

} // namespace

WasapiAudioOutputBackend::WasapiAudioOutputBackend(Mode mode)
    : m_mode(mode)
{
}

WasapiAudioOutputBackend::~WasapiAudioOutputBackend()
{
    close();
}

REFERENCE_TIME WasapiAudioOutputBackend::bytesToReferenceTime(int bytes, const QAudioFormat &format)
{
    if (!format.isValid() || format.bytesPerFrame() <= 0 || format.sampleRate() <= 0 || bytes <= 0)
        return 1000000; // 100 ms

    const qint64 frames = bytes / format.bytesPerFrame();
    if (frames <= 0)
        return 1000000;

    return static_cast<REFERENCE_TIME>((10000000LL * frames) / format.sampleRate());
}

bool WasapiAudioOutputBackend::buildWaveFormat(const QAudioFormat &format)
{
    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }

    if (format.channelCount() <= 0 || format.sampleRate() <= 0)
        return false;

    if (format.sampleFormat() != QAudioFormat::Float && format.sampleFormat() != QAudioFormat::Int16)
        return false;

    const WORD bits = static_cast<WORD>(format.bytesPerSample() * 8);
    const WORD blockAlign = static_cast<WORD>(format.channelCount() * format.bytesPerSample());

    auto *wf = static_cast<WAVEFORMATEX *>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wf)
        return false;

    std::memset(wf, 0, sizeof(WAVEFORMATEX));
    wf->wFormatTag = (format.sampleFormat() == QAudioFormat::Float) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wf->nChannels = static_cast<WORD>(format.channelCount());
    wf->nSamplesPerSec = static_cast<DWORD>(format.sampleRate());
    wf->wBitsPerSample = bits;
    wf->nBlockAlign = blockAlign;
    wf->nAvgBytesPerSec = static_cast<DWORD>(format.sampleRate()) * blockAlign;
    wf->cbSize = 0;

    m_waveFormat = wf;
    m_frameBytes = blockAlign;
    return true;
}

bool WasapiAudioOutputBackend::open(const QAudioFormat &format, int bufferBytes)
{
    close();

    if (!format.isValid())
        return false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        m_comInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        qWarning() << "WASAPI: CoInitializeEx failed" << toHexHr(hr);
        return false;
    }

    m_format = format;
    if (!buildWaveFormat(format)) {
        qWarning() << "WASAPI: unsupported QAudioFormat";
        close();
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&m_deviceEnumerator));
    if (FAILED(hr)) {
        qWarning() << "WASAPI: CoCreateInstance(MMDeviceEnumerator) failed" << toHexHr(hr);
        close();
        return false;
    }

    QString preferredId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        preferredId = m_preferredDeviceId;
    }

    if (!preferredId.isEmpty()) {
        hr = m_deviceEnumerator->GetDevice(reinterpret_cast<LPCWSTR>(preferredId.utf16()), &m_device);
        if (FAILED(hr)) {
            qWarning() << "WASAPI: preferred device not available, using default. id="
                       << preferredId << "hr=" << toHexHr(hr);
            m_device = nullptr;
        }
    }

    if (!m_device) {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        if (FAILED(hr)) {
            qWarning() << "WASAPI: GetDefaultAudioEndpoint failed" << toHexHr(hr);
            close();
            return false;
        }
    }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void **>(&m_audioClient));
    if (FAILED(hr)) {
        qWarning() << "WASAPI: Activate(IAudioClient) failed" << toHexHr(hr);
        close();
        return false;
    }

    AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_SHARED;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    REFERENCE_TIME duration = bytesToReferenceTime(bufferBytes, format);
    REFERENCE_TIME periodicity = 0;

    if (m_mode == Mode::Exclusive) {
        shareMode = AUDCLNT_SHAREMODE_EXCLUSIVE;
        flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minPeriod = 0;
        hr = m_audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
        if (FAILED(hr)) {
            qWarning() << "WASAPI: GetDevicePeriod failed" << toHexHr(hr);
            close();
            return false;
        }

        duration = (minPeriod > 0) ? minPeriod : defaultPeriod;
        if (duration <= 0)
            duration = 100000; // 10 ms
        periodicity = duration;

        WAVEFORMATEX *closest = nullptr;
        hr = m_audioClient->IsFormatSupported(shareMode, m_waveFormat, &closest);
        if (closest)
            CoTaskMemFree(closest);
        if (hr != S_OK) {
            qWarning() << "WASAPI: exclusive mode does not support current format" << format;
            close();
            return false;
        }
    }

    hr = m_audioClient->Initialize(shareMode, flags,
                                   duration, periodicity, m_waveFormat, nullptr);
    if (FAILED(hr)) {
        qWarning() << "WASAPI: IAudioClient::Initialize failed" << toHexHr(hr);
        close();
        return false;
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr) || m_bufferFrameCount == 0) {
        qWarning() << "WASAPI: GetBufferSize failed" << toHexHr(hr);
        close();
        return false;
    }

    m_eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_eventHandle || !m_shutdownEvent) {
        qWarning() << "WASAPI: CreateEvent failed";
        close();
        return false;
    }

    hr = m_audioClient->SetEventHandle(m_eventHandle);
    if (FAILED(hr)) {
        qWarning() << "WASAPI: SetEventHandle failed" << toHexHr(hr);
        close();
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient),
                                   reinterpret_cast<void **>(&m_renderClient));
    if (FAILED(hr)) {
        qWarning() << "WASAPI: GetService(IAudioRenderClient) failed" << toHexHr(hr);
        close();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeDeviceId = queryDeviceId(m_device);
        const QString friendly = queryDeviceName(m_device);
        m_activeDeviceName = friendly.isEmpty() ? QStringLiteral("System Default") : friendly;
    }

    {
        BYTE *initialData = nullptr;
        hr = m_renderClient->GetBuffer(m_bufferFrameCount, &initialData);
        if (SUCCEEDED(hr))
            m_renderClient->ReleaseBuffer(m_bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    const size_t deviceBytes = static_cast<size_t>(m_bufferFrameCount) * static_cast<size_t>(m_frameBytes);
    const size_t requestedBytes = bufferBytes > 0 ? static_cast<size_t>(bufferBytes) : deviceBytes;
    size_t ringBytes = std::max(deviceBytes * 6, requestedBytes * 6);
    const size_t frameAlign = static_cast<size_t>(qMax(1, m_frameBytes));
    ringBytes -= (ringBytes % frameAlign);
    if (ringBytes == 0)
        ringBytes = frameAlign;

    const auto detectedDevices = enumerateRenderDevices();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ring.assign(ringBytes, 0);
        m_ringReadPos = 0;
        m_ringWritePos = 0;
        m_ringSize = 0;
        m_cachedDevices = detectedDevices;
        m_cachedDevicesValid = true;
        m_cachedDevicesUpdatedAt = std::chrono::steady_clock::now();
    }

    m_paused.store(false);
    m_open.store(true);

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        qWarning() << "WASAPI: Start failed" << QString::number(hr, 16);
        close();
        return false;
    }

    m_renderThread = std::thread(&WasapiAudioOutputBackend::renderLoop, this);
    return true;
}

void WasapiAudioOutputBackend::close()
{
    const bool wasOpen = m_open.exchange(false);

    if (m_shutdownEvent)
        SetEvent(m_shutdownEvent);
    if (m_eventHandle)
        SetEvent(m_eventHandle);

    if (m_renderThread.joinable())
        m_renderThread.join();

    if (m_audioClient)
        m_audioClient->Stop();

    releaseWasapi();

    if (wasOpen) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ring.clear();
        m_ringReadPos = 0;
        m_ringWritePos = 0;
        m_ringSize = 0;
    }

    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

void WasapiAudioOutputBackend::releaseWasapi()
{
    IUnknown *unk = reinterpret_cast<IUnknown *>(m_renderClient);
    safeRelease(unk);
    m_renderClient = nullptr;

    unk = reinterpret_cast<IUnknown *>(m_audioClient);
    safeRelease(unk);
    m_audioClient = nullptr;

    unk = reinterpret_cast<IUnknown *>(m_device);
    safeRelease(unk);
    m_device = nullptr;

    unk = reinterpret_cast<IUnknown *>(m_deviceEnumerator);
    safeRelease(unk);
    m_deviceEnumerator = nullptr;

    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }

    if (m_eventHandle) {
        CloseHandle(m_eventHandle);
        m_eventHandle = nullptr;
    }
    if (m_shutdownEvent) {
        CloseHandle(m_shutdownEvent);
        m_shutdownEvent = nullptr;
    }

    m_bufferFrameCount = 0;
    m_frameBytes = 0;
}

bool WasapiAudioOutputBackend::isOpen() const
{
    return m_open.load();
}

void WasapiAudioOutputBackend::setVolume(float volume)
{
    m_volume.store(std::clamp(volume, 0.0f, 1.0f));
}

void WasapiAudioOutputBackend::suspend()
{
    m_paused.store(true);
    if (m_audioClient)
        m_audioClient->Stop();
}

void WasapiAudioOutputBackend::resume()
{
    m_paused.store(false);
    if (m_audioClient)
        m_audioClient->Start();
    if (m_eventHandle)
        SetEvent(m_eventHandle);
}

int WasapiAudioOutputBackend::bytesFree() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ring.empty() || m_ringSize >= m_ring.size())
        return 0;

    const int frameBytes = qMax(1, m_frameBytes);
    size_t freeBytes = m_ring.size() - m_ringSize;
    freeBytes -= (freeBytes % static_cast<size_t>(frameBytes));
    return static_cast<int>(freeBytes);
}

qint64 WasapiAudioOutputBackend::bytesQueued() const
{
    qint64 hwBytes = 0;
    {
        std::lock_guard<std::mutex> wasapiLock(m_wasapiMutex);
        if (m_audioClient) {
            UINT32 padding = 0;
            if (SUCCEEDED(m_audioClient->GetCurrentPadding(&padding)))
                hwBytes = static_cast<qint64>(padding) * m_frameBytes;
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<qint64>(m_ringSize) + hwBytes;
}

QVector<QPair<QString, QString>> WasapiAudioOutputBackend::availableOutputDevices() const
{
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cachedDevicesValid
            && (now - m_cachedDevicesUpdatedAt) < std::chrono::seconds(30)) {
            return m_cachedDevices;
        }
    }

    const auto devices = enumerateRenderDevices();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cachedDevices = devices;
        m_cachedDevicesValid = true;
        m_cachedDevicesUpdatedAt = now;
        return m_cachedDevices;
    }
}

void WasapiAudioOutputBackend::setPreferredDeviceId(const QString &deviceId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_preferredDeviceId = deviceId.trimmed();
}

QString WasapiAudioOutputBackend::preferredDeviceId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_preferredDeviceId;
}

QString WasapiAudioOutputBackend::activeDeviceId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeDeviceId;
}

QString WasapiAudioOutputBackend::activeDeviceName() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeDeviceName;
}

qint64 WasapiAudioOutputBackend::write(const char *data, qint64 len)
{
    if (!data || len <= 0 || !m_open.load())
        return 0;

    const qint64 frameBytes = qMax<qint64>(1, m_frameBytes);
    const qint64 alignedLen = len - (len % frameBytes);
    if (alignedLen <= 0)
        return 0;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ring.empty())
        return 0;

    const size_t written = ringWriteLocked(data, static_cast<size_t>(alignedLen));
    return static_cast<qint64>(written);
}

bool WasapiAudioOutputBackend::resetStream()
{
    const quint64 resetId = nextResetTraceId();

    qint64 queuedBefore = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queuedBefore = static_cast<qint64>(m_ringSize);
    }

    qInfo() << "[seek-output] reset begin"
            << "id=" << resetId
            << "open=" << m_open.load()
            << "paused=" << m_paused.load()
            << "queuedBytesBefore=" << queuedBefore;

    if (!m_audioClient) {
        qWarning() << "[seek-output] reset aborted: audioClient is null"
                   << "id=" << resetId;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_wasapiMutex);
        m_audioClient->Stop();

        HRESULT hr = m_audioClient->Reset();
        if (FAILED(hr)) {
            qWarning() << "[seek-output] WASAPI reset failed"
                       << "id=" << resetId
                       << "hr=" << toHexHr(hr);
            return false;
        }

        {
            std::lock_guard<std::mutex> ringLock(m_mutex);
            clearRingLocked();
        }

        // Restart if not paused - caller decides via resume()/suspend()
        if (!m_paused.load()) {
            hr = m_audioClient->Start();
            if (FAILED(hr)) {
                qWarning() << "[seek-output] WASAPI start-after-reset failed"
                           << "id=" << resetId
                           << "hr=" << toHexHr(hr);
                // Continue anyway - output will start on next write
            }
        }
    }

    if (m_eventHandle)
        SetEvent(m_eventHandle);

    qint64 queuedAfter = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queuedAfter = static_cast<qint64>(m_ringSize);
    }

    qInfo() << "[seek-output] reset complete"
            << "id=" << resetId
            << "queuedBytesAfter=" << queuedAfter
            << "paused=" << m_paused.load();

    return true;
}

size_t WasapiAudioOutputBackend::ringWriteLocked(const char *data, size_t len)
{
    const size_t frameAlign = static_cast<size_t>(qMax(1, m_frameBytes));
    const size_t freeBytes = m_ring.size() - m_ringSize;
    size_t toWrite = std::min(freeBytes, len);
    toWrite -= (toWrite % frameAlign);
    if (toWrite == 0)
        return 0;

    const size_t first = std::min(toWrite, m_ring.size() - m_ringWritePos);
    std::memcpy(m_ring.data() + m_ringWritePos, data, first);

    const size_t second = toWrite - first;
    if (second > 0)
        std::memcpy(m_ring.data(), data + first, second);

    m_ringWritePos = (m_ringWritePos + toWrite) % m_ring.size();
    m_ringSize += toWrite;
    return toWrite;
}

size_t WasapiAudioOutputBackend::ringReadLocked(char *dst, size_t len)
{
    const size_t toRead = std::min(m_ringSize, len);
    if (toRead == 0)
        return 0;

    const size_t first = std::min(toRead, m_ring.size() - m_ringReadPos);
    std::memcpy(dst, m_ring.data() + m_ringReadPos, first);

    const size_t second = toRead - first;
    if (second > 0)
        std::memcpy(dst + first, m_ring.data(), second);

    m_ringReadPos = (m_ringReadPos + toRead) % m_ring.size();
    m_ringSize -= toRead;
    return toRead;
}

void WasapiAudioOutputBackend::clearRingLocked()
{
    m_ringReadPos = 0;
    m_ringWritePos = 0;
    m_ringSize = 0;
}

void WasapiAudioOutputBackend::renderLoop()
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        qWarning() << "WASAPI: render thread CoInitializeEx failed" << toHexHr(coHr);
        return;
    }

    DWORD taskIndex = 0;
    const wchar_t *taskProfile = (m_mode == Mode::Exclusive) ? L"Pro Audio" : L"Audio";
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(taskProfile, &taskIndex);

    HANDLE waitHandles[2] = { m_shutdownEvent, m_eventHandle };

    while (m_open.load()) {
        const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 1000);
        if (waitResult == WAIT_OBJECT_0)
            break;
        if (waitResult != WAIT_OBJECT_0 + 1)
            continue;

        if (m_paused.load() || !m_audioClient || !m_renderClient)
            continue;

        enum class RenderResult { Ok, Skip, DeviceLost };
        RenderResult renderResult = RenderResult::Skip;

        {
            std::lock_guard<std::mutex> wasapiLock(m_wasapiMutex);

            if (m_paused.load() || !m_audioClient || !m_renderClient) {
                renderResult = RenderResult::Skip;
            } else {
                UINT32 padding = 0;
                const HRESULT paddingHr = m_audioClient->GetCurrentPadding(&padding);
                if (FAILED(paddingHr)) {
                    renderResult = isRecoverableDeviceLoss(paddingHr) ? RenderResult::DeviceLost : RenderResult::Skip;
                    if (renderResult == RenderResult::DeviceLost)
                        qWarning() << "WASAPI: device invalidated in render loop (GetCurrentPadding), forcing reopen. hr="
                                   << toHexHr(paddingHr);
                } else if (padding < m_bufferFrameCount) {
                    const UINT32 framesAvailable = m_bufferFrameCount - padding;
                    BYTE *out = nullptr;
                    const HRESULT getBufferHr = m_renderClient->GetBuffer(framesAvailable, &out);
                    if (FAILED(getBufferHr)) {
                        renderResult = isRecoverableDeviceLoss(getBufferHr) ? RenderResult::DeviceLost : RenderResult::Skip;
                        if (renderResult == RenderResult::DeviceLost)
                            qWarning() << "WASAPI: device invalidated in render loop (GetBuffer), forcing reopen. hr="
                                       << toHexHr(getBufferHr);
                    } else {
                        const size_t bytesNeeded = static_cast<size_t>(framesAvailable) * static_cast<size_t>(m_frameBytes);
                        size_t copied = 0;
                        {
                            std::lock_guard<std::mutex> ringLock(m_mutex);
                            copied = ringReadLocked(reinterpret_cast<char *>(out), bytesNeeded);
                        }

                        if (copied < bytesNeeded)
                            std::memset(out + copied, 0, bytesNeeded - copied);

                        if (copied > 0) {
                            const float v = m_volume.load();
                            if ((v < 0.999f || v > 1.001f) && m_format.sampleFormat() == QAudioFormat::Float) {
                                float *samples = reinterpret_cast<float *>(out);
                                const size_t count = copied / sizeof(float);
                                for (size_t i = 0; i < count; ++i)
                                    samples[i] *= v;
                            } else if ((v < 0.999f || v > 1.001f) && m_format.sampleFormat() == QAudioFormat::Int16) {
                                qint16 *samples = reinterpret_cast<qint16 *>(out);
                                const size_t count = copied / sizeof(qint16);
                                for (size_t i = 0; i < count; ++i) {
                                    const int scaled = static_cast<int>(samples[i] * v);
                                    samples[i] = static_cast<qint16>(std::clamp(scaled, -32768, 32767));
                                }
                            }
                        }

                        const DWORD releaseFlags = (copied == 0) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
                        const HRESULT releaseHr = m_renderClient->ReleaseBuffer(framesAvailable, releaseFlags);
                        if (FAILED(releaseHr) && isRecoverableDeviceLoss(releaseHr)) {
                            qWarning() << "WASAPI: device invalidated in render loop (ReleaseBuffer), forcing reopen. hr="
                                       << toHexHr(releaseHr);
                            renderResult = RenderResult::DeviceLost;
                        } else {
                            renderResult = RenderResult::Ok;
                        }
                    }
                }
            }
        }

        if (renderResult == RenderResult::DeviceLost) {
            m_open.store(false);
            break;
        }
    }

    if (mmcssHandle)
        AvRevertMmThreadCharacteristics(mmcssHandle);

    if (SUCCEEDED(coHr))
        CoUninitialize();
}

QString WasapiAudioOutputBackend::queryDeviceName(IMMDevice *device)
{
    if (!device)
        return {};

    IPropertyStore *props = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr) || !props)
        return {};

    PROPVARIANT value;
    PropVariantInit(&value);
    QString name;
    hr = props->GetValue(kPkeyDeviceFriendlyName, &value);
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal)
        name = QString::fromWCharArray(value.pwszVal);
    PropVariantClear(&value);
    props->Release();
    return name;
}

QString WasapiAudioOutputBackend::queryDeviceId(IMMDevice *device)
{
    if (!device)
        return {};

    LPWSTR id = nullptr;
    const HRESULT hr = device->GetId(&id);
    if (FAILED(hr) || !id)
        return {};

    const QString result = QString::fromWCharArray(id);
    CoTaskMemFree(id);
    return result;
}

QVector<QPair<QString, QString>> WasapiAudioOutputBackend::enumerateRenderDevices()
{
    QVector<QPair<QString, QString>> devices;

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return devices;

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDeviceCollection *collection = nullptr;

    const HRESULT createHr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (SUCCEEDED(createHr) && enumerator) {
        const HRESULT enumHr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(enumHr) && collection) {
            UINT count = 0;
            if (SUCCEEDED(collection->GetCount(&count))) {
                devices.reserve(static_cast<int>(count));
                for (UINT i = 0; i < count; ++i) {
                    IMMDevice *device = nullptr;
                    if (FAILED(collection->Item(i, &device)) || !device)
                        continue;

                    const QString id = queryDeviceId(device);
                    QString name = queryDeviceName(device);
                    if (name.isEmpty())
                        name = id;
                    if (!id.isEmpty())
                        devices.push_back(qMakePair(id, name));

                    device->Release();
                }
            }
        }
    }

    if (collection)
        collection->Release();
    if (enumerator)
        enumerator->Release();

    if (SUCCEEDED(coHr))
        CoUninitialize();

    return devices;
}

#endif // Q_OS_WIN