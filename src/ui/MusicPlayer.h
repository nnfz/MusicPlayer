#ifndef MUSICPLAYER_H
#define MUSICPLAYER_H

#include <QMainWindow>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSplitter>
#include <QFileDialog>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QMutex>
#include <QThread>
#include <QHash>
#include <QSet>
#include <QVector>

#ifdef Q_OS_WIN
class WinTaskbarButtons;
#endif

#include "TrackItem.h"
#include "GaplessAudioEngine.h"
#include "PlaylistManager.h"
#include "ClickableSlider.h"
#include "PlaylistTable.h"
#include "Equalizer.h"
#include "SettingsDialog.h"
#include "MetadataLoaderThread.h"
#include "FullscreenPlayer.h"

class MusicPlayer : public QMainWindow
{
    Q_OBJECT

public:
    MusicPlayer(QWidget *parent = nullptr);
    ~MusicPlayer();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void playPause();
    void stop();
    void next();
    void previous();
    void seek(int position);
    void updatePosition(qint64 position);
    void updateDuration(qint64 duration);
    void playlistItemDoubleClicked(int row, int column);
    void onEngineStateChanged(GaplessAudioEngine::State state);
    void onEngineTrackTransitioned();
    void onEnginePlaybackFinished();
    void volumeChanged(int value);
    void onFilesDropped(const QStringList &files, int targetVisualRow = -1);
    void filterPlaylist(const QString &text);
    void removeSelected();
    void onHeaderClicked(int logicalIndex);
    void onPlaylistCellClicked(int row, int column);
    void preloadTrackMetadata();
    void onMetaDataLoaded();
    void processDeferredMetadataBatch();
    void onBackgroundMetadataLoaded(const QString &filePath);
    void onRowsMoved(QList<int> sourceRows, int targetRow);
    void onPlaylistListRowsMoved(const QModelIndex &parent, int start, int end, const QModelIndex &destination, int row);
    void syncTracksToTableOrder();
    void onLikeButtonClicked();
    void showHeaderContextMenu(const QPoint &pos);
    void toggleColumn(int column);
    void resetColumns();
    void onPlaylistSelected(int row);
    void createNewPlaylist();
    void renameSelectedPlaylist();
    void deleteSelectedPlaylist();
    void importM3UPlaylist();
    void exportM3UPlaylist();
    void saveCurrentPlaylistTracks();
    void refreshPlaylistPanel();
    void openSettings();

private:
    void setupUI();
    void setupConnections();
    void addFile(const QString &filePath);
    void addCueFile(const QString &cuePath);
    void addDirectory(const QString &dirPath);
    void advanceCueTrack();
    void importPlaylistFiles(const QStringList &paths);
    void playCurrentItem();
    QString formatTime(qint64 milliseconds);
    void updatePlaylistHighlight();
    void refreshTable();
    void applyStandardColumnLayout(bool applyDefaultVisibility);
    void saveColumnSettings();
    void loadColumnSettings();
    void savePlaybackState();
    void restorePlaybackState();
    int getTrackIndexFromVisualRow(int visualRow) const;
    int getVisualRowFromTrackIndex(int trackIndex) const;
    void updateTrackRow(int trackIndex);
    void onPlaylistScroll(int value);
    void updateBottomBarFromTrack(TrackItem *track);
    void rememberCurrentTrackForPlaylist(const QString &playlistId);
    int findRememberedTrackIndexForPlaylist(const QString &playlistId) const;
    bool ensurePlaybackContextPlaylistActive();
    void primePlaylistCache(const QString &playlistId, const QStringList &paths);
    void syncCurrentPlaylistCacheFromTracks();
    void clearPlaylistCache(const QString &playlistId);
    void startDeferredMetadataLoading(int startIndex = 0);
    void stopDeferredMetadataLoading();
    void forceLoadMetadataForTracks(const QList<TrackItem*> &tracks, const QString &title);
    void configurePlaylistAutoSourceDirectory(const QString &playlistId);
    void setPlaylistAutoSourceDirectory(const QString &playlistId);
    void clearPlaylistAutoSourceDirectory(const QString &playlistId);
    void syncPlaylistFromAutoSource(const QString &playlistId);
    void scheduleGlobalMetadataPreload(int delayMs = 0);
    void processGlobalMetadataPreloadBatch();
    bool selectPlaylistById(const QString &playlistId);
    QStringList collectAllPlaylistTracks() const;
    QStringList collectUniqueTrackPathsForBackgroundPreload() const;
    QStringList collectTracksFromAutoSource(const QString &dirPath) const;
    QString findPlaylistIdByName(const QString &name) const;
    QString ensureLikedPlaylist();
    bool isTrackLiked(const QString &filePath) const;
    void updateLikeButtonState();
    bool setTrackLiked(const QString &filePath, bool liked);
    void refreshLikeIndicatorsForPath(const QString &filePath);
    void sortByColumn(int logicalIndex, Qt::SortOrder order);
    int resolvePlayingTrackIndex() const;
    void updatePlaybackTrackKey();
    int getNextTrackIndex(bool commitSelection = false,
                          bool ignoreRepeatOneForManualAdvance = false);
    int getPreviousTrackIndex();
    QList<int> buildOrderedTrackIndices() const;
    int chooseShuffleCandidate(const QList<int> &orderedIndices, bool commitSelection);
    void applyShuffleButtonStyle();
    void clearShuffleRuntimeState(bool clearHistory = true);
    void sanitizeShuffleRuntime(const QList<int> &orderedIndices);
    void commitShuffleAdvance(int previousIndex, int nextIndex);
    bool isShuffleHistoryMode() const;
    bool isValidTrackIndex(int index) const;
    int findTrackIndexByPath(const QString &filePath) const;
    void resyncPreparedNext();
    void prepareNextGapless();

    GaplessAudioEngine *m_engine = nullptr;
    MetadataLoaderThread *m_metadataThread = nullptr;

    QMediaPlayer *m_metadataLoader = nullptr;
    QTimer *m_metadataTimeout = nullptr;
    QTimer *m_deferredMetadataTimer = nullptr;
    QTimer *m_globalMetadataPreloadTimer = nullptr;

    QList<TrackItem*> m_tracks;
    int m_currentIndex;
    int m_metadataLoadIndex;
    int m_deferredMetadataIndex = -1;
    QString m_deferredMetadataPlaylistId;
    QString m_forceMetadataLoadPlaylistId;
    QStringList m_globalMetadataPreloadPaths;
    QSet<QString> m_globalMetadataQueuedKeys;
    int m_globalMetadataPreloadIndex = -1;

    enum ShuffleMode {
        ShuffleRandom = 0,
        ShuffleHistory = 1
    };

    bool m_shuffleEnabled = false;
    int m_shuffleMode = ShuffleHistory;
    bool m_shuffleNoRepeats = false;
    QVector<int> m_shuffleBackHistory;
    QVector<int> m_shuffleForwardHistory;
    QList<int> m_shuffleCyclePool;
    int m_shufflePendingNextIndex = -1;
    int m_repeatMode = 0;
    int m_preparedNextIndex = -1;
    QString m_preparedNextPath;
    QString m_resumeOnPlayPath;
    qint64 m_resumeOnPlayPositionMs = -1;
    qint64 m_activeCueEndMs = -1;

    PlaylistManager *m_playlistManager = nullptr;
    QHash<QString, QStringList> m_playlistTrackCache;
    QHash<QString, QString> m_playlistLastTrackPath;
    QHash<QString, int> m_playlistScrollPositions;
    QString m_currentPlaylistId;
    QString m_playbackPlaylistId;
    QString m_playbackTrackKey;
    QListWidget *m_playlistList = nullptr;
    QSplitter *m_splitter = nullptr;

    PlaylistTable *m_playlistTable = nullptr;

    QPushButton *m_playButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_previousButton = nullptr;
    QPushButton *m_likeButton = nullptr;
    QPushButton *m_shuffleButton = nullptr;
    QPushButton *m_repeatButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    ClickableSlider *m_positionSlider = nullptr;
    ClickableSlider *m_volumeSlider = nullptr;

    QLabel *m_currentTimeLabel = nullptr;
    QLabel *m_totalTimeLabel = nullptr;
    MarqueeLabel    *m_titleLabel        = nullptr;
    MarqueeLabel    *m_artistLabel       = nullptr;
    QLabel *m_bottomCoverLabel = nullptr;
    QLabel *m_volumeLabel = nullptr;
    QLabel *m_trackCountLabel = nullptr;

    QLineEdit *m_searchBox = nullptr;

    bool m_userSeeking;
    bool m_seekPending;
    quint64 m_seekUiEpoch = 0;

    QTimer *m_smoothSliderTimer = nullptr;
    qint64 m_targetSliderPosition = 0;

    QMap<int, QAction*> m_columnActions;

    enum Column {
        COL_COVER = 0, COL_TRACK, COL_TITLE, COL_ARTIST, COL_ALBUM,
        COL_YEAR, COL_GENRE, COL_BITRATE, COL_SAMPLERATE, COL_DURATION,
        COL_LIKED, COL_FILEPATH, COL_COUNT
    };

    struct ColumnInfo {
        QString name;
        int defaultWidth;
        bool visibleByDefault;
    };

    QMap<int, ColumnInfo> m_columnInfo;

    int m_rowHeight = 70;

    Equalizer *m_equalizer = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    FullscreenPlayer *m_fullscreenPlayer = nullptr;

    #ifdef Q_OS_WIN
        WinTaskbarButtons *m_winTaskbar = nullptr;
    #endif
};

#endif // MUSICPLAYER_H
