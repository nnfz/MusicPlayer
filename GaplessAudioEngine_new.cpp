#include "GaplessAudioEngine.h"
#include <QDebug>
#include <QFileInfo>

#ifdef MUSICPLAYER_HAS_BASS
#include <bass.h>
#include <bass_fx.h>
#include <bassflac.h>
#include <bassmix.h>
#endif

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
    // Load plugins
    BASS_PluginLoad("bassflac.dll", 0);
    BASS_PluginLoad("bass_fx.dll", 0);

    // Initialize default device
    if (!BASS_Init(-1, 44100, BASS_DEVICE_LATENCY, 0, NULL)) {
        qWarning() << "BASS_Init failed! Error:" << BASS_ErrorGetCode();
    } else {
        m_mixer = BASS_Mixer_StreamCreate(44100, 2, BASS_MIXER_NONSTOP);
        if (m_mixer) {
            BASS_ChannelPlay(m_mixer, FALSE);
        } else {
            qWarning() << "BASS_Mixer_StreamCreate failed! Error:" << BASS_ErrorGetCode();
        }
    }
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
    
    // Wrap in tempo stream for playback rate support. Crucially MUST BE A DECODE STREAM for the mixer!
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
    applyVolume(stream);
    applyPlaybackRate(stream);
    setupDsp(stream);
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::applyVolume(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (stream) {
        BASS_ChannelSetAttribute(stream, BASS_ATTRIB_VOL, m_volume);
    }
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::applyPlaybackRate(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (stream) {
        // BASS_ATTRIB_TEMPO is percentage relative to normal speed (0 = normal)
        float tempoPerc = (m_playbackRate - 1.0f) * 100.0f;
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

void GaplessAudioEngine::play(const QString &filePath)
{
#ifdef MUSICPLAYER_HAS_BASS
    stop();
    
    m_activeStream = createStream(filePath);
    if (!m_activeStream || !m_mixer) return;
    
    setupStream(m_activeStream);
    m_currentFilePath = filePath;
    
    // Add to mixer
    BASS_Mixer_StreamAddChannel(m_mixer, m_activeStream, BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN);
    
    // Setup End Sync. Mixtime sync is perfectly safe and required for perfect gapless when using a mixer
    m_endSync = BASS_ChannelSetSync(m_activeStream, BASS_SYNC_END | BASS_SYNC_MIXTIME, 0, EndSyncProc, this);
    
    // Setup Crossfade Sync if enabled
    if (m_crossfadeDurationMs > 0) {
        qint64 lenBytes = BASS_ChannelGetLength(m_activeStream, BASS_POS_BYTE);
        double lenSecs = BASS_ChannelBytes2Seconds(m_activeStream, lenBytes);
        double crossfadeSecs = m_crossfadeDurationMs / 1000.0;
        
        if (lenSecs > crossfadeSecs * 2) {
            qint64 syncBytes = BASS_ChannelSeconds2Bytes(m_activeStream, lenSecs - crossfadeSecs);
            m_crossfadeSync = BASS_ChannelSetSync(m_activeStream, BASS_SYNC_POS | BASS_SYNC_MIXTIME, syncBytes, CrossfadeSyncProc, this);
        }
    }
    
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
    if (m_mixer && BASS_ChannelIsActive(m_mixer) == BASS_ACTIVE_PLAYING) {
        BASS_ChannelPause(m_mixer);
        updateState(Paused);
    }
#endif
}

void GaplessAudioEngine::resume()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_mixer && BASS_ChannelIsActive(m_mixer) == BASS_ACTIVE_PAUSED) {
        BASS_ChannelPlay(m_mixer, FALSE);
        updateState(Playing);
    }
#endif
}

void GaplessAudioEngine::stop()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_activeStream) {
        if (m_mixer) BASS_Mixer_ChannelRemove(m_activeStream);
        BASS_StreamFree(m_activeStream);
        m_activeStream = 0;
    }
    if (m_nextStream) {
        if (m_mixer) BASS_Mixer_ChannelRemove(m_nextStream);
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
        BASS_Mixer_ChannelSetPosition(m_activeStream, posBytes, BASS_POS_BYTE);
        
        // Reset equalizer state for clean seek if custom equalizer exists
        if (m_equalizer) m_equalizer->prepareForSeek();
    }
#else
    Q_UNUSED(positionMs)
#endif
}

void GaplessAudioEngine::setVolume(float vol)
{
    m_volume = qBound(0.0f, vol, 1.0f);
    applyVolume(m_activeStream);
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

void GaplessAudioEngine::updateState(State newState)
{
    if (m_state != newState) {
        m_state = newState;
        if (m_state == Playing) {
            m_positionTimer->start();
        } else {
            m_positionTimer->stop();
        }
        emit stateChanged(m_state);
    }
}

void GaplessAudioEngine::onPositionTick()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_state == Playing && m_activeStream && m_mixer) {
        emit positionChanged(position());
        
        // Calculate audio level for visualizer from the active mixer source channel
        DWORD level = BASS_Mixer_ChannelGetLevel(m_activeStream);
        int left = LOWORD(level);
        int right = HIWORD(level);
        float avgLevel = (left + right) / (2.0f * 32768.0f); // Max is 32768
        emit currentAudioLevel(avgLevel);
        
        // Emulate bass level - simply a scaled version or maybe left channel heavily smoothed
        emit bassLevel(avgLevel * 1.2f); // Emulation for visualizer
        
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
            BASS_Mixer_StreamAddChannel(engine->m_mixer, engine->m_nextStream, BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN);
        }
        engine->m_transitionPending = true;
    }
#else
    Q_UNUSED(user)
#endif
}

void CALLBACK GaplessAudioEngine::CrossfadeSyncProc(HSYNC handle, unsigned long channel, unsigned long data, void *user)
{
    Q_UNUSED(handle)
    Q_UNUSED(data)
#ifdef MUSICPLAYER_HAS_BASS
    GaplessAudioEngine *engine = static_cast<GaplessAudioEngine*>(user);
    if (engine && engine->m_nextStream) {
        BASS_Mixer_StreamAddChannel(engine->m_mixer, engine->m_nextStream, BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN);
        
        // Start fading out current
        BASS_ChannelSlideAttribute(channel, BASS_ATTRIB_VOL, -1, engine->m_crossfadeDurationMs);
        
        // Start next track fading in
        BASS_ChannelSetAttribute(engine->m_nextStream, BASS_ATTRIB_VOL, 0.0f);
        BASS_ChannelSlideAttribute(engine->m_nextStream, BASS_ATTRIB_VOL, engine->m_volume, engine->m_crossfadeDurationMs);
        
        // Tell engine to swap pointers
        engine->m_transitionPending = true;
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
        // Transition to next stream
        if (BASS_Mixer_ChannelIsActive(m_activeStream) != BASS_ACTIVE_PLAYING || m_crossfadeDurationMs > 0) {
            // Unplug old stream from mixer if gapless (for crossfade, it stops when volume reaches -1)
            if (m_crossfadeDurationMs == 0) {
                BASS_Mixer_ChannelRemove(m_activeStream);
            }
            
            m_activeStream = m_nextStream;
            m_nextStream = 0;
            m_currentFilePath = m_preparedNextPath;
            m_preparedNextPath.clear();
            
            BASS_ChannelSetAttribute(m_activeStream, BASS_ATTRIB_VOL, m_volume);
            
            // Reattach syncs
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
        }
    } else {
        if (BASS_Mixer_ChannelIsActive(m_activeStream) == BASS_ACTIVE_STOPPED) {
            updateState(Stopped);
            emit playbackFinished();
        }
    }
#endif
}
