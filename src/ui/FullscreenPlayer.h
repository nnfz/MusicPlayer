#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QPixmap>
#include <QImage>
#include <QColor>
#include <QVector>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QMouseEvent>
#include "ClickableSlider.h"

// ---------------------------------------------------------------------------
// MarqueeLabel — прокручивающийся текст
// ---------------------------------------------------------------------------

class MarqueeLabel : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset)

public:
    explicit MarqueeLabel(QWidget *parent = nullptr);

    void setText(const QString &text);
    void setTextStyle(const QFont &font, const QColor &color);
    void setOpacity(double v) { m_opacity = v; update(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    int  scrollOffset() const { return m_offset; }
    void setScrollOffset(int v) { m_offset = v; update(); }

protected:
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *) override;

private:
    void restartScroll();

    QString            m_text;
    QFont              m_font;
    QColor             m_color { Qt::white };
    int                m_textW { 0 };
    int                m_offset { 0 };
    double             m_opacity { 1.0 };
    QPropertyAnimation *m_anim { nullptr };

    static constexpr int kSpeed { 60 };
    static constexpr int kGap   { 60 };
    static constexpr int kFade  { 24 };
};

// ---------------------------------------------------------------------------
// FullscreenPlayer
// ---------------------------------------------------------------------------

class FullscreenPlayer : public QWidget
{
    Q_OBJECT

public:
    explicit FullscreenPlayer(QWidget *parent = nullptr);

    void openFor(const QPixmap &cover, const QString &title, const QString &artist,
                 int durationMs, int positionMs, bool isPlaying, int volume);

    void updateTrack(const QPixmap &cover, const QString &title,
                     const QString &artist, int durationMs);
    void updatePosition(int ms);
    void updatePlayState(bool playing);
    void updateVolume(int value);
    void updateLikeState(bool liked);
    void updateShuffleState(bool enabled, int mode);
    void updateRepeatState(int mode);

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
    bool        eventFilter(QObject *w, QEvent *e) override;
    void        paintEvent(QPaintEvent *) override;
    void        keyPressEvent(QKeyEvent *e) override;
    void        resizeEvent(QResizeEvent *e) override;

private slots:
    void animateTick();

private:
    void extractPalette(const QPixmap &albumArt);
    void updateNoiseFrame();
    void layoutCard();
    void updateCoverWidget();

    QWidget               *m_card         { nullptr };
    QGraphicsOpacityEffect *m_cardOpacity  { nullptr };

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

    QTimer                *m_animTimer    { nullptr };
    QParallelAnimationGroup *m_closeAnim  { nullptr };

    QPixmap                m_rawCover;
    int                    m_durationMs   { 0 };
    bool                   m_userSeeking  { false };
    bool                   m_isOpen       { false };
    int                    m_volumeValue  { 0 };

    QVector<QColor>        m_palette;
    QImage                 m_noiseFrame;
    float                  m_phase        { 0.f };
};