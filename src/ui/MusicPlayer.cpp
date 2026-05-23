#include "MusicPlayer.h"
#include "CueParser.h"
#include "TrackItem.h"
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QMimeData>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QSettings>
#include <algorithm>
#include <QTimer>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QDirIterator>
#include <QMessageBox>
#include <QScrollBar>
#include <QProgressDialog>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMediaMetaData>
#include <QApplication>
#include <QTextEdit>

#ifdef Q_OS_WIN
#include "WinTaskbarButtons.h"
#endif

namespace {
// Qt metadata preload can destabilize startup on some Windows/FFmpeg hook stacks.
// Keep disabled by default; TrackItem fallback metadata remains active.
constexpr bool kEnableQtMetadataPreload = false;
const QString kLikedPlaylistName = QStringLiteral("liked");
const QString kAllPlaylistVirtualId = QStringLiteral("__all__");
const QString kAllPlaylistDisplayName = QStringLiteral("all");
constexpr int kPlaylistColumnSettingsSchema = 3;
constexpr int kPlaylistWarmupCount = 24;
constexpr int kDeferredMetadataBatchSize = 24;
constexpr int kDeferredMetadataBatchDelayMs = 80;
constexpr int kDeferredMetadataMaxTracks = 120;
constexpr int kGlobalMetadataPreloadBatchSize = 16;
constexpr int kGlobalMetadataPreloadBatchDelayMs = 250;
const char *kSeekDiagBuildMarker = "seek-fix-2026-04-14-r3";
}

class CornerGlowWidget : public QWidget {
public:
    explicit CornerGlowWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_colorAnim = new QVariantAnimation(this);
        m_colorAnim->setDuration(1200);
        m_colorAnim->setEasingCurve(QEasingCurve::InOutCubic);
        connect(m_colorAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_currentColor = v.value<QColor>();
            update();
        });
        m_currentColor = QColor(40, 40, 40, 0);
    }

    void setColor(const QColor &c) {
        QColor target = c;
        if (target == Qt::transparent) target = QColor(40, 40, 40, 0);
        if (m_currentColor == target) return;
        m_colorAnim->stop();
        m_colorAnim->setStartValue(m_currentColor);
        m_colorAnim->setEndValue(target);
        m_colorAnim->start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (m_currentColor.alpha() == 0 && m_currentColor.value() < 10) return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        
        // Very large, soft radial glow from bottom-left
        qreal radius = std::sqrt(width()*width() + height()*height());
        QRadialGradient g(0, height(), radius);
        
        QColor c = m_currentColor;
        // Moderate vibrancy for background use
        c.setAlpha(80); 
        g.setColorAt(0, c);
        g.setColorAt(0.2, c);
        c.setAlpha(0);
        g.setColorAt(0.8, c);
        g.setColorAt(1.0, Qt::transparent);
        
        p.fillRect(rect(), g);
    }

private:
    QColor m_currentColor = Qt::transparent;
    QVariantAnimation *m_colorAnim;
};

QColor extractDominantColor(const QPixmap &pixmap) {
    if (pixmap.isNull()) return Qt::transparent;
    // Scale to 1x1 to get the average/dominant color efficiently
    QImage img = pixmap.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).toImage();
    return img.pixelColor(0, 0);
}

QString normalizePathForCompare(const QString &path)
{
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(QDir::cleanPath(path)).toLower();
#else
    return QDir::cleanPath(path);
#endif
}

bool isPlaylistFilePath(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QStringLiteral("m3u") || ext == QStringLiteral("m3u8");
}

static bool isCueSavedPath(const QString &path)
{
    return path.startsWith(QLatin1String("CUE|"));
}

static bool parseCueSavedPath(const QString &path, QString &cuePath, int &trackNum)
{
    if (!path.startsWith(QLatin1String("CUE|")))
        return false;
    const int first = path.indexOf('|');
    const int last  = path.lastIndexOf('|');
    if (first == last || first < 0)
        return false;
    cuePath  = path.mid(first + 1, last - first - 1);
    trackNum = path.mid(last + 1).toInt();
    return !cuePath.isEmpty() && trackNum > 0;
}

static QString makeCueSavedPath(const QString &cuePath, int trackNum)
{
    return QStringLiteral("CUE|") + cuePath + '|' + QString::number(trackNum);
}

QStringList audioNameFilters()
{
    return {
        QStringLiteral("*.mp3"), QStringLiteral("*.wav"), QStringLiteral("*.flac"),
        QStringLiteral("*.ogg"), QStringLiteral("*.m4a"), QStringLiteral("*.aac"),
        QStringLiteral("*.wma"), QStringLiteral("*.opus"), QStringLiteral("*.ape")
    };
}

const char *engineStateToString(GaplessAudioEngine::State state)
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

// ============= MusicPlayer Implementation =============

MusicPlayer::MusicPlayer(QWidget *parent)
    : QMainWindow(parent)
    , m_currentIndex(-1)
    , m_metadataLoadIndex(-1)
    , m_userSeeking(false)
    , m_seekPending(false)
{
    qDebug() << "[init] MusicPlayer constructor BEGIN";
    qInfo() << "[seek-ui] build marker" << kSeekDiagBuildMarker;

    m_engine = new GaplessAudioEngine(this);
    m_equalizer = new Equalizer(this);
    m_engine->setEqualizer(m_equalizer);

    {
        QSettings s("MyCompany", "MusicPlayer");
        m_engine->setDecoderPreferenceId(
            s.value("Audio/decoder", GaplessAudioEngine::decoderFfmpegId()).toString());
        m_engine->setOutputDevicePreferenceId(s.value("Audio/outputDeviceId", QString()).toString());
        m_engine->setBackendPreferenceId(
            s.value("Audio/backend", GaplessAudioEngine::backendWasapiSharedId()).toString());
        m_engine->setPlaybackRate(static_cast<float>(
            qBound(0.5, s.value("Playback/playbackRate", 1.0).toDouble(), 2.0)));
        m_engine->setCrossfadeDurationMs(
            qBound(0, s.value("Playback/crossfadeSeconds", 3).toInt(), 8) * 1000);
        m_equalizer->setPreamp(s.value("Equalizer/preamp", 0.0).toDouble());
        m_equalizer->setAutoLevelEnabled(s.value("Equalizer/autoLevel", false).toBool());
        m_equalizer->setEnabled(s.value("Equalizer/enabled", true).toBool());
        m_shuffleMode = qBound(0,
                       s.value("Playback/shuffleMode", static_cast<int>(ShuffleHistory)).toInt(),
                       1);
        m_shuffleNoRepeats = s.value("Playback/shuffleNoRepeats", false).toBool();
        for (int i = 0; i < EQ_BAND_COUNT; ++i)
            m_equalizer->setBandGain(i, s.value(QString("Equalizer/band_%1").arg(i), 0.0).toDouble());
    }

    if (kEnableQtMetadataPreload)
        m_metadataLoader = new QMediaPlayer(this);

    // Start background metadata loading thread
    m_metadataThread = new MetadataLoaderThread();
    m_metadataThread->start();
    connect(m_metadataThread, &MetadataLoaderThread::metadataLoaded,
            this, &MusicPlayer::onBackgroundMetadataLoaded);

    m_playlistManager = new PlaylistManager(this);

    m_deferredMetadataTimer = new QTimer(this);
    m_deferredMetadataTimer->setSingleShot(true);
    connect(m_deferredMetadataTimer, &QTimer::timeout,
            this, &MusicPlayer::processDeferredMetadataBatch);

        m_globalMetadataPreloadTimer = new QTimer(this);
        m_globalMetadataPreloadTimer->setSingleShot(true);
        connect(m_globalMetadataPreloadTimer, &QTimer::timeout,
            this, &MusicPlayer::processGlobalMetadataPreloadBatch);

    if (kEnableQtMetadataPreload) {
        m_metadataTimeout = new QTimer(this);
        m_metadataTimeout->setSingleShot(true);
        m_metadataTimeout->setInterval(5000);
        connect(m_metadataTimeout, &QTimer::timeout, this, [this]() {
            qDebug() << "Metadata timeout for index" << m_metadataLoadIndex
                     << (m_metadataLoadIndex >= 0 && m_metadataLoadIndex < m_tracks.count()
                             ? m_tracks[m_metadataLoadIndex]->filePath() : "");
            if (m_metadataLoadIndex >= 0 && m_metadataLoadIndex < m_tracks.count()) {
                TrackItem *track = m_tracks[m_metadataLoadIndex];
                if (!track->isMetadataLoaded()) {
                    track->metadata().duration = 0;
                    int visualRow = getVisualRowFromTrackIndex(m_metadataLoadIndex);
                    if (visualRow != -1)
                        m_playlistTable->item(visualRow, COL_DURATION)->setText("--:--");
                }
            }
            m_metadataLoadIndex++;
            QTimer::singleShot(50, this, &MusicPlayer::preloadTrackMetadata);
        });
    }

    m_columnInfo[COL_COVER] = {"Cover", 70, true};
    m_columnInfo[COL_TRACK] = {"#", 50, true};
    m_columnInfo[COL_TITLE] = {"Title", 300, true};
    m_columnInfo[COL_ARTIST] = {"Artist", 200, true};
    m_columnInfo[COL_ALBUM] = {"Album", 200, false};
    m_columnInfo[COL_YEAR] = {"Year", 70, false};
    m_columnInfo[COL_GENRE] = {"Genre", 120, false};
    m_columnInfo[COL_BITRATE] = {"Bitrate (kbps)", 100, false};
    m_columnInfo[COL_SAMPLERATE] = {"Sample Rate (kHz)", 120, false};
    m_columnInfo[COL_DURATION] = {"Duration", 80, true};
    m_columnInfo[COL_LIKED] = {"Like", 50, true};
    m_columnInfo[COL_FILEPATH] = {"File Path", 300, false};

    setupUI();

    m_fullscreenPlayer = new FullscreenPlayer(this);
    connect(m_fullscreenPlayer, &FullscreenPlayer::playPauseRequested,    this, &MusicPlayer::playPause);
    connect(m_fullscreenPlayer, &FullscreenPlayer::nextRequested,         this, &MusicPlayer::next);
    connect(m_fullscreenPlayer, &FullscreenPlayer::previousRequested,     this, &MusicPlayer::previous);
    connect(m_fullscreenPlayer, &FullscreenPlayer::seekRequested,         this, &MusicPlayer::seek);
    connect(m_fullscreenPlayer, &FullscreenPlayer::volumeChangeRequested, this, &MusicPlayer::volumeChanged);
    connect(m_fullscreenPlayer, &FullscreenPlayer::muteToggleRequested,   this, [this]() {
        static int s_preMuteVol = 60;
        const int value = m_volumeSlider->value();
        if (value > 0) {
            s_preMuteVol = value;
            m_volumeSlider->setValue(0);
        } else {
            m_volumeSlider->setValue(s_preMuteVol > 0 ? s_preMuteVol : 60);
        }
    });
    connect(m_fullscreenPlayer, &FullscreenPlayer::shuffleToggleRequested, this, [this]() {
        m_shuffleButton->click();
    });
    connect(m_fullscreenPlayer, &FullscreenPlayer::repeatToggleRequested, this, [this]() {
        m_repeatButton->click();
    });
    connect(m_fullscreenPlayer, &FullscreenPlayer::likeToggleRequested, this, [this]() {
        onLikeButtonClicked();
    });
    m_fullscreenPlayer->updateShuffleState(m_shuffleEnabled, m_shuffleMode);
    m_fullscreenPlayer->updateRepeatState(m_repeatMode);
    m_fullscreenPlayer->updateLikeState(false);

    setupConnections();
    applyShuffleButtonStyle();
    loadColumnSettings();

    setAcceptDrops(true);

    QSettings initSettings("MyCompany", "MusicPlayer");
    m_rowHeight = initSettings.value("Appearance/rowHeight", 70).toInt();

    const int savedVolume = qBound(0, initSettings.value("Playback/volume", 70).toInt(), 100);
    m_fullscreenPlayer->updateVolume(savedVolume);
    m_volumeSlider->setValue(savedVolume);

    setWindowTitle("Без названия - Неизвестен beta .3 APE DONT WORK");
    setMinimumSize(980, 620);
    if (!initSettings.contains("MainWindow/geometry"))
        resize(1200, 700);

    refreshPlaylistPanel();
    if (m_playlistManager->count() == 0)
        m_playlistManager->createPlaylist("Default");
    refreshPlaylistPanel();

    QSettings settings("MyCompany", "MusicPlayer");
    QString lastId = settings.value("MainWindow/lastPlaylistId").toString();
    int startRow = 0;
    for (int i = 0; i < m_playlistList->count(); ++i) {
        if (m_playlistList->item(i)->data(Qt::UserRole).toString() == lastId) {
            startRow = i;
            break;
        }
    }
    if (m_playlistList->count() > 0) {
        m_playlistList->setCurrentRow(startRow);
        onPlaylistSelected(startRow);
        restorePlaybackState();
    }
    #ifdef Q_OS_WIN
        QTimer::singleShot(0, this, [this]() {
            m_winTaskbar = new WinTaskbarButtons(this);
            m_winTaskbar->attach(static_cast<quintptr>(winId()));
            connect(m_winTaskbar, &WinTaskbarButtons::previousClicked,  this, &MusicPlayer::previous);
            connect(m_winTaskbar, &WinTaskbarButtons::playPauseClicked, this, &MusicPlayer::playPause);
            connect(m_winTaskbar, &WinTaskbarButtons::nextClicked,      this, &MusicPlayer::next);
        });
    #endif
    updateLikeButtonState();
    scheduleGlobalMetadataPreload(350);
    qDebug() << "[init] MusicPlayer constructor END";
}

MusicPlayer::~MusicPlayer()
{
    savePlaybackState();
    saveCurrentPlaylistTracks();
    if (m_playlistTable)
        saveColumnSettings();

    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
        m_engine->stop();
    }

    if (m_metadataTimeout)
        m_metadataTimeout->stop();
    if (m_metadataLoader)
        m_metadataLoader->setSource(QUrl());
    stopDeferredMetadataLoading();
    if (m_globalMetadataPreloadTimer)
        m_globalMetadataPreloadTimer->stop();
    m_globalMetadataPreloadPaths.clear();
    m_globalMetadataPreloadIndex = -1;

    // Stop background metadata thread
    if (m_metadataThread) {
        m_metadataThread->stop();
        m_metadataThread->quit();
        m_metadataThread->wait(1000);
        delete m_metadataThread;
        m_metadataThread = nullptr;
    }

    qDeleteAll(m_tracks);
    m_tracks.clear();
}

void MusicPlayer::setupUI()
{
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // ========== SPLITTER: sidebar | content ==========
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setHandleWidth(1);
    m_splitter->setStyleSheet("QSplitter::handle { background: #3d3d3d; }");

    // --- LEFT: Playlist sidebar ---
    QWidget *sidebarWidget = new QWidget();
    sidebarWidget->setStyleSheet(
        "QWidget#sidebarPanel { background-color: #1a1a1a; border: 2px solid #555; border-radius: 5px; }");
    sidebarWidget->setObjectName("sidebarPanel");
    QVBoxLayout *sidebarLayout = new QVBoxLayout(sidebarWidget);
    sidebarLayout->setContentsMargins(8, 10, 8, 10);
    sidebarLayout->setSpacing(6);

    QLabel *sidebarTitle = new QLabel("Playlists");
    sidebarTitle->setStyleSheet("background: transparent; color: #888; font-size: 11px; font-weight: bold; text-transform: uppercase; letter-spacing: 1px;");
    sidebarLayout->addWidget(sidebarTitle);

    m_playlistList = new NoXButtonListWidget();
    m_playlistList->setStyleSheet(
        "QListWidget { background: #1a1a1a; border: none; color: white; font-size: 13px; outline: none; }"
        "QListWidget::item { padding: 8px 8px 8px 12px; border-radius: 4px; background: transparent; border-left: 3px solid transparent; }"
        "QListWidget::item:selected { background: #3d3d3d; color: #1db954; font-weight: bold; border-left: 3px solid #1db954; }"
        "QListWidget::item:hover:!selected { background: #3a3a3a; }");
    m_playlistList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_playlistList->setDragDropMode(QAbstractItemView::InternalMove);
    m_playlistList->setDefaultDropAction(Qt::MoveAction);
    m_playlistList->setAcceptDrops(true);
    m_playlistList->viewport()->setAcceptDrops(true);
    m_playlistList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_playlistList->installEventFilter(this);
    m_playlistList->viewport()->installEventFilter(this);
    sidebarLayout->addWidget(m_playlistList, 1);

    connect(m_playlistList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        const QString newName = item->text().trimmed();
        if (!id.isEmpty() && !newName.isEmpty()) {
            if (id == kAllPlaylistVirtualId) {
                QSettings settings;
                settings.setValue("AllPlaylistName", newName);
            } else {
                m_playlistManager->renamePlaylist(id, newName);
            }
        }
    });

    connect(m_playlistList->model(), &QAbstractItemModel::rowsMoved,
            this, &MusicPlayer::onPlaylistListRowsMoved);

    QHBoxLayout *sidebarBtnRow = new QHBoxLayout();
    sidebarBtnRow->setSpacing(4);
    QString sidebarBtnStyle = "QPushButton { background: #333; border: none; color: #ccc; font-size: 16px; padding: 4px 10px; border-radius: 4px; }"
                              "QPushButton:hover { background: #444; color: white; }";

    QPushButton *btnNewPl = new QPushButton("+");
    btnNewPl->setToolTip("New Playlist");
    btnNewPl->setStyleSheet(sidebarBtnStyle);
    btnNewPl->setCursor(Qt::PointingHandCursor);

    QPushButton *btnImportM3U = new QPushButton(QString::fromUtf8("\xF0\x9F\x93\xA5"));
    btnImportM3U->setToolTip("Import M3U");
    btnImportM3U->setStyleSheet(sidebarBtnStyle);
    btnImportM3U->setCursor(Qt::PointingHandCursor);

    sidebarBtnRow->addWidget(btnNewPl);
    sidebarBtnRow->addWidget(btnImportM3U);
    sidebarBtnRow->addStretch();
    sidebarLayout->addLayout(sidebarBtnRow);

    sidebarWidget->setMinimumWidth(140);
    sidebarWidget->setMaximumWidth(300);
    m_splitter->addWidget(sidebarWidget);

    connect(btnNewPl, &QPushButton::clicked, this, &MusicPlayer::createNewPlaylist);
    connect(btnImportM3U, &QPushButton::clicked, this, &MusicPlayer::importM3UPlaylist);
    connect(m_playlistList, &QListWidget::currentRowChanged, this, &MusicPlayer::onPlaylistSelected);
    connect(m_playlistList, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_playlistList->itemAt(pos);
        if (!item) return;
        const QString playlistId = item->data(Qt::UserRole).toString();
        const bool isAllPlaylist = (playlistId == kAllPlaylistVirtualId);
        const bool isLikedPlaylist = (playlistId == getLikedPlaylistId());

        QMenu menu;
        menu.setStyleSheet("QMenu { background: #2b2b2b; color: white; border: 1px solid #555; } QMenu::item:selected { background: #0078d7; }");
        QAction *renameAct = menu.addAction("Rename");
        QAction *autoSourceAct = menu.addAction("Auto Source...");
        QAction *exportAct = menu.addAction("Export as M3U");
        menu.addSeparator();
        QAction *deleteAct = menu.addAction("Delete");
        deleteAct->setIcon(QIcon());

        if (isAllPlaylist) {
            autoSourceAct->setEnabled(false);
            exportAct->setEnabled(false);
        }
        
        if (isAllPlaylist || isLikedPlaylist) {
            deleteAct->setEnabled(false);
        }

        QAction *chosen = menu.exec(m_playlistList->mapToGlobal(pos));
        if (chosen == renameAct) renameSelectedPlaylist();
        else if (chosen == autoSourceAct) configurePlaylistAutoSourceDirectory(playlistId);
        else if (chosen == deleteAct) deleteSelectedPlaylist();
        else if (chosen == exportAct) exportM3UPlaylist();
    });

    // --- RIGHT: main content ---
    QWidget *contentWidget = new QWidget();
    QVBoxLayout *contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setSpacing(10);
    contentLayout->setContentsMargins(4, 10, 10, 10);

    QHBoxLayout *toolbarLayout = new QHBoxLayout();

    m_searchBox = new QLineEdit();
    m_searchBox->setPlaceholderText(QString::fromUtf8("\xF0\x9F\x94\x8D Search tracks..."));
    m_searchBox->setStyleSheet("QLineEdit { background-color: #3d3d3d; color: white; border: 2px solid #555; border-radius: 5px; padding: 5px 10px; min-width: 200px; } QLineEdit:focus { border: 2px solid #0078d7; }");

    m_trackCountLabel = new QLabel("0 tracks");
    m_trackCountLabel->setStyleSheet("color: #888; font-style: italic; font-size: 13px;");

    m_settingsButton = new QPushButton(QString::fromUtf8("\xE2\x9A\x99"));
    m_settingsButton->setFixedSize(32, 32);
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton->setStyleSheet(
        "QPushButton { background: transparent; border: none; color: #888; font-size: 18px; border-radius: 4px; }"
        "QPushButton:hover { background: #3a3a3a; color: white; }");

    toolbarLayout->addWidget(m_searchBox);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(m_trackCountLabel);
    toolbarLayout->addWidget(m_settingsButton);

    contentLayout->addLayout(toolbarLayout);

    // ========== PLAYLIST TABLE ==========
    m_playlistTable = new PlaylistTable();
    m_playlistTable->setColumnCount(COL_COUNT);

    QStringList headers;
    for (int i = 0; i < COL_COUNT; ++i) headers << m_columnInfo[i].name;
    m_playlistTable->setHorizontalHeaderLabels(headers);

    for (int i = 0; i < COL_COUNT; ++i) m_playlistTable->setColumnWidth(i, m_columnInfo[i].defaultWidth);
    m_playlistTable->snapshotIdealWidths();

    for (int i = 0; i < COL_COUNT; ++i) m_playlistTable->horizontalHeader()->setSectionResizeMode(i, QHeaderView::Interactive);
    m_playlistTable->horizontalHeader()->setMinimumSectionSize(40);
    m_playlistTable->horizontalHeader()->setStretchLastSection(false);
    m_playlistTable->enforceRightmostResizeLock();

    contentLayout->addWidget(m_playlistTable, 1);

    m_splitter->addWidget(contentWidget);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({180, 1020});

    mainLayout->addWidget(m_splitter, 1);

    // ========== BOTTOM PLAYER BAR ==========
    QWidget *bottomBar = new QWidget();
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(12, 8, 12, 8);
    bottomLayout->setSpacing(6);

    QHBoxLayout *controlsRow = new QHBoxLayout();
    controlsRow->setSpacing(0);

    // LEFT section
    QHBoxLayout *leftSection = new QHBoxLayout();
    leftSection->setSpacing(10);
    leftSection->setContentsMargins(0, 0, 0, 0);

    m_bottomCoverLabel = new QLabel();
    m_bottomCoverLabel->setFixedSize(48, 48);
    QPixmap defaultCover(48, 48);
    defaultCover.fill(QColor(40, 40, 40));
    { QPainter p(&defaultCover); p.setPen(QColor(100,100,100)); p.setFont(QFont("Segoe UI Emoji", 20)); p.drawText(defaultCover.rect(), Qt::AlignCenter, QString::fromUtf8("\xF0\x9F\x8E\xB5")); }
    m_bottomCoverLabel->setPixmap(defaultCover);
    m_bottomCoverLabel->setStyleSheet("border-radius: 4px;");
    m_bottomCoverLabel->setCursor(Qt::PointingHandCursor);
    m_bottomCoverLabel->installEventFilter(this);

    QVBoxLayout *trackInfoLayout = new QVBoxLayout();
    trackInfoLayout->setSpacing(1);
    trackInfoLayout->setContentsMargins(0, 0, 0, 0);
    m_titleLabel = new MarqueeLabel();
    m_titleLabel->setFixedWidth(200);
    { QFont f; f.setPixelSize(13); f.setBold(true);
      m_titleLabel->setTextStyle(f, QColor(255,255,255,255)); }
    m_titleLabel->setText("No track playing");

    m_artistLabel = new MarqueeLabel();
    m_artistLabel->setFixedWidth(200);
    { QFont f; f.setPixelSize(11);
      m_artistLabel->setTextStyle(f, QColor(179,179,179,255)); }
    m_artistLabel->setText("");
    trackInfoLayout->addWidget(m_titleLabel);
    trackInfoLayout->addWidget(m_artistLabel);

    m_likeButton = new QPushButton(QString::fromUtf8("\xE2\x99\xA1"));

    leftSection->addWidget(m_bottomCoverLabel);
    leftSection->addLayout(trackInfoLayout);
    leftSection->addWidget(m_likeButton);
    leftSection->addStretch();

    QWidget *leftWidget = new QWidget();
    leftWidget->setLayout(leftSection);
    leftWidget->setStyleSheet("background: transparent;");

    // CENTER section
    QHBoxLayout *centerSection = new QHBoxLayout();
    centerSection->setSpacing(8);
    centerSection->setContentsMargins(0, 0, 0, 0);
    centerSection->setAlignment(Qt::AlignCenter);

    QString ctrlBtnStyle = "QPushButton { background: transparent; border: none; color: #b3b3b3; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: white; }";
    QString playBtnStyle = "QPushButton { background: transparent; border: none; color: white; font-size: 28px; padding: 4px 12px; } QPushButton:hover { color: #0078d7; }";

    m_shuffleButton = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x80"));
    m_previousButton = new QPushButton(QString::fromUtf8("\xE2\x8F\xAE"));
    m_playButton = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"));
    m_nextButton = new QPushButton(QString::fromUtf8("\xE2\x8F\xAD"));
    m_repeatButton = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x81"));

    m_shuffleButton->setStyleSheet(ctrlBtnStyle);
    m_previousButton->setStyleSheet(ctrlBtnStyle);
    m_playButton->setStyleSheet(playBtnStyle);
    m_nextButton->setStyleSheet(ctrlBtnStyle);
    m_repeatButton->setStyleSheet(ctrlBtnStyle);
    m_likeButton->setStyleSheet(ctrlBtnStyle);

    m_shuffleButton->setCursor(Qt::PointingHandCursor);
    m_previousButton->setCursor(Qt::PointingHandCursor);
    m_playButton->setCursor(Qt::PointingHandCursor);
    m_nextButton->setCursor(Qt::PointingHandCursor);
    m_repeatButton->setCursor(Qt::PointingHandCursor);
    m_likeButton->setCursor(Qt::PointingHandCursor);

    centerSection->addWidget(m_shuffleButton);
    centerSection->addWidget(m_previousButton);
    centerSection->addWidget(m_playButton);
    centerSection->addWidget(m_nextButton);
    centerSection->addWidget(m_repeatButton);

    QWidget *centerWidget = new QWidget();
    centerWidget->setLayout(centerSection);
    centerWidget->setStyleSheet("background: transparent;");

    // RIGHT section
    QHBoxLayout *rightSection = new QHBoxLayout();
    rightSection->setSpacing(8);
    rightSection->setContentsMargins(0, 0, 0, 0);
    rightSection->setAlignment(Qt::AlignVCenter);

    m_volumeLabel = new QLabel(QString::fromUtf8("\xF0\x9F\x94\x8A"));
    m_volumeLabel->setStyleSheet("font-size: 16px; color: #b3b3b3; background: transparent;");
    m_volumeLabel->setCursor(Qt::PointingHandCursor);
    m_volumeLabel->installEventFilter(this);

    QString sliderStyle =
        "QSlider { min-height: 16px; background: transparent; }"
        "QSlider::groove:horizontal { height: 4px; background: #4d4d4d; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #b3b3b3; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #fff; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }"
        "QSlider::handle:horizontal:hover { background: #0078d7; }";

    m_volumeSlider = new ClickableSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(120);
    m_volumeSlider->setStyleSheet(sliderStyle);
    m_volumeSlider->setCursor(Qt::PointingHandCursor);

    rightSection->addStretch();
    rightSection->addWidget(m_volumeLabel);
    rightSection->addWidget(m_volumeSlider);

    QWidget *rightWidget = new QWidget();
    rightWidget->setLayout(rightSection);
    rightWidget->setStyleSheet("background: transparent;");

    controlsRow->addWidget(leftWidget, 1);
    controlsRow->addWidget(centerWidget, 0);
    controlsRow->addWidget(rightWidget, 1);

    bottomLayout->addLayout(controlsRow);

    // --- Seek bar row ---
    QHBoxLayout *seekRow = new QHBoxLayout();
    seekRow->setSpacing(8);

    m_currentTimeLabel = new QLabel("00:00");
    m_currentTimeLabel->setStyleSheet("color: #b3b3b3; font-size: 11px; background: transparent;");
    m_currentTimeLabel->setFixedWidth(40);

    m_positionSlider = new ClickableSlider(Qt::Horizontal);
    m_positionSlider->setRange(0, 0);
    m_positionSlider->setSingleStep(5000);
    m_positionSlider->setPageStep(15000);
    m_positionSlider->setStyleSheet(sliderStyle);
    m_positionSlider->setCursor(Qt::PointingHandCursor);

    m_totalTimeLabel = new QLabel("00:00");
    m_totalTimeLabel->setStyleSheet("color: #b3b3b3; font-size: 11px; background: transparent;");
    m_totalTimeLabel->setFixedWidth(40);
    m_totalTimeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    seekRow->addWidget(m_currentTimeLabel);
    seekRow->addWidget(m_positionSlider, 1);
    seekRow->addWidget(m_totalTimeLabel);

    bottomLayout->addLayout(seekRow);

    mainLayout->addWidget(bottomBar);

    m_bottomGlow = new CornerGlowWidget(centralWidget);
    m_bottomGlow->resize(800, 600); // Larger glow area
    m_bottomGlow->lower(); // Move behind all other widgets

    // Adjust styles to be slightly transparent so the glow is visible from behind
    centralWidget->setStyleSheet("background-color: #121212;");
    sidebarWidget->setStyleSheet(
        "QWidget#sidebarPanel { background-color: rgba(26, 26, 26, 200); border: 2px solid #333; border-radius: 5px; }");
    bottomBar->setStyleSheet("background-color: transparent;");
    
    setCentralWidget(centralWidget);
}

void MusicPlayer::prepareNextGapless()
{
    if (m_currentIndex >= 0 && m_currentIndex < m_tracks.count()
            && m_tracks[m_currentIndex]->metadata().isCueTrack) {
        m_preparedNextIndex = -1;
        m_preparedNextPath.clear();
        m_engine->prepareNext(QString());
        return;
    }

    int nextIdx = getNextTrackIndex();
    if (nextIdx < 0 || nextIdx >= m_tracks.count()) {
        m_preparedNextIndex = -1;
        m_preparedNextPath.clear();
        m_engine->prepareNext(QString());
        return;
    }
    m_preparedNextIndex = nextIdx;
    m_preparedNextPath = m_tracks[nextIdx]->filePath();
    m_engine->prepareNext(m_preparedNextPath);
}

void MusicPlayer::setupConnections()
{
    connect(m_playButton, &QPushButton::clicked, this, &MusicPlayer::playPause);
    connect(m_nextButton, &QPushButton::clicked, this, &MusicPlayer::next);
    connect(m_previousButton, &QPushButton::clicked, this, &MusicPlayer::previous);
    connect(m_playlistTable, &PlaylistTable::deleteRequested, this, &MusicPlayer::removeSelected);
    qApp->installEventFilter(this);
    
    connect(m_positionSlider, &ClickableSlider::sliderPressed, this, [this]() {
        ++m_seekUiEpoch;
        m_userSeeking = true;
        m_seekPending = false;
        qInfo() << "[seek-ui] sliderPressed"
            << "epoch=" << m_seekUiEpoch
                << "valueMs=" << m_positionSlider->value()
                << "enginePosMs=" << (m_engine ? m_engine->position() : -1)
                << "durationMs=" << (m_engine ? m_engine->duration() : -1)
                << "state=" << (m_engine ? engineStateToString(m_engine->state()) : "null");
    });
    connect(m_positionSlider, &ClickableSlider::valueChanged, this, [this](int val) {
        if (m_userSeeking) m_currentTimeLabel->setText(formatTime(val));
    });
    connect(m_positionSlider, &ClickableSlider::sliderReleased, this, [this]() {
        const quint64 releaseEpoch = m_seekUiEpoch;
        const int seekTargetMs = m_positionSlider->value();
        qInfo() << "[seek-ui] sliderReleased"
            << "epoch=" << releaseEpoch
                << "targetMs=" << seekTargetMs
                << "userSeeking=" << m_userSeeking
                << "seekPending(before)=" << m_seekPending;

        // Set m_seekPending BEFORE seek to prevent stale position updates
        m_seekPending = true;
        // Pre-anchor the fullscreen lyrics interpolator at the seek target so
        // it doesn't drift further from the stale pre-seek anchor while the
        // engine catches up. Without this, fast successive seeks could leave
        // lyrics frozen on an old line until the engine settled.
        if (m_fullscreenPlayer)
            m_fullscreenPlayer->updatePosition(seekTargetMs);
        seek(seekTargetMs);
        // Slider will update naturally as decoder catches up with audio
        QTimer::singleShot(300, this, [this, releaseEpoch]() {
            if (releaseEpoch != m_seekUiEpoch) {
                qInfo() << "[seek-ui] skip stale seek flag reset"
                        << "timerEpoch=" << releaseEpoch
                        << "activeEpoch=" << m_seekUiEpoch;
                return;
            }

            m_seekPending = false;
            m_userSeeking = false;
            qInfo() << "[seek-ui] seek flags reset"
                    << "epoch=" << releaseEpoch
                    << "userSeeking=" << m_userSeeking
                    << "seekPending=" << m_seekPending;
        });
    });
    connect(m_volumeSlider, &ClickableSlider::valueChanged, this, &MusicPlayer::volumeChanged);

    connect(m_engine, &GaplessAudioEngine::positionChanged, this, &MusicPlayer::updatePosition);
    connect(m_engine, &GaplessAudioEngine::durationChanged, this, &MusicPlayer::updateDuration);
    connect(m_engine, &GaplessAudioEngine::stateChanged, this, &MusicPlayer::onEngineStateChanged);
    connect(m_engine, &GaplessAudioEngine::trackTransitioned, this, &MusicPlayer::onEngineTrackTransitioned);
    connect(m_engine, &GaplessAudioEngine::playbackFinished, this, &MusicPlayer::onEnginePlaybackFinished);
    connect(m_engine, &GaplessAudioEngine::bassLevel, m_fullscreenPlayer, &FullscreenPlayer::updateBassLevel);

    connect(m_shuffleButton, &QPushButton::clicked, this, [this]() {
        m_shuffleEnabled = !m_shuffleEnabled;
        clearShuffleRuntimeState(true);
        applyShuffleButtonStyle();
        resyncPreparedNext();
    });

    connect(m_repeatButton, &QPushButton::clicked, this, [this]() {
        m_repeatMode = (m_repeatMode + 1) % 3;
        if (m_repeatMode == 0) {
            m_repeatButton->setText(QString::fromUtf8("\xF0\x9F\x94\x81"));
            m_repeatButton->setStyleSheet("QPushButton { background: transparent; border: none; color: #b3b3b3; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: white; }");
        } else if (m_repeatMode == 1) {
            m_repeatButton->setText(QString::fromUtf8("\xF0\x9F\x94\x81"));
            m_repeatButton->setStyleSheet("QPushButton { background: transparent; border: none; color: #1db954; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: #1ed760; }");
        } else {
            m_repeatButton->setText(QString::fromUtf8("\xF0\x9F\x94\x82"));
            m_repeatButton->setStyleSheet("QPushButton { background: transparent; border: none; color: #1db954; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: #1ed760; }");
        }

        if (m_fullscreenPlayer)
            m_fullscreenPlayer->updateRepeatState(m_repeatMode);
    });

    connect(m_playlistTable, &QTableWidget::cellDoubleClicked, this, &MusicPlayer::playlistItemDoubleClicked);
    connect(m_playlistTable, &QTableWidget::cellClicked, this, &MusicPlayer::onPlaylistCellClicked);
    connect(m_playlistTable, &PlaylistTable::filesDropped, this, &MusicPlayer::onFilesDropped);
    connect(m_playlistTable, &PlaylistTable::internalRowsMoved, this, &MusicPlayer::onRowsMoved);

    connect(m_playlistTable->horizontalHeader(), &QHeaderView::sectionClicked, this, &MusicPlayer::onHeaderClicked);
    connect(m_playlistTable->horizontalHeader(), &QHeaderView::customContextMenuRequested, this, &MusicPlayer::showHeaderContextMenu);

    m_playlistTable->viewport()->installEventFilter(this);
    m_playlistList->viewport()->installEventFilter(this);

    // Connect scroll for lazy metadata loading of visible rows
    auto *scrollBar = m_playlistTable->verticalScrollBar();
    if (scrollBar) {
        connect(scrollBar, &QAbstractSlider::valueChanged,
                this, &MusicPlayer::onPlaylistScroll);
    }

    connect(m_searchBox, &QLineEdit::textChanged, this, &MusicPlayer::filterPlaylist);
    connect(m_likeButton, &QPushButton::clicked, this, &MusicPlayer::onLikeButtonClicked);

    if (m_metadataLoader)
        connect(m_metadataLoader, &QMediaPlayer::mediaStatusChanged, this, &MusicPlayer::onMetaDataLoaded);

    connect(m_settingsButton, &QPushButton::clicked, this, &MusicPlayer::openSettings);
}

void MusicPlayer::onHeaderClicked(int logicalIndex)
{
    const bool sameColumn = (m_playlistTable->horizontalHeader()->sortIndicatorSection() == logicalIndex);
    const bool ascendingNow = (m_playlistTable->horizontalHeader()->sortIndicatorOrder() == Qt::AscendingOrder);
    const Qt::SortOrder order = (sameColumn && ascendingNow)
                                    ? Qt::DescendingOrder
                                    : Qt::AscendingOrder;
    sortByColumn(logicalIndex, order);
}

void MusicPlayer::sortByColumn(int logicalIndex, Qt::SortOrder order)
{
    if (!m_playlistTable)
        return;
    if (logicalIndex < 0 || logicalIndex >= COL_COUNT)
        return;

    m_playlistTable->sortItems(logicalIndex, order);
    syncTracksToTableOrder();
    m_playlistTable->horizontalHeader()->setSortIndicator(logicalIndex, order);
}

void MusicPlayer::showHeaderContextMenu(const QPoint &pos)
{
    const int clickedColumn = m_playlistTable->horizontalHeader()->logicalIndexAt(pos);
    QMenu contextMenu(this);

    QMenu *sortingMenu = contextMenu.addMenu("Sorting");
    const bool sortableColumn = (clickedColumn >= 0 && clickedColumn < COL_COUNT && clickedColumn != COL_COVER);

    QAction *sortCurrentAscAction = sortingMenu->addAction("Current Column - Ascending");
    QAction *sortCurrentDescAction = sortingMenu->addAction("Current Column - Descending");
    sortCurrentAscAction->setEnabled(sortableColumn);
    sortCurrentDescAction->setEnabled(sortableColumn);

    connect(sortCurrentAscAction, &QAction::triggered, this, [this, clickedColumn]() {
        sortByColumn(clickedColumn, Qt::AscendingOrder);
    });
    connect(sortCurrentDescAction, &QAction::triggered, this, [this, clickedColumn]() {
        sortByColumn(clickedColumn, Qt::DescendingOrder);
    });

    sortingMenu->addSeparator();

    const auto addSortSubmenu = [this, sortingMenu](const QString &title, int column) {
        QMenu *fieldMenu = sortingMenu->addMenu(title);
        QAction *ascAction = fieldMenu->addAction("Ascending");
        QAction *descAction = fieldMenu->addAction("Descending");
        connect(ascAction, &QAction::triggered, this, [this, column]() {
            sortByColumn(column, Qt::AscendingOrder);
        });
        connect(descAction, &QAction::triggered, this, [this, column]() {
            sortByColumn(column, Qt::DescendingOrder);
        });
    };

    addSortSubmenu("By Track Number", COL_TRACK);
    addSortSubmenu("By Title", COL_TITLE);
    addSortSubmenu("By Artist", COL_ARTIST);
    addSortSubmenu("By Album", COL_ALBUM);
    addSortSubmenu("By Year", COL_YEAR);
    addSortSubmenu("By Duration", COL_DURATION);
    addSortSubmenu("By Bitrate", COL_BITRATE);
    addSortSubmenu("By Sample Rate", COL_SAMPLERATE);

    sortingMenu->addSeparator();
    QAction *clearSortAction = sortingMenu->addAction("Clear Sorting");
    clearSortAction->setEnabled(m_playlistTable->horizontalHeader()->sortIndicatorSection() >= 0);
    connect(clearSortAction, &QAction::triggered, this, [this]() {
        m_playlistTable->setSortingEnabled(false);
        m_playlistTable->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
        m_playlistTable->setSortingEnabled(true);
    });

    QMenu *columnsMenu = contextMenu.addMenu("Columns");

    for (int col = 0; col < COL_COUNT; ++col) {
        if (!m_columnActions.contains(col)) {
            QAction *action = new QAction(m_columnInfo[col].name, this);
            action->setCheckable(true);
            action->setChecked(!m_playlistTable->isColumnHidden(col));
            connect(action, &QAction::triggered, this, [this, col](){ toggleColumn(col); });
            m_columnActions[col] = action;
        }
        columnsMenu->addAction(m_columnActions[col]);
    }
    columnsMenu->addSeparator();
    QAction *resetAction = new QAction("Reset Columns", this);
    connect(resetAction, &QAction::triggered, this, &MusicPlayer::resetColumns);
    columnsMenu->addAction(resetAction);

    contextMenu.exec(m_playlistTable->horizontalHeader()->mapToGlobal(pos));
}

void MusicPlayer::toggleColumn(int column)
{
    bool isHidden = m_playlistTable->isColumnHidden(column);
    m_playlistTable->setColumnHidden(column, !isHidden);
    if(m_columnActions.contains(column))
        m_columnActions[column]->setChecked(isHidden);
    m_playlistTable->enforceRightmostResizeLock();
}

void MusicPlayer::applyStandardColumnLayout(bool applyDefaultVisibility)
{
    const QList<int> standardOrder = {
        COL_TRACK,
        COL_COVER,
        COL_TITLE,
        COL_ARTIST,
        COL_DURATION,
        COL_LIKED,
        COL_ALBUM,
        COL_YEAR,
        COL_GENRE,
        COL_BITRATE,
        COL_SAMPLERATE,
        COL_FILEPATH
    };

    for (int col = 0; col < COL_COUNT; ++col) {
        if (applyDefaultVisibility) {
            const bool hidden = !m_columnInfo[col].visibleByDefault;
            m_playlistTable->setColumnHidden(col, hidden);
            if (m_columnActions.contains(col))
                m_columnActions[col]->setChecked(!hidden);
        }
        m_playlistTable->setColumnWidth(col, m_columnInfo[col].defaultWidth);
    }

    QHeaderView *header = m_playlistTable->horizontalHeader();
    for (int visualIndex = 0; visualIndex < standardOrder.size(); ++visualIndex) {
        const int logical = standardOrder[visualIndex];
        const int currentVisual = header->visualIndex(logical);
        if (currentVisual >= 0 && currentVisual != visualIndex)
            header->moveSection(currentVisual, visualIndex);
    }
    m_playlistTable->enforceRightmostResizeLock();
}

void MusicPlayer::resetColumns()
{
    applyStandardColumnLayout(true);
    m_playlistTable->snapshotIdealWidths();
    saveColumnSettings();
}

// ============= Playlist Panel =============

void MusicPlayer::refreshPlaylistPanel()
{
    m_playlistList->blockSignals(true);
    QString prevId;
    if (QListWidgetItem *prevItem = m_playlistList->currentItem())
        prevId = prevItem->data(Qt::UserRole).toString();

    m_playlistList->clear();

    QSettings settings("MyCompany", "MusicPlayer");
    QString allName = settings.value("AllPlaylistName", kAllPlaylistDisplayName).toString();

    QListWidgetItem *allItem = new QListWidgetItem(allName);
    allItem->setData(Qt::UserRole, kAllPlaylistVirtualId);
    allItem->setFlags((allItem->flags() | Qt::ItemIsEditable) & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled);
    m_playlistList->addItem(allItem);

    for (const auto &pl : m_playlistManager->playlists()) {
        QListWidgetItem *item = new QListWidgetItem(pl.name);
        item->setData(Qt::UserRole, pl.id);
        item->setFlags((item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsEditable) & ~Qt::ItemIsDropEnabled);
        m_playlistList->addItem(item);
    }

    int rowToSelect = -1;
    if (!prevId.isEmpty()) {
        for (int i = 0; i < m_playlistList->count(); ++i) {
            if (m_playlistList->item(i)->data(Qt::UserRole).toString() == prevId) {
                rowToSelect = i;
                break;
            }
        }
    }

    if (rowToSelect < 0 && m_playlistList->count() > 0)
        rowToSelect = 0;
    if (rowToSelect >= 0)
        m_playlistList->setCurrentRow(rowToSelect);

    m_playlistList->blockSignals(false);
}

bool MusicPlayer::selectPlaylistById(const QString &playlistId)
{
    if (playlistId.isEmpty() || !m_playlistList)
        return false;

    if (playlistId == m_currentPlaylistId)
        return true;

    for (int i = 0; i < m_playlistList->count(); ++i) {
        QListWidgetItem *item = m_playlistList->item(i);
        if (!item)
            continue;

        if (item->data(Qt::UserRole).toString() != playlistId)
            continue;

        m_playlistList->setCurrentRow(i);
        return (m_currentPlaylistId == playlistId);
    }

    return false;
}

QString MusicPlayer::findPlaylistIdByName(const QString &name) const
{
    if (!m_playlistManager)
        return {};

    const QString wanted = name.trimmed();
    if (wanted.isEmpty())
        return {};

    for (const auto &pl : m_playlistManager->playlists()) {
        if (pl.name.trimmed().compare(wanted, Qt::CaseInsensitive) == 0)
            return pl.id;
    }

    return {};
}

QString MusicPlayer::getLikedPlaylistId() const
{
    QSettings settings("MyCompany", "MusicPlayer");
    QString id = settings.value("LikedPlaylistId").toString();
    if (!id.isEmpty() && m_playlistManager->playlist(id))
        return id;

    // Fallback if not found in settings
    for (const auto &pl : m_playlistManager->playlists()) {
        if (pl.name.trimmed().compare(kLikedPlaylistName, Qt::CaseInsensitive) == 0) {
            settings.setValue("LikedPlaylistId", pl.id);
            return pl.id;
        }
    }
    return {};
}

QString MusicPlayer::ensureLikedPlaylist()
{
    const QString existingId = getLikedPlaylistId();
    if (!existingId.isEmpty())
        return existingId;

    const QString likedId = m_playlistManager->createPlaylist(kLikedPlaylistName);
    QSettings settings("MyCompany", "MusicPlayer");
    settings.setValue("LikedPlaylistId", likedId);

    primePlaylistCache(likedId, {});
    refreshPlaylistPanel();
    return likedId;
}

bool MusicPlayer::isTrackLiked(const QString &filePath) const
{
    const QString likedId = findPlaylistIdByName(kLikedPlaylistName);
    if (likedId.isEmpty())
        return false;

    const QString needle = normalizePathForCompare(filePath);
    for (const auto &pl : m_playlistManager->playlists()) {
        if (pl.id != likedId)
            continue;

        for (const QString &path : pl.trackPaths) {
            if (normalizePathForCompare(path) == needle)
                return true;
        }

        break;
    }

    return false;
}

void MusicPlayer::updateLikeButtonState()
{
    if (!m_likeButton)
        return;

    const QString outlineHeart = QString::fromUtf8("\xE2\x99\xA1");
    const QString filledHeart = QString::fromUtf8("\xE2\x99\xA5");
    const QString defaultStyle = "QPushButton { background: transparent; border: none; color: #b3b3b3; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: white; }";
    const QString activeStyle = "QPushButton { background: transparent; border: none; color: #1db954; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: #1ed760; }";

    if (m_currentIndex < 0 || m_currentIndex >= m_tracks.count()) {
        m_likeButton->setText(outlineHeart);
        m_likeButton->setStyleSheet(defaultStyle);
        return;
    }

    const QString filePath = m_tracks[m_currentIndex]->filePath();
    const bool liked = isTrackLiked(filePath);

    m_likeButton->setText(liked ? filledHeart : outlineHeart);
    m_likeButton->setStyleSheet(liked ? activeStyle : defaultStyle);

    if (m_fullscreenPlayer)
        m_fullscreenPlayer->updateLikeState(liked);
}

void MusicPlayer::onLikeButtonClicked()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_tracks.count())
        return;

    const QString filePath = QFileInfo(m_tracks[m_currentIndex]->filePath()).absoluteFilePath();
    if (filePath.isEmpty())
        return;

    const bool shouldLike = !isTrackLiked(filePath);
    if (!setTrackLiked(filePath, shouldLike)) {
        updateLikeButtonState();
        refreshLikeIndicatorsForPath(filePath);
        return;
    }

    const QString likedId = findPlaylistIdByName(kLikedPlaylistName);
    if (!likedId.isEmpty() && m_currentPlaylistId == likedId) {
        for (int i = 0; i < m_playlistList->count(); ++i) {
            if (m_playlistList->item(i)->data(Qt::UserRole).toString() == likedId) {
                m_currentPlaylistId.clear();
                onPlaylistSelected(i);
                break;
            }
        }
    } else {
        refreshLikeIndicatorsForPath(filePath);
    }

    updateLikeButtonState();
}

bool MusicPlayer::setTrackLiked(const QString &filePath, bool liked)
{
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();
    if (absolutePath.isEmpty())
        return false;

    QString likedId = findPlaylistIdByName(kLikedPlaylistName);

    if (liked) {
        likedId = ensureLikedPlaylist();
        if (likedId.isEmpty())
            return false;
        if (!isTrackLiked(absolutePath))
            m_playlistManager->addTrack(likedId, absolutePath);
    } else {
        if (likedId.isEmpty())
            return false;

        PlaylistInfo *likedPlaylist = m_playlistManager->playlist(likedId);
        if (!likedPlaylist)
            return false;

        const QString needle = normalizePathForCompare(absolutePath);
        QStringList updatedPaths;
        updatedPaths.reserve(likedPlaylist->trackPaths.size());
        for (const QString &path : likedPlaylist->trackPaths) {
            if (normalizePathForCompare(path) != needle)
                updatedPaths.append(path);
        }

        if (updatedPaths.size() == likedPlaylist->trackPaths.size())
            return false;

        m_playlistManager->setTracks(likedId, updatedPaths);
    }

    if (!likedId.isEmpty()) {
        if (PlaylistInfo *likedPlaylist = m_playlistManager->playlist(likedId))
            primePlaylistCache(likedId, likedPlaylist->trackPaths);
    }

    return true;
}

void MusicPlayer::refreshLikeIndicatorsForPath(const QString &filePath)
{
    const QString needle = normalizePathForCompare(filePath);
    for (int trackIndex = 0; trackIndex < m_tracks.count(); ++trackIndex) {
        if (normalizePathForCompare(m_tracks[trackIndex]->filePath()) == needle)
            updateTrackRow(trackIndex);
    }
}

void MusicPlayer::onBackgroundMetadataLoaded(const QString &filePath)
{
    // Safety check - if tracks list changed (playlist switched), ignore
    if (m_tracks.isEmpty())
        return;

    // Update UI for tracks matching this file path
    const QString needle = normalizePathForCompare(filePath);
    const int playingTrackIndex = resolvePlayingTrackIndex();

    for (int trackIndex = 0; trackIndex < m_tracks.count(); ++trackIndex) {
        TrackItem *track = m_tracks[trackIndex];
        if (track && normalizePathForCompare(track->filePath()) == needle) {
            track->ensureMetadataLoaded();
            updateTrackRow(trackIndex);
            
            const bool shouldUpdateBottom = (playingTrackIndex >= 0)
                ? (trackIndex == playingTrackIndex)
                : (m_engine->state() == GaplessAudioEngine::Stopped && trackIndex == m_currentIndex);

            if (shouldUpdateBottom)
                updateBottomBarFromTrack(track);
        }
    }
}

void MusicPlayer::onPlaylistCellClicked(int row, int column)
{
    if (column != COL_LIKED)
        return;

    const int trackIndex = getTrackIndexFromVisualRow(row);
    if (trackIndex < 0 || trackIndex >= m_tracks.count())
        return;

    const QString filePath = QFileInfo(m_tracks[trackIndex]->filePath()).absoluteFilePath();
    if (filePath.isEmpty())
        return;

    const bool shouldLike = !isTrackLiked(filePath);
    if (!setTrackLiked(filePath, shouldLike)) {
        updateLikeButtonState();
        refreshLikeIndicatorsForPath(filePath);
        return;
    }

    const QString likedId = findPlaylistIdByName(kLikedPlaylistName);
    if (!likedId.isEmpty() && m_currentPlaylistId == likedId) {
        for (int i = 0; i < m_playlistList->count(); ++i) {
            if (m_playlistList->item(i)->data(Qt::UserRole).toString() == likedId) {
                m_currentPlaylistId.clear();
                onPlaylistSelected(i);
                break;
            }
        }
    } else {
        refreshLikeIndicatorsForPath(filePath);
    }

    updateLikeButtonState();
}

void MusicPlayer::onPlaylistListRowsMoved(const QModelIndex &, int, int, const QModelIndex &, int)
{
    QStringList newOrder;
    for (int i = 0; i < m_playlistList->count(); ++i) {
        QListWidgetItem *item = m_playlistList->item(i);
        if (!item) continue;
        QString id = item->data(Qt::UserRole).toString();
        if (id != kAllPlaylistVirtualId) {
            newOrder.append(id);
        }
    }
    m_playlistManager->reorderPlaylists(newOrder);
}

void MusicPlayer::saveCurrentPlaylistTracks()
{
    if (m_currentPlaylistId.isEmpty() || m_currentPlaylistId == kAllPlaylistVirtualId)
        return;

    syncCurrentPlaylistCacheFromTracks();
    QStringList paths;
    for (TrackItem *t : m_tracks) {
        if (t->metadata().isCueTrack)
            paths.append(makeCueSavedPath(t->metadata().cueFilePath, t->metadata().trackNumber));
        else
            paths.append(t->filePath());
    }
    m_playlistManager->setTracks(m_currentPlaylistId, paths);
}

void MusicPlayer::onPlaylistSelected(int row)
{
    if (row < 0 || row >= m_playlistList->count()) return;
    QString newId = m_playlistList->item(row)->data(Qt::UserRole).toString();
    if (newId == m_currentPlaylistId) return;
    const bool isAllPlaylist = (newId == kAllPlaylistVirtualId);
    const bool forceMetadataLoad = (!m_forceMetadataLoadPlaylistId.isEmpty()
        && m_forceMetadataLoadPlaylistId == newId);
    if (forceMetadataLoad)
        m_forceMetadataLoadPlaylistId.clear();

    if (m_playlistTable)
        m_playlistTable->resetSmoothScroll();

    if (!m_currentPlaylistId.isEmpty() && m_playlistTable && m_playlistTable->verticalScrollBar()) {
        m_playlistScrollPositions.insert(m_currentPlaylistId,
                                         m_playlistTable->verticalScrollBar()->value());
    }

    rememberCurrentTrackForPlaylist(m_currentPlaylistId);

    const bool keepPlayback = (m_engine->state() == GaplessAudioEngine::Playing
                            || m_engine->state() == GaplessAudioEngine::Paused);
    const bool browsingDifferentPlaylistWhilePlaying =
        keepPlayback
        && !m_playbackPlaylistId.isEmpty()
        && m_playbackPlaylistId != newId;
    const QString activePath = m_engine->currentFilePath();

    saveCurrentPlaylistTracks();
    stopDeferredMetadataLoading();

    if (m_metadataTimeout)
        m_metadataTimeout->stop();
    m_metadataLoadIndex = -1;
    if (m_metadataLoader)
        m_metadataLoader->setSource(QUrl());

    if (!keepPlayback)
        m_engine->stop();

    if (!browsingDifferentPlaylistWhilePlaying) {
        m_engine->prepareNext(QString());
        m_preparedNextPath.clear();
    }
    m_currentIndex = -1;
    m_preparedNextIndex = -1;
    if (!(keepPlayback && !m_playbackPlaylistId.isEmpty()))
        clearShuffleRuntimeState(true);
    qDeleteAll(m_tracks);
    m_tracks.clear();

    m_currentPlaylistId = newId;
    if (!isAllPlaylist)
        syncPlaylistFromAutoSource(newId);

    QStringList cachedPaths;
    if (isAllPlaylist) {
        cachedPaths = collectAllPlaylistTracks();
        m_playlistTrackCache.insert(newId, cachedPaths);
    } else if (m_playlistTrackCache.contains(newId)) {
        cachedPaths = m_playlistTrackCache.value(newId);
    } else {
        PlaylistInfo *pl = m_playlistManager->playlist(newId);
        const QStringList paths = pl ? pl->trackPaths : QStringList{};
        primePlaylistCache(newId, paths);
        cachedPaths = m_playlistTrackCache.value(newId);
    }

    m_tracks.reserve(cachedPaths.size());
    {
        QMap<QString, QList<CueTrack>> cueCache;
        QMap<QString, QImage>          coverCache;

        for (const QString &path : cachedPaths) {
            if (isCueSavedPath(path)) {
                QString cuePath; int trackNum;
                if (!parseCueSavedPath(path, cuePath, trackNum))
                    continue;

                if (!cueCache.contains(cuePath))
                    cueCache[cuePath] = CueParser::parse(cuePath);

                for (const CueTrack &ct : cueCache[cuePath]) {
                    if (ct.trackNumber != trackNum)
                        continue;
                    // Pass a null QImage to defer cover loading to the background thread.
                    // The first track will cache the cover, subsequent tracks will use the cache.
                    m_tracks.append(TrackItem::fromCueTrack(ct, QImage()));
                    break;
                }
                continue;
            }

            m_tracks.append(new TrackItem(path, true));
        }
    }

    if (m_tracks.size() != cachedPaths.size()) {
        if (isAllPlaylist) {
            QStringList paths;
            paths.reserve(m_tracks.size());
            for (TrackItem *track : m_tracks)
                paths.append(track->filePath());
            m_playlistTrackCache.insert(m_currentPlaylistId, paths);
        } else {
            syncCurrentPlaylistCacheFromTracks();
            QStringList paths;
            paths.reserve(m_tracks.size());
            for (TrackItem *track : m_tracks)
                paths.append(track->filePath());
            m_playlistManager->setTracks(m_currentPlaylistId, paths);
        }
    }

    m_currentIndex = findRememberedTrackIndexForPlaylist(newId);

    if (m_currentIndex < 0 && keepPlayback)
        m_currentIndex = resolvePlayingTrackIndex();

    if (m_currentIndex >= 0)
        rememberCurrentTrackForPlaylist(newId);

    if (m_currentIndex >= 0 && m_currentIndex < m_tracks.count()) {
        TrackItem *currentTrack = m_tracks[m_currentIndex];
        if (currentTrack && !currentTrack->isMetadataLoaded() && m_metadataThread)
            m_metadataThread->queuePriorityRange(QStringList{currentTrack->filePath()});
    }

    refreshTable();
    if (forceMetadataLoad)
        forceLoadMetadataForTracks(m_tracks, QStringLiteral("Loading metadata"));

    const QString targetPlaylistId = newId;
    const int savedScroll = m_playlistScrollPositions.value(targetPlaylistId, 0);
    QTimer::singleShot(0, this, [this, targetPlaylistId, savedScroll]() {
        if (!m_playlistTable || m_currentPlaylistId != targetPlaylistId)
            return;
        QScrollBar *bar = m_playlistTable->verticalScrollBar();
        if (!bar)
            return;
        const int clamped = qBound(bar->minimum(), savedScroll, bar->maximum());
        bar->setValue(clamped);
        m_playlistTable->resetSmoothScroll();
    });

    // Prioritize visible rows immediately after table is rendered.
    QTimer::singleShot(50, this, [this]() { onPlaylistScroll(0); });

    // Start metadata warmup immediately on playlist enter (no scroll required).
    if (m_metadataThread && !m_tracks.isEmpty()) {
        const int warmupCount = qMin(kPlaylistWarmupCount, m_tracks.count());
        QStringList warmup;
        warmup.reserve(warmupCount);
        for (int i = 0; i < warmupCount; ++i) {
            TrackItem *track = m_tracks[i];
            if (track && !track->isMetadataLoaded())
                warmup.append(track->filePath());
        }
        if (!warmup.isEmpty())
            m_metadataThread->queuePriorityRange(warmup);
    }

    if (keepPlayback && !browsingDifferentPlaylistWhilePlaying)
        resyncPreparedNext();
    startDeferredMetadataLoading(0);
    scheduleGlobalMetadataPreload(120);
    m_metadataLoadIndex = 0;
    preloadTrackMetadata();
    updatePlaylistHighlight();
    updateLikeButtonState();
}

void MusicPlayer::setPlaylistAutoSourceDirectory(const QString &playlistId)
{
    PlaylistInfo *pl = m_playlistManager->playlist(playlistId);
    if (!pl)
        return;

    const QString initialDir = pl->autoSourceDir.isEmpty()
        ? QDir::homePath()
        : pl->autoSourceDir;

    const QString selectedDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Auto Source Folder"),
        initialDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (selectedDir.isEmpty())
        return;

    const QString absoluteDir = QFileInfo(selectedDir).absoluteFilePath();
    m_playlistManager->setAutoSourceDir(playlistId, absoluteDir);
    syncPlaylistFromAutoSource(playlistId);
    refreshPlaylistPanel();

    if (playlistId == m_currentPlaylistId) {
        for (int i = 0; i < m_playlistList->count(); ++i) {
            if (m_playlistList->item(i)->data(Qt::UserRole).toString() == playlistId) {
                m_currentPlaylistId.clear();
                onPlaylistSelected(i);
                break;
            }
        }
    }
}

void MusicPlayer::configurePlaylistAutoSourceDirectory(const QString &playlistId)
{
    if (playlistId == kAllPlaylistVirtualId)
        return;

    PlaylistInfo *pl = m_playlistManager->playlist(playlistId);
    if (!pl)
        return;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Auto Source"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("Playlist: %1").arg(pl->name));

    const QString currentSource = pl->autoSourceDir.trimmed();
    if (currentSource.isEmpty()) {
        box.setInformativeText(QStringLiteral("No auto source folder is configured."));
    } else {
        box.setInformativeText(
            QStringLiteral("Current auto source folder:\n%1")
                .arg(QDir::toNativeSeparators(currentSource)));
    }

    QPushButton *chooseButton = box.addButton(QStringLiteral("Choose Folder..."), QMessageBox::AcceptRole);
    QPushButton *clearButton = box.addButton(QStringLiteral("Clear"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);

    if (currentSource.isEmpty())
        clearButton->setEnabled(false);

    box.exec();

    if (box.clickedButton() == chooseButton) {
        setPlaylistAutoSourceDirectory(playlistId);
    } else if (box.clickedButton() == clearButton) {
        clearPlaylistAutoSourceDirectory(playlistId);
    }
}

void MusicPlayer::clearPlaylistAutoSourceDirectory(const QString &playlistId)
{
    if (playlistId.isEmpty())
        return;

    m_playlistManager->setAutoSourceDir(playlistId, QString());
    refreshPlaylistPanel();
}

void MusicPlayer::syncPlaylistFromAutoSource(const QString &playlistId)
{
    if (playlistId == kAllPlaylistVirtualId)
        return;

    PlaylistInfo *pl = m_playlistManager->playlist(playlistId);
    if (!pl)
        return;

    const QString sourceDir = pl->autoSourceDir.trimmed();
    if (sourceDir.isEmpty())
        return;

    QFileInfo dirInfo(sourceDir);
    if (!dirInfo.exists() || !dirInfo.isDir())
        return;

    const QStringList tracks = collectTracksFromAutoSource(dirInfo.absoluteFilePath());
    m_playlistManager->setTracks(playlistId, tracks);
    primePlaylistCache(playlistId, tracks);
    scheduleGlobalMetadataPreload(80);
}

void MusicPlayer::scheduleGlobalMetadataPreload(int delayMs)
{
    if (!m_metadataThread || !m_globalMetadataPreloadTimer || !m_playlistManager)
        return;

    const QStringList allPaths = collectUniqueTrackPathsForBackgroundPreload();
    if (allPaths.isEmpty())
        return;

    int added = 0;
    for (const QString &path : allPaths) {
        const QString key = normalizePathForCompare(path);
        if (key.isEmpty() || m_globalMetadataQueuedKeys.contains(key))
            continue;

        m_globalMetadataQueuedKeys.insert(key);
        m_globalMetadataPreloadPaths.append(path);
        ++added;
    }

    if (added <= 0)
        return;

    if (m_globalMetadataPreloadIndex < 0)
        m_globalMetadataPreloadIndex = 0;

    if (!m_globalMetadataPreloadTimer->isActive())
        m_globalMetadataPreloadTimer->start(qMax(0, delayMs));
}

void MusicPlayer::processGlobalMetadataPreloadBatch()
{
    if (!m_metadataThread)
        return;

    if (m_globalMetadataPreloadIndex < 0
        || m_globalMetadataPreloadIndex >= m_globalMetadataPreloadPaths.size()) {
        m_globalMetadataPreloadPaths.clear();
        m_globalMetadataPreloadIndex = -1;
        return;
    }

    const int endIndex = qMin(m_globalMetadataPreloadIndex + kGlobalMetadataPreloadBatchSize,
                              m_globalMetadataPreloadPaths.size());
    QStringList batch;
    batch.reserve(endIndex - m_globalMetadataPreloadIndex);
    for (int i = m_globalMetadataPreloadIndex; i < endIndex; ++i)
        batch.append(m_globalMetadataPreloadPaths[i]);

    m_globalMetadataPreloadIndex = endIndex;

    if (!batch.isEmpty())
        m_metadataThread->queuePathRange(batch);

    if (m_globalMetadataPreloadIndex >= m_globalMetadataPreloadPaths.size()) {
        m_globalMetadataPreloadPaths.clear();
        m_globalMetadataPreloadIndex = -1;
        return;
    }

    if (m_globalMetadataPreloadTimer)
        m_globalMetadataPreloadTimer->start(kGlobalMetadataPreloadBatchDelayMs);
}

QStringList MusicPlayer::collectUniqueTrackPathsForBackgroundPreload() const
{
    QStringList result;
    if (!m_playlistManager)
        return result;

    QSet<QString> seen;
    QMap<QString, QList<CueTrack>> cueCache;

    const auto appendIfExists = [&result, &seen](const QString &candidate) {
        QFileInfo fi(candidate);
        if (!fi.exists() || !fi.isFile())
            return;

        const QString absolutePath = fi.absoluteFilePath();
        const QString key = normalizePathForCompare(absolutePath);
        if (key.isEmpty() || seen.contains(key))
            return;

        seen.insert(key);
        result.append(absolutePath);
    };

    for (const auto &pl : m_playlistManager->playlists()) {
        for (const QString &path : pl.trackPaths) {
            if (isCueSavedPath(path)) {
                QString cuePath;
                int trackNum = 0;
                if (!parseCueSavedPath(path, cuePath, trackNum) || !QFileInfo::exists(cuePath))
                    continue;

                if (!cueCache.contains(cuePath))
                    cueCache.insert(cuePath, CueParser::parse(cuePath));

                for (const CueTrack &ct : cueCache.value(cuePath)) {
                    if (ct.trackNumber == trackNum) {
                        appendIfExists(ct.audioFilePath);
                        break;
                    }
                }
                continue;
            }

            appendIfExists(path);
        }
    }

    return result;
}

QStringList MusicPlayer::collectAllPlaylistTracks() const
{
    QStringList tracks;
    QSet<QString> seen;
    const QString likedId = findPlaylistIdByName(kLikedPlaylistName);

    for (const auto &pl : m_playlistManager->playlists()) {
        if (!likedId.isEmpty() && pl.id == likedId)
            continue;
        if (pl.name.trimmed().compare(kLikedPlaylistName, Qt::CaseInsensitive) == 0)
            continue;

        for (const QString &path : pl.trackPaths) {
            if (isCueSavedPath(path)) {
                QString cuePath;
                int trackNum = 0;
                if (parseCueSavedPath(path, cuePath, trackNum) && QFileInfo::exists(cuePath)) {
                    if (!seen.contains(path)) {
                        seen.insert(path);
                        tracks.append(path);
                    }
                }
                continue;
            }

            QFileInfo fi(path);
            if (!fi.exists())
                continue;

            const QString absolutePath = fi.absoluteFilePath();
            const QString key = normalizePathForCompare(absolutePath);
            if (seen.contains(key))
                continue;

            seen.insert(key);
            tracks.append(absolutePath);
        }
    }

    return tracks;
}

QStringList MusicPlayer::collectTracksFromAutoSource(const QString &dirPath) const
{
    QStringList tracks;
    QStringList filters = audioNameFilters();
    filters.append(QStringLiteral("*.cue"));
    
    QDirIterator it(dirPath,
                    filters,
                    QDir::Files,
                    QDirIterator::Subdirectories);

    QSet<QString> cueAudioKeys;

    while (it.hasNext()) {
        const QString path = it.next();
        QFileInfo fi(path);
        if (!fi.exists()) continue;

        if (fi.suffix().toLower() == QStringLiteral("cue")) {
            const QList<CueTrack> cueTracks = CueParser::parse(fi.absoluteFilePath());
            for (const CueTrack &ct : cueTracks) {
                if (QFileInfo::exists(ct.audioFilePath)) {
                    tracks.append(makeCueSavedPath(ct.cueFilePath, ct.trackNumber));
                    cueAudioKeys.insert(normalizePathForCompare(ct.audioFilePath));
                }
            }
        } else {
            tracks.append(fi.absoluteFilePath());
        }
    }

    // Filter out audio files that were referenced by CUE files
    QStringList finalTracks;
    for (const QString &path : tracks) {
        if (isCueSavedPath(path)) {
            finalTracks.append(path);
        } else {
            if (!cueAudioKeys.contains(normalizePathForCompare(path))) {
                finalTracks.append(path);
            }
        }
    }

    std::sort(finalTracks.begin(), finalTracks.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });

    finalTracks.removeDuplicates();
    return finalTracks;
}

void MusicPlayer::createNewPlaylist()
{
    saveCurrentPlaylistTracks();
    QString name = "New Playlist";
    int num = 1;
    QSet<QString> names;
    for (const auto &pl : m_playlistManager->playlists())
        names.insert(pl.name);
    while (names.contains(name))
        name = QString("New Playlist %1").arg(++num);

    QString id = m_playlistManager->createPlaylist(name);
    refreshPlaylistPanel();

    for (int i = 0; i < m_playlistList->count(); ++i) {
        if (m_playlistList->item(i)->data(Qt::UserRole).toString() == id) {
            m_playlistList->setCurrentRow(i);
            m_playlistList->editItem(m_playlistList->item(i));
            break;
        }
    }
}

void MusicPlayer::renameSelectedPlaylist()
{
    QListWidgetItem *item = m_playlistList->currentItem();
    if (!item) return;

    item->setFlags(item->flags() | Qt::ItemIsEditable);
    m_playlistList->editItem(item);
    // Note: actual rename persistence is handled by the QListWidget::itemChanged
    // signal connected in the constructor, so double-click, F2, and context-menu
    // rename all flow through the same path. Edit triggers stay set to
    // DoubleClicked | EditKeyPressed (set once at widget creation) so double-click
    // continues to work after any rename.
}

void MusicPlayer::deleteSelectedPlaylist()
{
    QListWidgetItem *item = m_playlistList->currentItem();
    if (!item) return;
    QString id = item->data(Qt::UserRole).toString();
    if (id == kAllPlaylistVirtualId || id == getLikedPlaylistId())
        return;

    m_playlistScrollPositions.remove(id);

    const bool deletingPlayingPlaylist = (id == m_playbackPlaylistId);
    const bool deletingViewedPlaylist = (id == m_currentPlaylistId);

    if (deletingPlayingPlaylist) {
        m_engine->stop();
        m_playbackPlaylistId.clear();
        m_currentIndex = -1;
        m_playbackTrackKey.clear();
    }

    if (deletingViewedPlaylist) {
        stopDeferredMetadataLoading();
        qDeleteAll(m_tracks);
        m_tracks.clear();
        m_currentPlaylistId.clear();
        refreshTable();
    }

    clearPlaylistCache(id);
    m_playlistLastTrackPath.remove(id);

    m_playlistManager->deletePlaylist(id);
    refreshPlaylistPanel();

    if (m_playlistManager->count() == 0) {
        m_playlistManager->createPlaylist("Default");
        refreshPlaylistPanel();
    }
    if (m_playlistList->count() > 0 && m_playlistList->currentRow() < 0)
        m_playlistList->setCurrentRow(0);
}

void MusicPlayer::importM3UPlaylist()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        "Import Playlist Files",
        QString(),
        "Playlist files (*.m3u *.m3u8);;All Files (*)");
    importPlaylistFiles(paths);
}

void MusicPlayer::importPlaylistFiles(const QStringList &paths)
{
    if (paths.isEmpty())
        return;

    saveCurrentPlaylistTracks();

    QSet<QString> usedNames;
    for (const auto &pl : m_playlistManager->playlists())
        usedNames.insert(pl.name);

    QString firstImportedId;

    for (const QString &path : paths) {
        if (!isPlaylistFilePath(path))
            continue;

        const QStringList tracks = m_playlistManager->importM3U(path);
        if (tracks.isEmpty())
            continue;

        QString baseName = QFileInfo(path).completeBaseName().trimmed();
        if (baseName.isEmpty())
            baseName = QStringLiteral("Imported Playlist");

        QString playlistName = baseName;
        int suffix = 2;
        while (usedNames.contains(playlistName)) {
            playlistName = QStringLiteral("%1 (%2)").arg(baseName).arg(suffix);
            ++suffix;
        }
        usedNames.insert(playlistName);

        const QString id = m_playlistManager->createPlaylist(playlistName);
        m_playlistManager->setTracks(id, tracks);
        primePlaylistCache(id, tracks);
        if (firstImportedId.isEmpty())
            firstImportedId = id;
    }

    if (firstImportedId.isEmpty())
        return;

    refreshPlaylistPanel();
    m_forceMetadataLoadPlaylistId = firstImportedId;
    for (int i = 0; i < m_playlistList->count(); ++i) {
        if (m_playlistList->item(i)->data(Qt::UserRole).toString() == firstImportedId) {
            m_playlistList->setCurrentRow(i);
            break;
        }
    }

    scheduleGlobalMetadataPreload(60);
}

void MusicPlayer::exportM3UPlaylist()
{
    QListWidgetItem *item = m_playlistList->currentItem();
    if (!item) return;
    QString id = item->data(Qt::UserRole).toString();
    if (id == kAllPlaylistVirtualId)
        return;

    saveCurrentPlaylistTracks();

    PlaylistInfo *pl = m_playlistManager->playlist(id);
    if (!pl) return;

    QString path = QFileDialog::getSaveFileName(this, "Export M3U Playlist", pl->name + ".m3u",
                                                 "M3U Playlists (*.m3u);;All Files (*)");
    if (path.isEmpty()) return;
    m_playlistManager->exportM3U(id, path);
}

// ============= Drag & Drop / Files =============

bool MusicPlayer::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Space) {
            QWidget *focused = QApplication::focusWidget();
            const bool inTextInput = qobject_cast<QLineEdit *>(focused) != nullptr
                                || qobject_cast<QTextEdit *>(focused) != nullptr;
            if (!inTextInput) {
                playPause();
                return true;
            }
        }
    }
    if (event->type() == QEvent::Wheel) {
        auto *we = static_cast<QWheelEvent *>(event);
        if (we->modifiers() & Qt::AltModifier) {
            const int raw = we->angleDelta().x() + we->angleDelta().y();
            if (raw == 0) return true;
            const int delta = raw > 0 ? 1 : -1;
            m_volumeSlider->setValue(qBound(0, m_volumeSlider->value() + delta, 100));
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::XButton1 || me->button() == Qt::XButton2) {
            if (event->type() == QEvent::MouseButtonPress)
                return true;

            const bool isQWidgetWindow = watched->inherits("QWidgetWindow");
            if (isQWidgetWindow) {
                if (me->button() == Qt::XButton1) previous();
                else next();
            }
            return true;
        }
    }
    // Volume icon click → toggle mute
    if (watched == m_volumeLabel && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            static int s_preMuteVol = 60;
            int v = m_volumeSlider->value();
            if (v > 0) {
                s_preMuteVol = v;
                m_volumeSlider->setValue(0);
            } else {
                m_volumeSlider->setValue(s_preMuteVol > 0 ? s_preMuteVol : 60);
            }
            return true;
        }
    }

    // Cover art click → open fullscreen player
    if (watched == m_bottomCoverLabel && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            TrackItem *track = (m_currentIndex >= 0 && m_currentIndex < m_tracks.count())
                               ? m_tracks[m_currentIndex] : nullptr;
            QPixmap cover;
            QString title  = "No track playing";
            QString artist;
            QString album;
            int     dur    = 0;
            if (track) {
                cover  = track->metadata().coverPixmap();
                title  = track->metadata().title.isEmpty()
                         ? QFileInfo(track->filePath()).baseName()
                         : track->metadata().title;
                artist = track->metadata().artist;
                album  = track->metadata().album;
                dur    = static_cast<int>(track->metadata().duration);
            }
            bool playing = m_engine && m_engine->state() == GaplessAudioEngine::Playing;
            int  pos     = m_engine ? static_cast<int>(m_engine->position()) : 0;
            int  vol     = m_volumeSlider->value();

            m_fullscreenPlayer->setGeometry(rect());
            m_fullscreenPlayer->openFor(cover, title, artist, album, dur, pos, playing, vol);
            return true;
        }
    }

    const bool isPlaylistKeyTarget = (watched == m_playlistList || watched == m_playlistList->viewport());
    if (isPlaylistKeyTarget && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_F2) {
            renameSelectedPlaylist();
            return true;
        }
    }

    if (watched == m_playlistList->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            QListWidgetItem *item = m_playlistList->itemAt(me->pos());
            if (!item) {
                createNewPlaylist();
                return true;
            }
        }
    }

    const bool isPlaylistDropTarget = (watched == m_playlistList || watched == m_playlistList->viewport());
    if (!isPlaylistDropTarget)
        return QMainWindow::eventFilter(watched, event);

    if (event->type() == QEvent::DragEnter) {
        auto *dragEvent = static_cast<QDragEnterEvent *>(event);
        if (!dragEvent->mimeData()->hasUrls())
            return false;

        bool hasAcceptablePath = false;
        for (const QUrl &url : dragEvent->mimeData()->urls()) {
            const QString local = url.toLocalFile();
            if (local.isEmpty())
                continue;

            if (isPlaylistFilePath(local)) {
                hasAcceptablePath = true;
                break;
            }

            QFileInfo fi(local);
            if (fi.exists() && (fi.isFile() || fi.isDir())) {
                hasAcceptablePath = true;
                break;
            }
        }

        if (hasAcceptablePath) {
            dragEvent->acceptProposedAction();
            return true;
        }

        return false;
    }

    if (event->type() == QEvent::DragMove) {
        auto *dragMoveEvent = static_cast<QDragMoveEvent *>(event);
        if (!dragMoveEvent->mimeData()->hasUrls())
            return false;

        bool hasAcceptablePath = false;
        for (const QUrl &url : dragMoveEvent->mimeData()->urls()) {
            const QString local = url.toLocalFile();
            if (local.isEmpty())
                continue;

            if (isPlaylistFilePath(local)) {
                hasAcceptablePath = true;
                break;
            }

            QFileInfo fi(local);
            if (fi.exists() && (fi.isFile() || fi.isDir())) {
                hasAcceptablePath = true;
                break;
            }
        }

        if (hasAcceptablePath) {
            dragMoveEvent->acceptProposedAction();
            return true;
        }

        return false;
    }

    if (event->type() == QEvent::Drop) {
        auto *dropEvent = static_cast<QDropEvent *>(event);
        if (!dropEvent->mimeData()->hasUrls())
            return false;

        QStringList playlistFiles;
        QStringList mediaFiles;
        for (const QUrl &url : dropEvent->mimeData()->urls()) {
            const QString local = url.toLocalFile();
            if (local.isEmpty())
                continue;

            if (isPlaylistFilePath(local))
                playlistFiles.append(local);
            else
                mediaFiles.append(local);
        }

        if (!playlistFiles.isEmpty())
            importPlaylistFiles(playlistFiles);
        if (!mediaFiles.isEmpty())
            onFilesDropped(mediaFiles, -1);

        if (!playlistFiles.isEmpty() || !mediaFiles.isEmpty()) {
            dropEvent->acceptProposedAction();
            return true;
        }

        return false;
    }

    return QMainWindow::eventFilter(watched, event);
}

void MusicPlayer::dragEnterEvent(QDragEnterEvent *event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }

void MusicPlayer::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_bottomGlow) {
        m_bottomGlow->move(0, centralWidget()->height() - m_bottomGlow->height());
    }
    if (m_fullscreenPlayer)
        m_fullscreenPlayer->setGeometry(rect());
}

void MusicPlayer::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);
    // child widget — no action needed, moves with parent automatically
}
void MusicPlayer::dropEvent(QDropEvent *event) {
    if (event->mimeData()->hasUrls()) {
        QStringList playlistFiles;
        QStringList mediaFiles;
        for (const QUrl &url : event->mimeData()->urls()) {
            const QString local = url.toLocalFile();
            if (isPlaylistFilePath(local))
                playlistFiles << local;
            else
                mediaFiles << local;
        }

        if (!playlistFiles.isEmpty())
            importPlaylistFiles(playlistFiles);
        if (!mediaFiles.isEmpty())
            onFilesDropped(mediaFiles, -1);

        event->acceptProposedAction();
    }
}

void MusicPlayer::onFilesDropped(const QStringList &files, int targetVisualRow)
{
    if (m_currentPlaylistId == kAllPlaylistVirtualId) {
        QMessageBox::information(this,
                                 "Add Tracks",
                                 "Select a playlist to add files. The 'all' playlist is read-only.");
        return;
    }

    const int prevCount = m_tracks.count();
    TrackItem *currentTrack = (m_currentIndex >= 0 && m_currentIndex < m_tracks.count())
                                  ? m_tracks[m_currentIndex] : nullptr;

    int insertIndex = -1;
    if (targetVisualRow >= 0) {
        if (targetVisualRow >= m_playlistTable->rowCount()) {
            insertIndex = prevCount;
        } else {
            const int targetTrackIndex = getTrackIndexFromVisualRow(targetVisualRow);
            if (targetTrackIndex >= 0 && targetTrackIndex <= prevCount)
                insertIndex = targetTrackIndex;
        }
    }

    for (const QString &path : files) {
        QFileInfo fileInfo(path);
        if (fileInfo.isDir()) addDirectory(path);
        else if (fileInfo.isFile()) addFile(path);
    }

    const int newCount = m_tracks.count();
    if (newCount == prevCount)
        return;

    int metadataStartIndex = prevCount;
    QList<TrackItem*> insertedTracks;
    if (insertIndex >= 0 && insertIndex < prevCount) {
        insertedTracks.reserve(newCount - prevCount);
        for (int i = prevCount; i < newCount; ++i)
            insertedTracks.append(m_tracks[i]);

        for (int i = newCount - 1; i >= prevCount; --i)
            m_tracks.removeAt(i);

        const int boundedInsertIndex = qBound(0, insertIndex, m_tracks.count());
        for (int i = 0; i < insertedTracks.count(); ++i)
            m_tracks.insert(boundedInsertIndex + i, insertedTracks[i]);

        metadataStartIndex = boundedInsertIndex;
    }

    QList<TrackItem*> importedTracks;
    if (!insertedTracks.isEmpty()) {
        importedTracks = insertedTracks;
    } else {
        importedTracks.reserve(newCount - prevCount);
        for (int i = prevCount; i < newCount; ++i)
            importedTracks.append(m_tracks[i]);
    }

    if (currentTrack)
        m_currentIndex = m_tracks.indexOf(currentTrack);

    clearShuffleRuntimeState(false);
    refreshTable();
    QTimer::singleShot(50, this, [this]() { onPlaylistScroll(0); });
    m_trackCountLabel->setText(QString("%1 tracks").arg(m_tracks.count()));
    forceLoadMetadataForTracks(importedTracks, QStringLiteral("Loading metadata"));
    saveCurrentPlaylistTracks();
    resyncPreparedNext();
    startDeferredMetadataLoading(metadataStartIndex);
    scheduleGlobalMetadataPreload(0);

    if (kEnableQtMetadataPreload && m_metadataLoadIndex < 0) {
        m_metadataLoadIndex = metadataStartIndex;
        preloadTrackMetadata();
    }
}

// ============= Metadata Loading =============

void MusicPlayer::preloadTrackMetadata()
{
    if (!kEnableQtMetadataPreload || !m_metadataLoader || !m_metadataTimeout) {
        m_metadataLoadIndex = -1;
        return;
    }

    // Skip if already loading
    if (m_metadataLoader->mediaStatus() == QMediaPlayer::LoadingMedia)
        return;

    while (m_metadataLoadIndex >= 0 && m_metadataLoadIndex < m_tracks.count()) {
        if (!m_tracks[m_metadataLoadIndex]->isMetadataLoaded())
            break;
        m_metadataLoadIndex++;
    }

    if (m_metadataLoadIndex >= m_tracks.count() || m_metadataLoadIndex < 0) {
        m_metadataLoadIndex = -1;
        m_metadataTimeout->stop();
        return;
    }

    qDebug() << "Loading metadata for index" << m_metadataLoadIndex
             << "of" << m_tracks.count()
             << m_tracks[m_metadataLoadIndex]->filePath();

    // Use single-shot timer to defer loading to next event loop tick
    // This prevents blocking the main thread during playlist load
    QTimer::singleShot(1, this, [this]() {
        if (m_metadataLoadIndex >= 0 && m_metadataLoadIndex < m_tracks.count()) {
            m_metadataTimeout->start();
            m_metadataLoader->setSource(QUrl::fromLocalFile(m_tracks[m_metadataLoadIndex]->filePath()));
        }
    });
}

void MusicPlayer::forceLoadMetadataForTracks(const QList<TrackItem*> &tracks, const QString &title)
{
    if (tracks.isEmpty())
        return;

    QList<TrackItem*> pending;
    pending.reserve(tracks.size());
    for (TrackItem *track : tracks) {
        if (track && !track->isMetadataLoaded())
            pending.append(track);
    }

    if (pending.isEmpty())
        return;

    const int total = pending.count();

    QProgressDialog progress(this);
    progress.setWindowTitle(title.isEmpty()
                                ? QStringLiteral("Loading metadata")
                                : title);
    progress.setLabelText(QStringLiteral("Loading metadata..."));
    progress.setRange(0, total);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.show();

    if (!m_metadataThread || !m_metadataThread->isRunning()) {
        int loaded = 0;
        for (TrackItem *track : pending) {
            if (!track)
                continue;
            track->ensureMetadataLoaded();
            ++loaded;
            progress.setValue(loaded);
            if ((loaded % 4) == 0)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        progress.setValue(total);
    } else {
        QStringList pendingPaths;
        pendingPaths.reserve(pending.size());
        for (TrackItem *track : pending) {
            if (track)
                pendingPaths.append(track->filePath());
        }

        m_metadataThread->clearTrackQueue();
        m_metadataThread->queuePriorityRange(pendingPaths);

        QElapsedTimer timer;
        timer.start();
        int lastLoaded = -1;
        while (true) {
            int loaded = 0;
            QString currentPath;
            for (TrackItem *track : pending) {
                if (!track)
                    continue;
                if (track->isMetadataLoaded()) {
                    ++loaded;
                } else if (currentPath.isEmpty()) {
                    currentPath = track->filePath();
                }
            }

            if (loaded != lastLoaded) {
                lastLoaded = loaded;
                timer.restart(); // Reset timeout on progress
                QString label = QStringLiteral("Loading metadata... %1/%2")
                                    .arg(loaded)
                                    .arg(total);
                if (!currentPath.isEmpty())
                    label += QStringLiteral("\n%1").arg(QFileInfo(currentPath).fileName());
                progress.setLabelText(label);
                progress.setValue(loaded);
            }

            if (loaded >= total)
                break;

            // Safety timeout: 15 seconds without any progress
            if (timer.elapsed() > 15000) {
                qWarning() << "forceLoadMetadataForTracks: timeout reached, breaking loop";
                break;
            }

            if (progress.wasCanceled())
                break;

            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            QThread::msleep(10);
        }

        progress.setValue(total);
    }

    QList<TrackItem*> coverPending;
    coverPending.reserve(pending.size());
    for (TrackItem *track : pending) {
        if (track && track->isCoverPlaceholder())
            coverPending.append(track);
    }

    if (coverPending.isEmpty())
        return;

    QMediaPlayer coverPlayer;
    QAudioOutput coverOutput;
    coverOutput.setVolume(0.0f);
    coverPlayer.setAudioOutput(&coverOutput);

    auto loadCoverFromQtMetadata = [&coverPlayer](const QString &filePath, int timeoutMs) -> QImage {
        if (filePath.isEmpty())
            return {};

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);

        bool finished = false;
        auto finish = [&]() {
            if (finished)
                return;
            finished = true;
            loop.quit();
        };

        QObject::connect(&coverPlayer, &QMediaPlayer::mediaStatusChanged, &loop,
                         [&](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::LoadedMedia
                || status == QMediaPlayer::BufferedMedia
                || status == QMediaPlayer::InvalidMedia) {
                finish();
            }
        });
        QObject::connect(&coverPlayer, &QMediaPlayer::metaDataChanged, &loop, [&]() {
            if (!coverPlayer.metaData().isEmpty())
                finish();
        });
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { finish(); });

        coverPlayer.setSource(QUrl::fromLocalFile(filePath));
        timer.start(qMax(200, timeoutMs));
        loop.exec();

        QImage cover;
        const QMediaMetaData metaData = coverPlayer.metaData();
        QVariant coverVar = metaData.value(QMediaMetaData::ThumbnailImage);
        if (!coverVar.isValid())
            coverVar = metaData.value(QMediaMetaData::CoverArtImage);
        if (coverVar.isValid())
            cover = coverVar.value<QImage>();

        coverPlayer.setSource(QUrl());
        return cover;
    };

    progress.setLabelText(QStringLiteral("Loading covers..."));
    progress.setRange(0, coverPending.count());
    progress.setValue(0);

    int coverLoaded = 0;
    for (TrackItem *track : coverPending) {
        if (!track)
            continue;

        const QImage cover = loadCoverFromQtMetadata(track->filePath(), 1400);
        if (track->applyMetadataCover(cover)) {
            const int trackIndex = m_tracks.indexOf(track);
            if (trackIndex >= 0)
                updateTrackRow(trackIndex);

            const int playingIdx = resolvePlayingTrackIndex();
            const bool shouldUpdateBottom = (playingIdx >= 0)
                ? (trackIndex == playingIdx)
                : (m_engine->state() == GaplessAudioEngine::Stopped && trackIndex == m_currentIndex);

            if (shouldUpdateBottom)
                updateBottomBarFromTrack(track);
        }

        ++coverLoaded;
        progress.setValue(coverLoaded);
        if ((coverLoaded % 2) == 0)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void MusicPlayer::onMetaDataLoaded()
{
    if (!kEnableQtMetadataPreload || !m_metadataLoader || !m_metadataTimeout)
        return;

    QMediaPlayer::MediaStatus status = m_metadataLoader->mediaStatus();

    if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
        m_metadataTimeout->stop();

        if (m_metadataLoadIndex >= 0 && m_metadataLoadIndex < m_tracks.count()) {
            TrackItem *track = m_tracks[m_metadataLoadIndex];

            track->loadFullMetadata(m_metadataLoader->metaData());

            qint64 dur = m_metadataLoader->duration();
            if (dur > 0)
                track->metadata().duration = dur;

            if (track->metadata().bitrate == 0 && track->metadata().duration > 0) {
                QFileInfo fi(track->filePath());
                track->metadata().bitrate = static_cast<int>((fi.size() * 8.0) / (track->metadata().duration / 1000.0));
            }

            updateTrackRow(m_metadataLoadIndex);

            const int playingIdx = resolvePlayingTrackIndex();
            const bool shouldUpdateBottom = (playingIdx >= 0)
                ? (m_metadataLoadIndex == playingIdx)
                : (m_engine->state() == GaplessAudioEngine::Stopped && m_metadataLoadIndex == m_currentIndex);

            if (shouldUpdateBottom)
                updateBottomBarFromTrack(track);
        }

        m_metadataLoadIndex++;
        QTimer::singleShot(50, this, &MusicPlayer::preloadTrackMetadata);

    } else if (status == QMediaPlayer::InvalidMedia) {
        m_metadataTimeout->stop();
        qDebug() << "Invalid media at index" << m_metadataLoadIndex
                 << (m_metadataLoadIndex >= 0 && m_metadataLoadIndex < m_tracks.count()
                         ? m_tracks[m_metadataLoadIndex]->filePath() : "");
        m_metadataLoadIndex++;
        QTimer::singleShot(50, this, &MusicPlayer::preloadTrackMetadata);
    }
}

void MusicPlayer::startDeferredMetadataLoading(int startIndex)
{
    if (!m_deferredMetadataTimer)
        return;

    if (m_tracks.isEmpty()) {
        stopDeferredMetadataLoading();
        return;
    }

    m_deferredMetadataPlaylistId = m_currentPlaylistId;
    m_deferredMetadataIndex = qBound(0, startIndex, m_tracks.count() - 1);
    m_deferredMetadataStopIndex = qMin(m_deferredMetadataIndex + kDeferredMetadataMaxTracks,
                                       m_tracks.count());
    if (!m_deferredMetadataTimer->isActive())
        m_deferredMetadataTimer->start(0);
}

void MusicPlayer::stopDeferredMetadataLoading()
{
    if (m_deferredMetadataTimer)
        m_deferredMetadataTimer->stop();
    m_deferredMetadataIndex = -1;
    m_deferredMetadataStopIndex = -1;
    m_deferredMetadataPlaylistId.clear();
    if (m_metadataThread)
        m_metadataThread->clearTrackQueue();
}

void MusicPlayer::processDeferredMetadataBatch()
{
    // Background loading - just add tracks to thread queue
    if (!m_metadataThread)
        return;

    if (m_deferredMetadataIndex < 0
        || m_deferredMetadataIndex >= m_tracks.count()
        || m_deferredMetadataStopIndex < 0
        || m_deferredMetadataIndex >= m_deferredMetadataStopIndex
        || m_deferredMetadataPlaylistId != m_currentPlaylistId) {
        stopDeferredMetadataLoading();
        return;
    }

    const int batchSize = kDeferredMetadataBatchSize;
    QStringList batch;
    for (int i = 0;
         i < batchSize
             && m_deferredMetadataIndex < m_tracks.count()
             && m_deferredMetadataIndex < m_deferredMetadataStopIndex;
         ++i) {
        TrackItem *track = m_tracks[m_deferredMetadataIndex];
        if (track && !track->isMetadataLoaded()) {
            batch.append(track->filePath());
        }
        ++m_deferredMetadataIndex;
    }

    if (!batch.isEmpty()) {
        m_metadataThread->queueRange(batch);
    }

    if (m_deferredMetadataIndex >= m_tracks.count()
        || (m_deferredMetadataStopIndex >= 0 && m_deferredMetadataIndex >= m_deferredMetadataStopIndex)) {
        stopDeferredMetadataLoading();
        return;
    }

    m_deferredMetadataTimer->start(kDeferredMetadataBatchDelayMs);
}

// ============= Scroll-based Lazy Loading =============

void MusicPlayer::onPlaylistScroll(int value)
{
    Q_UNUSED(value);

    if (!m_playlistTable)
        return;

    if (!m_currentPlaylistId.isEmpty()) {
        if (QScrollBar *bar = m_playlistTable->verticalScrollBar())
            m_playlistScrollPositions.insert(m_currentPlaylistId, bar->value());
    }

    if (!m_metadataThread)
        return;

    const int rowCount = m_playlistTable->rowCount();
    if (rowCount == 0)
        return;

    // Use rowAt for visible range; fall back to row height estimate if not yet rendered
    int firstVisible = m_playlistTable->rowAt(0);
    int lastVisible = m_playlistTable->rowAt(m_playlistTable->viewport()->height() - 1);

    if (firstVisible < 0) {
        // Table not yet rendered — estimate from row height
        const int rowH = qMax(1, m_rowHeight);
        const int viewH = qMax(1, m_playlistTable->viewport()->height());
        firstVisible = 0;
        lastVisible = qMin(rowCount - 1, viewH / rowH + 1);
    } else if (lastVisible < 0) {
        lastVisible = rowCount - 1;
    }

    QStringList priority;
    QStringList nearby;

    // Visible rows — highest priority
    for (int row = firstVisible; row <= lastVisible && row < rowCount; ++row) {
        int trackIdx = getTrackIndexFromVisualRow(row);
        if (trackIdx >= 0 && trackIdx < m_tracks.count()) {
            TrackItem *track = m_tracks[trackIdx];
            if (track && !track->isMetadataLoaded())
                priority.append(track->filePath());
        }
    }

    // Rows just above and below visible area
    for (int offset = 1; offset <= 20; ++offset) {
        for (int row : {firstVisible - offset, lastVisible + offset}) {
            if (row < 0 || row >= rowCount)
                continue;
            int trackIdx = getTrackIndexFromVisualRow(row);
            if (trackIdx >= 0 && trackIdx < m_tracks.count()) {
                TrackItem *t = m_tracks[trackIdx];
                if (t && !t->isMetadataLoaded() && !nearby.contains(t->filePath()) && !priority.contains(t->filePath()))
                    nearby.append(t->filePath());
            }
        }
    }

    if (!priority.isEmpty())
        m_metadataThread->queuePriorityRange(priority);
    if (!nearby.isEmpty())
        m_metadataThread->queuePriorityRange(nearby);
}

// ============= Row Reorder =============

void MusicPlayer::onRowsMoved(QList<int> sourceRows, int targetRow)
{
    if (m_currentPlaylistId == kAllPlaylistVirtualId)
        return;

    if (sourceRows.isEmpty()) return;

    TrackItem *currentTrack = (m_currentIndex >= 0 && m_currentIndex < m_tracks.count())
                                  ? m_tracks[m_currentIndex] : nullptr;

    QList<TrackItem*> movedTracks;
    for (int row : sourceRows) {
        int trackIndex = getTrackIndexFromVisualRow(row);
        if (trackIndex >= 0 && trackIndex < m_tracks.count())
            movedTracks.append(m_tracks[trackIndex]);
    }
    if (movedTracks.isEmpty()) return;

    for (TrackItem *track : movedTracks)
        m_tracks.removeOne(track);

    int adjustedTarget = targetRow;
    for (int row : sourceRows) {
        if (row < targetRow) adjustedTarget--;
    }
    adjustedTarget = qBound(0, adjustedTarget, m_tracks.count());

    for (int i = 0; i < movedTracks.count(); ++i)
        m_tracks.insert(adjustedTarget + i, movedTracks[i]);

    if (currentTrack)
        m_currentIndex = m_tracks.indexOf(currentTrack);

    clearShuffleRuntimeState(true);
    refreshTable();
    saveCurrentPlaylistTracks();
    resyncPreparedNext();
}

void MusicPlayer::syncTracksToTableOrder()
{
    if (m_currentPlaylistId == kAllPlaylistVirtualId)
        return;

    TrackItem *currentTrack = (m_currentIndex >= 0 && m_currentIndex < m_tracks.count())
                                  ? m_tracks[m_currentIndex] : nullptr;

    QList<TrackItem*> reorderedTracks;
    for (int i = 0; i < m_playlistTable->rowCount(); ++i) {
        int trackIndex = getTrackIndexFromVisualRow(i);
        if (trackIndex >= 0 && trackIndex < m_tracks.count())
            reorderedTracks.append(m_tracks[trackIndex]);
    }

    m_tracks = reorderedTracks;

    if (currentTrack)
        m_currentIndex = m_tracks.indexOf(currentTrack);

    clearShuffleRuntimeState(true);
    refreshTable();
    syncCurrentPlaylistCacheFromTracks();
    resyncPreparedNext();
}

// ============= File Helpers =============

void MusicPlayer::addFile(const QString &filePath)
{
    QFileInfo fileInfo(filePath);
    const QString ext = fileInfo.suffix().toLower();

    if (ext == QStringLiteral("cue")) {
        addCueFile(filePath);
        return;
    }

    static const QStringList audioFormats = {
        QStringLiteral("mp3"),  QStringLiteral("wav"),  QStringLiteral("flac"),
        QStringLiteral("ogg"),  QStringLiteral("m4a"),  QStringLiteral("aac"),
        QStringLiteral("wma"),  QStringLiteral("opus"), QStringLiteral("ape"),
    };

    if (audioFormats.contains(ext))
        m_tracks.append(new TrackItem(filePath, true));
}

void MusicPlayer::addDirectory(const QString &dirPath)
{
    QStringList filters = audioNameFilters();
    filters.append(QStringLiteral("*.cue"));

    QDirIterator it(dirPath, filters, QDir::Files, QDirIterator::Subdirectories);
    QStringList cueFiles;
    QStringList directAudioFiles;

    while (it.hasNext()) {
        QString path = it.next();
        if (path.endsWith(QStringLiteral(".cue"), Qt::CaseInsensitive))
            cueFiles.append(path);
        else
            directAudioFiles.append(path);
    }

    QSet<QString> cueAudioKeys;
    // Process CUE files first to identify referenced audio files
    for (const QString &cuePath : cueFiles) {
        const QList<CueTrack> cueTracks = CueParser::parse(cuePath);
        if (cueTracks.isEmpty()) continue;

        QMap<QString, QImage> coverCache;
        for (const CueTrack &ct : cueTracks) {
            if (!QFileInfo::exists(ct.audioFilePath)) continue;

            cueAudioKeys.insert(normalizePathForCompare(ct.audioFilePath));
            if (!coverCache.contains(ct.audioFilePath))
                coverCache[ct.audioFilePath] = TrackItem::coverArtFromFile(ct.audioFilePath);

            m_tracks.append(TrackItem::fromCueTrack(ct, coverCache[ct.audioFilePath]));
        }
    }

    // Add remaining audio files that are NOT part of a CUE
    for (const QString &audioPath : directAudioFiles) {
        if (cueAudioKeys.contains(normalizePathForCompare(audioPath)))
            continue;
        addFile(audioPath);
    }
}

void MusicPlayer::addCueFile(const QString &cuePath)
{
    const QList<CueTrack> cueTracks = CueParser::parse(cuePath);
    if (cueTracks.isEmpty()) {
        QMessageBox::warning(this,
                             "CUE Import",
                             "No tracks were found in the CUE file.");
        return;
    }

    int added = 0;

    for (const CueTrack &ct : cueTracks) {
        if (!QFileInfo::exists(ct.audioFilePath))
            continue;

        // Pass a null QImage to defer cover loading to the background thread.
        m_tracks.append(TrackItem::fromCueTrack(ct, QImage()));
        ++added;
    }

    if (added == 0) {
        QMessageBox::warning(this,
                             "CUE Import",
                             "CUE references audio files that were not found.");
    }
}

void MusicPlayer::advanceCueTrack()
{
    const QString viewedPlaylistId = m_currentPlaylistId;
    const bool shouldRestoreView = !viewedPlaylistId.isEmpty()
        && !m_playbackPlaylistId.isEmpty()
        && viewedPlaylistId != m_playbackPlaylistId;
    
    if (shouldRestoreView)
        setUpdatesEnabled(false);

    ensurePlaybackContextPlaylistActive();

    const int currentIdx = m_currentIndex;
    m_activeCueEndMs = -1;

    const int nextIdx = getNextTrackIndex(true);
    if (nextIdx < 0 || nextIdx >= m_tracks.count()) {
        m_engine->stop();
        m_positionSlider->setValue(0);
        
        if (shouldRestoreView) {
            if (m_currentPlaylistId != viewedPlaylistId)
                selectPlaylistById(viewedPlaylistId);
            setUpdatesEnabled(true);
            update();
        }
        return;
    }

    const bool canDoInFileCueSeek =
        currentIdx >= 0
        && currentIdx < m_tracks.count()
        && m_engine
        && m_tracks[currentIdx]->metadata().isCueTrack
        && m_tracks[nextIdx]->metadata().isCueTrack
        && normalizePathForCompare(m_tracks[currentIdx]->filePath())
            == normalizePathForCompare(m_tracks[nextIdx]->filePath())
        && normalizePathForCompare(m_engine->currentFilePath())
            == normalizePathForCompare(m_tracks[nextIdx]->filePath());

    m_currentIndex = nextIdx;
    updatePlaybackTrackKey();

    if (canDoInFileCueSeek) {
        const qint64 currentEndMs = m_tracks[currentIdx]->metadata().cueEndMs;
        TrackItem *track = m_tracks[m_currentIndex];
        const qint64 nextStartMs = qMax<qint64>(0, track->metadata().cueStartMs);

        m_activeIsCue = track->metadata().isCueTrack;
        m_activeCueStartMs = nextStartMs;
        m_activeCueEndMs = track->metadata().cueEndMs;

        // Skip in-file seek for contiguous CUE boundaries to avoid decoder/output reset glitches.
        const bool contiguousBoundary = currentEndMs >= 0
            && qAbs(nextStartMs - currentEndMs) <= 15;
        if (!contiguousBoundary)
            m_engine->seek(nextStartMs);
        
        const qint64 dur = track->metadata().duration;
        if (dur > 0) {
            m_positionSlider->setRange(0, static_cast<int>(dur));
            m_totalTimeLabel->setText(formatTime(dur));
        }
        m_currentTimeLabel->setText(formatTime(0));
        updateBottomBarFromTrack(track);
        updateLikeButtonState();
        updatePlaylistHighlight();

        if (shouldRestoreView) {
            if (m_currentPlaylistId != viewedPlaylistId)
                selectPlaylistById(viewedPlaylistId);
            setUpdatesEnabled(true);
            update();
        }
        return;
    }

    playCurrentItem();

    if (shouldRestoreView) {
        if (m_currentPlaylistId != viewedPlaylistId)
            selectPlaylistById(viewedPlaylistId);
        setUpdatesEnabled(true);
        update();
    }
}

// ============= Table =============

void MusicPlayer::refreshTable()
{
    const QString outlineHeart = QString::fromUtf8("\xE2\x99\xA1");
    const QString filledHeart = QString::fromUtf8("\xE2\x99\xA5");

    int coverSize = qMax(30, m_rowHeight - 10);
    m_playlistTable->setIconSize(QSize(coverSize, coverSize));

    m_playlistTable->setUpdatesEnabled(false);
    m_playlistTable->setSortingEnabled(false);
    m_playlistTable->clearContents();
    m_playlistTable->setRowCount(m_tracks.count());

    for(int i = 0; i < m_tracks.count(); ++i) {
        m_playlistTable->setRowHeight(i, m_rowHeight);

        const TrackMetadata &meta = m_tracks[i]->metadata();

        QTableWidgetItem *coverItem = new QTableWidgetItem();
        if (!meta.coverArt.isNull())
            coverItem->setData(Qt::DecorationRole, QPixmap::fromImage(meta.coverArt));
        coverItem->setData(Qt::UserRole, i);
        coverItem->setFlags(coverItem->flags() & ~Qt::ItemIsEditable);
        m_playlistTable->setItem(i, COL_COVER, coverItem);

        QTableWidgetItem *trackNumItem = new QTableWidgetItem();
        trackNumItem->setData(Qt::UserRole, i);
        trackNumItem->setData(Qt::DisplayRole, meta.trackNumber > 0 ? meta.trackNumber : i + 1);
        trackNumItem->setFlags(trackNumItem->flags() & ~Qt::ItemIsEditable);
        m_playlistTable->setItem(i, COL_TRACK, trackNumItem);

        const bool liked = isTrackLiked(meta.filePath);
        QTableWidgetItem *likedItem = new QTableWidgetItem(liked ? filledHeart : outlineHeart);
        likedItem->setTextAlignment(Qt::AlignCenter);
        likedItem->setData(Qt::UserRole, i);
        likedItem->setFlags(likedItem->flags() & ~Qt::ItemIsEditable);
        likedItem->setForeground(liked ? QColor("#1db954") : QColor("#b3b3b3"));
        m_playlistTable->setItem(i, COL_LIKED, likedItem);

        const auto addTextItem = [this, i](int col, const QString &text) {
            QTableWidgetItem *item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, i);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_playlistTable->setItem(i, col, item);
        };

        addTextItem(COL_TITLE, meta.title);
        addTextItem(COL_ARTIST, meta.artist);
        addTextItem(COL_ALBUM, meta.album);
        addTextItem(COL_YEAR, meta.year);
        addTextItem(COL_GENRE, meta.genre);
        addTextItem(COL_BITRATE, meta.bitrate > 0 ? QString::number(meta.bitrate / 1000) : QString());
        addTextItem(COL_SAMPLERATE, meta.sampleRate > 0 ? QString::number(meta.sampleRate / 1000.0, 'f', 1) : QString());
        addTextItem(COL_DURATION, formatTime(meta.duration));
        addTextItem(COL_FILEPATH, meta.filePath);
    }
    m_playlistTable->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    m_playlistTable->setSortingEnabled(true);
    m_playlistTable->setUpdatesEnabled(true);
    m_trackCountLabel->setText(QString("%1 tracks").arg(m_tracks.count()));
    updatePlaylistHighlight();
}

void MusicPlayer::filterPlaylist(const QString &text)
{
    for (int row = 0; row < m_playlistTable->rowCount(); ++row) {
        int trackIndex = getTrackIndexFromVisualRow(row);
        if (trackIndex == -1) continue;

        const TrackMetadata &meta = m_tracks[trackIndex]->metadata();

        bool matches = text.isEmpty() ||
                       meta.title.contains(text, Qt::CaseInsensitive) ||
                       meta.artist.contains(text, Qt::CaseInsensitive) ||
                       meta.album.contains(text, Qt::CaseInsensitive);

        m_playlistTable->setRowHidden(row, !matches);
    }
}

void MusicPlayer::removeSelected()
{
    if (m_currentPlaylistId == kAllPlaylistVirtualId)
        return;

    QList<QTableWidgetItem*> selectedItems = m_playlistTable->selectedItems();
    if (selectedItems.isEmpty()) return;

    QSet<int> rowsToRemove;
    for (QTableWidgetItem *item : selectedItems) rowsToRemove.insert(item->row());

    QList<int> sortedRows = rowsToRemove.values();
    std::sort(sortedRows.begin(), sortedRows.end(), std::greater<int>());

    QList<int> trackIndicesToRemove;
    for (int row : sortedRows) {
        int trackIndex = getTrackIndexFromVisualRow(row);
        if (trackIndex != -1) trackIndicesToRemove.append(trackIndex);
    }
    std::sort(trackIndicesToRemove.begin(), trackIndicesToRemove.end(), std::greater<int>());

    TrackItem *currentTrack = (m_currentIndex >= 0 && m_currentIndex < m_tracks.count())
                                  ? m_tracks[m_currentIndex] : nullptr;
    bool removedCurrentTrack = false;

    for (int trackIndex : trackIndicesToRemove) {
        if (trackIndex < 0 || trackIndex >= m_tracks.count())
            continue;
        TrackItem *track = m_tracks[trackIndex];
        if (track == currentTrack)
            removedCurrentTrack = true;
        delete m_tracks.takeAt(trackIndex);
    }

    if (removedCurrentTrack) {
        if (m_engine->state() != GaplessAudioEngine::Stopped)
            m_engine->stop();
        m_currentIndex = -1;
        m_positionSlider->setValue(0);
    } else if (currentTrack) {
        m_currentIndex = m_tracks.indexOf(currentTrack);
    }

    m_preparedNextIndex = -1;
    m_preparedNextPath.clear();
    clearShuffleRuntimeState(true);
    refreshTable();
    QTimer::singleShot(50, this, [this]() { onPlaylistScroll(0); });
    m_trackCountLabel->setText(QString("%1 tracks").arg(m_tracks.count()));
    saveCurrentPlaylistTracks();
    resyncPreparedNext();
    startDeferredMetadataLoading(0);
    updateLikeButtonState();
}

// ============= Playback =============

void MusicPlayer::playPause()
{
    if (m_engine->state() == GaplessAudioEngine::Playing) {
        m_engine->pause();
    } else if (m_engine->state() == GaplessAudioEngine::Paused) {
        m_engine->resume();
    } else {
        if (m_tracks.isEmpty()) return;
        if (m_currentIndex < 0) {
            for (int i = 0; i < m_playlistTable->rowCount(); ++i) {
                if (!m_playlistTable->isRowHidden(i)) {
                    m_currentIndex = getTrackIndexFromVisualRow(i);
                    break;
                }
            }
            if (m_currentIndex < 0) m_currentIndex = 0;
        }
        playCurrentItem();
    }
}

void MusicPlayer::stop() { 
    m_engine->stop(); 
    m_positionSlider->setValue(0);
    m_activeIsCue = false;
    m_activeCueStartMs = -1;
    m_activeCueEndMs = -1;
}

bool MusicPlayer::isValidTrackIndex(int index) const
{
    return index >= 0 && index < m_tracks.count();
}

bool MusicPlayer::isShuffleHistoryMode() const
{
    return m_shuffleMode == ShuffleHistory;
}

void MusicPlayer::clearShuffleRuntimeState(bool clearHistory)
{
    m_shufflePendingNextIndex = -1;
    m_shuffleCyclePool.clear();
    if (clearHistory) {
        m_shuffleBackHistory.clear();
        m_shuffleForwardHistory.clear();
    }
}

void MusicPlayer::applyShuffleButtonStyle()
{
    if (!m_shuffleButton)
        return;

    if (m_fullscreenPlayer)
        m_fullscreenPlayer->updateShuffleState(m_shuffleEnabled, m_shuffleMode);

    if (!m_shuffleEnabled) {
        m_shuffleButton->setText(QString::fromUtf8("\xF0\x9F\x94\x80"));
        m_shuffleButton->setToolTip("Shuffle: Off");
        m_shuffleButton->setStyleSheet("QPushButton { background: transparent; border: none; color: #b3b3b3; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: white; }");
        return;
    }

    if (isShuffleHistoryMode()) {
        m_shuffleButton->setText(QString::fromUtf8("\xF0\x9F\x94\x80") + "S");
        m_shuffleButton->setToolTip("Shuffle mode: History-aware");
        m_shuffleButton->setStyleSheet("QPushButton { background: transparent; border: none; color: #1db954; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: #1ed760; }");
    } else {
        m_shuffleButton->setText(QString::fromUtf8("\xF0\x9F\x94\x80") + "R");
        m_shuffleButton->setToolTip("Shuffle mode: Random in both directions");
        m_shuffleButton->setStyleSheet("QPushButton { background: transparent; border: none; color: #00bcd4; font-size: 16px; padding: 4px 8px; } QPushButton:hover { color: #38d6ea; }");
    }
}

QList<int> MusicPlayer::buildOrderedTrackIndices() const
{
    QList<int> orderedIndices;
    for (int i = 0; i < m_playlistTable->rowCount(); ++i) {
        if (m_playlistTable->isRowHidden(i))
            continue;

        const int trackIndex = getTrackIndexFromVisualRow(i);
        if (trackIndex >= 0 && trackIndex < m_tracks.count() && !orderedIndices.contains(trackIndex))
            orderedIndices.append(trackIndex);
    }

    // Fallback: if filtering hides all rows, preserve logical playlist traversal.
    if (orderedIndices.isEmpty()) {
        orderedIndices.reserve(m_tracks.count());
        for (int i = 0; i < m_tracks.count(); ++i)
            orderedIndices.append(i);
    }

    return orderedIndices;
}

void MusicPlayer::sanitizeShuffleRuntime(const QList<int> &orderedIndices)
{
    QSet<int> allowed;
    for (int idx : orderedIndices)
        allowed.insert(idx);

    auto sanitizeStack = [this, &allowed](QVector<int> &stack) {
        QVector<int> cleaned;
        cleaned.reserve(stack.size());
        for (int idx : stack) {
            if (isValidTrackIndex(idx) && allowed.contains(idx))
                cleaned.append(idx);
        }
        stack.swap(cleaned);
    };

    sanitizeStack(m_shuffleBackHistory);
    sanitizeStack(m_shuffleForwardHistory);

    QList<int> cleanedPool;
    cleanedPool.reserve(m_shuffleCyclePool.size());
    for (int idx : m_shuffleCyclePool) {
        if (isValidTrackIndex(idx) && allowed.contains(idx))
            cleanedPool.append(idx);
    }
    m_shuffleCyclePool.swap(cleanedPool);

    if (!isValidTrackIndex(m_shufflePendingNextIndex) || !allowed.contains(m_shufflePendingNextIndex))
        m_shufflePendingNextIndex = -1;
}

int MusicPlayer::chooseShuffleCandidate(const QList<int> &orderedIndices, bool commitSelection)
{
    if (orderedIndices.isEmpty())
        return -1;

    if (orderedIndices.size() == 1) {
        const int only = orderedIndices.first();
        if (commitSelection)
            m_shufflePendingNextIndex = -1;
        else
            m_shufflePendingNextIndex = only;
        return only;
    }

    QList<int> candidates = orderedIndices;
    candidates.removeAll(m_currentIndex);
    if (candidates.isEmpty())
        candidates = orderedIndices;

    const auto candidateContains = [&candidates](int idx) {
        return candidates.contains(idx);
    };

    if (m_shufflePendingNextIndex >= 0 && candidateContains(m_shufflePendingNextIndex)) {
        const int pending = m_shufflePendingNextIndex;
        if (commitSelection) {
            m_shufflePendingNextIndex = -1;
            if (m_shuffleNoRepeats)
                m_shuffleCyclePool.removeAll(pending);
        }
        return pending;
    }

    if (!m_shuffleNoRepeats) {
        const int chosen = candidates[QRandomGenerator::global()->bounded(candidates.size())];
        if (commitSelection)
            m_shufflePendingNextIndex = -1;
        else
            m_shufflePendingNextIndex = chosen;
        return chosen;
    }

    QList<int> filteredPool;
    filteredPool.reserve(m_shuffleCyclePool.size());
    for (int idx : m_shuffleCyclePool) {
        if (candidates.contains(idx))
            filteredPool.append(idx);
    }
    m_shuffleCyclePool.swap(filteredPool);

    if (m_shuffleCyclePool.isEmpty())
        m_shuffleCyclePool = candidates;

    if (m_shuffleCyclePool.isEmpty())
        return -1;

    const int chosen = m_shuffleCyclePool[QRandomGenerator::global()->bounded(m_shuffleCyclePool.size())];
    if (commitSelection) {
        m_shuffleCyclePool.removeAll(chosen);
        m_shufflePendingNextIndex = -1;
    } else {
        m_shufflePendingNextIndex = chosen;
    }

    return chosen;
}

int MusicPlayer::getNextTrackIndex(bool commitSelection,
                                   bool ignoreRepeatOneForManualAdvance)
{
    if (m_tracks.isEmpty()) return -1;

    if (!ignoreRepeatOneForManualAdvance
        && m_repeatMode == 2
        && isValidTrackIndex(m_currentIndex))
        return m_currentIndex;

    const QList<int> orderedIndices = buildOrderedTrackIndices();

    if (orderedIndices.isEmpty())
        return -1;

    if (!m_shuffleEnabled) {
        if (commitSelection)
            clearShuffleRuntimeState(true);

        const int currentPos = orderedIndices.indexOf(m_currentIndex);
        if (currentPos < 0)
            return orderedIndices.first();

        if (currentPos + 1 < orderedIndices.size())
            return orderedIndices[currentPos + 1];

        return orderedIndices.first();
    }

    sanitizeShuffleRuntime(orderedIndices);

    if (isShuffleHistoryMode()) {
        if (commitSelection) {
            while (!m_shuffleForwardHistory.isEmpty()) {
                const int forwardIdx = m_shuffleForwardHistory.takeLast();
                if (!orderedIndices.contains(forwardIdx))
                    continue;
                if (orderedIndices.size() > 1 && forwardIdx == m_currentIndex)
                    continue;

                if (isValidTrackIndex(m_currentIndex) && m_currentIndex != forwardIdx)
                    m_shuffleBackHistory.append(m_currentIndex);

                if (m_shuffleNoRepeats)
                    m_shuffleCyclePool.removeAll(forwardIdx);
                m_shufflePendingNextIndex = -1;
                return forwardIdx;
            }
        } else {
            for (int i = m_shuffleForwardHistory.size() - 1; i >= 0; --i) {
                const int forwardIdx = m_shuffleForwardHistory[i];
                if (!orderedIndices.contains(forwardIdx))
                    continue;
                if (orderedIndices.size() > 1 && forwardIdx == m_currentIndex)
                    continue;
                m_shufflePendingNextIndex = forwardIdx;
                return forwardIdx;
            }
        }
    }

    const int chosen = chooseShuffleCandidate(orderedIndices, commitSelection);
    if (chosen < 0)
        return -1;

    if (commitSelection && isShuffleHistoryMode()) {
        if (isValidTrackIndex(m_currentIndex) && m_currentIndex != chosen)
            m_shuffleBackHistory.append(m_currentIndex);
        m_shuffleForwardHistory.clear();
    }

    return chosen;
}

void MusicPlayer::commitShuffleAdvance(int previousIndex, int nextIndex)
{
    if (!m_shuffleEnabled)
        return;

    if (!isValidTrackIndex(previousIndex) || !isValidTrackIndex(nextIndex))
        return;

    if (previousIndex == nextIndex) {
        m_shufflePendingNextIndex = -1;
        return;
    }

    const QList<int> orderedIndices = buildOrderedTrackIndices();
    sanitizeShuffleRuntime(orderedIndices);

    if (isShuffleHistoryMode()) {
        if (!m_shuffleForwardHistory.isEmpty() && m_shuffleForwardHistory.last() == nextIndex)
            m_shuffleForwardHistory.removeLast();
        else
            m_shuffleForwardHistory.clear();

        m_shuffleBackHistory.append(previousIndex);
    }

    if (m_shuffleNoRepeats)
        m_shuffleCyclePool.removeAll(nextIndex);

    m_shufflePendingNextIndex = -1;
}

int MusicPlayer::getPreviousTrackIndex()
{
    if (m_tracks.isEmpty())
        return -1;

    const QList<int> orderedIndices = buildOrderedTrackIndices();
    if (orderedIndices.isEmpty())
        return -1;

    if (!m_shuffleEnabled) {
        const int currentPos = orderedIndices.indexOf(m_currentIndex);
        if (currentPos < 0)
            return orderedIndices.last();
        if (currentPos > 0)
            return orderedIndices[currentPos - 1];
        return orderedIndices.last();
    }

    sanitizeShuffleRuntime(orderedIndices);

    if (!isShuffleHistoryMode())
        return chooseShuffleCandidate(orderedIndices, true);

    while (!m_shuffleBackHistory.isEmpty()) {
        const int prevIdx = m_shuffleBackHistory.takeLast();
        if (!orderedIndices.contains(prevIdx))
            continue;
        if (orderedIndices.size() > 1 && prevIdx == m_currentIndex)
            continue;

        if (isValidTrackIndex(m_currentIndex) && m_currentIndex != prevIdx)
            m_shuffleForwardHistory.append(m_currentIndex);

        m_shufflePendingNextIndex = -1;
        return prevIdx;
    }

    const int currentPos = orderedIndices.indexOf(m_currentIndex);
    if (currentPos < 0)
        return orderedIndices.last();
    if (currentPos > 0)
        return orderedIndices[currentPos - 1];
    return orderedIndices.last();
}

int MusicPlayer::findTrackIndexByPath(const QString &filePath) const
{
    if (filePath.isEmpty())
        return -1;

    if (isCueSavedPath(filePath)) {
        QString cuePath;
        int trackNum = 0;
        if (parseCueSavedPath(filePath, cuePath, trackNum)) {
            const QString needle = normalizePathForCompare(cuePath);
            for (int i = 0; i < m_tracks.count(); ++i) {
                const TrackMetadata &md = m_tracks[i]->metadata();
                if (md.isCueTrack && md.trackNumber == trackNum &&
                    normalizePathForCompare(md.cueFilePath) == needle) {
                    return i;
                }
            }
        }
        return -1;
    }

    const QString needle = normalizePathForCompare(filePath);
    for (int i = 0; i < m_tracks.count(); ++i) {
        if (normalizePathForCompare(m_tracks[i]->filePath()) == needle)
            return i;
    }

    return -1;
}

void MusicPlayer::resyncPreparedNext()
{
    if (m_engine->state() == GaplessAudioEngine::Playing
        || m_engine->state() == GaplessAudioEngine::Paused) {
        prepareNextGapless();
        return;
    }

    m_preparedNextIndex = -1;
    m_preparedNextPath.clear();
    m_engine->prepareNext(QString());
}

void MusicPlayer::next()
{
    const QString viewedPlaylistId = m_currentPlaylistId;
    const bool shouldRestoreView = !viewedPlaylistId.isEmpty()
        && !m_playbackPlaylistId.isEmpty()
        && viewedPlaylistId != m_playbackPlaylistId;
    if (shouldRestoreView)
        setUpdatesEnabled(false);

    ensurePlaybackContextPlaylistActive();

    if (!m_tracks.isEmpty()) {
        if (!isValidTrackIndex(m_currentIndex)) {
            const int rememberedIndex = findRememberedTrackIndexForPlaylist(m_currentPlaylistId);
            if (isValidTrackIndex(rememberedIndex))
                m_currentIndex = rememberedIndex;
        }

        const int nextIdx = getNextTrackIndex(true, true);
        if (nextIdx >= 0) {
            m_currentIndex = nextIdx;
            playCurrentItem();
        }
    }

    if (shouldRestoreView) {
        if (m_currentPlaylistId != viewedPlaylistId)
            selectPlaylistById(viewedPlaylistId);
        setUpdatesEnabled(true);
        update();
    }
}

void MusicPlayer::previous()
{
    const QString viewedPlaylistId = m_currentPlaylistId;
    const bool shouldRestoreView = !viewedPlaylistId.isEmpty()
        && !m_playbackPlaylistId.isEmpty()
        && viewedPlaylistId != m_playbackPlaylistId;
    if (shouldRestoreView)
        setUpdatesEnabled(false);

    ensurePlaybackContextPlaylistActive();

    if (!m_tracks.isEmpty()) {
        if (!isValidTrackIndex(m_currentIndex)) {
            const int rememberedIndex = findRememberedTrackIndexForPlaylist(m_currentPlaylistId);
            if (isValidTrackIndex(rememberedIndex))
                m_currentIndex = rememberedIndex;
        }

        bool activePlaylistTrackIsPlaying = false;
        if (m_engine && isValidTrackIndex(m_currentIndex)) {
            const QString enginePath = QFileInfo(m_engine->currentFilePath()).absoluteFilePath();
            const QString selectedPath = QFileInfo(m_tracks[m_currentIndex]->filePath()).absoluteFilePath();
            if (!enginePath.isEmpty() && !selectedPath.isEmpty()) {
                activePlaylistTrackIsPlaying =
                    normalizePathForCompare(enginePath) == normalizePathForCompare(selectedPath);
            }
        }

        qint64 currentPos = m_engine->position();
        if (m_activeIsCue)
            currentPos -= m_activeCueStartMs;

        // If current track is playing (or paused) and we're more than 3 seconds in, restart it.
        // Otherwise, go to the previous track in the playlist.
        if (activePlaylistTrackIsPlaying && currentPos > 3000) {
            m_resumeOnPlayPath.clear();
            m_resumeOnPlayPositionMs = -1;
            playCurrentItem();
        } else {
            const int prevIdx = getPreviousTrackIndex();
            if (isValidTrackIndex(prevIdx)) {
                m_currentIndex = prevIdx;
                m_resumeOnPlayPath.clear();
                m_resumeOnPlayPositionMs = -1;
                playCurrentItem();
            }
        }
    }

    if (shouldRestoreView) {
        if (m_currentPlaylistId != viewedPlaylistId)
            selectPlaylistById(viewedPlaylistId);
        setUpdatesEnabled(true);
        update();
    }
}

void MusicPlayer::seek(int position)
{
    if (!m_engine) {
        qWarning() << "[seek-ui] seek ignored: engine is null" << "targetMs=" << position;
        return;
    }

    qInfo() << "[seek-ui] dispatch seek"
            << "targetMs=" << position
            << "enginePosBeforeMs=" << m_engine->position()
            << "durationMs=" << m_engine->duration()
            << "state=" << engineStateToString(m_engine->state());
    
    qint64 engineTarget = position;
    if (m_activeIsCue)
        engineTarget = m_activeCueStartMs + static_cast<qint64>(position);
    
    m_engine->seek(engineTarget);
}

void MusicPlayer::updatePosition(qint64 position) {
    if (m_activeIsCue) {
        if (m_activeCueEndMs >= 0 && position >= m_activeCueEndMs) {
            m_activeCueEndMs = -1;
            QTimer::singleShot(0, this, &MusicPlayer::advanceCueTrack);
            return;
        }
        const qint64 rel = qMax<qint64>(0, position - m_activeCueStartMs);
        if (!m_userSeeking && !m_seekPending)
            m_positionSlider->setValue(static_cast<int>(rel));
        // Don't update time label during seek - position values are unreliable
        if (!m_seekPending)
            m_currentTimeLabel->setText(formatTime(rel));
        // Always forward to fullscreen player — lyrics need the updates to
        // re-anchor the interpolator. Without this, a main-window seek would
        // leave the fullscreen interpolator running forward from a stale
        // anchor for 300ms, then desync until updatePosition resumes, which
        // sometimes left the lyrics view permanently stuck on the wrong line
        // after multiple consecutive seeks.
        if (m_fullscreenPlayer)
            m_fullscreenPlayer->updatePosition(static_cast<int>(rel));
        return;
    }

    if (!m_userSeeking && !m_seekPending) m_positionSlider->setValue(static_cast<int>(position));
    // Don't update time label during seek - position values are unreliable until decoder catches up
    if (!m_seekPending)
        m_currentTimeLabel->setText(formatTime(position));

    // See comment above — fullscreen lyrics interpolator must always re-anchor.
    if (m_fullscreenPlayer)
        m_fullscreenPlayer->updatePosition(static_cast<int>(position));

    if (m_userSeeking || m_seekPending) {
        qInfo() << "[seek-ui] position feedback"
                << "positionMs=" << position
                << "userSeeking=" << m_userSeeking
                << "seekPending=" << m_seekPending
                << "sliderValueMs=" << (m_positionSlider ? m_positionSlider->value() : -1);
    }
}

void MusicPlayer::updateDuration(qint64 duration) {
    if (m_activeIsCue) {
        if (m_activeCueEndMs < 0) {
            const qint64 cueDur = qMax<qint64>(0, duration - m_activeCueStartMs);
            m_positionSlider->setRange(0, static_cast<int>(cueDur));
            m_totalTimeLabel->setText(formatTime(cueDur));
            
            int playingIdx = resolvePlayingTrackIndex();
            if (playingIdx >= 0) {
                TrackItem *t = m_tracks[playingIdx];
                t->metadata().duration = cueDur;
                const int vr = getVisualRowFromTrackIndex(playingIdx);
                if (vr != -1) {
                    if (QTableWidgetItem *durItem = m_playlistTable->item(vr, COL_DURATION))
                        durItem->setText(formatTime(cueDur));
                }
            }

            if (m_fullscreenPlayer) {
                QString title;
                QString artist;
                QString album;
                if (playingIdx >= 0) {
                    title = m_tracks[playingIdx]->metadata().title;
                    artist = m_tracks[playingIdx]->metadata().artist;
                    album = m_tracks[playingIdx]->metadata().album;
                }
                if (title.isEmpty()) title = QFileInfo(m_engine->currentFilePath()).baseName();

                m_fullscreenPlayer->updateTrack(m_bottomCoverLabel->pixmap(),
                                                title,
                                                artist,
                                                album,
                                                static_cast<int>(cueDur));
            }
        }
        return;
    }

    // Always update duration - don't block on small decreases
    m_positionSlider->setRange(0, static_cast<int>(duration));
    m_totalTimeLabel->setText(formatTime(duration));
    
    int playingIdx = resolvePlayingTrackIndex();
    if (playingIdx >= 0) {
        if (m_tracks[playingIdx]->metadata().duration == 0) {
            m_tracks[playingIdx]->metadata().duration = duration;
            int visualRow = getVisualRowFromTrackIndex(playingIdx);
            if (visualRow != -1) {
                if (QTableWidgetItem *durItem = m_playlistTable->item(visualRow, COL_DURATION))
                    durItem->setText(formatTime(duration));
            }
        }
    }
}

void MusicPlayer::playlistItemDoubleClicked(int row, int column) {
    if (column == COL_LIKED)
        return;

    int trackIndex = getTrackIndexFromVisualRow(row);
    if (trackIndex != -1) {
        m_resumeOnPlayPath.clear();
        m_resumeOnPlayPositionMs = -1;
        m_currentIndex = trackIndex;
        playCurrentItem();
    }
}

void MusicPlayer::playCurrentItem()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_tracks.count()) return;

    const bool playbackContextChanged =
        !m_playbackPlaylistId.isEmpty() && m_playbackPlaylistId != m_currentPlaylistId;
    m_playbackPlaylistId = m_currentPlaylistId;
    if (playbackContextChanged)
        clearShuffleRuntimeState(true);

    TrackItem *track = m_tracks[m_currentIndex];
    updatePlaybackTrackKey();
    if (track && !track->isMetadataLoaded()) {
        track->ensureMetadataLoaded();
        updateTrackRow(m_currentIndex);
    }

    const QString currentPath = QFileInfo(track->filePath()).absoluteFilePath();
    QString currentKey = currentPath;
    if (track->metadata().isCueTrack)
        currentKey = makeCueSavedPath(track->metadata().cueFilePath, track->metadata().trackNumber);

    // If the track we are about to play doesn't match the resume path, clear resume info
    if (!m_resumeOnPlayPath.isEmpty() && normalizePathForCompare(currentKey) != normalizePathForCompare(m_resumeOnPlayPath)) {
        m_resumeOnPlayPath.clear();
        m_resumeOnPlayPositionMs = -1;
    }

    m_engine->play(track->filePath());

    m_activeIsCue = track->metadata().isCueTrack;
    m_activeCueStartMs = track->metadata().cueStartMs;
    m_activeCueEndMs = -1;

    if (m_activeIsCue) {
        m_engine->prepareNext(QString());
        m_preparedNextPath.clear();
        m_preparedNextIndex = -1;

        m_activeCueEndMs = track->metadata().cueEndMs;

        const qint64 dur = track->metadata().duration;
        if (dur > 0) {
            m_positionSlider->setRange(0, static_cast<int>(dur));
            m_totalTimeLabel->setText(formatTime(dur));
        }

        if (!m_resumeOnPlayPath.isEmpty() && m_resumeOnPlayPositionMs > 0) {
            const qint64 seekPos = m_resumeOnPlayPositionMs;
            m_resumeOnPlayPath.clear();
            m_resumeOnPlayPositionMs = -1;
            m_engine->seek(m_activeCueStartMs + seekPos);
            m_positionSlider->setValue(static_cast<int>(seekPos));
            m_currentTimeLabel->setText(formatTime(seekPos));
        } else if (m_activeCueStartMs > 0) {
            m_engine->seek(m_activeCueStartMs);
        }

        updateBottomBarFromTrack(track);
        updateLikeButtonState();
        updatePlaylistHighlight();
        return;
    }

    if (!currentPath.isEmpty())
        rememberCurrentTrackForPlaylist(m_currentPlaylistId);

    if (!m_resumeOnPlayPath.isEmpty()
        && m_resumeOnPlayPositionMs > 0
        && normalizePathForCompare(currentPath) == normalizePathForCompare(m_resumeOnPlayPath)) {
        const qint64 seekPos = m_resumeOnPlayPositionMs;
        m_resumeOnPlayPath.clear();
        m_resumeOnPlayPositionMs = -1;
        m_engine->seek(seekPos);
        m_positionSlider->setValue(static_cast<int>(seekPos));
        m_currentTimeLabel->setText(formatTime(seekPos));
    }

    if (track->metadata().duration > 0) {
        m_positionSlider->setRange(0, static_cast<int>(track->metadata().duration));
        m_totalTimeLabel->setText(formatTime(track->metadata().duration));
    }

    updateBottomBarFromTrack(track);
    updateLikeButtonState();
    updatePlaylistHighlight();
    prepareNextGapless();
}

void MusicPlayer::onEngineStateChanged(GaplessAudioEngine::State state) {
    qInfo() << "[seek-ui] engine stateChanged" << engineStateToString(state);
    const bool playing = (state == GaplessAudioEngine::Playing);
    if (playing) m_playButton->setText(QString::fromUtf8("\xE2\x8F\xB8"));
    else m_playButton->setText(QString::fromUtf8("\xE2\x96\xB6"));
    #ifdef Q_OS_WIN
        if (m_winTaskbar)
            m_winTaskbar->setPlaying(playing);
    #endif
    if (m_fullscreenPlayer)
        m_fullscreenPlayer->updatePlayState(playing);
    updatePlaylistHighlight();
}

void MusicPlayer::onEngineTrackTransitioned() {
    qInfo() << "[ui] trackTransitioned: START, m_currentIndex=" << m_currentIndex;
    m_positionSlider->setValue(0);
    m_currentTimeLabel->setText(QStringLiteral("00:00"));
    const QString viewedPlaylistId = m_currentPlaylistId;
    const bool shouldRestoreView = !viewedPlaylistId.isEmpty()
        && !m_playbackPlaylistId.isEmpty()
        && viewedPlaylistId != m_playbackPlaylistId;
    if (shouldRestoreView)
        setUpdatesEnabled(false);

    ensurePlaybackContextPlaylistActive();
    const int previousIndex = m_currentIndex;

    int nextIdx = -1;
    const QString engineCurrentPath = m_engine ? m_engine->currentFilePath() : QString();
    if (!engineCurrentPath.isEmpty())
        nextIdx = findTrackIndexByPath(engineCurrentPath);

    if (nextIdx < 0 && !m_preparedNextPath.isEmpty())
        nextIdx = findTrackIndexByPath(m_preparedNextPath);

    if (nextIdx < 0 && m_preparedNextIndex >= 0 && m_preparedNextIndex < m_tracks.count()) {
        const QString indexedPreparedPath = m_tracks[m_preparedNextIndex]->filePath();
        if (m_preparedNextPath.isEmpty()
            || normalizePathForCompare(indexedPreparedPath) == normalizePathForCompare(m_preparedNextPath)) {
            nextIdx = m_preparedNextIndex;
        }
    }

    if (nextIdx < 0 || nextIdx >= m_tracks.count()) {
        const int guessed = getNextTrackIndex();
        if (guessed >= 0 && guessed < m_tracks.count())
            nextIdx = guessed;
    }
    if (nextIdx < 0 || nextIdx >= m_tracks.count()) {
        m_preparedNextIndex = -1;
        m_preparedNextPath.clear();
        updatePlaylistHighlight();
    } else {
        m_currentIndex = nextIdx;
        rememberCurrentTrackForPlaylist(m_currentPlaylistId);
        commitShuffleAdvance(previousIndex, m_currentIndex);
        m_preparedNextIndex = -1;
        m_preparedNextPath.clear();
        TrackItem *track = m_tracks[m_currentIndex];
        updatePlaybackTrackKey();

        if (track && !track->isMetadataLoaded()) {
            track->ensureMetadataLoaded();
            updateTrackRow(m_currentIndex);
        }

        if (track->metadata().duration > 0) {
            m_positionSlider->setRange(0, static_cast<int>(track->metadata().duration));
            m_totalTimeLabel->setText(formatTime(track->metadata().duration));
        }

        updateBottomBarFromTrack(track);
        updateLikeButtonState();
        updatePlaylistHighlight();
        prepareNextGapless();
    }

    qInfo() << "[ui] trackTransitioned: END, m_currentIndex=" << m_currentIndex
            << "slider=" << m_positionSlider->value() << "/" << m_positionSlider->maximum();

    if (shouldRestoreView) {
        if (m_currentPlaylistId != viewedPlaylistId)
            selectPlaylistById(viewedPlaylistId);
        setUpdatesEnabled(true);
        update();
    }
}

void MusicPlayer::onEnginePlaybackFinished() {
    const QString viewedPlaylistId = m_currentPlaylistId;
    const bool shouldRestoreView = !viewedPlaylistId.isEmpty()
        && !m_playbackPlaylistId.isEmpty()
        && viewedPlaylistId != m_playbackPlaylistId;
    if (shouldRestoreView)
        setUpdatesEnabled(false);

    ensurePlaybackContextPlaylistActive();
    m_preparedNextIndex = -1;
    m_preparedNextPath.clear();

    if (m_engine) {
        const bool currentIsCue = m_currentIndex >= 0
            && m_currentIndex < m_tracks.count()
            && m_tracks[m_currentIndex]->metadata().isCueTrack;
        if (!currentIsCue) {
            const int engineTrackIndex = findTrackIndexByPath(m_engine->currentFilePath());
            if (engineTrackIndex >= 0)
                m_currentIndex = engineTrackIndex;
        }
    }
    m_activeCueEndMs = -1;

    const int nextIdx = getNextTrackIndex(true);
    if (nextIdx < 0 || nextIdx >= m_tracks.count()) {
        m_engine->stop();
        m_positionSlider->setValue(0);
    } else {
        m_currentIndex = nextIdx;
        playCurrentItem();
    }

    if (shouldRestoreView) {
        if (m_currentPlaylistId != viewedPlaylistId)
            selectPlaylistById(viewedPlaylistId);
        setUpdatesEnabled(true);
        update();
    }
}

void MusicPlayer::volumeChanged(int value) {
    float vol = value / 100.0f;
    if (m_engine) m_engine->setVolume(vol);
    if (value == 0) m_volumeLabel->setText(QString::fromUtf8("\xF0\x9F\x94\x87"));
    else if (value < 50) m_volumeLabel->setText(QString::fromUtf8("\xF0\x9F\x94\x89"));
    else m_volumeLabel->setText(QString::fromUtf8("\xF0\x9F\x94\x8A"));

    if (m_fullscreenPlayer) {
        m_fullscreenPlayer->blockSignals(true);
        m_fullscreenPlayer->updateVolume(value);
        m_fullscreenPlayer->blockSignals(false);
    }
    
    if (m_volumeSlider) {
        m_volumeSlider->blockSignals(true);
        m_volumeSlider->setValue(value);
        m_volumeSlider->blockSignals(false);
    }
}

// ============= UI Helpers =============

void MusicPlayer::updatePlaylistHighlight()
{
    int coverSize = qMax(30, m_rowHeight - 10);
    bool isPlaying = m_engine->state() == GaplessAudioEngine::Playing;
    bool isPaused = m_engine->state() == GaplessAudioEngine::Paused;
    int playingTrackIndex = resolvePlayingTrackIndex();
    if (playingTrackIndex < 0 && !isPlaying && !isPaused)
        playingTrackIndex = m_currentIndex;

    int playingVisualRow = getVisualRowFromTrackIndex(playingTrackIndex);
    m_playlistTable->setPlayingRow(playingVisualRow);

    for (int row = 0; row < m_playlistTable->rowCount(); ++row) {
        int trackIndex = getTrackIndexFromVisualRow(row);
        bool isCurrent = (trackIndex == m_currentIndex && m_currentIndex >= 0);
        bool isPlayingTrack = (trackIndex == playingTrackIndex && playingTrackIndex >= 0);

        QFont font;
        font.setBold(isCurrent);

        for (int col = 0; col < m_playlistTable->columnCount(); ++col) {
            QTableWidgetItem *item = m_playlistTable->item(row, col);
            if (item)
                item->setFont(font);
        }

        QTableWidgetItem *coverItem = m_playlistTable->item(row, COL_COVER);
        if (coverItem && trackIndex >= 0 && trackIndex < m_tracks.count()) {
            QPixmap base = m_tracks[trackIndex]->metadata().coverPixmap();
            if (base.isNull()) {
                base = QPixmap(coverSize, coverSize);
                base.fill(Qt::transparent);
            }

            if (isPlayingTrack && (isPlaying || isPaused)) {
                QPixmap overlay = base.scaled(coverSize, coverSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QPainter p(&overlay);
                p.setRenderHint(QPainter::Antialiasing);
                p.fillRect(overlay.rect(), QColor(0, 0, 0, 120));

                QString icon = isPlaying ? QString::fromUtf8("\xE2\x96\xB6") : QString::fromUtf8("\xE2\x8F\xB8");
                QFont iconFont;
                iconFont.setPixelSize(qMax(14, coverSize / 3));
                p.setFont(iconFont);
                p.setPen(Qt::white);
                p.drawText(overlay.rect(), Qt::AlignCenter, icon);
                p.end();

                coverItem->setData(Qt::DecorationRole, overlay);
            } else {
                coverItem->setData(Qt::DecorationRole, base);
            }
        }

        QTableWidgetItem *titleItem = m_playlistTable->item(row, COL_TITLE);
        if (titleItem && trackIndex >= 0 && trackIndex < m_tracks.count())
            titleItem->setText(m_tracks[trackIndex]->metadata().title);
    }
}

void MusicPlayer::updatePlaybackTrackKey()
{
    if (!isValidTrackIndex(m_currentIndex)) {
        m_playbackTrackKey.clear();
        return;
    }

    TrackItem *track = m_tracks[m_currentIndex];
    if (!track) {
        m_playbackTrackKey.clear();
        return;
    }

    if (track->metadata().isCueTrack) {
        m_playbackTrackKey = makeCueSavedPath(track->metadata().cueFilePath,
                                              track->metadata().trackNumber);
    } else {
        m_playbackTrackKey = QFileInfo(track->filePath()).absoluteFilePath();
    }
}

int MusicPlayer::resolvePlayingTrackIndex() const
{
    if (!m_engine)
        return -1;

    const bool isPlaying = (m_engine->state() == GaplessAudioEngine::Playing);
    const bool isPaused = (m_engine->state() == GaplessAudioEngine::Paused);
    if (!isPlaying && !isPaused)
        return -1;

    int playingTrackIndex = -1;

    if (!m_playbackTrackKey.isEmpty()) {
        if (isCueSavedPath(m_playbackTrackKey)) {
            QString cuePath;
            int trackNum = 0;
            if (parseCueSavedPath(m_playbackTrackKey, cuePath, trackNum)) {
                const QString needle = normalizePathForCompare(cuePath);
                for (int i = 0; i < m_tracks.count(); ++i) {
                    const TrackMetadata &md = m_tracks[i]->metadata();
                    if (!md.isCueTrack)
                        continue;
                    if (md.trackNumber != trackNum)
                        continue;
                    if (normalizePathForCompare(md.cueFilePath) == needle) {
                        playingTrackIndex = i;
                        break;
                    }
                }
            }
        } else {
            playingTrackIndex = findTrackIndexByPath(m_playbackTrackKey);
        }
    }

    if (playingTrackIndex < 0) {
        const QString enginePath = m_engine->currentFilePath();
        if (!enginePath.isEmpty())
            playingTrackIndex = findTrackIndexByPath(enginePath);
    }

    return playingTrackIndex;
}

QString MusicPlayer::formatTime(qint64 milliseconds) {
    if (milliseconds <= 0) return "00:00";
    int seconds = milliseconds / 1000;
    int minutes = seconds / 60;
    seconds %= 60;
    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

// ============= Settings Persistence =============

void MusicPlayer::saveColumnSettings() {
    QSettings settings("MyCompany", "MusicPlayer");
    settings.beginGroup("MainWindow");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    settings.setValue("lastPlaylistId", m_currentPlaylistId);
    settings.endGroup();

    settings.beginGroup("PlaylistColumns");
    settings.setValue("schemaVersion", kPlaylistColumnSettingsSchema);
    settings.setValue("headerState", m_playlistTable->horizontalHeader()->saveState());
    for(int i = 0; i < COL_COUNT; ++i)
        settings.setValue(QString("col_%1_hidden").arg(i), m_playlistTable->isColumnHidden(i));
    settings.endGroup();
}

void MusicPlayer::loadColumnSettings() {
    QSettings settings("MyCompany", "MusicPlayer");
    settings.beginGroup("MainWindow");
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
    settings.endGroup();

    settings.beginGroup("PlaylistColumns");

    const int schemaVersion = settings.value("schemaVersion", 0).toInt();
    if (schemaVersion < 2) {
        const bool hasLegacyFilePathHidden = settings.contains(QStringLiteral("col_10_hidden"));
        const bool hasNewFilePathHidden = settings.contains(QStringLiteral("col_11_hidden"));

        if (hasLegacyFilePathHidden && !hasNewFilePathHidden) {
            const bool oldFilePathHidden = settings.value(QStringLiteral("col_10_hidden"), true).toBool();
            settings.setValue(QStringLiteral("col_10_hidden"), false);
            settings.setValue(QStringLiteral("col_11_hidden"), oldFilePathHidden);
        }
    }

    QByteArray headerState = settings.value("headerState").toByteArray();
    if (!headerState.isEmpty() && schemaVersion >= kPlaylistColumnSettingsSchema)
        m_playlistTable->horizontalHeader()->restoreState(headerState);

    for (int i = 0; i < COL_COUNT; ++i)
        m_playlistTable->horizontalHeader()->setSectionResizeMode(i, QHeaderView::Interactive);
    m_playlistTable->horizontalHeader()->setStretchLastSection(false);

    if (schemaVersion < kPlaylistColumnSettingsSchema) {
        applyStandardColumnLayout(true);
        settings.setValue("schemaVersion", kPlaylistColumnSettingsSchema);
        settings.setValue("headerState", m_playlistTable->horizontalHeader()->saveState());
        for (int i = 0; i < COL_COUNT; ++i)
            settings.setValue(QString("col_%1_hidden").arg(i), m_playlistTable->isColumnHidden(i));
    } else {
        for (int i = 0; i < COL_COUNT; ++i) {
            bool isHidden = settings.value(QString("col_%1_hidden").arg(i), !m_columnInfo[i].visibleByDefault).toBool();
            m_playlistTable->setColumnHidden(i, isHidden);
            if (m_columnActions.contains(i))
                m_columnActions[i]->setChecked(!isHidden);
        }

        if (headerState.isEmpty())
            applyStandardColumnLayout(false);
    }

    settings.endGroup();

    m_playlistTable->enforceRightmostResizeLock();
    m_playlistTable->snapshotIdealWidths();
}

void MusicPlayer::savePlaybackState()
{
    QSettings settings("MyCompany", "MusicPlayer");
    settings.beginGroup("Playback");

    int volume = 70;
    if (m_volumeSlider)
        volume = m_volumeSlider->value();
    else if (m_engine)
        volume = qRound(m_engine->volume() * 100.0f);
    settings.setValue("volume", qBound(0, volume, 100));

    QString currentPath;
    qint64 positionMs = 0;

    if (isValidTrackIndex(m_currentIndex)) {
        TrackItem *track = m_tracks[m_currentIndex];
        if (track->metadata().isCueTrack)
            currentPath = makeCueSavedPath(track->metadata().cueFilePath, track->metadata().trackNumber);
        else
            currentPath = QFileInfo(track->filePath()).absoluteFilePath();
        
        if (m_engine && m_engine->state() != GaplessAudioEngine::Stopped) {
            positionMs = m_engine->position();
            if (track->metadata().isCueTrack)
                positionMs = qMax<qint64>(0, positionMs - track->metadata().cueStartMs);
        } else {
            positionMs = m_positionSlider->value();
        }
    } else if (m_engine && !m_engine->currentFilePath().isEmpty()) {
        currentPath = QFileInfo(m_engine->currentFilePath()).absoluteFilePath();
        positionMs = m_engine->position();
    }

    if (!m_resumeOnPlayPath.isEmpty()
        && !currentPath.isEmpty()
        && normalizePathForCompare(m_resumeOnPlayPath) == normalizePathForCompare(currentPath)
        && m_resumeOnPlayPositionMs > 0) {
        positionMs = m_resumeOnPlayPositionMs;
    }

    settings.setValue("lastTrackPath", currentPath);
    settings.setValue("lastPositionMs", positionMs);
    settings.endGroup();
}

void MusicPlayer::restorePlaybackState()
{
    if (m_tracks.isEmpty())
        return;

    QSettings settings("MyCompany", "MusicPlayer");
    settings.beginGroup("Playback");
    const QString storedTrackPath = settings.value("lastTrackPath").toString().trimmed();
    const qint64 storedPosition = qMax<qint64>(0, settings.value("lastPositionMs", 0).toLongLong());
    settings.endGroup();

    if (storedTrackPath.isEmpty())
        return;

    const QString absoluteTrackPath = QFileInfo(storedTrackPath).absoluteFilePath();
    const int trackIndex = findTrackIndexByPath(absoluteTrackPath);
    if (trackIndex < 0 || trackIndex >= m_tracks.count())
        return;

    m_currentIndex = trackIndex;
    m_playbackPlaylistId = m_currentPlaylistId;
    rememberCurrentTrackForPlaylist(m_currentPlaylistId);
    updatePlaybackTrackKey();
    TrackItem *track = m_tracks[m_currentIndex];
    if (track && !track->isMetadataLoaded()) {
        track->ensureMetadataLoaded();
        updateTrackRow(m_currentIndex);
    }

    updateBottomBarFromTrack(track);
    updatePlaylistHighlight();
    updateLikeButtonState();

    if (track && track->metadata().duration > 0) {
        m_positionSlider->setRange(0, static_cast<int>(track->metadata().duration));
        m_totalTimeLabel->setText(formatTime(track->metadata().duration));
    }

    if (storedPosition > 0) {
        const qint64 boundedPosition = (track && track->metadata().duration > 0)
            ? qMin(storedPosition, track->metadata().duration)
            : storedPosition;
        m_resumeOnPlayPath = track ? QFileInfo(track->filePath()).absoluteFilePath() : absoluteTrackPath;
        m_resumeOnPlayPositionMs = boundedPosition;
        m_positionSlider->setValue(static_cast<int>(boundedPosition));
        m_currentTimeLabel->setText(formatTime(boundedPosition));
    } else {
        m_resumeOnPlayPath.clear();
        m_resumeOnPlayPositionMs = -1;
        m_positionSlider->setValue(0);
        m_currentTimeLabel->setText(QStringLiteral("00:00"));
    }
}

// ============= Index Mapping =============

void MusicPlayer::rememberCurrentTrackForPlaylist(const QString &playlistId)
{
    if (playlistId.isEmpty())
        return;

    if (!isValidTrackIndex(m_currentIndex))
        return;

    TrackItem *track = m_tracks[m_currentIndex];
    QString key;
    if (track->metadata().isCueTrack)
        key = makeCueSavedPath(track->metadata().cueFilePath, track->metadata().trackNumber);
    else
        key = QFileInfo(track->filePath()).absoluteFilePath();

    if (!key.isEmpty())
        m_playlistLastTrackPath.insert(playlistId, key);
}

int MusicPlayer::findRememberedTrackIndexForPlaylist(const QString &playlistId) const
{
    if (playlistId.isEmpty())
        return -1;

    const QString path = m_playlistLastTrackPath.value(playlistId);
    if (path.isEmpty())
        return -1;

    return findTrackIndexByPath(path);
}

bool MusicPlayer::ensurePlaybackContextPlaylistActive()
{
    if (!m_engine)
        return true;

    const bool isPlayingContext = (m_engine->state() == GaplessAudioEngine::Playing
                                || m_engine->state() == GaplessAudioEngine::Paused);
    if (!isPlayingContext)
        return true;

    if (m_playbackPlaylistId.isEmpty())
        return true;

    if (m_playbackPlaylistId == m_currentPlaylistId)
        return true;

    if (selectPlaylistById(m_playbackPlaylistId))
        return true;

    m_playbackPlaylistId.clear();
    return false;
}

int MusicPlayer::getTrackIndexFromVisualRow(int visualRow) const {
    if (visualRow < 0 || visualRow >= m_playlistTable->rowCount()) return -1;
    QTableWidgetItem *item = m_playlistTable->item(visualRow, COL_TRACK);
    if (!item) return -1;
    return item->data(Qt::UserRole).toInt();
}

void MusicPlayer::primePlaylistCache(const QString &playlistId, const QStringList &paths)
{
    if (playlistId.isEmpty())
        return;

    QStringList cachedPaths;
    cachedPaths.reserve(paths.size());
    for (const QString &path : paths) {
        if (isCueSavedPath(path)) {
            QString cuePath; int trackNum;
            if (parseCueSavedPath(path, cuePath, trackNum) && QFileInfo::exists(cuePath))
                cachedPaths.append(path);
            continue;
        }
        QFileInfo fi(path);
        if (!fi.exists())
            continue;
        cachedPaths.append(fi.absoluteFilePath());
    }

    m_playlistTrackCache.insert(playlistId, cachedPaths);
}

void MusicPlayer::syncCurrentPlaylistCacheFromTracks()
{
    if (m_currentPlaylistId.isEmpty())
        return;

    QStringList paths;
    paths.reserve(m_tracks.size());
    for (TrackItem *track : m_tracks) {
        if (track->metadata().isCueTrack)
            paths.append(makeCueSavedPath(track->metadata().cueFilePath, track->metadata().trackNumber));
        else
            paths.append(track->filePath());
    }
    m_playlistTrackCache.insert(m_currentPlaylistId, paths);
}

void MusicPlayer::clearPlaylistCache(const QString &playlistId)
{
    if (playlistId.isEmpty())
        return;

    m_playlistTrackCache.remove(playlistId);
}

int MusicPlayer::getVisualRowFromTrackIndex(int trackIndex) const {
    if (trackIndex < 0 || trackIndex >= m_tracks.count()) return -1;
    for (int i = 0; i < m_playlistTable->rowCount(); ++i) {
        if (getTrackIndexFromVisualRow(i) == trackIndex)
            return i;
    }
    return -1;
}

void MusicPlayer::updateTrackRow(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.count()) return;
    int visualRow = getVisualRowFromTrackIndex(trackIndex);
    if (visualRow == -1) return;

    TrackItem *track = m_tracks[trackIndex];
    const TrackMetadata &md = track->metadata();

    if (QTableWidgetItem *coverItem = m_playlistTable->item(visualRow, COL_COVER)) {
        const int coverSize = qMax(30, m_rowHeight - 10);
        QPixmap base = md.coverPixmap();
        if (base.isNull()) {
            base = QPixmap(coverSize, coverSize);
            base.fill(Qt::transparent);
        }

        const bool isPlaying = m_engine->state() == GaplessAudioEngine::Playing;
        const bool isPaused = m_engine->state() == GaplessAudioEngine::Paused;
        const int playingTrackIndex = resolvePlayingTrackIndex();
        const bool isPlayingTrack = (trackIndex == playingTrackIndex && playingTrackIndex >= 0);

        if (isPlayingTrack && (isPlaying || isPaused)) {
            QPixmap overlay = base.scaled(coverSize, coverSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPainter p(&overlay);
            p.setRenderHint(QPainter::Antialiasing);
            p.fillRect(overlay.rect(), QColor(0, 0, 0, 120));

            QString icon = isPlaying ? QString::fromUtf8("\xE2\x96\xB6") : QString::fromUtf8("\xE2\x8F\xB8");
            QFont iconFont;
            iconFont.setPixelSize(qMax(14, coverSize / 3));
            p.setFont(iconFont);
            p.setPen(Qt::white);
            p.drawText(overlay.rect(), Qt::AlignCenter, icon);
            p.end();

            coverItem->setData(Qt::DecorationRole, overlay);
        } else {
            coverItem->setData(Qt::DecorationRole, base);
        }
    }

    if (QTableWidgetItem *titleItem = m_playlistTable->item(visualRow, COL_TITLE))
        titleItem->setText(md.title);
    if (QTableWidgetItem *artistItem = m_playlistTable->item(visualRow, COL_ARTIST))
        artistItem->setText(md.artist);
    if (QTableWidgetItem *albumItem = m_playlistTable->item(visualRow, COL_ALBUM))
        albumItem->setText(md.album);
    if (QTableWidgetItem *yearItem = m_playlistTable->item(visualRow, COL_YEAR))
        yearItem->setText(md.year);
    if (QTableWidgetItem *genreItem = m_playlistTable->item(visualRow, COL_GENRE))
        genreItem->setText(md.genre);
    if (QTableWidgetItem *trackItem = m_playlistTable->item(visualRow, COL_TRACK)) {
        trackItem->setData(Qt::DisplayRole,
            md.trackNumber > 0 ? md.trackNumber : trackIndex + 1);
    }

    const QString outlineHeart = QString::fromUtf8("\xE2\x99\xA1");
    const QString filledHeart = QString::fromUtf8("\xE2\x99\xA5");
    const bool liked = isTrackLiked(track->filePath());
    if (QTableWidgetItem *likedItem = m_playlistTable->item(visualRow, COL_LIKED)) {
        likedItem->setText(liked ? filledHeart : outlineHeart);
        likedItem->setTextAlignment(Qt::AlignCenter);
        likedItem->setForeground(liked ? QColor("#1db954") : QColor("#b3b3b3"));
    }

    if (QTableWidgetItem *durItem = m_playlistTable->item(visualRow, COL_DURATION)) {
        durItem->setText(md.duration > 0 ? formatTime(md.duration) : "");
        // Removed durItem->setTextAlignment(Qt::AlignCenter) to keep default left alignment
    }
    if (QTableWidgetItem *bitrateItem = m_playlistTable->item(visualRow, COL_BITRATE)) {
        bitrateItem->setText(md.bitrate > 0 ? QString::number(md.bitrate / 1000) : "");
    }
    if (QTableWidgetItem *rateItem = m_playlistTable->item(visualRow, COL_SAMPLERATE)) {
        rateItem->setText(md.sampleRate > 0 ? QString::number(md.sampleRate / 1000.0, 'f', 1) : "");
    }
}

void MusicPlayer::updateBottomBarFromTrack(TrackItem *track)
{
    if (!track) return;
    const TrackMetadata &md = track->metadata();

    const QString title = md.title.isEmpty() ? QFileInfo(track->filePath()).baseName() : md.title;
    m_titleLabel->setText(title);
    m_artistLabel->setText(md.artist);

    const QPixmap cover = md.coverPixmap();
    if (!cover.isNull()) {
        m_bottomCoverLabel->setPixmap(cover.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        if (m_bottomGlow) {
            m_bottomGlow->setColor(extractDominantColor(cover));
        }
    } else {
        if (m_bottomGlow) m_bottomGlow->setColor(Qt::transparent);
    }

    if (m_fullscreenPlayer) {
        m_fullscreenPlayer->updateTrack(cover, title, md.artist, md.album,
                                        static_cast<int>(md.duration));
        m_fullscreenPlayer->updateShuffleState(m_shuffleEnabled, m_shuffleMode);
        m_fullscreenPlayer->updateRepeatState(m_repeatMode);
    }

    updateLikeButtonState();
}

// ============= Settings =============

void MusicPlayer::openSettings()
{
    if (m_settingsDialog) {
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }

    m_settingsDialog = new SettingsDialog(m_equalizer, m_engine, this);
    m_settingsDialog->setWindowFlags(Qt::Tool);
    m_settingsDialog->setRowHeight(m_rowHeight);

    connect(m_settingsDialog, &SettingsDialog::rowHeightChanged, this, [this](int h) {
        m_rowHeight = h;
        int coverSize = qMax(30, m_rowHeight - 10);
        m_playlistTable->setIconSize(QSize(coverSize, coverSize));

        for (int i = 0; i < m_playlistTable->rowCount(); ++i) {
            m_playlistTable->setRowHeight(i, m_rowHeight);

            int trackIndex = getTrackIndexFromVisualRow(i);
            if (trackIndex >= 0 && trackIndex < m_tracks.count()) {
                QPixmap cover = m_tracks[trackIndex]->metadata().coverPixmap();
                if (!cover.isNull()) {
                    m_playlistTable->item(i, COL_COVER)->setData(Qt::DecorationRole, cover);
                }
            }
        }

        QSettings settings("MyCompany", "MusicPlayer");
        settings.setValue("Appearance/rowHeight", m_rowHeight);
    });

    connect(m_settingsDialog, &SettingsDialog::shuffleOptionsChanged, this,
            [this](int mode, bool noRepeats) {
        const int normalizedMode = qBound(0, mode, 1);
        const bool modeChanged = (m_shuffleMode != normalizedMode);
        const bool repeatChanged = (m_shuffleNoRepeats != noRepeats);

        m_shuffleMode = normalizedMode;
        m_shuffleNoRepeats = noRepeats;

        if (modeChanged || repeatChanged) {
            clearShuffleRuntimeState(modeChanged);
            applyShuffleButtonStyle();
            resyncPreparedNext();
        }
    });

    connect(m_settingsDialog, &SettingsDialog::playbackRateChanged, this,
            [this](double rate) {
        if (!m_engine)
            return;
        m_engine->setPlaybackRate(static_cast<float>(qBound(0.5, rate, 2.0)));
    });

    connect(m_settingsDialog, &SettingsDialog::crossfadeDurationChanged, this,
            [this](int seconds) {
        if (!m_engine)
            return;
        m_engine->setCrossfadeDurationMs(qBound(0, seconds, 8) * 1000);
    });

    connect(m_settingsDialog, &QDialog::finished, this, [this]() {
        m_settingsDialog->deleteLater();
        m_settingsDialog = nullptr;
    });

    m_settingsDialog->show();
}