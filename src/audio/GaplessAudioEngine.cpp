#include "GaplessAudioEngine.h"
#include "Equalizer.h"
#include "FfmpegDecoderBackend.h"
#ifdef Q_OS_WIN
#include "WasapiAudioOutputBackend.h"
#endif
#include <QDateTime>
#include <QDebug>
#include <utility>

namespace {
constexpr qint64 kOutputBufferUsNoEq = 150000; // 150 ms buffer - smooth playback
constexpr qint64 kOutputBufferUsEq = 200000;   // 200 ms buffer for EQ mode
constexpr qint64 kDecoderHighWaterUs = 400000; // 400 ms decoder buffer
constexpr qint64 kDecoderLowWaterUs = 100000;  // 100 ms low water mark
constexpr int kNextBufferWaitTimeoutMs = 500;
constexpr int kOutputSwitchFadeFrames = 512;
constexpr int kPendingBacklogMaxBytes = 4 * 1024 * 1024;
constexpr int kCrossfadeMinMs = 0;
constexpr int kCrossfadeMaxMs = 8000;
constexpr int kSeekBackoffMaxAttempts = 3;
constexpr qint64 kSeekNearEndTargetSlackMs = 3000;
constexpr qint64 kSeekNearEndDecoderSlackMs = 1200;
constexpr qint64 kSeekEarlyRegionMs = 3000;
constexpr qint64 kSeekEarlyWrongLandingSlackMs = 1200;
constexpr qint64 kSeekStartTargetWindowMs = 150;
constexpr qint64 kSeekStartWrongLandingMs = 700;

const char *stateToString(GaplessAudioEngine::State state)
{
    switch (state) {
    case GaplessAudioEngine::Stopped:
        return "Stopped";
    case GaplessAudioEngine::Playing:
        return "Playing";
    case GaplessAudioEngine::Paused:
        return "Paused";
    }
    return "Unknown";
}

quint64 nextSeekTraceId()
{
    static quint64 s_seekTraceId = 0;
    return ++s_seekTraceId;
}
}

// ========== AudioBuffer ==========

void AudioBuffer::write(const char *data, qint64 len)
{
    if (!data || len <= 0) return;
    QMutexLocker lock(&m_mutex);
    m_buf.append(data, len);
}

qint64 AudioBuffer::read(char *data, qint64 maxLen)
{
    QMutexLocker lock(&m_mutex);
    qint64 avail = m_buf.size() - m_readPos;
    if (avail <= 0 || maxLen <= 0) return 0;

    qint64 toRead = qMin(maxLen, avail);
    memcpy(data, m_buf.constData() + m_readPos, toRead);
    m_readPos += toRead;

    if (m_readPos > 128 * 1024) {
        m_buf.remove(0, m_readPos);
        m_readPos = 0;
    }

    return toRead;
}

qint64 AudioBuffer::size() const
{
    QMutexLocker lock(&m_mutex);
    return m_buf.size() - m_readPos;
}

void AudioBuffer::clear()
{
    QMutexLocker lock(&m_mutex);
    m_buf.clear();
    m_readPos = 0;
}

void AudioBuffer::swap(AudioBuffer &other)
{
    if (this == &other)
        return;

    QMutexLocker thisLock(&m_mutex);
    QMutexLocker otherLock(&other.m_mutex);
    m_buf.swap(other.m_buf);
    std::swap(m_readPos, other.m_readPos);
}

// ========== GaplessAudioEngine ==========

GaplessAudioEngine::GaplessAudioEngine(QObject *parent)
    : QObject(parent)
{
    m_format.setSampleRate(44100);
    m_format.setChannelCount(2);
    m_format.setSampleFormat(QAudioFormat::Float);

    rebuildDecoderBackend();

    m_positionTimer = new QTimer(this);
    m_positionTimer->setInterval(50);
    connect(m_positionTimer, &QTimer::timeout, this, &GaplessAudioEngine::onPositionTick);

    m_feedTimer = new QTimer(this);
    m_feedTimer->setInterval(5);
    connect(m_feedTimer, &QTimer::timeout, this, &GaplessAudioEngine::feedSink);

    rebuildOutputBackend();
}

GaplessAudioEngine::~GaplessAudioEngine()
{
    // Do not emit stateChanged/playback signals during QObject teardown.
    disconnect();

    if (m_positionTimer)
        m_positionTimer->stop();
    if (m_feedTimer)
        m_feedTimer->stop();

    if (m_decoder) {
        m_decoder->disconnect(this);
        m_decoder->stop();
    }

    clearPreparedNext();
    destroyOutput();
}

bool GaplessAudioEngine::formatsMatch(const QAudioFormat &a, const QAudioFormat &b) const
{
    return a.sampleRate() == b.sampleRate()
        && a.channelCount() == b.channelCount()
        && a.sampleFormat() == b.sampleFormat();
}

QString GaplessAudioEngine::backendQtId()
{
    return QStringLiteral("qt");
}

QString GaplessAudioEngine::decoderQtId()
{
    return QStringLiteral("qt-decoder");
}

QString GaplessAudioEngine::decoderFfmpegId()
{
    return QStringLiteral("ffmpeg-decoder");
}

QString GaplessAudioEngine::backendWasapiSharedId()
{
    return QStringLiteral("wasapi-shared");
}

QString GaplessAudioEngine::backendWasapiExclusiveId()
{
    return QStringLiteral("wasapi-exclusive");
}

QString GaplessAudioEngine::backendWasapiCustomId()
{
    return backendWasapiSharedId();
}

QString GaplessAudioEngine::normalizeBackendId(const QString &backendId) const
{
    const QString normalized = backendId.trimmed().toLower();
    if (normalized == QStringLiteral("qt")
        || normalized == QStringLiteral("qtmultimedia")
        || normalized == QStringLiteral("qt multimedia")
        || normalized == QStringLiteral("wasapi-custom")
        || normalized == backendWasapiSharedId()) {
        return backendWasapiSharedId();
    }
    if (normalized == backendWasapiExclusiveId())
        return backendWasapiExclusiveId();
    return backendWasapiSharedId();
}

QString GaplessAudioEngine::activeBackendDisplayName() const
{
    if (m_activeBackendId == backendWasapiExclusiveId())
        return QStringLiteral("Custom WASAPI Exclusive");
    return QStringLiteral("Custom WASAPI Shared");
}

QString GaplessAudioEngine::normalizeDecoderId(const QString &decoderId) const
{
    const QString normalized = decoderId.trimmed().toLower();
    if (normalized == QStringLiteral("qt")
        || normalized == QStringLiteral("qt-decoder")
        || normalized == QStringLiteral("ffmpeg")
        || normalized == QStringLiteral("ffmpeg-decoder")) {
        return decoderFfmpegId();
    }
    return decoderFfmpegId();
}

QString GaplessAudioEngine::activeDecoderDisplayName() const
{
    return QStringLiteral("FFmpeg Decoder");
}

AudioDecoderBackend *GaplessAudioEngine::createDecoderBackend()
{
    return new FfmpegDecoderBackend(m_format, this);
}

void GaplessAudioEngine::connectDecoderSignals()
{
    if (!m_decoder)
        return;

    connect(m_decoder, &AudioDecoderBackend::audioBufferReceived,
            this, &GaplessAudioEngine::onDecoderBufferReady);
    connect(m_decoder, &AudioDecoderBackend::endOfStream,
            this, &GaplessAudioEngine::onDecoderEndOfStream);
    connect(m_decoder, &AudioDecoderBackend::durationChanged,
            this, &GaplessAudioEngine::onDecoderDurationChanged);
}

void GaplessAudioEngine::rebuildDecoderBackend()
{
    clearPreparedNext();

    if (m_decoder) {
        m_decoder->disconnect(this);
        delete m_decoder;
        m_decoder = nullptr;
    }

    m_decoderPreferenceId = normalizeDecoderId(m_decoderPreferenceId);
    m_decoder = createDecoderBackend();
    if (m_decoder)
        m_decoder->setPlaybackRate(m_playbackRate);

    connectDecoderSignals();

    const QString newActive = decoderFfmpegId();
    const bool changed = (m_activeDecoderId != newActive);
    m_activeDecoderId = newActive;
    if (changed)
        emit decoderChanged();
}

void GaplessAudioEngine::clearPreparedNext()
{
    m_nextBuf.clear();
    m_nextSourceEnded = false;
    m_nextDuration = 0;
    m_preparedNextPath.clear();
    m_waitingForNextBuffer = false;
    m_nextBufferWaitTimer.invalidate();
    m_crossfadeActive = false;
    m_crossfadePendingPromotion = false;
    m_crossfadeFramesTotal = 0;
    m_crossfadeFramesDone = 0;
    m_gaplessTailBytes = -1;
    m_gaplessTailPrev = -1;

    if (m_nextDecoder) {
        m_nextDecoder->disconnect(this);
        m_nextDecoder->stop();
        delete m_nextDecoder;
        m_nextDecoder = nullptr;
    }
}

void GaplessAudioEngine::beginPrepareNextDecoder()
{
    if (m_nextFilePath.isEmpty() || !m_decoder)
        return;

    if (m_nextDecoder && m_preparedNextPath == m_nextFilePath)
        return;

    clearPreparedNext();
    m_preparedNextPath = m_nextFilePath;

    m_nextDecoder = createDecoderBackend();
    if (!m_nextDecoder) {
        m_preparedNextPath.clear();
        return;
    }
    m_nextDecoder->setPlaybackRate(m_playbackRate);

    connect(m_nextDecoder, &AudioDecoderBackend::audioBufferReceived,
            this, &GaplessAudioEngine::onNextDecoderBufferReady);
    connect(m_nextDecoder, &AudioDecoderBackend::endOfStream,
            this, &GaplessAudioEngine::onNextDecoderEndOfStream);
    connect(m_nextDecoder, &AudioDecoderBackend::durationChanged,
            this, &GaplessAudioEngine::onNextDecoderDurationChanged);

    if (!m_nextDecoder->openSource(m_nextFilePath)) {
        qWarning() << "Failed to preload next track in strict custom mode:" << m_nextFilePath;
        clearPreparedNext();
        return;
    }

    m_nextDecoder->play();
    if (m_state == Paused)
        m_nextDecoder->pause();
}

void GaplessAudioEngine::promotePreparedNextDecoder()
{
    if (!m_nextDecoder)
        return;

    const QString previousPath = m_currentFilePath;

    if (m_decoder) {
        m_decoder->disconnect(this);
        m_decoder->stop();
        delete m_decoder;
        m_decoder = nullptr;
    }

    m_nextDecoder->disconnect(this);
    m_decoder = m_nextDecoder;
    m_nextDecoder = nullptr;
    connectDecoderSignals();

    const bool promotedDecoderEnded = m_nextSourceEnded;

    m_currentBuf.swap(m_nextBuf);
    m_nextBuf.clear();
    // Preserve EOS state from preloaded decoder.
    // If it already ended before promotion, feedSink must still finalize/advance
    // once buffered PCM is drained.
    m_sourceEnded = promotedDecoderEnded;
    m_nextSourceEnded = false;

    const QString promotedPath = m_nextFilePath;
    m_currentFilePath = promotedPath;
    m_nextFilePath.clear();
    m_preparedNextPath.clear();

    const qint64 nextDuration = m_nextDuration;
    m_nextDuration = 0;

    m_pendingPcm.clear();
    m_playbackFinishedEmitted = false;
    m_needEqPrebuffer = false;
    m_waitingForNextBuffer = false;
    m_nextBufferWaitTimer.invalidate();
    m_crossfadeActive = false;
    m_crossfadePendingPromotion = false;
    m_crossfadeFramesTotal = 0;
    m_crossfadeFramesDone = 0;

    // Align transition start so position() begins from ~0 even if the promoted
    // decoder has already pre-decoded a large buffered chunk.
    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const int sampleRate = qMax(1, m_format.sampleRate());
    qint64 queuedBytes = m_currentBuf.size();
    if (m_output && m_output->isOpen())
        queuedBytes += qMax<qint64>(0, m_output->bytesQueued());
    queuedBytes -= (queuedBytes % frameBytes);
    const qint64 queuedFrames = queuedBytes / frameBytes;
    const qint64 queuedMs = (queuedFrames * 1000LL) / sampleRate;
    const qint64 decoderPosMs = m_decoder ? qMax<qint64>(0, m_decoder->position()) : 0;
    m_decoderPositionBaseMs = decoderPosMs - queuedMs;

    m_duration = nextDuration;

    if (m_state == Playing)
        m_decoder->play();
    else if (m_state == Paused)
        m_decoder->pause();

    m_pendingTrackTransitioned = true;
    m_pendingNextDuration = m_duration;
    markAudibleTransition(previousPath, promotedPath, "gapless-promote");

    if (!m_audibleTransitionPending)
        finalizePendingTrackTransition();
}

void GaplessAudioEngine::setDecoderPreferenceId(const QString &decoderId)
{
    const QString normalized = normalizeDecoderId(decoderId);
    if (m_decoderPreferenceId == normalized)
        return;

    const QString currentFile = m_currentFilePath;
    const qint64 currentPos = position();
    const bool wasPlaying = (m_state == Playing);
    const bool wasPaused = (m_state == Paused);

    m_decoderPreferenceId = normalized;
    rebuildDecoderBackend();

    if ((wasPlaying || wasPaused) && !currentFile.isEmpty()) {
        play(currentFile);
        if (currentPos > 0)
            seek(currentPos);
        if (wasPaused)
            pause();
    }

    emit decoderChanged();
}

void GaplessAudioEngine::setOutputDevicePreferenceId(const QString &deviceId)
{
    const QString normalized = deviceId.trimmed();
    if (m_outputDevicePreferenceId == normalized)
        return;

    m_outputDevicePreferenceId = normalized;
    if (m_output)
        m_output->setPreferredDeviceId(m_outputDevicePreferenceId);

    const bool wasPaused = (m_state == Paused);
    const bool wasActive = (m_state == Playing || m_state == Paused);
    if (wasActive) {
        destroyOutput();
        ensureSinkRunning();
        if (wasPaused && m_output)
            m_output->suspend();
    } else {
        syncActiveOutputDeviceInfo();
    }

    emit outputDeviceChanged();
}

bool GaplessAudioEngine::canSelectOutputDevice() const
{
    return m_activeBackendId == backendWasapiSharedId()
        || m_activeBackendId == backendWasapiExclusiveId();
}

QVector<QPair<QString, QString>> GaplessAudioEngine::availableOutputDevices() const
{
    if (!m_output || !m_output->supportsDeviceSelection())
        return {};
    return m_output->availableOutputDevices();
}

void GaplessAudioEngine::syncActiveOutputDeviceInfo()
{
    QString id;
    QString name = QStringLiteral("System Default");
    if (m_output) {
        id = m_output->activeDeviceId();
        const QString backendName = m_output->activeDeviceName();
        if (!backendName.isEmpty())
            name = backendName;
    }

    if (id != m_activeOutputDeviceId || name != m_activeOutputDeviceName) {
        m_activeOutputDeviceId = id;
        m_activeOutputDeviceName = name;
        emit outputDeviceChanged();
    }
}

void GaplessAudioEngine::markAudibleTransition(const QString &fromPath,
                                               const QString &toPath,
                                               const char *reason)
{
    if (toPath.isEmpty())
        return;

    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const int sampleRate = qMax(1, m_format.sampleRate());

    qint64 queuedBytes = 0;
    if (m_output && m_output->isOpen())
        queuedBytes = qMax<qint64>(0, m_output->bytesQueued());

    queuedBytes -= (queuedBytes % frameBytes);
    const qint64 queuedFrames = queuedBytes / frameBytes;
    const qint64 queuedMs = (queuedFrames * 1000LL) / sampleRate;

    m_audibleTransitionPending = true;
    m_audibleTransitionMarkedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_audibleTransitionEstimatedDelayMs = queuedMs;
    m_audibleTransitionFromPath = fromPath;
    m_audibleTransitionToPath = toPath;

    qInfo() << "[gapless-audible] mark"
            << "reason=" << reason
            << "from=" << fromPath
            << "to=" << toPath
            << "markWallMs=" << m_audibleTransitionMarkedAtMs
            << "queuedBytesAtMark=" << queuedBytes
            << "estimatedDelayMs=" << queuedMs;

    if (queuedMs <= 0) {
        qInfo() << "[gapless-audible] heard"
                << "from=" << fromPath
                << "to=" << toPath
                << "elapsedMs=" << 0
                << "expectedDelayMs=" << 0
                << "markWallMs=" << m_audibleTransitionMarkedAtMs
                << "detectedWallMs=" << m_audibleTransitionMarkedAtMs;
        m_audibleTransitionPending = false;

        finalizePendingTrackTransition();
    }
}

void GaplessAudioEngine::updateAudibleTransitionTrace()
{
    if (!m_audibleTransitionPending)
        return;

    if (m_state != Playing)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = qMax<qint64>(0, nowMs - m_audibleTransitionMarkedAtMs);
    if (elapsedMs < m_audibleTransitionEstimatedDelayMs)
        return;

    qInfo() << "[gapless-audible] heard"
            << "from=" << m_audibleTransitionFromPath
            << "to=" << m_audibleTransitionToPath
            << "elapsedMs=" << elapsedMs
            << "expectedDelayMs=" << m_audibleTransitionEstimatedDelayMs
            << "markWallMs=" << m_audibleTransitionMarkedAtMs
            << "detectedWallMs=" << nowMs;

    m_audibleTransitionPending = false;

    finalizePendingTrackTransition();
}

void GaplessAudioEngine::finalizePendingTrackTransition()
{
    if (!m_pendingTrackTransitioned)
        return;

    const qint64 pendingDuration = m_pendingNextDuration;
    m_pendingTrackTransitioned = false;
    m_pendingNextDuration = 0;

    // Пересчитываем base на текущий момент чтобы position() вернул ~0
    if (m_decoder) {
        const int frameBytes = qMax(1, m_format.bytesPerFrame());
        const int sampleRate = qMax(1, m_format.sampleRate());
        qint64 queuedBytes = m_currentBuf.size() + m_pendingPcm.size();
        if (m_output && m_output->isOpen())
            queuedBytes += qMax<qint64>(0, m_output->bytesQueued());
        queuedBytes -= (queuedBytes % frameBytes);
        const qint64 queuedFrames = queuedBytes / frameBytes;
        const qint64 queuedMs = (queuedFrames * 1000LL) / sampleRate;
        const qint64 decoderPosMs = qMax<qint64>(0, m_decoder->position());
        m_decoderPositionBaseMs = decoderPosMs - queuedMs;
    }

    emit trackTransitioned();
    if (pendingDuration > 0)
        emit durationChanged(pendingDuration);
}

void GaplessAudioEngine::setBackendPreferenceId(const QString &backendId)
{
    const QString normalized = normalizeBackendId(backendId);
    if (m_backendPreferenceId == normalized)
        return;

    m_backendPreferenceId = normalized;
    rebuildOutputBackend();

    m_pendingPcm.clear();
    m_needEqPrebuffer = true;

    if (m_state == Playing || m_state == Paused) {
        ensureSinkRunning();
        if (m_state == Paused && m_output)
            m_output->suspend();
    }

    emit backendChanged();
}

void GaplessAudioEngine::rebuildOutputBackend()
{
    if (m_output)
        m_output->close();

    m_backendPreferenceId = normalizeBackendId(m_backendPreferenceId);
    const QString requested = m_backendPreferenceId;
#ifdef Q_OS_WIN
    if (requested == backendWasapiExclusiveId()) {
        m_output = std::make_unique<WasapiAudioOutputBackend>(WasapiAudioOutputBackend::Mode::Exclusive);
    } else {
        m_output = std::make_unique<WasapiAudioOutputBackend>(WasapiAudioOutputBackend::Mode::Shared);
    }
#else
    qWarning() << "Custom WASAPI backend is only available on Windows.";
    m_output.reset();
#endif

    if (m_output)
        m_output->setPreferredDeviceId(m_outputDevicePreferenceId);

    const QString newActive = requested;
    const bool activeChanged = (m_activeBackendId != newActive);
    m_activeBackendId = newActive;
    if (activeChanged)
        emit backendChanged();

    syncActiveOutputDeviceInfo();
}

void GaplessAudioEngine::updateState(State newState)
{
    if (m_state == newState) return;
    m_state = newState;
    emit stateChanged(m_state);
}

void GaplessAudioEngine::destroyOutput()
{
    if (m_output)
        m_output->close();
}

void GaplessAudioEngine::ensureSinkRunning()
{
    if (!m_output)
        rebuildOutputBackend();
    if (!m_output)
        return;

    const bool eqOn = (m_equalizer && m_equalizer->isEnabled());
    const qint64 targetBufferUs = eqOn ? kOutputBufferUsEq : kOutputBufferUsNoEq;
    const int targetBufferBytes = static_cast<int>(m_format.bytesForDuration(targetBufferUs));

    if (!m_output->isOpen()) {
        if (!m_output->open(m_format, targetBufferBytes)) {
            qWarning() << "Custom WASAPI backend open failed for"
                       << m_activeBackendId
                       << ". Playback cannot continue in strict custom mode.";
            return;
        }
        m_sinkEqMode = eqOn;
        m_outputFadeFramesRemaining = kOutputSwitchFadeFrames;
        syncActiveOutputDeviceInfo();
    } else if (m_sinkEqMode != eqOn) {
        const bool keepSuspended = (m_state == Paused);
        m_output->close();
        if (!m_output->open(m_format, targetBufferBytes))
            return;
        m_sinkEqMode = eqOn;
        m_needEqPrebuffer = eqOn;
        m_outputFadeFramesRemaining = kOutputSwitchFadeFrames;
        if (keepSuspended)
            m_output->suspend();
        syncActiveOutputDeviceInfo();
    }

    m_output->setVolume(m_volume);
}

void GaplessAudioEngine::resetStreamState()
{
    m_currentBuf.clear();
    m_pendingPcm.clear();
    m_sourceEnded = false;
    m_playbackFinishedEmitted = false;
    m_needEqPrebuffer = true;
    m_waitingForNextBuffer = false;
    m_nextBufferWaitTimer.invalidate();
    m_crossfadeActive = false;
    m_crossfadePendingPromotion = false;
    m_crossfadeFramesTotal = 0;
    m_crossfadeFramesDone = 0;
    m_inGaplessTransition = false;
    m_gaplessTransitionInitialBufMs = 0;
    m_gaplessTransitionDecoderOffsetMs = 0;
    m_decoderPositionBaseMs = 0;
    m_gaplessTailBytes = -1;
    m_gaplessTailPrev = -1;
    m_audibleTransitionPending = false;
    m_audibleTransitionMarkedAtMs = 0;
    m_audibleTransitionEstimatedDelayMs = 0;
    m_audibleTransitionFromPath.clear();
    m_audibleTransitionToPath.clear();
    m_pendingTrackTransitioned = false;
    m_pendingNextDuration = 0;
}

void GaplessAudioEngine::play(const QString &filePath)
{
    const bool wasPaused = (m_state == Paused);

    clearPreparedNext();
    m_currentFilePath = filePath;
    m_nextFilePath.clear();
    m_duration = 0;
    m_decoderPositionBaseMs = 0;
    m_seekTraceWaitingForFirstBuffer = false;
    m_seekTraceWaitingForFirstWrite = false;
    m_seekTraceWarnedNoWrite = false;
    m_seekTraceRecoveryAttempted = false;
    m_seekTraceReopenAttempted = false;
    m_seekTraceWrongStartRecoveryAttempted = false;
    m_seekTraceBackoffAttempts = 0;
    m_seekTraceImmediateEosCount = 0;
    m_seekTraceTargetMs = -1;
    m_seekTraceTimer.invalidate();

    resetStreamState();
    ensureSinkRunning();

    if (m_output && m_output->isOpen()) {
        if (wasPaused) {
            m_output->resetStream();
            m_outputFadeFramesRemaining = kOutputSwitchFadeFrames;
        } else {
            // Clear output buffer quickly for instant track switch
            m_output->resetStream();
            m_outputFadeFramesRemaining = kOutputSwitchFadeFrames;
            // Start output immediately
            if (m_output)
                m_output->resume();
        }
    }

    // Skip stop() - openSource handles cleanup internally and is faster
    // This avoids unnecessary seek/flush operations when switching tracks
    if (!m_decoder->openSource(filePath)) {
        qWarning() << "FFmpeg decoder failed to open source in strict custom mode:" << filePath;
        updateState(Stopped);
        return;
    }
    m_decoder->play();
    if (wasPaused && m_output)
        m_output->resume();
    emit positionChanged(0);

    m_positionTimer->start();
    m_feedTimer->start();
    updateState(Playing);
}

void GaplessAudioEngine::pause()
{
    if (m_state != Playing) return;

    m_decoder->pause();
    if (m_nextDecoder)
        m_nextDecoder->pause();
    if (m_output) m_output->suspend();

    m_positionTimer->stop();
    m_feedTimer->stop();
    updateState(Paused);
}

void GaplessAudioEngine::resume()
{
    if (m_state != Paused) return;

    ensureSinkRunning();
    if (m_output) m_output->resume();
    m_decoder->play();
    if (m_nextDecoder)
        m_nextDecoder->play();

    m_positionTimer->start();
    m_feedTimer->start();
    updateState(Playing);
}

void GaplessAudioEngine::stop()
{
    m_decoder->stop();
    clearPreparedNext();
    m_nextFilePath.clear();
    m_decoderPositionBaseMs = 0;
    m_seekTraceWaitingForFirstBuffer = false;
    m_seekTraceWaitingForFirstWrite = false;
    m_seekTraceWarnedNoWrite = false;
    m_seekTraceRecoveryAttempted = false;
    m_seekTraceReopenAttempted = false;
    m_seekTraceWrongStartRecoveryAttempted = false;
    m_seekTraceBackoffAttempts = 0;
    m_seekTraceImmediateEosCount = 0;
    m_seekTraceTargetMs = -1;
    m_seekTraceTimer.invalidate();
    resetStreamState();

    m_positionTimer->stop();
    m_feedTimer->stop();

    destroyOutput();

    m_duration = 0;
    updateState(Stopped);
}

void GaplessAudioEngine::seek(qint64 positionMs)
{
    if (m_state == Stopped) {
        qInfo() << "[seek-engine] ignored seek while stopped"
                << "requestedMs=" << positionMs
                << "file=" << m_currentFilePath;
        return;
    }

    if (!m_decoder) {
        qWarning() << "[seek-engine] seek ignored: decoder is null"
                   << "requestedMs=" << positionMs
                   << "state=" << stateToString(m_state)
                   << "file=" << m_currentFilePath;
        return;
    }

    const qint64 targetMs = qMax<qint64>(0, positionMs);
    const qint64 outputQueuedBefore = (m_output && m_output->isOpen())
        ? qMax<qint64>(0, m_output->bytesQueued())
        : -1;

    m_seekTraceId = nextSeekTraceId();
    m_seekTraceTargetMs = targetMs;
    m_seekTraceDecoderPositionBaseMs = (m_decoder ? m_decoder->position() : 0);
    m_seekTraceWaitingForFirstBuffer = true;
    m_seekTraceWaitingForFirstWrite = true;
    m_seekTraceWarnedNoWrite = false;
    m_seekTraceRecoveryAttempted = false;
    m_seekTraceReopenAttempted = false;
    m_seekTraceWrongStartRecoveryAttempted = false;
    m_seekTraceBackoffAttempts = 0;
    m_seekTraceImmediateEosCount = 0;
    m_seekTraceTimer.restart();

    qInfo() << "[seek-engine] start"
            << "id=" << m_seekTraceId
            << "requestedMs=" << positionMs
            << "targetMs=" << targetMs
            << "state=" << stateToString(m_state)
            << "enginePosBeforeMs=" << position()
            << "decoderPosBeforeMs=" << m_decoder->position()
            << "bufferedCurrentBytes=" << m_currentBuf.size()
            << "pendingBytes=" << m_pendingPcm.size()
            << "outputQueuedBytes=" << outputQueuedBefore
            << "file=" << m_currentFilePath;

    m_decoderPositionBaseMs = 0;
    resetStreamState();
    clearPreparedNext();

    if (m_equalizer)
        m_equalizer->prepareForSeek();

    bool outputResetOk = true;
    if (m_output) {
        outputResetOk = m_output->resetStream();
        m_outputFadeFramesRemaining = kOutputSwitchFadeFrames;
        // Ensure output is running after seek (it was stopped in resetStream)
        if (m_state == Playing)
            m_output->resume();
        else if (m_state == Paused)
            m_output->suspend();
    }

    qInfo() << "[seek-engine] reset-complete"
            << "id=" << m_seekTraceId
            << "outputResetOk=" << outputResetOk
            << "bufferedCurrentBytes=" << m_currentBuf.size()
            << "pendingBytes=" << m_pendingPcm.size();

    m_decoder->seek(targetMs);
    m_seekTraceDecoderPositionBaseMs = (m_decoder ? m_decoder->position() : 0);
    qInfo() << "[seek-engine] decoder-seek-dispatched"
            << "id=" << m_seekTraceId
            << "decoderPosAfterDispatchMs=" << m_decoder->position();

    if (m_state == Playing && m_decoder->state() != AudioDecoderBackend::State::Playing) {
        m_decoder->play();
        qInfo() << "[seek-engine] decoder resumed after seek"
                << "id=" << m_seekTraceId
                << "decoderState=" << static_cast<int>(m_decoder->state());
    }

    // Don't emit positionChanged here - the position timer will naturally emit
    // correct positions as audio buffers are decoded and fed to output.
    // Emitting immediately causes slider to show stale position briefly.
}

bool GaplessAudioEngine::attemptSeekBackoffRecovery(const char *reason)
{
    if (!m_decoder)
        return false;
    if (m_seekTraceBackoffAttempts >= kSeekBackoffMaxAttempts)
        return false;
    if (m_seekTraceTargetMs <= 0)
        return false;

    const int attempt = m_seekTraceBackoffAttempts + 1;
    const qint64 targetMs = m_seekTraceTargetMs;

    qWarning() << "[seek-engine] retry decoder seek without target shift"
               << "reason=" << reason
               << "seekId=" << m_seekTraceId
               << "attempt=" << attempt
               << "targetMs=" << targetMs;

    m_seekTraceBackoffAttempts = attempt;
    m_seekTraceImmediateEosCount = 0;
    m_seekTraceWarnedNoWrite = false;
    m_seekTraceWaitingForFirstBuffer = true;
    m_seekTraceWaitingForFirstWrite = true;
    m_seekTraceTimer.restart();
    m_seekTraceDecoderPositionBaseMs = (m_decoder ? m_decoder->position() : 0);

    m_sourceEnded = false;
    m_playbackFinishedEmitted = false;
    m_decoder->seek(targetMs);
    if (m_state == Playing)
        m_decoder->play();
    else if (m_state == Paused)
        m_decoder->pause();

    if (m_output)
        m_output->resetStream();

    return true;
}

void GaplessAudioEngine::setVolume(float vol)
{
    m_volume = vol;
    if (m_output)
        m_output->setVolume(vol);
}

void GaplessAudioEngine::setPlaybackRate(float rate)
{
    const float normalized = qBound(0.5f, rate, 2.0f);
    if (qAbs(m_playbackRate - normalized) < 0.0001f)
        return;

    m_playbackRate = normalized;

    if (m_decoder)
        m_decoder->setPlaybackRate(m_playbackRate);
    if (m_nextDecoder)
        m_nextDecoder->setPlaybackRate(m_playbackRate);

    // Resume decoder if it was throttled by backpressure.
    if (m_state == Playing && m_decoder
        && m_decoder->state() == AudioDecoderBackend::State::Paused)
        m_decoder->play();

    emit playbackRateChanged(m_playbackRate);
}

void GaplessAudioEngine::setCrossfadeDurationMs(int durationMs)
{
    const int normalized = qBound(kCrossfadeMinMs, durationMs, kCrossfadeMaxMs);
    if (m_crossfadeDurationMs == normalized)
        return;

    m_crossfadeDurationMs = normalized;
    m_crossfadeActive = false;
    m_crossfadePendingPromotion = false;
    m_crossfadeFramesTotal = 0;
    m_crossfadeFramesDone = 0;
}

void GaplessAudioEngine::applyOutputFade(char *data, qint64 bytes)
{
    if (!data || bytes <= 0)
        return;
    if (m_outputFadeFramesRemaining <= 0)
        return;
    if (m_format.sampleFormat() != QAudioFormat::Float)
        return;

    const int channels = qMax(1, m_format.channelCount());
    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const qint64 frames = bytes / frameBytes;
    float *samples = reinterpret_cast<float *>(data);

    for (qint64 f = 0; f < frames && m_outputFadeFramesRemaining > 0; ++f) {
        const int progressed = kOutputSwitchFadeFrames - m_outputFadeFramesRemaining + 1;
        const float gain = qBound(0.0f,
                                  progressed / static_cast<float>(kOutputSwitchFadeFrames),
                                  1.0f);
        for (int c = 0; c < channels; ++c)
            samples[f * channels + c] *= gain;
        --m_outputFadeFramesRemaining;
    }
}

void GaplessAudioEngine::prepareNext(const QString &filePath)
{
    m_nextFilePath = filePath.trimmed();
    if (m_nextFilePath.isEmpty()) {
        clearPreparedNext();
        return;
    }

    if (m_nextFilePath != m_preparedNextPath)
        clearPreparedNext();

    if (m_state == Playing || m_state == Paused)
        beginPrepareNextDecoder();
}

qint64 GaplessAudioEngine::position() const
{
    if (!m_decoder)
        return 0;

    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const int sampleRate = qMax(1, m_format.sampleRate());

    // Handle gapless transition: we're consuming pre-buffered audio from Song 2.
    // Position = consumed pre-buffered audio amount.
    // Normal position calculation
    const qint64 decodedMs = qMax<qint64>(0, m_decoder->position() - m_decoderPositionBaseMs);

    qint64 queuedBytes = m_currentBuf.size() + m_pendingPcm.size();
    qint64 outputQueuedBytes = 0;
    if (m_output && m_output->isOpen())
        outputQueuedBytes = m_output->bytesQueued();
    queuedBytes += qMax<qint64>(0, outputQueuedBytes);

    queuedBytes -= (queuedBytes % frameBytes);
    const qint64 queuedFrames = queuedBytes / frameBytes;
    const qint64 queuedMs = (queuedFrames * 1000LL) / sampleRate;

    const qint64 result = qMax<qint64>(0, decodedMs - queuedMs);

    // Pin to duration only after full drain; otherwise the slider can jump
    // to end while audible tail is still queued in output/backend buffers.
    if (m_sourceEnded && m_duration > 0 && m_nextFilePath.isEmpty() && queuedBytes <= 0)
        return m_duration;

    // Do not report a fully-finished UI position while any audio is still queued.
    if (m_duration > 0 && result >= m_duration && m_nextFilePath.isEmpty() && queuedBytes > 0)
        return qMax<qint64>(0, m_duration - 1);

    // Cap at duration
    if (m_duration > 0 && result > m_duration)
        return m_duration;

    return result;
}

void GaplessAudioEngine::onDecoderBufferReady(const QAudioBuffer &buffer)
{
    if (!buffer.isValid()) return;

    const QAudioFormat fmt = buffer.format();
    if (fmt.isValid() && !formatsMatch(fmt, m_format)) {
        m_format = fmt;
        destroyOutput();
        ensureSinkRunning();
        m_needEqPrebuffer = true;
        if (m_state == Paused && m_output)
            m_output->suspend();
    }

    // Ensure equalizer knows the correct sample rate
    if (m_equalizer)
        m_equalizer->setSampleRate(m_format.sampleRate());

    const char *data = buffer.constData<char>();
    const qint64 len = buffer.byteCount();
    if (len <= 0 || !data) return;

    const qint64 decoderPosMs = (m_decoder ? m_decoder->position() : -1);

    // DEBUG: log buffer during normal playback to catch jumps
    static int s_debugBufCount = 0;
    if (!m_seekTraceWaitingForFirstBuffer && !m_seekTraceWaitingForFirstWrite && m_state == Playing) {
        const qint64 enginePosMs = position();
        if (s_debugBufCount < 10) {
            qInfo() << "[audio-debug] buffer decoderPosMs=" << decoderPosMs
                    << "enginePosMs=" << enginePosMs << "bufferBytes=" << len
                    << "currentBufSize=" << m_currentBuf.size();
            ++s_debugBufCount;
        }
    }

    const bool seekNearStartRequested = (m_seekTraceTargetMs >= 0
                                         && m_seekTraceTargetMs <= kSeekStartTargetWindowMs);
    const bool seekInEarlyRegion = (m_seekTraceTargetMs >= 0
                                    && m_seekTraceTargetMs <= kSeekEarlyRegionMs);
    const qint64 wrongLandingSlackMs = seekNearStartRequested
        ? kSeekStartWrongLandingMs
        : kSeekEarlyWrongLandingSlackMs;
    const qint64 wrongLandingThresholdMs = qMax<qint64>(0, m_seekTraceTargetMs + wrongLandingSlackMs);
    if (m_seekTraceWaitingForFirstBuffer
        && seekInEarlyRegion
        && decoderPosMs > wrongLandingThresholdMs
        && !m_seekTraceWrongStartRecoveryAttempted
        && m_decoder) {
        m_seekTraceWrongStartRecoveryAttempted = true;

        const qint64 recoveryTargetMs = qMax<qint64>(0, m_seekTraceTargetMs);

        qWarning() << "[seek-engine] suspicious first buffer after early seek, forcing reseek"
                   << "id=" << m_seekTraceId
                   << "targetMs=" << m_seekTraceTargetMs
                   << "decoderPosMs=" << decoderPosMs
                   << "thresholdMs=" << wrongLandingThresholdMs;

        resetStreamState();
        if (m_output) {
            m_output->resetStream();
            m_outputFadeFramesRemaining = kOutputSwitchFadeFrames;
            if (m_state == Paused)
                m_output->suspend();
        }

        m_seekTraceWaitingForFirstBuffer = true;
        m_seekTraceWaitingForFirstWrite = true;
        m_seekTraceWarnedNoWrite = false;
        m_seekTraceImmediateEosCount = 0;
        m_seekTraceTimer.restart();
        m_seekTraceDecoderPositionBaseMs = (m_decoder ? m_decoder->position() : 0);

        m_decoder->seek(recoveryTargetMs);
        if (m_state == Playing)
            m_decoder->play();
        else if (m_state == Paused)
            m_decoder->pause();

        emit positionChanged(recoveryTargetMs);
        return;
    }

    if (m_seekTraceWaitingForFirstBuffer) {
        m_seekTraceWaitingForFirstBuffer = false;
        m_seekTraceImmediateEosCount = 0;
        qInfo() << "[seek-engine] first-decoder-buffer"
                << "id=" << m_seekTraceId
                << "bufferBytes=" << len
                << "elapsedMs=" << (m_seekTraceTimer.isValid() ? m_seekTraceTimer.elapsed() : -1)
                << "decoderPosMs=" << decoderPosMs
                << "enginePosMs=" << position();
    }

    m_currentBuf.write(data, len);
    m_playbackFinishedEmitted = false;

    // Backpressure: pause decoder if buffer exceeds high watermark
    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const int sampleRate = qMax(1, m_format.sampleRate());
    const qint64 bufferedUs = (m_currentBuf.size() * 1000000LL) / (frameBytes * sampleRate);
    if (bufferedUs > kDecoderHighWaterUs && m_decoder && m_decoder->state() == AudioDecoderBackend::State::Playing) {
        m_decoder->pause();
    }

    if (m_state == Playing)
        feedSink();
}

void GaplessAudioEngine::onDecoderStateChanged(AudioDecoderBackend::State state)
{
    Q_UNUSED(state);
}

void GaplessAudioEngine::onDecoderEndOfStream()
{
    const qint64 seekElapsedMs = m_seekTraceTimer.isValid() ? m_seekTraceTimer.elapsed() : -1;
    const bool inEarlyPostSeekWindow = (seekElapsedMs >= 0 && seekElapsedMs < 1200);
    const bool seekTargetKnown = (m_seekTraceTargetMs >= 0);
    const qint64 decoderPosMs = (m_decoder ? m_decoder->position() : -1);
    const bool decoderAtOrNearEnd = (decoderPosMs >= 0
                                     && m_duration > 0
                                     && (decoderPosMs + kSeekNearEndDecoderSlackMs >= m_duration));
    bool seekTargetNotNearEnd = !seekTargetKnown
        || m_duration <= 0
        || (m_seekTraceTargetMs + kSeekNearEndTargetSlackMs < m_duration);
    if (decoderAtOrNearEnd)
        seekTargetNotNearEnd = false;
    const bool inPostSeekGuardWindow = (seekElapsedMs >= 0 && seekElapsedMs < 5000);

    if (m_seekTraceWaitingForFirstBuffer)
        ++m_seekTraceImmediateEosCount;
    else
        m_seekTraceImmediateEosCount = 0;

    qInfo() << "[seek-engine] decoder EOS"
            << "seekId=" << m_seekTraceId
            << "seekElapsedMs=" << seekElapsedMs
            << "waitingFirstBuffer=" << m_seekTraceWaitingForFirstBuffer
            << "seekTargetMs=" << m_seekTraceTargetMs
            << "durationMs=" << m_duration
            << "decoderPosMs=" << (m_decoder ? m_decoder->position() : -1)
            << "state=" << stateToString(m_state)
            << "nextPrepared=" << (!m_nextFilePath.isEmpty());

    if (inEarlyPostSeekWindow && seekTargetNotNearEnd) {
        qWarning() << "[seek-engine] suppressing suspicious EOS shortly after seek"
                   << "seekId=" << m_seekTraceId
                   << "seekElapsedMs=" << seekElapsedMs
                   << "seekTargetMs=" << m_seekTraceTargetMs
                   << "durationMs=" << m_duration;

        if (!m_seekTraceRecoveryAttempted && m_decoder) {
            m_seekTraceRecoveryAttempted = true;
            const qint64 retryTargetMs = qMax<qint64>(0, m_seekTraceTargetMs);
            qWarning() << "[seek-engine] retry decoder seek after suspicious EOS"
                       << "seekId=" << m_seekTraceId
                       << "retryTargetMs=" << retryTargetMs;
            m_seekTraceDecoderPositionBaseMs = (m_decoder ? m_decoder->position() : 0);
            m_decoder->seek(retryTargetMs);
            if (m_state == Playing)
                m_decoder->play();
        } else if (m_seekTraceWaitingForFirstBuffer
                   && m_seekTraceImmediateEosCount >= 2
                   && !m_seekTraceReopenAttempted
                   && m_decoder) {
            m_seekTraceReopenAttempted = true;
            const qint64 reopenTargetMs = qMax<qint64>(0, m_seekTraceTargetMs);
            const QString currentPath = m_currentFilePath;
            qWarning() << "[seek-engine] reopen decoder after repeated immediate EOS"
                       << "seekId=" << m_seekTraceId
                       << "targetMs=" << reopenTargetMs
                       << "file=" << currentPath;

            m_decoder->stop();
            if (!m_decoder->openSource(currentPath)) {
                qWarning() << "[seek-engine] decoder reopen failed after repeated immediate EOS"
                           << "seekId=" << m_seekTraceId
                           << "file=" << currentPath;
            } else {
                m_seekTraceImmediateEosCount = 0;
                m_sourceEnded = false;
                m_playbackFinishedEmitted = false;
                m_seekTraceDecoderPositionBaseMs = 0;
                m_decoder->seek(reopenTargetMs);
                if (m_state == Playing)
                    m_decoder->play();
                else if (m_state == Paused)
                    m_decoder->pause();

                if (m_output)
                    m_output->resetStream();
            }
        } else if (m_seekTraceWaitingForFirstBuffer
                   && m_seekTraceReopenAttempted
                   && m_seekTraceRecoveryAttempted
                   && m_seekTraceImmediateEosCount >= 2) {
            attemptSeekBackoffRecovery("repeated immediate EOS");
        }

        if (m_state == Playing && m_decoder
            && m_decoder->state() == AudioDecoderBackend::State::Paused) {
            m_decoder->play();
        }
        return;
    }

    if (inPostSeekGuardWindow && seekTargetNotNearEnd && m_seekTraceWaitingForFirstWrite
        && !m_seekTraceReopenAttempted) {
        qWarning() << "[seek-engine] blocking EOS transition while waiting post-seek audio"
                   << "seekId=" << m_seekTraceId
                   << "seekElapsedMs=" << seekElapsedMs
                   << "seekTargetMs=" << m_seekTraceTargetMs
                   << "durationMs=" << m_duration;

        m_seekTraceReopenAttempted = true;
        const qint64 reopenTargetMs = qMax<qint64>(0, m_seekTraceTargetMs);
        const QString currentPath = m_currentFilePath;
        qWarning() << "[seek-engine] reopen decoder from EOS guard"
                   << "seekId=" << m_seekTraceId
                   << "targetMs=" << reopenTargetMs
                   << "file=" << currentPath;

        m_decoder->stop();
        if (!m_decoder->openSource(currentPath)) {
            qWarning() << "[seek-engine] decoder reopen failed from EOS guard"
                       << "seekId=" << m_seekTraceId
                       << "file=" << currentPath;
        } else {
            m_sourceEnded = false;
            m_playbackFinishedEmitted = false;
            m_decoder->seek(reopenTargetMs);
            if (m_state == Playing)
                m_decoder->play();
            else if (m_state == Paused)
                m_decoder->pause();

            if (m_output)
                m_output->resetStream();
        }
        return;
    }

    if (m_seekTraceWaitingForFirstBuffer
        && m_seekTraceRecoveryAttempted
        && m_seekTraceReopenAttempted
        && m_seekTraceBackoffAttempts >= kSeekBackoffMaxAttempts) {
        qWarning() << "[seek-engine] all recovery attempts exhausted, forcing source-ended"
                   << "seekId=" << m_seekTraceId
                   << "seekElapsedMs=" << seekElapsedMs
                   << "seekTargetMs=" << m_seekTraceTargetMs;
        m_seekTraceWaitingForFirstBuffer = false;
        m_seekTraceWaitingForFirstWrite = false;
    }

    m_sourceEnded = true;
    if (!inEarlyPostSeekWindow && !m_nextFilePath.isEmpty() && !m_nextDecoder)
        beginPrepareNextDecoder();
    feedSink();
}

void GaplessAudioEngine::onDecoderDurationChanged(qint64 durationMs)
{
    if (durationMs <= 0) return;

    if (m_duration > 0 && durationMs * 10 < m_duration * 8) {
        qWarning() << "[seek-engine] ignoring suspicious duration shrink"
                   << "oldDurationMs=" << m_duration
                   << "newDurationMs=" << durationMs
                   << "file=" << m_currentFilePath;
        return;
    }

    m_duration = durationMs;
    emit durationChanged(durationMs);
}

void GaplessAudioEngine::onNextDecoderBufferReady(const QAudioBuffer &buffer)
{
    if (!buffer.isValid())
        return;

    const char *data = buffer.constData<char>();
    const qint64 len = buffer.byteCount();
    if (len <= 0 || !data)
        return;

    m_nextBuf.write(data, len);
    m_waitingForNextBuffer = false;
    m_nextBufferWaitTimer.invalidate();
}

void GaplessAudioEngine::onNextDecoderEndOfStream()
{
    m_nextSourceEnded = true;
}

void GaplessAudioEngine::onNextDecoderDurationChanged(qint64 durationMs)
{
    if (durationMs <= 0)
        return;
    m_nextDuration = durationMs;
}

void GaplessAudioEngine::feedSink()
{
    if (m_state != Playing) return;

    const qint64 seekElapsedMs = m_seekTraceTimer.isValid() ? m_seekTraceTimer.elapsed() : -1;
    const bool inEarlyPostSeekWindow = (seekElapsedMs >= 0 && seekElapsedMs < 1200);

    if (!inEarlyPostSeekWindow && !m_nextFilePath.isEmpty() && !m_nextDecoder)
        beginPrepareNextDecoder();

    ensureSinkRunning();
    if (!m_output || !m_output->isOpen())
        return;

    const int frameBytes = qMax(1, m_format.bytesPerFrame());
    const bool eqOn = (m_equalizer && m_equalizer->isEnabled());

    // Resume decoder if buffer dropped below low watermark
    if (!m_sourceEnded && m_decoder && m_decoder->state() == AudioDecoderBackend::State::Paused) {
        const int sampleRate = qMax(1, m_format.sampleRate());
        const qint64 bufferedUs = (m_currentBuf.size() * 1000000LL) / (frameBytes * sampleRate);
        if (bufferedUs < kDecoderLowWaterUs)
            m_decoder->play();
    }

    if (eqOn && m_needEqPrebuffer && !m_sourceEnded) {
        // Biquad EQ has zero latency — just consume one small tick to ensure state is stable.
        m_needEqPrebuffer = false;
    }

    int freeBytes = m_output->bytesFree();
    if (freeBytes <= 0) return;

    // DEBUG: log feedSink activity
    static int s_feedCount = 0;
    if (s_feedCount < 2) {
        qInfo() << "[feedSink] freeBytes=" << freeBytes
                << "pendingSize=" << m_pendingPcm.size()
                << "currentBufSize=" << m_currentBuf.size();
        ++s_feedCount;
    }

    if (!m_pendingPcm.isEmpty()) {
        qint64 writtenPending = m_output->write(m_pendingPcm.constData(), m_pendingPcm.size());
        if (writtenPending > 0)
            m_pendingPcm.remove(0, static_cast<int>(writtenPending));

        if (writtenPending > 0 && m_seekTraceWaitingForFirstWrite) {
            m_seekTraceWaitingForFirstWrite = false;
            qInfo() << "[seek-engine] first-output-write"
                    << "id=" << m_seekTraceId
                    << "phase=pending"
                    << "writtenBytes=" << writtenPending
                    << "elapsedMs=" << (m_seekTraceTimer.isValid() ? m_seekTraceTimer.elapsed() : -1)
                    << "outputQueuedBytes=" << m_output->bytesQueued();
        }

        if (!m_pendingPcm.isEmpty())
            return;

        freeBytes = m_output->bytesFree();
        if (freeBytes <= 0)
            return;
    }

    int chunkBytes = qMin(freeBytes, 32768); // Larger chunks for smoother playback

    chunkBytes -= (chunkBytes % frameBytes);
    if (chunkBytes <= 0)
        return;

    if (m_crossfadePendingPromotion && m_pendingPcm.isEmpty() && m_nextDecoder && m_nextBuf.size() > 0) {
        m_waitingForNextBuffer = false;
        m_nextBufferWaitTimer.invalidate();
        m_crossfadePendingPromotion = false;
        promotePreparedNextDecoder();
        return;
    }

    if (!m_crossfadeActive) {
        const bool canStartCrossfade = (m_crossfadeDurationMs > 0)
            && !inEarlyPostSeekWindow
            && m_duration > 0
            && m_nextDecoder
            && !m_nextFilePath.isEmpty();

        if (canStartCrossfade) {
            const qint64 posMs = position();
            const qint64 remainingMs = qMax<qint64>(0, m_duration - posMs);
            if (remainingMs <= m_crossfadeDurationMs && m_nextBuf.size() > 0) {
                m_crossfadeActive = true;
                m_crossfadeFramesTotal = qMax<qint64>(1,
                    (static_cast<qint64>(m_crossfadeDurationMs) * qMax(1, m_format.sampleRate())) / 1000);
                m_crossfadeFramesDone = 0;
            }
        }
    }

    if (m_crossfadeActive
        && (!m_nextDecoder
            || m_nextFilePath.isEmpty()
            || m_format.sampleFormat() != QAudioFormat::Float)) {
        m_crossfadeActive = false;
        m_crossfadePendingPromotion = false;
        m_crossfadeFramesTotal = 0;
        m_crossfadeFramesDone = 0;
    }

    if (m_chunkScratch.size() < chunkBytes)
        m_chunkScratch.resize(chunkBytes);

    qint64 read = 0;
    bool usedCrossfadeChunk = false;

    if (m_crossfadeActive
        && m_nextDecoder
        && m_format.sampleFormat() == QAudioFormat::Float
        && m_crossfadeFramesTotal > 0) {
        int mixBytes = static_cast<int>(qMin<qint64>(chunkBytes,
            qMin(m_currentBuf.size(), m_nextBuf.size())));
        mixBytes -= (mixBytes % frameBytes);

        if (mixBytes > 0) {
            if (m_crossfadeScratch.size() < mixBytes)
                m_crossfadeScratch.resize(mixBytes);
            if (m_chunkScratch.size() < mixBytes)
                m_chunkScratch.resize(mixBytes);

            const qint64 currentRead = m_currentBuf.read(m_chunkScratch.data(), mixBytes);
            const qint64 nextRead = m_nextBuf.read(m_crossfadeScratch.data(), mixBytes);
            const qint64 mixedRead = qMin(currentRead, nextRead);
            const qint64 alignedRead = mixedRead - (mixedRead % frameBytes);

            if (alignedRead > 0) {
                const int channels = qMax(1, m_format.channelCount());
                const qint64 frames = alignedRead / frameBytes;
                float *outSamples = reinterpret_cast<float *>(m_chunkScratch.data());
                const float *inSamples = reinterpret_cast<const float *>(m_crossfadeScratch.constData());

                for (qint64 frame = 0; frame < frames; ++frame) {
                    const float t = qBound(0.0f,
                        static_cast<float>(m_crossfadeFramesDone + frame)
                            / static_cast<float>(qMax<qint64>(1, m_crossfadeFramesTotal)),
                        1.0f);
                    const float outGain = 1.0f - t;
                    const float inGain = t;
                    const qint64 base = frame * channels;
                    for (int c = 0; c < channels; ++c) {
                        const qint64 idx = base + c;
                        outSamples[idx] = outSamples[idx] * outGain + inSamples[idx] * inGain;
                    }
                }

                m_crossfadeFramesDone += frames;
                read = alignedRead;
                usedCrossfadeChunk = true;

                if (m_crossfadeFramesDone >= m_crossfadeFramesTotal)
                    m_crossfadePendingPromotion = true;
            }
        }
    }

    if (!usedCrossfadeChunk)
        read = m_currentBuf.read(m_chunkScratch.data(), chunkBytes);

    if (read > 0) {
        if (m_equalizer && m_equalizer->isEnabled())
            m_equalizer->process(m_chunkScratch.data(), read, m_format.channelCount(), m_format.bytesPerSample());

        applyOutputFade(m_chunkScratch.data(), read);

        qint64 written = m_output->write(m_chunkScratch.constData(), read);
        if (written < read) {
            const qint64 keepFrom = qMax<qint64>(written, 0);
            m_pendingPcm.append(m_chunkScratch.constData() + keepFrom, read - keepFrom);
            if (m_pendingPcm.size() > kPendingBacklogMaxBytes) {
                int drop = m_pendingPcm.size() - kPendingBacklogMaxBytes;
                const int align = qMax(1, frameBytes);
                const int rem = drop % align;
                if (rem != 0)
                    drop += (align - rem);
                drop = qMin(drop, m_pendingPcm.size());
                if (drop > 0) {
                    m_pendingPcm.chop(drop);
                    qWarning() << "Audio backlog capped, dropped" << drop << "bytes";
                }
            }
        }

        if (written > 0 && m_seekTraceWaitingForFirstWrite) {
            m_seekTraceWaitingForFirstWrite = false;
            qInfo() << "[seek-engine] first-output-write"
                    << "id=" << m_seekTraceId
                    << "phase=fresh"
                    << "writtenBytes=" << written
                    << "readBytes=" << read
                    << "elapsedMs=" << (m_seekTraceTimer.isValid() ? m_seekTraceTimer.elapsed() : -1)
                    << "outputQueuedBytes=" << m_output->bytesQueued();
        }
    }

    if (m_crossfadePendingPromotion
        && m_pendingPcm.isEmpty()
        && m_nextDecoder
        && m_nextBuf.size() > 0) {
        m_waitingForNextBuffer = false;
        m_nextBufferWaitTimer.invalidate();
        m_crossfadePendingPromotion = false;
        promotePreparedNextDecoder();
        return;
    }

    if (m_seekTraceWaitingForFirstWrite
        && !m_seekTraceWarnedNoWrite
        && m_seekTraceTimer.isValid()
        && m_seekTraceTimer.elapsed() > 500) {
        m_seekTraceWarnedNoWrite = true;
        qWarning() << "[seek-engine] no output write 500ms after seek"
                   << "id=" << m_seekTraceId
                   << "targetMs=" << m_seekTraceTargetMs
                   << "state=" << stateToString(m_state)
                   << "currentBufBytes=" << m_currentBuf.size()
                   << "pendingBytes=" << m_pendingPcm.size()
                   << "sourceEnded=" << m_sourceEnded;

        if (!m_seekTraceRecoveryAttempted && m_decoder) {
            m_seekTraceRecoveryAttempted = true;
            const qint64 retryTargetMs = qMax<qint64>(0, m_seekTraceTargetMs);
            qWarning() << "[seek-engine] retry decoder seek after no-write stall"
                       << "id=" << m_seekTraceId
                       << "retryTargetMs=" << retryTargetMs;
            m_seekTraceDecoderPositionBaseMs = (m_decoder ? m_decoder->position() : 0);
            m_decoder->seek(retryTargetMs);
            if (m_state == Playing)
                m_decoder->play();
        } else if (m_seekTraceRecoveryAttempted
                   && m_seekTraceReopenAttempted
                   && m_seekTraceWaitingForFirstBuffer) {
            attemptSeekBackoffRecovery("no-write stall");
        }
    }

    if (m_seekTraceWaitingForFirstWrite
        && m_seekTraceTimer.isValid()
        && m_seekTraceTimer.elapsed() > 3000
        && m_seekTraceReopenAttempted
        && m_seekTraceBackoffAttempts >= kSeekBackoffMaxAttempts) {
        qWarning() << "[seek-engine] giving up after all recovery, unblocking seek"
                   << "id=" << m_seekTraceId
                   << "targetMs=" << m_seekTraceTargetMs;
        m_seekTraceWaitingForFirstBuffer = false;
        m_seekTraceWaitingForFirstWrite = false;
    }

    if (m_seekTraceWaitingForFirstWrite
        && m_seekTraceTimer.isValid()
        && m_seekTraceTimer.elapsed() > 1200
        && !m_seekTraceReopenAttempted
        && m_decoder) {
        m_seekTraceReopenAttempted = true;
        const qint64 reopenTargetMs = qMax<qint64>(0, m_seekTraceTargetMs);
        const QString currentPath = m_currentFilePath;

        qWarning() << "[seek-engine] reopen decoder after persistent no-write stall"
                   << "id=" << m_seekTraceId
                   << "targetMs=" << reopenTargetMs
                   << "file=" << currentPath;

        m_decoder->stop();
        if (!m_decoder->openSource(currentPath)) {
            qWarning() << "[seek-engine] decoder reopen failed during stall recovery"
                       << "id=" << m_seekTraceId
                       << "file=" << currentPath;
        } else {
            m_sourceEnded = false;
            m_playbackFinishedEmitted = false;
            m_seekTraceDecoderPositionBaseMs = 0;
            m_decoder->seek(reopenTargetMs);
            if (m_state == Playing)
                m_decoder->play();
            else if (m_state == Paused)
                m_decoder->pause();

            if (m_output)
                m_output->resetStream();
        }
    }

    if (m_sourceEnded && m_currentBuf.size() == 0 && m_pendingPcm.isEmpty()) {
        const bool seekTargetKnown = (m_seekTraceTargetMs >= 0);
        const qint64 decoderPosMs = (m_decoder ? m_decoder->position() : -1);
        const bool decoderAtOrNearEnd = (decoderPosMs >= 0
                                         && m_duration > 0
                                         && (decoderPosMs + kSeekNearEndDecoderSlackMs >= m_duration));
        const bool seekTargetNotNearEnd = !seekTargetKnown
            || m_duration <= 0
            || ((m_seekTraceTargetMs + kSeekNearEndTargetSlackMs < m_duration) && !decoderAtOrNearEnd);
        const bool inPostSeekGuardWindow = m_seekTraceTimer.isValid()
            && m_seekTraceTimer.elapsed() < 5000;

        const bool waitingPostSeekAudio = m_seekTraceWaitingForFirstBuffer
            || m_seekTraceWaitingForFirstWrite;
        if (inPostSeekGuardWindow
            && seekTargetNotNearEnd
            && waitingPostSeekAudio) {
            qWarning() << "[seek-engine] blocking source-ended transition while waiting post-seek audio"
                       << "seekId=" << m_seekTraceId
                       << "seekElapsedMs=" << m_seekTraceTimer.elapsed()
                       << "targetMs=" << m_seekTraceTargetMs;
            m_sourceEnded = false;
            return;
        }

        if (inPostSeekGuardWindow && seekTargetNotNearEnd) {
            qWarning() << "[seek-engine] blocking source-ended transition in post-seek guard window"
                       << "seekId=" << m_seekTraceId
                       << "seekElapsedMs=" << m_seekTraceTimer.elapsed()
                       << "targetMs=" << m_seekTraceTargetMs;
            m_sourceEnded = false;
            return;
        }

        if (inEarlyPostSeekWindow && seekTargetNotNearEnd) {
            qWarning() << "[seek-engine] suppressing source-ended transition during post-seek window"
                       << "seekId=" << m_seekTraceId
                       << "seekElapsedMs=" << seekElapsedMs
                       << "seekTargetMs=" << m_seekTraceTargetMs
                       << "durationMs=" << m_duration
                       << "nextPrepared=" << (!m_nextFilePath.isEmpty());

            m_sourceEnded = false;
            if (m_state == Playing && m_decoder
                && m_decoder->state() == AudioDecoderBackend::State::Paused) {
                m_decoder->play();
            }
            return;
        }

        if (m_nextDecoder) {
            if (m_nextBuf.size() > 0) {
                const qint64 outputQueued = (m_output && m_output->isOpen())
                    ? m_output->bytesQueued() : 0;

                if (m_gaplessTailBytes < 0) {
                    m_gaplessTailBytes = outputQueued;
                    m_gaplessTailPrev = outputQueued;
                    return;
                }

                const bool stillDraining = (outputQueued < m_gaplessTailPrev);
                m_gaplessTailPrev = outputQueued;

                if (stillDraining)
                    return;

                m_gaplessTailBytes = -1;
                m_gaplessTailPrev = -1;

                qInfo() << "[seek-engine] promoting prepared next decoder"
                        << "seekId=" << m_seekTraceId
                        << "seekElapsedMs=" << seekElapsedMs
                        << "nextPath=" << m_nextFilePath;
                m_waitingForNextBuffer = false;
                m_nextBufferWaitTimer.invalidate();
                promotePreparedNextDecoder();
                return;
            }

            if (m_nextSourceEnded) {
                if (m_nextBuf.size() > 0) {
                    m_waitingForNextBuffer = false;
                    m_nextBufferWaitTimer.invalidate();
                    promotePreparedNextDecoder();
                    return;
                }
                qWarning() << "Gapless preload reached EOS without buffered PCM, falling back:"
                           << m_nextFilePath;
                clearPreparedNext();
            } else {
                if (!m_waitingForNextBuffer) {
                    m_waitingForNextBuffer = true;
                    m_nextBufferWaitTimer.start();
                }

                if (m_waitingForNextBuffer
                    && m_nextBufferWaitTimer.elapsed() < kNextBufferWaitTimeoutMs) {
                    return;
                }

                qWarning() << "Gapless preload timed out, falling back to direct next-track play:"
                           << m_nextFilePath;
                clearPreparedNext();
            }
        }

        if (!m_nextFilePath.isEmpty()) {
            const QString nextPath = m_nextFilePath;
            const QString previousPath = m_currentFilePath;
            m_nextFilePath.clear();
            qInfo() << "[seek-engine] fallback play next after source-ended"
                    << "seekId=" << m_seekTraceId
                    << "seekElapsedMs=" << seekElapsedMs
                    << "nextPath=" << nextPath;
            play(nextPath);
            markAudibleTransition(previousPath, nextPath, "gapless-fallback-play");
            emit trackTransitioned();
            return;
        }

        if (!m_playbackFinishedEmitted) {
            const qint64 outputQueued = (m_output && m_output->isOpen())
                ? qMax<qint64>(0, m_output->bytesQueued())
                : 0;
            if (outputQueued > 0)
                return;

            m_playbackFinishedEmitted = true;
            m_positionTimer->stop();
            m_feedTimer->stop();
            updateState(Stopped);
            emit playbackFinished();
        }
    }

    // Don't auto-resume decoder - keep it running to maintain buffer level
}

void GaplessAudioEngine::onPositionTick()
{
    if (m_state == Playing || m_state == Paused) {
        updateAudibleTransitionTrace();
        if (m_pendingTrackTransitioned)
            return;
        emit positionChanged(position());
    }
}