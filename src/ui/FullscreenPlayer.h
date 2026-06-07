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
#include <QPainter>
#include <QEasingCurve>
#include <QStyleOptionButton>
#include <QStyle>
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
    void setAlignment(Qt::Alignment align) { m_alignment = align; update(); }
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
    Qt::Alignment      m_alignment { Qt::AlignLeft | Qt::AlignVCenter };
    QPropertyAnimation *m_anim { nullptr };

    static constexpr int kSpeed { 60 };
    static constexpr int kGap   { 60 };
    static constexpr int kFade  { 24 };
};

class AnimatedScaleButton : public QPushButton {
    Q_OBJECT
    Q_PROPERTY(float scale READ scale WRITE setScale)
    Q_PROPERTY(float crossfade READ crossfade WRITE setCrossfade)
public:
    AnimatedScaleButton(QWidget *parent = nullptr) : QPushButton(parent) {
        m_anim = new QPropertyAnimation(this, "scale", this);
        m_anim->setDuration(120);
        m_anim->setEasingCurve(QEasingCurve::OutQuad);
        
        m_fadeAnim = new QPropertyAnimation(this, "crossfade", this);
        m_fadeAnim->setDuration(400);
        m_fadeAnim->setEasingCurve(QEasingCurve::InOutQuad);
    }
    float scale() const { return m_scale; }
    void setScale(float s) { m_scale = s; update(); }
    float groupOpacity() const { return m_groupOpacity; }
    void setGroupOpacity(float o) { m_groupOpacity = o; update(); }
    void setIconAlignment(Qt::Alignment a) { m_iconAlignment = a; update(); }
    
    float crossfade() const { return m_crossfade; }
    void setCrossfade(float f) { m_crossfade = f; update(); }

    void setIconAnimated(const QIcon &newIcon) {
        m_oldIcon = icon();
        setIcon(newIcon);
        m_crossfade = 0.0f;
        m_fadeAnim->stop();
        m_fadeAnim->setStartValue(0.0f);
        m_fadeAnim->setEndValue(1.0f);
        m_fadeAnim->start();
        
        // Force the scale animation back to the correct state since setIcon can interrupt it
        m_anim->stop();
        m_anim->setEndValue(isActuallyUnderMouse() ? 1.15f : 1.0f);
        m_anim->start();
    }

    void pulse() {
        m_anim->stop();
        m_scale = 0.85f;
        update();
        m_anim->setEndValue(1.0f);
        m_anim->start();
    }

    bool isActuallyUnderMouse() const {
        return rect().contains(mapFromGlobal(QCursor::pos()));
    }

protected:
    void enterEvent(QEnterEvent *e) override {
        QPushButton::enterEvent(e);
        m_anim->stop(); m_anim->setEndValue(1.15f); m_anim->start();
    }
    void leaveEvent(QEvent *e) override {
        QPushButton::leaveEvent(e);
        if (isActuallyUnderMouse()) return;
        m_anim->stop(); m_anim->setEndValue(1.0f); m_anim->start();
    }
    void mousePressEvent(QMouseEvent *e) override {
        QPushButton::mousePressEvent(e);
        m_anim->stop(); m_anim->setEndValue(0.85f); m_anim->start();
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        QPushButton::mouseReleaseEvent(e);
        m_anim->stop();
        // Force hover state (1.15f) on release. 
        // We know the mouse is here because they just clicked it.
        // This defeats any bugs where Qt layout shifts cause hit-tests to fail.
        m_anim->setEndValue(1.15f);
        m_anim->start();
        
        // Safety net: if they really moved the mouse away instantly, leaveEvent will catch it later, 
        // or we can schedule a delayed check.
        QTimer::singleShot(150, this, [this]() {
            if (!isActuallyUnderMouse()) {
                m_anim->stop();
                m_anim->setEndValue(1.0f);
                m_anim->start();
            }
        });
    }
    void paintEvent(QPaintEvent *e) override {
        Q_UNUSED(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        
        p.setOpacity(m_groupOpacity);
        
        QRect cr = contentsRect();
        QPointF anchor = rect().center();
        
        if (!icon().isNull()) {
            QPixmap dummyPix = icon().pixmap(iconSize());
            if (m_iconAlignment & Qt::AlignLeft) {
                anchor = QRect(QPoint(cr.left(), cr.top() + (cr.height() - dummyPix.height()) / 2), dummyPix.size()).center();
            } else if (m_iconAlignment & Qt::AlignRight) {
                anchor = QRect(QPoint(cr.right() - dummyPix.width() + 1, cr.top() + (cr.height() - dummyPix.height()) / 2), dummyPix.size()).center();
            }
        }
        
        p.translate(anchor);
        p.scale(m_scale, m_scale);
        p.translate(-anchor);

        QStyleOptionButton opt;
        initStyleOption(&opt);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

        auto drawIcon = [&](const QIcon& icn, float alpha) {
    if (icn.isNull() || alpha <= 0.0f) return;
    QPixmap pix = icn.pixmap(iconSize(), isEnabled() ? (isActuallyUnderMouse() ? QIcon::Active : QIcon::Normal) : QIcon::Disabled, isDown() ? QIcon::On : QIcon::Off);
            QColor iconColor = palette().color(QPalette::ButtonText);
            if (iconColor.isValid() && iconColor.alpha() > 0) {
                QPainter pixPainter(&pix);
                pixPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                pixPainter.fillRect(pix.rect(), iconColor);
                pixPainter.end();
            }
            
            QRect iconRect;
            if (m_iconAlignment & Qt::AlignLeft) {
                iconRect = QRect(QPoint(cr.left(), cr.top() + (cr.height() - pix.height()) / 2), pix.size());
            } else if (m_iconAlignment & Qt::AlignRight) {
                iconRect = QRect(QPoint(cr.right() - pix.width() + 1, cr.top() + (cr.height() - pix.height()) / 2), pix.size());
            } else {
                iconRect = QRect(cr.center() - pix.rect().center(), pix.size());
            }
            
            float oldOpacity = p.opacity();
            p.setOpacity(oldOpacity * alpha);
            p.drawPixmap(iconRect, pix);
            p.setOpacity(oldOpacity);
        };

        if (!icon().isNull()) {
            if (m_crossfade < 1.0f && !m_oldIcon.isNull()) {
                p.setCompositionMode(QPainter::CompositionMode_Plus);
                drawIcon(m_oldIcon, 1.0f - m_crossfade);
                drawIcon(icon(), m_crossfade);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            } else {
                drawIcon(icon(), 1.0f);
            }
        } else if (!text().isEmpty()) {
            QColor txtColor = palette().color(QPalette::ButtonText);
            txtColor.setAlphaF(m_groupOpacity);
            p.setPen(txtColor);
            p.setFont(font());
            p.drawText(cr, Qt::AlignCenter, text());
        }
    }
private:
    float m_scale = 1.0f;
    float m_groupOpacity = 1.0f;
    QPropertyAnimation* m_anim;
    Qt::Alignment m_iconAlignment = Qt::AlignCenter;
    float m_crossfade = 1.0f;
    QPropertyAnimation* m_fadeAnim;
    QIcon m_oldIcon;
};

class FullscreenPlayer : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal mainUiOpacity READ mainUiOpacity WRITE setMainUiOpacity)
    Q_PROPERTY(qreal controlsOpacity READ controlsOpacity WRITE setControlsOpacity)
    Q_PROPERTY(qreal openAlpha READ openAlpha WRITE setOpenAlpha)

public:
    explicit FullscreenPlayer(QWidget *parent = nullptr);
    ~FullscreenPlayer();

    void openFor(const QPixmap &cover, const QString &title, const QString &artist,
                 const QString &album, int durationMs, int positionMs, bool isPlaying, int volume);
    void activate();
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
qreal openAlpha() const { return (qreal)m_openAlpha; }
void setOpenAlpha(qreal v);

bool isOpen() const { return m_isOpen; }

public slots:
    QPixmap grabUi();
    void setUiHidden(bool hidden);
    void setFsAnimating(bool animating);

    static void setLyricsFontFamily(const QString &family) { s_lyricsFontFamily = family; }
    static QString lyricsFontFamily() { return s_lyricsFontFamily; }

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
    void closeRequested();

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
    static QString s_lyricsFontFamily;
    void extractPalette(const QPixmap &albumArt);
    void updateCoverWidget();
    void requestLyrics();
    void sendLyricsRequest(const QUrl &url);
    void applyLyrics(const QString &synced, const QString &plain, bool instrumental);
    void parseSyncedLyrics(const QString &text);
    void parsePlainLyrics(const QString &text);
    void updateLyricsButtonState();
    void updateLyricsHighlight(int ms);
    void setLyricsVisible(bool visible, bool animate, bool isUserAction = false);
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
    AnimatedScaleButton   *m_lyricsHint   { nullptr };

    FullscreenBackgroundGL *m_bgWidget    { nullptr };
    QWidget               *m_dimOverlay   { nullptr };
    QGraphicsOpacityEffect *m_rootOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_centerAreaOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_titleBarOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_seekBarOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_playbackControlsOpacityEffect { nullptr };
    QGraphicsOpacityEffect *m_lyricsHintOpacityEffect { nullptr };

    float                  m_controlsAlpha { 0.f };
    float                  m_hintAlpha     { 0.f };

    QLabel                *m_coverLabel   { nullptr };
    MarqueeLabel          *m_titleLabel   { nullptr };
    MarqueeLabel          *m_artistLabel  { nullptr };
    QLabel                *m_currentTime  { nullptr };
    QLabel                *m_totalTime    { nullptr };
    ClickableSlider       *m_seekSlider   { nullptr };
    AnimatedScaleButton   *m_shuffleBtn   { nullptr };
    AnimatedScaleButton   *m_prevBtn      { nullptr };
    AnimatedScaleButton   *m_playBtn      { nullptr };
    AnimatedScaleButton   *m_nextBtn      { nullptr };
    AnimatedScaleButton   *m_repeatBtn    { nullptr };
    AnimatedScaleButton   *m_likeBtn      { nullptr };
    AnimatedScaleButton   *m_muteBtn      { nullptr };
    ClickableSlider       *m_volumeSlider { nullptr };
    QListWidget           *m_lyricsList   { nullptr };

    QLabel                *m_contentSnapshotLabel { nullptr };
    bool                  m_fsAnimating     { false };

    QVariantAnimation     *m_lyricsHighlightAnim { nullptr };
    LyricsItemDelegate    *m_lyricsDelegate      { nullptr };

    QParallelAnimationGroup *m_openCloseAnim { nullptr };
    QVariantAnimation       *m_paletteTransitionAnim { nullptr };
    QVariantAnimation       *m_speedPulseAnim { nullptr };

    QTimer                *m_hideControlsTimer  { nullptr };
    QElapsedTimer          m_frameTimer;
    QElapsedTimer          m_lyricsScrollClock;
    qint64                 m_lastFrameMs { 0 };
    bool                   m_lyricsScrollActive { false };

    qreal                  m_mainUiOpacity  { 1.0 };
    qreal                  m_controlsOpacity { 0.0 };
    float                  m_openAlpha       { 0.0 };

    QPointF                m_centerOffset;
    QPointF                m_centerOffsetTarget;
    QPointF                m_centerOffsetVelocity;

    float m_hintExtScale  { 1.0f };
    float m_hintExtScaleV { 0.0f };
    bool  m_hintExtPress  { false };

    float                  m_lyricsPanelX        { 0.f };
    float                  m_lyricsPanelXTarget  { 0.f };
    float                  m_lyricsPanelXVelocity { 0.f };

    float                  m_controlsY       { 0.f };
    float                  m_controlsYTarget { 0.f };
    float                  m_controlsYVelocity { 0.f };

    float                  m_hintX       { 0.f };
    float                  m_hintXTarget { 0.f };
    float                  m_hintXVelocity       { 0.f };

    int                    m_lyricsScrollTarget   { 0 };
    float                  m_lyricsScrollVelocity { 0.f };

    bool                   m_lyricsVisible { false };
    bool                   m_lyricsWasManuallyOpened { false };
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

    int                    m_currentCoverSize { 420 };

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