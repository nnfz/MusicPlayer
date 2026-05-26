#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QElapsedTimer>
#include <QPixmap>
#include <QImage>
#include <QColor>
#include <QVector>
#include <QStringList>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QMouseEvent>
#include "ClickableSlider.h"

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;
class QListWidget;
class QScrollBar;
class QVariantAnimation;
class LyricsItemDelegate;

class MarqueeLabel : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal scrollOffset READ scrollOffset WRITE setScrollOffset)

public:
    explicit MarqueeLabel(QWidget *parent = nullptr);

    void setText(const QString &text);
    void setTextStyle(const QFont &font, const QColor &color);
    void setOpacity(double v) { m_opacity = v; update(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    qreal scrollOffset() const { return m_offset; }
    void setScrollOffset(qreal v) { m_offset = v; update(); }

protected:
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *) override;

private:
    void restartScroll();

    QString            m_text;
    QFont              m_font;
    QColor             m_color { Qt::white };
    int                m_textW { 0 };
    qreal              m_offset { 0.0 };
    double             m_opacity { 1.0 };
    QPropertyAnimation *m_anim { nullptr };

    static constexpr int kSpeed { 60 };
    static constexpr int kGap   { 60 };
    static constexpr int kFade  { 24 };
};

class FullscreenPlayer : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal mainUiOpacity READ mainUiOpacity WRITE setMainUiOpacity)
    Q_PROPERTY(qreal controlsOpacity READ controlsOpacity WRITE setControlsOpacity)

public:
    explicit FullscreenPlayer(QWidget *parent = nullptr);
    ~FullscreenPlayer() override;

    void openFor(const QPixmap &cover, const QString &title, const QString &artist,
                 const QString &album, int durationMs, int positionMs, bool isPlaying, int volume);
    void updateTrack(const QPixmap &cover, const QString &title,
                     const QString &artist, const QString &album, int durationMs);
    void updatePosition(int ms);
    void updatePlayState(bool playing);
    void updateVolume(int value);
    void updateLikeState(bool liked);
    void updateShuffleState(bool enabled, int mode);
    void updateRepeatState(int mode);
    void updateBassLevel(float level);

    qreal mainUiOpacity() const { return m_mainUiOpacity; }
    void setMainUiOpacity(qreal v);

    qreal controlsOpacity() const { return m_controlsOpacity; }
    void setControlsOpacity(qreal v);

    bool isOpen() const { return m_isOpen; }

signals:
    void seekRequested(int ms);
    void playPauseRequested();
    void previousRequested();
    void nextRequested();
    void volumeChangeRequested(int value);
    void muteToggleRequested();
    void shuffleToggleRequested();
    void repeatToggleRequested();
    void likeToggleRequested();

public slots:
    void closeOverlay();

protected:
    bool eventFilter(QObject *w, QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private slots:
    void animateTick();
    void toggleLyrics();
    void onLyricsReplyFinished();
    void updateLayout();
    void tickLyricsSmoothScroll();

private:
    void extractPalette(const QPixmap &albumArt);
    void updateCoverWidget();
    void requestLyrics();
    void sendLyricsRequest(const QUrl &url);
    void applyLyrics(const QString &synced, const QString &plain, bool instrumental);
    void parseSyncedLyrics(const QString &text);
    void parsePlainLyrics(const QString &text);
    void updateLyricsButtonState();
    void updateLyricsHighlight(int ms);
    void setLyricsVisible(bool visible, bool animate);
    void rebuildLyricsList();
    void startLyricsHighlightAnimation(int prevIndex, int nextIndex);
    void animateLyricsScrollTo(int index, bool force = false, bool instant = false);
    int  lyricsScrollTargetForIndex(int index) const;
    void suspendLyricsAutoScroll();
    bool lyricsAutoScrollSuspended() const;
    void maybeResumeLyricsAutoScroll();
    bool hasLyrics() const;
    void updateState();
    void showControls();
    void hideControls();

    QPointF springStep(QPointF current, QPointF target, QPointF &velocity, float dt, float stiffness, float damping);
    float   springStep1D(float current, float target, float &velocity, float dt, float stiffness, float damping);

    class FullscreenBackgroundGL;

    QWidget               *m_rootLayout   { nullptr };
    QWidget               *m_titleBar     { nullptr };
    QWidget               *m_centerArea   { nullptr };
    QWidget               *m_lyricsPanel  { nullptr };
    QWidget               *m_playbackControls { nullptr };
    QWidget               *m_seekBarArea  { nullptr };
    QPushButton           *m_lyricsHint   { nullptr };

    FullscreenBackgroundGL *m_bgWidget    { nullptr };
    QWidget               *m_dimOverlay   { nullptr };
    QGraphicsOpacityEffect *m_rootOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_centerAreaOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_titleBarOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_seekBarOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_playbackControlsOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_lyricsHintOpacityEffect { nullptr };

    QLabel                *m_coverLabel   { nullptr };
    MarqueeLabel          *m_titleLabel   { nullptr };
    MarqueeLabel          *m_artistLabel  { nullptr };
    QLabel                *m_currentTime  { nullptr };
    QLabel                *m_totalTime    { nullptr };
    ClickableSlider       *m_seekSlider   { nullptr };
    QPushButton           *m_shuffleBtn   { nullptr };
    QPushButton           *m_prevBtn      { nullptr };
    QPushButton           *m_playBtn      { nullptr };
    QPushButton           *m_nextBtn      { nullptr };
    QPushButton           *m_repeatBtn    { nullptr };
    QPushButton           *m_likeBtn      { nullptr };
    QPushButton           *m_textBtn      { nullptr };
    QPushButton           *m_closeBtn     { nullptr };
    QPushButton           *m_muteBtn      { nullptr };
    ClickableSlider       *m_volumeSlider { nullptr };
    QListWidget           *m_lyricsList   { nullptr };

    QVariantAnimation     *m_lyricsHighlightAnim { nullptr };
    LyricsItemDelegate    *m_lyricsDelegate      { nullptr };

    QParallelAnimationGroup *m_openCloseAnim { nullptr };
    QVariantAnimation       *m_paletteTransitionAnim { nullptr };
    QVariantAnimation       *m_speedPulseAnim { nullptr };

    QTimer                *m_animTimer          { nullptr };
    QTimer                *m_hideControlsTimer  { nullptr };
    QTimer                *m_lyricsScrollTimer  { nullptr };
    QElapsedTimer          m_frameTimer;
    QElapsedTimer          m_lyricsScrollClock;
    qint64                 m_lastFrameMs { 0 };

    qreal                  m_mainUiOpacity  { 1.0 };
    qreal                  m_controlsOpacity { 0.0 };

    QPointF                m_centerOffset;
    QPointF                m_centerOffsetTarget;
    QPointF                m_centerOffsetVelocity;

    float                  m_lyricsPanelX        { 0.f };
    float                  m_lyricsPanelXTarget  { 0.f };
    float                  m_lyricsPanelXVelocity{ 0.f };

    float                  m_controlsY           { 0.f };
    float                  m_controlsYTarget     { 0.f };
    float                  m_controlsYVelocity   { 0.f };

    float                  m_controlsAlpha       { 0.f };
    float                  m_controlsAlphaTarget { 0.f };
    float                  m_controlsAlphaVelocity { 0.f };

    float                  m_hintAlpha           { 0.f };
    float                  m_hintAlphaTarget     { 0.f };
    float                  m_hintAlphaVelocity   { 0.f };

    float                  m_hintX               { 0.f };
    float                  m_hintXTarget         { 0.f };
    float                  m_hintXVelocity       { 0.f };

    float                  m_openAlpha           { 1.f };
    float                  m_openAlphaTarget     { 1.f };
    float                  m_openAlphaVelocity   { 0.f };

    int                    m_lyricsScrollTarget   { 0 };
    float                  m_lyricsScrollVelocity { 0.f };

    bool                   m_lyricsVisible { false };
    bool                   m_stateHinted   { false };
    bool                   m_stateLifted   { false };
    bool                   m_isPlaying     { false };
    bool                   m_isOpen        { false };
    bool                   m_userSeeking   { false };

    qint64                 m_seekIgnoreUntilMs      { 0 };
    int                    m_expectedSeekPositionMs { -1 };
    int                    m_volumeValue            { 0 };
    float                  m_lastLevel              { 0.f };
    int                    m_lastPositionMs         { 0 };
    int                    m_durationMs             { 0 };

    QString                m_trackTitle;
    QString                m_trackArtist;
    QString                m_trackAlbum;
    int                    m_trackDurationSec { 0 };

    QNetworkAccessManager *m_lyricsNet          { nullptr };
    QNetworkReply         *m_lyricsReply        { nullptr };
    int                    m_lyricsRequestToken { 0 };
    bool                   m_lyricsRequestInFlight { false };
    QString                m_lyricsKey;
    bool                   m_lyricsSyncedAvailable { false };
    QStringList            m_lyricsPlainLines;
    QStringList            m_lyricsSyncedLines;
    QVector<int>           m_lyricsSyncedTimes;
    int                    m_lyricsCurrentIndex    { -1 };
    int                    m_lyricsPrevIndex       { -1 };
    qint64                 m_lyricsHoldUntilMs     { 0 };
    bool                   m_lyricsAutoScrollSuppressed { false };

    qint64                 m_positionAnchorWallMs  { 0 };
    int                    m_positionAnchorAudioMs { 0 };
    bool                   m_positionAnchorPlaying { false };

    QVector<QColor>        m_palette;
    float                  m_phase          { 0.f };
    float                  m_animationSpeed { 1.f };

    QPixmap                m_rawCover;
};