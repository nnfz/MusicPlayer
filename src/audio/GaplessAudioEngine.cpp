#include "GaplessAudioEngine.h"
#include <QDebug>
#include <QFileInfo>

#ifdef MUSICPLAYER_HAS_BASS
#include <bass.h>
#include <bass_fx.h>
#include <bassflac.h>
#include <bassmix.h>
#endif

namespace {
const char *engineStateToString(GaplessAudioEngine::State state)
{
    switch (state) {
    case GaplessAudioEngine::Stopped: return "Stopped";
    case GaplessAudioEngine::Playing: return "Playing";
    case GaplessAudioEngine::Paused:  return "Paused";
    default: return "Unknown";
    }
}
}

GaplessAudioEngine::GaplessAudioEngine(QObject *parent)
    : QObject(parent)
{
    m_positionTimer = new QTimer(this);
    m_positionTimer->setInterval(30);
    connect(m_positionTimer, &QTimer::timeout, this, &GaplessAudioEngine::onPositionTick);

    initBass();
}

GaplessAudioEngine::~GaplessAudioEngine()
{
    stop();
    freeBass();
}

void GaplessAudioEngine::initBass()
{
#ifdef MUSICPLAYER_HAS_BASS
    static bool baseInitialized = false;
    if (baseInitialized) {
        // Just make sure mixer exists if we re-init
        if (!m_mixer) {
            m_mixer = BASS_Mixer_StreamCreate(44100, 2, BASS_MIXER_NONSTOP);
            if (m_mixer) BASS_ChannelPlay(m_mixer, FALSE);
        }
        return;
    }

    qDebug() << "[bass] Initializing BASS system...";
    
    // BASS itself doesn't need PluginLoad. 
    // Plugins like FLAC/APE/OPUS etc. should be loaded once.
    // FX and MIX are usually linked or loaded automatically if in same dir, 
    // but explicit load is safer IF not already loaded.
    
    const char* plugins[] = {
        "bassflac.dll", "bassape.dll", "bassalac.dll",
        "bassopus.dll", "basswv.dll", "basswma.dll",
        "bassmidi.dll", "basshls.dll"
    };

    for (const char* plugin : plugins) {
        BASS_PluginLoad(plugin, 0);
    }

    // Initialize device
    if (!BASS_Init(-1, 44100, BASS_DEVICE_LATENCY, 0, nullptr)) {
        int err = BASS_ErrorGetCode();
        if (err != 14) qWarning() << "[bass] BASS_Init failed:" << err;
    }

    m_mixer = BASS_Mixer_StreamCreate(44100, 2, BASS_MIXER_NONSTOP);
    if (m_mixer) {
        BASS_ChannelPlay(m_mixer, FALSE);
        qDebug() << "[bass] Mixer ready";
    } else {
        qWarning() << "[bass] Mixer failed:" << BASS_ErrorGetCode();
    }

    baseInitialized = true;
#endif
}

void GaplessAudioEngine::freeBass()
{
#ifdef MUSICPLAYER_HAS_BASS
    BASS_Free();
#endif
}

bool GaplessAudioEngine::selectBassDevice(int deviceIndex)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (BASS_SetDevice(deviceIndex)) {
        if (!BASS_Init(deviceIndex, 44100, BASS_DEVICE_LATENCY, 0, NULL)) {
            // Already initialized or failed
        }
        return true;
    }
#endif
    return false;
}

int GaplessAudioEngine::getBassDeviceIndex(const QString &deviceId) const
{
#ifdef MUSICPLAYER_HAS_BASS
    BASS_DEVICEINFO info;
    for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
        if (QString::fromUtf8(info.driver) == deviceId || QString::fromUtf8(info.name) == deviceId) {
            return i;
        }
    }
#endif
    return -1; // -1 for default
}

void GaplessAudioEngine::setOutputDevicePreferenceId(const QString &deviceId)
{
    m_outputDevicePreferenceId = deviceId;
#ifdef MUSICPLAYER_HAS_BASS
    int idx = getBassDeviceIndex(deviceId);
    if (idx != -1 || deviceId.isEmpty()) {
        selectBassDevice(idx == -1 ? -1 : idx);
        
        m_activeOutputDeviceId = deviceId;
        
        BASS_DEVICEINFO info;
        if (BASS_GetDeviceInfo(idx == -1 ? BASS_GetDevice() : idx, &info)) {
            m_activeOutputDeviceName = QString::fromUtf8(info.name);
        } else {
            m_activeOutputDeviceName = "System Default";
        }
        
        emit outputDeviceChanged();
    }
#endif
}

QString GaplessAudioEngine::activeOutputDeviceName() const
{
    if (!m_activeOutputDeviceName.isEmpty())
        return m_activeOutputDeviceName;
    return "System Default";
}

QVector<QPair<QString, QString>> GaplessAudioEngine::availableOutputDevices() const
{
    QVector<QPair<QString, QString>> list;
    list.append(QPair<QString, QString>("System Default", ""));
#ifdef MUSICPLAYER_HAS_BASS
    BASS_DEVICEINFO info;
    for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
        if (info.flags & BASS_DEVICE_ENABLED) {
            list.append(QPair<QString, QString>(QString::fromUtf8(info.name), QString::fromUtf8(info.driver)));
        }
    }
#endif
    return list;
}

HSTREAM GaplessAudioEngine::createStream(const QString &filePath)
{
#ifdef MUSICPLAYER_HAS_BASS
    // Create decode stream
    HSTREAM decodeStream = BASS_StreamCreateFile(FALSE, filePath.utf16(), 0, 0, BASS_STREAM_DECODE | BASS_UNICODE | BASS_SAMPLE_FLOAT);
    if (!decodeStream) {
        qWarning() << "BASS_StreamCreateFile failed:" << BASS_ErrorGetCode();
        return 0;
    }
    
    // Wrap in tempo stream for playback rate support
    HSTREAM fxStream = BASS_FX_TempoCreate(decodeStream, BASS_FX_FREESOURCE | BASS_STREAM_DECODE);
    if (!fxStream) {
        qWarning() << "BASS_FX_TempoCreate failed:" << BASS_ErrorGetCode();
        BASS_StreamFree(decodeStream);
        return 0;
    }

    return fxStream;
#else
    Q_UNUSED(filePath)
    return 0;
#endif
}

void GaplessAudioEngine::setupStream(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (!stream) return;
    // Don't apply volume here if we apply it to mixer
    applyPlaybackRate(stream);
    setupDsp(stream);
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::applyVolume(HSTREAM stream)
{
    Q_UNUSED(stream)
    // Volume is applied to mixer
}

void GaplessAudioEngine::applyPlaybackRate(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (stream) {
        // BASS_ATTRIB_TEMPO is percentage relative to normal speed (0 = normal)
        float tempoPerc = (m_playbackRate - 1.0f) * 100.0f;
        
        // To get the classic "slowed" effect where pitch drops with speed:
        // We use BASS_ATTRIB_TEMPO_FREQ which scales the frequency (pitch) together with tempo.
        BASS_ChannelSetAttribute(stream, BASS_ATTRIB_TEMPO_FREQ, tempoPerc);
        
        // Also set TEMPO to ensure duration/position remains consistent
        BASS_ChannelSetAttribute(stream, BASS_ATTRIB_TEMPO, tempoPerc);
    }
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::setupDsp(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (stream && m_equalizer && m_equalizer->isEnabled()) {
        m_eqDsp = BASS_ChannelSetDSP(stream, EqDspProc, this, 0);
    }
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::removeDsp(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (stream && m_eqDsp) {
        BASS_ChannelRemoveDSP(stream, m_eqDsp);
        m_eqDsp = 0;
    }
#else
    Q_UNUSED(stream)
#endif
}

void CALLBACK GaplessAudioEngine::EqDspProc(HDSP handle, unsigned long channel, void *buffer, unsigned long length, void *user)
{
    Q_UNUSED(handle)
#ifdef MUSICPLAYER_HAS_BASS
    GaplessAudioEngine *engine = static_cast<GaplessAudioEngine*>(user);
    if (engine && engine->m_equalizer && engine->m_equalizer->isEnabled()) {
        BASS_CHANNELINFO info;
        BASS_ChannelGetInfo(channel, &info);
        int bytesPerSample = (info.flags & BASS_SAMPLE_FLOAT) ? 4 : ((info.flags & BASS_SAMPLE_8BITS) ? 1 : 2);
        
        engine->m_equalizer->setSampleRate(info.freq);
        engine->m_equalizer->process(static_cast<char*>(buffer), length, info.chans, bytesPerSample);
    }
#else
    Q_UNUSED(channel)
    Q_UNUSED(buffer)
    Q_UNUSED(length)
    Q_UNUSED(user)
#endif
}

void GaplessAudioEngine::setEqualizer(Equalizer *eq)
{
    m_equalizer = eq;
#ifdef MUSICPLAYER_HAS_BASS
    if (m_activeStream) {
        removeDsp(m_activeStream);
        setupDsp(m_activeStream);
    }
#endif
}

void GaplessAudioEngine::updateState(State newState)
{
    if (m_state != newState) {
        qDebug() << "[bass] State change:" << engineStateToString(m_state) << "->" << engineStateToString(newState);
        m_state = newState;
        if (m_state == Playing) {
            m_positionTimer->start();
        } else {
            m_positionTimer->stop();
        }
        emit stateChanged(m_state);
    }
}

void GaplessAudioEngine::play(const QString &filePath)
{
#ifdef MUSICPLAYER_HAS_BASS
    qDebug() << "[bass] play() requested for:" << filePath;

    // 1. COMPLETELY remove and free previous streams to ensure a clean start
    if (m_activeStream) {
        BASS_Mixer_ChannelRemove(m_activeStream);
        BASS_StreamFree(m_activeStream);
        m_activeStream = 0;
    }
    if (m_nextStream) {
        BASS_Mixer_ChannelRemove(m_nextStream);
        BASS_StreamFree(m_nextStream);
        m_nextStream = 0;
    }
    
    m_endSync = 0;
    m_crossfadeSync = 0;
    m_transitionPending = false;

    // 2. Create the new stream
    m_activeStream = createStream(filePath);
    if (!m_activeStream) {
        qWarning() << "[bass] play failed: could not create stream for" << filePath;
        return;
    }
    
    if (!m_mixer) {
        initBass();
        if (!m_mixer) return;
    }
    
    setupStream(m_activeStream);
    m_currentFilePath = filePath;
    
    // 3. Add to mixer with IMMEDIATE start and AUTOFREE
    if (!BASS_Mixer_StreamAddChannel(m_mixer, m_activeStream, BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN | BASS_STREAM_AUTOFREE)) {
        qWarning() << "[bass] Mixer AddChannel failed:" << BASS_ErrorGetCode();
        BASS_StreamFree(m_activeStream);
        m_activeStream = 0;
        return;
    }
    
    // Flush mixer buffer specifically for this channel start to avoid teleportation/remnants
    BASS_Mixer_ChannelSetPosition(m_activeStream, 0, BASS_POS_BYTE | BASS_POS_MIXER_RESET);
    
    // 4. Setup End Sync (MIXTIME for gapless precision)
    m_endSync = BASS_ChannelSetSync(m_activeStream, BASS_SYNC_END | BASS_SYNC_MIXTIME, 0, EndSyncProc, this);
    
    // 5. Setup Crossfade if enabled
    if (m_crossfadeDurationMs > 0) {
        qint64 lenBytes = BASS_ChannelGetLength(m_activeStream, BASS_POS_BYTE);
        double lenSecs = BASS_ChannelBytes2Seconds(m_activeStream, lenBytes);
        double crossfadeSecs = m_crossfadeDurationMs / 1000.0;
        if (lenSecs > crossfadeSecs * 2) {
            qint64 syncBytes = BASS_ChannelSeconds2Bytes(m_activeStream, lenSecs - crossfadeSecs);
            m_crossfadeSync = BASS_ChannelSetSync(m_activeStream, BASS_SYNC_POS | BASS_SYNC_MIXTIME, syncBytes, CrossfadeSyncProc, this);
        }
    }
    
    // 6. Ensure mixer is playing
    BASS_ChannelPlay(m_mixer, FALSE);
    
    updateState(Playing);
    emit durationChanged(duration());
#else
    Q_UNUSED(filePath)
#endif
}

void GaplessAudioEngine::prepareNext(const QString &filePath)
{
    m_preparedNextPath = filePath;
#ifdef MUSICPLAYER_HAS_BASS
    if (m_nextStream) {
        BASS_StreamFree(m_nextStream);
        m_nextStream = 0;
    }
    
    if (!filePath.isEmpty()) {
        m_nextStream = createStream(filePath);
        if (m_nextStream) {
            setupStream(m_nextStream);
            // Volume to 0 initially if crossfading
            if (m_crossfadeDurationMs > 0) {
                BASS_ChannelSetAttribute(m_nextStream, BASS_ATTRIB_VOL, 0.0f);
            }
        }
    }
#endif
}

void GaplessAudioEngine::pause()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_mixer) {
        BASS_ChannelPause(m_mixer);
        updateState(Paused);
    }
#endif
}

void GaplessAudioEngine::resume()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_mixer) {
        BASS_ChannelPlay(m_mixer, FALSE);
        updateState(Playing);
    }
#endif
}

void GaplessAudioEngine::stop()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_activeStream) {
        BASS_Mixer_ChannelRemove(m_activeStream);
        BASS_StreamFree(m_activeStream);
        m_activeStream = 0;
    }
    if (m_nextStream) {
        BASS_Mixer_ChannelRemove(m_nextStream);
        BASS_StreamFree(m_nextStream);
        m_nextStream = 0;
    }
    m_endSync = 0;
    m_crossfadeSync = 0;
    m_eqDsp = 0;
    m_currentFilePath.clear();
    m_preparedNextPath.clear();
#endif
    updateState(Stopped);
    emit positionChanged(0);
}

void GaplessAudioEngine::seek(qint64 positionMs)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_activeStream && m_mixer) {
        qint64 posBytes = BASS_ChannelSeconds2Bytes(m_activeStream, positionMs / 1000.0);
        
        // Use BASS_POS_MIXER_RESET to ensure the mixer clears its buffer for this channel
        BASS_Mixer_ChannelSetPosition(m_activeStream, posBytes, BASS_POS_BYTE | BASS_POS_MIXER_RESET);
        
        if (m_equalizer) m_equalizer->prepareForSeek();
    }
#else
    Q_UNUSED(positionMs)
#endif
}

void GaplessAudioEngine::setVolume(float vol)
{
    m_volume = qBound(0.0f, vol, 1.0f);
    // Apply volume to the MIXER, not individual streams, for consistent behavior
#ifdef MUSICPLAYER_HAS_BASS
    if (m_mixer) {
        BASS_ChannelSetAttribute(m_mixer, BASS_ATTRIB_VOL, m_volume);
    }
#endif
}

void GaplessAudioEngine::setPlaybackRate(float rate)
{
    m_playbackRate = rate;
    applyPlaybackRate(m_activeStream);
    emit playbackRateChanged(rate);
}

void GaplessAudioEngine::setCrossfadeDurationMs(int durationMs)
{
    m_crossfadeDurationMs = durationMs;
}

qint64 GaplessAudioEngine::position() const
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_activeStream && m_mixer) {
        qint64 posBytes = BASS_Mixer_ChannelGetPosition(m_activeStream, BASS_POS_BYTE);
        if (posBytes == -1) return 0;
        double secs = BASS_ChannelBytes2Seconds(m_activeStream, posBytes);
        return static_cast<qint64>(secs * 1000.0);
    }
#endif
    return 0;
}

qint64 GaplessAudioEngine::duration() const
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_activeStream) {
        qint64 lenBytes = BASS_ChannelGetLength(m_activeStream, BASS_POS_BYTE);
        double secs = BASS_ChannelBytes2Seconds(m_activeStream, lenBytes);
        return static_cast<qint64>(secs * 1000.0);
    }
#endif
    return 0;
}

void GaplessAudioEngine::onPositionTick()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_state == Playing && m_activeStream && m_mixer) {
        // Check if current stream is actually still active in mixer
        if (BASS_Mixer_ChannelIsActive(m_activeStream) == BASS_ACTIVE_STOPPED) {
            if (m_transitionPending) {
                m_transitionPending = false;
                processPendingTransitions();
            } else {
                updateState(Stopped);
                emit playbackFinished();
                return;
            }
        }

        emit positionChanged(position());
        
        DWORD level = BASS_Mixer_ChannelGetLevel(m_activeStream);
        if (level != -1) {
            int left = LOWORD(level);
            int right = HIWORD(level);
            float avgLevel = (left + right) / (2.0f * 32768.0f);
            emit currentAudioLevel(avgLevel);
            emit bassLevel(avgLevel * 1.2f);
        }
        
        if (m_transitionPending) {
            m_transitionPending = false;
            processPendingTransitions();
        }
    }
#endif
}

void CALLBACK GaplessAudioEngine::EndSyncProc(HSYNC handle, unsigned long channel, unsigned long data, void *user)
{
    Q_UNUSED(handle)
    Q_UNUSED(channel)
    Q_UNUSED(data)
#ifdef MUSICPLAYER_HAS_BASS
    GaplessAudioEngine *engine = static_cast<GaplessAudioEngine*>(user);
    if (engine) {
        if (engine->m_crossfadeDurationMs == 0 && engine->m_nextStream) {
            // Start next track immediately in the audio thread context for perfect gapless
            BASS_Mixer_StreamAddChannel(engine->m_mixer, engine->m_nextStream, BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN | BASS_STREAM_AUTOFREE);
        }
        engine->m_transitionPending = true;
        // Schedule UI update immediately to swap pointers and update sliders
        QMetaObject::invokeMethod(engine, "onPositionTick", Qt::QueuedConnection);
    }
#endif
}

void CALLBACK GaplessAudioEngine::CrossfadeSyncProc(HSYNC handle, unsigned long channel, unsigned long data, void *user)
{
    Q_UNUSED(handle)
    Q_UNUSED(data)
#ifdef MUSICPLAYER_HAS_BASS
    GaplessAudioEngine *engine = static_cast<GaplessAudioEngine*>(user);
    if (engine && engine->m_nextStream) {
        BASS_Mixer_StreamAddChannel(engine->m_mixer, engine->m_nextStream, BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN | BASS_STREAM_AUTOFREE);
        
        // Start fading out current
        BASS_ChannelSlideAttribute(channel, BASS_ATTRIB_VOL, -1, engine->m_crossfadeDurationMs);
        
        // Start next track fading in
        BASS_ChannelSetAttribute(engine->m_nextStream, BASS_ATTRIB_VOL, 0.0f);
        BASS_ChannelSlideAttribute(engine->m_nextStream, BASS_ATTRIB_VOL, engine->m_volume, engine->m_crossfadeDurationMs);
        
        // Tell engine to swap pointers
        engine->m_transitionPending = true;
        // Schedule UI update immediately
        QMetaObject::invokeMethod(engine, "onPositionTick", Qt::QueuedConnection);
    }
#else
    Q_UNUSED(channel)
    Q_UNUSED(user)
#endif
}

void GaplessAudioEngine::processPendingTransitions()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_nextStream) {
        qDebug() << "[bass] Performing gapless transition pointers swap";
        m_activeStream = m_nextStream;
        m_nextStream = 0;
        m_currentFilePath = m_preparedNextPath;
        m_preparedNextPath.clear();
        
        // Reattach syncs to the now-active stream
        m_endSync = BASS_ChannelSetSync(m_activeStream, BASS_SYNC_END | BASS_SYNC_MIXTIME, 0, EndSyncProc, this);
        if (m_crossfadeDurationMs > 0) {
            qint64 lenBytes = BASS_ChannelGetLength(m_activeStream, BASS_POS_BYTE);
            double lenSecs = BASS_ChannelBytes2Seconds(m_activeStream, lenBytes);
            double crossfadeSecs = m_crossfadeDurationMs / 1000.0;
            if (lenSecs > crossfadeSecs * 2) {
                qint64 syncBytes = BASS_ChannelSeconds2Bytes(m_activeStream, lenSecs - crossfadeSecs);
                m_crossfadeSync = BASS_ChannelSetSync(m_activeStream, BASS_SYNC_POS | BASS_SYNC_MIXTIME, syncBytes, CrossfadeSyncProc, this);
            }
        }
        
        emit trackTransitioned();
        emit durationChanged(duration());
    } else {
        if (BASS_Mixer_ChannelIsActive(m_activeStream) == BASS_ACTIVE_STOPPED) {
            updateState(Stopped);
            emit playbackFinished();
        }
    }
#endif
}
