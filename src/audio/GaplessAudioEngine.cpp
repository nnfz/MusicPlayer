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

void CALLBACK GaplessAudioEngine::AudioEndSyncProc(HSYNC handle, unsigned long channel, unsigned long data, void *user)
{
    Q_UNUSED(handle)
    Q_UNUSED(channel)
    Q_UNUSED(data)
#ifdef MUSICPLAYER_HAS_BASS
    GaplessAudioEngine *engine = static_cast<GaplessAudioEngine*>(user);
    if (!engine || !engine->m_nextStream || engine->m_crossfadeDurationMs > 0) return;
    BASS_Mixer_StreamAddChannel(engine->m_mixer, engine->m_nextStream,
        BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN | BASS_STREAM_AUTOFREE | BASS_MIXER_CHAN_BUFFER);
    QMetaObject::invokeMethod(engine, "processPendingTransitions", Qt::QueuedConnection);
#endif
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
    BASS_SetConfig(BASS_CONFIG_BUFFER, 100);
    BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 30);
    static bool baseInitialized = false;
    if (baseInitialized) {
        if (!m_mixer) {
            m_mixer = BASS_Mixer_StreamCreate(44100, 2, BASS_MIXER_NONSTOP | BASS_SAMPLE_FLOAT);
            if (m_mixer) BASS_ChannelPlay(m_mixer, FALSE);
        }
        return;
    }

    qDebug() << "[bass] Initializing BASS system...";

    const char* plugins[] = {
        "bassflac.dll", "bassape.dll", "bassalac.dll",
        "bassopus.dll", "basswv.dll", "basswma.dll",
        "bassmidi.dll", "basshls.dll"
    };
    for (const char* plugin : plugins)
        BASS_PluginLoad(plugin, 0);

    if (!BASS_Init(-1, 44100, BASS_DEVICE_LATENCY, 0, nullptr)) {
        int err = BASS_ErrorGetCode();
        if (err != 14) qWarning() << "[bass] BASS_Init failed:" << err;
    }

    m_mixer = BASS_Mixer_StreamCreate(44100, 2, BASS_MIXER_NONSTOP | BASS_SAMPLE_FLOAT);
    if (m_mixer) {
        BASS_ChannelPlay(m_mixer, FALSE);
        qDebug() << "[bass] Mixer ready";
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
        if (!BASS_Init(deviceIndex, 44100, BASS_DEVICE_LATENCY, 0, NULL)) {}
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
        if (QString::fromUtf8(info.driver) == deviceId || QString::fromUtf8(info.name) == deviceId)
            return i;
    }
#endif
    return -1;
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
        if (BASS_GetDeviceInfo(idx == -1 ? BASS_GetDevice() : idx, &info))
            m_activeOutputDeviceName = QString::fromUtf8(info.name);
        else
            m_activeOutputDeviceName = "System Default";
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
        if (info.flags & BASS_DEVICE_ENABLED)
            list.append(QPair<QString, QString>(QString::fromUtf8(info.name), QString::fromUtf8(info.driver)));
    }
#endif
    return list;
}

HSTREAM GaplessAudioEngine::createStream(const QString &filePath)
{
#ifdef MUSICPLAYER_HAS_BASS
    HSTREAM decodeStream = BASS_StreamCreateFile(FALSE, filePath.utf16(), 0, 0,
        BASS_STREAM_DECODE | BASS_UNICODE | BASS_SAMPLE_FLOAT);
    if (!decodeStream) {
        qWarning() << "BASS_StreamCreateFile failed:" << BASS_ErrorGetCode();
        return 0;
    }
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
    applyPlaybackRate(stream);
    setupEq(stream);
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::applyVolume(HSTREAM stream)
{
    Q_UNUSED(stream)
}

void GaplessAudioEngine::applyPlaybackRate(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (stream) {
        // BASS_ATTRIB_TEMPO_FREQ expects absolute frequency in Hz.
        float nativeFreq = 44100.0f;
        BASS_CHANNELINFO info;
        if (BASS_ChannelGetInfo(stream, &info)) {
            nativeFreq = static_cast<float>(info.freq);
        }

        float freq = nativeFreq * m_playbackRate;
        BASS_ChannelSetAttribute(stream, BASS_ATTRIB_TEMPO_FREQ, freq);
        
        // Disable tempo (time-stretching) to achieve the "tape-style" resampled sound
        BASS_ChannelSetAttribute(stream, BASS_ATTRIB_TEMPO, 0); 
        
        // Add Reverb if slowed down (Slowed + Reverb vibe)
        FxState &fx = m_streamFx[stream];
        if (m_playbackRate < 0.95f) {
            if (!fx.reverb) fx.reverb = BASS_ChannelSetFX(stream, BASS_FX_BFX_FREEVERB, 2);
            if (fx.reverb) {
                BASS_BFX_FREEVERB rv;
                rv.fDryMix = 0.9f;
                rv.fWetMix = (1.0f - m_playbackRate) * 0.4f;
                rv.fRoomSize = 0.75f;
                rv.fDamp = 0.4f;
                rv.fWidth = 1.0f;
                rv.lMode = 0;
                rv.lChannel = BASS_BFX_CHANALL;
                BASS_FXSetParameters(fx.reverb, &rv);
            }
        } else if (fx.reverb) {
            BASS_ChannelRemoveFX(stream, fx.reverb);
            fx.reverb = 0;
        }
    }
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::setupEq(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (!stream || !m_equalizer) return;
    HFX hEq = BASS_ChannelSetFX(stream, BASS_FX_BFX_PEAKEQ, 0);
    HFX hVol = BASS_ChannelSetFX(stream, BASS_FX_BFX_VOLUME, 1);
    if (hEq && hVol) {
        m_streamFx[stream] = {hEq, hVol};
        updateEq(stream);
    }
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::updateEq(HSTREAM stream)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (!stream || !m_equalizer) return;
    FxState fx = m_streamFx.value(stream);
    if (!fx.eq || !fx.vol) return;

    BASS_BFX_PEAKEQ p;
    p.fBandwidth = 1.0f; 
    p.fQ = 0.0f;
    p.lChannel = BASS_BFX_CHANALL;

    for (int i = 0; i < EQ_BAND_COUNT; ++i) {
        p.lBand = i;
        p.fCenter = static_cast<float>(EQ_FREQUENCIES[i]);
        p.fGain = m_equalizer->isEnabled() ? static_cast<float>(m_equalizer->bandGain(i)) : 0.0f;
        BASS_FXSetParameters(fx.eq, &p);
    }

    float totalGainDb = m_equalizer->isEnabled() ? (m_equalizer->preamp() + m_equalizer->autoLevelDb()) : 0.0f;
    float linearGain = std::pow(10.0f, totalGainDb / 20.0f);
    
    BASS_BFX_VOLUME v;
    v.lChannel = BASS_BFX_CHANALL;
    v.fVolume = linearGain;
    BASS_FXSetParameters(fx.vol, &v);
#else
    Q_UNUSED(stream)
#endif
}

void GaplessAudioEngine::applyEqToAll()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_activeStream) updateEq(m_activeStream);
    if (m_nextStream) updateEq(m_nextStream);
#endif
}

void GaplessAudioEngine::setEqualizer(Equalizer *eq)
{
    if (m_equalizer) disconnect(m_equalizer, nullptr, this, nullptr);
    m_equalizer = eq;
    if (m_equalizer) {
        connect(m_equalizer, &Equalizer::bandsChanged, this, &GaplessAudioEngine::applyEqToAll);
        applyEqToAll();
    }
}

void GaplessAudioEngine::updateState(State newState)
{
    if (m_state != newState) {
        qDebug() << "[bass] State change:" << engineStateToString(m_state) << "->" << engineStateToString(newState);
        m_state = newState;
        if (m_state == Playing)
            m_positionTimer->start();
        else
            m_positionTimer->stop();
        emit stateChanged(m_state);
    }
}

void GaplessAudioEngine::play(const QString &filePath)
{
#ifdef MUSICPLAYER_HAS_BASS
    if (m_mixer) {
        HSTREAM sources[32];
        int count = BASS_Mixer_StreamGetChannels(m_mixer, sources, 32);
        for (int i = 0; i < count; i++) {
            m_streamFx.remove(sources[i]);
            BASS_Mixer_ChannelRemove(sources[i]);
        }
    }

    if (m_activeStream) {
        m_streamFx.remove(m_activeStream);
        BASS_StreamFree(m_activeStream);
        m_activeStream = 0;
    }
    if (m_nextStream) {
        m_streamFx.remove(m_nextStream);
        BASS_StreamFree(m_nextStream);
        m_nextStream = 0;
    }

    m_endSync = 0;
    m_crossfadeSync = 0;
    m_transitionPending = false;

    m_activeStream = createStream(filePath);
    if (!m_activeStream) return;

    setupStream(m_activeStream);
    m_currentFilePath = filePath;

    if (!BASS_Mixer_StreamAddChannel(m_mixer, m_activeStream,
            BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN | BASS_STREAM_AUTOFREE | BASS_MIXER_CHAN_BUFFER)) {
        BASS_StreamFree(m_activeStream);
        m_activeStream = 0;
        return;
    }

    BASS_ChannelSetAttribute(m_activeStream, BASS_ATTRIB_VOL, 1.0f);
    BASS_Mixer_ChannelSetPosition(m_activeStream, 0, BASS_POS_BYTE | BASS_POS_MIXER_RESET);

    m_endSync = BASS_ChannelSetSync(m_activeStream,
        BASS_SYNC_END | BASS_SYNC_MIXTIME, 0, AudioEndSyncProc, this);

    if (m_crossfadeDurationMs > 0) {
        qint64 lenBytes = BASS_ChannelGetLength(m_activeStream, BASS_POS_BYTE);
        double lenSecs = BASS_ChannelBytes2Seconds(m_activeStream, lenBytes);
        double crossfadeSecs = m_crossfadeDurationMs / 1000.0;
        if (lenSecs > crossfadeSecs * 2) {
            qint64 syncBytes = BASS_ChannelSeconds2Bytes(m_activeStream, lenSecs - crossfadeSecs);
            m_crossfadeSync = BASS_ChannelSetSync(m_activeStream,
                BASS_SYNC_POS | BASS_SYNC_MIXTIME, syncBytes, CrossfadeSyncProc, this);
        }
    }

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
        m_streamFx.remove(m_nextStream);
        BASS_StreamFree(m_nextStream);
        m_nextStream = 0;
    }
    if (!filePath.isEmpty()) {
        m_nextStream = createStream(filePath);
        if (m_nextStream) {
            setupStream(m_nextStream);
            if (m_crossfadeDurationMs > 0)
                BASS_ChannelSetAttribute(m_nextStream, BASS_ATTRIB_VOL, 0.0f);
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
    if (m_mixer) {
        HSTREAM sources[32];
        int count = BASS_Mixer_StreamGetChannels(m_mixer, sources, 32);
        for (int i = 0; i < count; i++) BASS_Mixer_ChannelRemove(sources[i]);
    }
    if (m_activeStream) BASS_StreamFree(m_activeStream);
    if (m_nextStream) BASS_StreamFree(m_nextStream);
    m_activeStream = 0;
    m_nextStream = 0;
    m_streamFx.clear();
    m_endSync = 0;
    m_crossfadeSync = 0;
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
        BASS_Mixer_ChannelSetPosition(m_activeStream, posBytes, BASS_POS_BYTE | BASS_POS_MIXER_RESET);
    }
#else
    Q_UNUSED(positionMs)
#endif
}

void GaplessAudioEngine::setVolume(float vol)
{
    m_volume = qBound(0.0f, vol, 1.0f);
#ifdef MUSICPLAYER_HAS_BASS
    if (m_mixer)
        BASS_ChannelSetAttribute(m_mixer, BASS_ATTRIB_VOL, m_volume);
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
        if (posBytes == (qint64)-1) return 0;
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
        if (BASS_Mixer_ChannelIsActive(m_activeStream) == BASS_ACTIVE_STOPPED) {
            qint64 pos = position();
            qint64 dur = duration();
            if (dur > 0 && pos < dur - 100) {
                qDebug() << "[bass] Active stream stopped prematurely at" << pos << "/" << dur << "- attempting recovery or stop";
                if (!m_nextStream) {
                    updateState(Stopped);
                    emit playbackFinished();
                    return;
                }
            }
            if (m_nextStream) {
                processPendingTransitions();
            } else {
                updateState(Stopped);
                emit playbackFinished();
                return;
            }
        }

        emit positionChanged(position());

        DWORD level = BASS_Mixer_ChannelGetLevel(m_activeStream);
        if (level != (DWORD)-1) {
            int left = LOWORD(level);
            int right = HIWORD(level);
            float avgLevel = (left + right) / (2.0f * 32768.0f);
            emit currentAudioLevel(avgLevel);
            emit bassLevel(avgLevel * 1.2f);
        }
    }
#endif
}

void CALLBACK GaplessAudioEngine::CrossfadeSyncProc(HSYNC handle, unsigned long channel, unsigned long data, void *user)
{
    Q_UNUSED(handle)
    Q_UNUSED(data)
#ifdef MUSICPLAYER_HAS_BASS
    GaplessAudioEngine *engine = static_cast<GaplessAudioEngine*>(user);
    if (!engine || !engine->m_nextStream) return;
    
    BASS_Mixer_StreamAddChannel(engine->m_mixer, engine->m_nextStream,
        BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN | BASS_STREAM_AUTOFREE | BASS_MIXER_CHAN_BUFFER);
    
    BASS_ChannelSlideAttribute(channel, BASS_ATTRIB_VOL, 0.0f, engine->m_crossfadeDurationMs);
    BASS_ChannelSetAttribute(engine->m_nextStream, BASS_ATTRIB_VOL, 0.0f);
    BASS_ChannelSlideAttribute(engine->m_nextStream, BASS_ATTRIB_VOL, 1.0f, engine->m_crossfadeDurationMs);
    
    engine->m_transitionPending = true;
    QMetaObject::invokeMethod(engine, "processPendingTransitions", Qt::QueuedConnection);
#else
    Q_UNUSED(channel)
    Q_UNUSED(user)
#endif
}

void GaplessAudioEngine::processPendingTransitions()
{
#ifdef MUSICPLAYER_HAS_BASS
    if (!m_nextStream) return;

    m_streamFx.remove(m_activeStream);
    m_activeStream = m_nextStream;
    m_nextStream = 0;
    m_currentFilePath = m_preparedNextPath;
    m_preparedNextPath.clear();

    m_endSync = BASS_ChannelSetSync(m_activeStream,
        BASS_SYNC_END, 0, AudioEndSyncProc, this);

    if (m_crossfadeDurationMs > 0) {
        qint64 lenBytes = BASS_ChannelGetLength(m_activeStream, BASS_POS_BYTE);
        double lenSecs = BASS_ChannelBytes2Seconds(m_activeStream, lenBytes);
        double crossfadeSecs = m_crossfadeDurationMs / 1000.0;
        if (lenSecs > crossfadeSecs * 2) {
            qint64 syncBytes = BASS_ChannelSeconds2Bytes(m_activeStream, lenSecs - crossfadeSecs);
            m_crossfadeSync = BASS_ChannelSetSync(m_activeStream,
                BASS_SYNC_POS | BASS_SYNC_MIXTIME, syncBytes, CrossfadeSyncProc, this);
        }
    }

    emit trackTransitioned();
    emit durationChanged(duration());
#endif
}
