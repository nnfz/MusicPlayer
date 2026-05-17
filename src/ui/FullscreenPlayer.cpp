#include "FullscreenPlayer.h"

#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QEasingCurve>
#include <QLinearGradient>
#include <QShortcut>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <QDateTime>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QString fmt(int ms)
{
    int s = ms / 1000, m = s / 60; s %= 60;
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

static QPixmap rounded(const QPixmap &src, int r)
{
    if (src.isNull()) return {};
    QPixmap out(src.size()); out.fill(Qt::transparent);
    QPainter p(&out); p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path; path.addRoundedRect(out.rect(), r, r);
    p.setClipPath(path); p.drawPixmap(0, 0, src); return out;
}

// ---------------------------------------------------------------------------
// OpenGL background
// ---------------------------------------------------------------------------

class FullscreenPlayer::FullscreenBackgroundGL
    : public QOpenGLWidget
    , protected QOpenGLFunctions
{
public:
    explicit FullscreenBackgroundGL(QWidget *parent = nullptr)
        : QOpenGLWidget(parent)
    {
        setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }

    void setPalette(const QVector<QColor> &colors)
    {
        m_palette = colors;
        update();
    }

    void setTime(float t)
    {
        m_time = t;
        update();
    }

    void setBeat(float b)
    {
        m_beat = b;
        update();
    }

protected:
    void initializeGL() override
    {
        initializeOpenGLFunctions();

        static const char *kVert =
            "#version 330 core\n"
            "out vec2 v_uv;\n"
            "const vec2 verts[4] = vec2[](\n"
            "  vec2(-1.0, -1.0),\n"
            "  vec2( 1.0, -1.0),\n"
            "  vec2(-1.0,  1.0),\n"
            "  vec2( 1.0,  1.0));\n"
            "void main() {\n"
            "  vec2 pos = verts[gl_VertexID];\n"
            "  v_uv = pos * 0.5 + 0.5;\n"
            "  gl_Position = vec4(pos, 0.0, 1.0);\n"
            "}\n";

        static const char *kFrag =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "out vec4 fragColor;\n"
            "uniform vec3 u_color0;\n"
            "uniform vec3 u_color1;\n"
            "uniform vec3 u_color2;\n"
            "uniform float u_time;\n"
            "uniform float u_beat;\n"
            "float hash(vec2 p) {\n"
            "  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);\n"
            "}\n"
            "float noise(vec2 p) {\n"
            "  vec2 i = floor(p);\n"
            "  vec2 f = fract(p);\n"
            "  float a = hash(i);\n"
            "  float b = hash(i + vec2(1.0, 0.0));\n"
            "  float c = hash(i + vec2(0.0, 1.0));\n"
            "  float d = hash(i + vec2(1.0, 1.0));\n"
            "  vec2 u = f * f * (3.0 - 2.0 * f);\n"
            "  return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;\n"
            "}\n"
            "void main() {\n"
            "  vec2 p = v_uv * 1.5;\n"
            "  float t = u_time * 0.12;\n"
            "  float n0 = noise(p + vec2(t * 0.8, t));\n"
            "  float n1 = noise(p * 1.3 + vec2(-t, t * 1.1) + 5.2);\n"
            "  float n2 = noise(p * 0.7 + vec2(t * 1.2, -t * 0.9) + 8.7);\n"
            "  float w0 = pow(n0, 4.0);\n"
            "  float w1 = pow(n1, 4.0);\n"
            "  float w2 = pow(n2, 4.0);\n"
            "  float sum = w0 + w1 + w2;\n"
            "  if (sum < 0.001) { w0 = 1.0; sum = 1.0; }\n"
            "  vec3 col = (u_color0 * w0 + u_color1 * w1 + u_color2 * w2) / sum;\n"
            "  float dither = (hash(v_uv + fract(u_time)) - 0.5) / 128.0;\n"
            "  fragColor = vec4(col * 0.6 + dither, 1.0);\n"
            "}\n";

        m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
        m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
        m_program.link();
    }

    void paintGL() override
    {
        glViewport(0, 0, width(), height());
        glClearColor(0.07f, 0.07f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!m_program.isLinked())
            return;

        QVector<QColor> colors = m_palette;
        if (colors.size() < 3)
            colors = { QColor(180, 60, 80), QColor(60, 80, 180), QColor(80, 160, 120) };

        m_program.bind();
        m_program.setUniformValue("u_time", m_time);
        m_program.setUniformValue("u_beat", m_beat);
        m_program.setUniformValue("u_color0", colors[0].redF(), colors[0].greenF(), colors[0].blueF());
        m_program.setUniformValue("u_color1", colors[1].redF(), colors[1].greenF(), colors[1].blueF());
        m_program.setUniformValue("u_color2", colors[2].redF(), colors[2].greenF(), colors[2].blueF());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_program.release();
    }

private:
    QOpenGLShaderProgram m_program;
    QVector<QColor> m_palette;
    float m_time = 0.0f;
    float m_beat = 0.0f;
};

// ---------------------------------------------------------------------------
// MarqueeLabel
// ---------------------------------------------------------------------------

MarqueeLabel::MarqueeLabel(QWidget *parent) : QWidget(parent)
{
    m_anim = new QPropertyAnimation(this, "scrollOffset", this);
}

void MarqueeLabel::setText(const QString &text)
{
    if (m_text == text) return;
    m_text = text;
    m_offset = 0;
    m_anim->stop();
    m_textW = fontMetrics().horizontalAdvance(m_text);
    restartScroll();
}

void MarqueeLabel::setTextStyle(const QFont &font, const QColor &color)
{
    m_font = font;
    m_color = color;
    m_textW = QFontMetrics(m_font).horizontalAdvance(m_text);
    restartScroll();
}

QSize MarqueeLabel::sizeHint() const { return {200, QFontMetrics(m_font).height() + 4}; }
QSize MarqueeLabel::minimumSizeHint() const { return sizeHint(); }

void MarqueeLabel::restartScroll()
{
    m_anim->stop();
    m_offset = 0;
    if (m_textW <= width()) { update(); return; }

    int dist = m_textW + kGap;
    m_anim->setDuration(dist * 1000 / kSpeed);
    m_anim->setStartValue(0);
    m_anim->setEndValue(dist);
    m_anim->setLoopCount(-1);
    m_anim->start();
}

void MarqueeLabel::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); restartScroll(); }

void MarqueeLabel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);
    p.setFont(m_font);
    p.setPen(m_color);

    if (m_textW <= width()) {
        p.drawText(rect(), Qt::AlignCenter, m_text);
    } else {
        int dist = m_textW + kGap;
        p.drawText(rect().translated(-m_offset, 0), Qt::AlignLeft | Qt::AlignVCenter, m_text);
        p.drawText(rect().translated(-m_offset + dist, 0), Qt::AlignLeft | Qt::AlignVCenter, m_text);
    }
}

// ---------------------------------------------------------------------------
// FullscreenPlayer
// ---------------------------------------------------------------------------

FullscreenPlayer::FullscreenPlayer(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    new QShortcut(QKeySequence(Qt::Key_Escape), this, SLOT(closeOverlay()), nullptr, Qt::WidgetWithChildrenShortcut);
    hide();

    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(16);
    connect(m_animTimer, &QTimer::timeout, this, &FullscreenPlayer::animateTick);

    m_card = new QWidget(this);
    m_card->setStyleSheet("background:transparent;");
    m_card->setFixedWidth(440);
    m_cardOpacity = new QGraphicsOpacityEffect(m_card);
    m_cardOpacity->setOpacity(0.0);
    m_card->setGraphicsEffect(m_cardOpacity);

    m_bgWidget = new FullscreenBackgroundGL(this);
    m_bgWidget->setGeometry(rect());
    m_bgWidget->lower();

    m_coverLabel = new QLabel(m_card);
    m_coverLabel->setFixedSize(320, 320);
    m_coverLabel->setAlignment(Qt::AlignCenter);

    m_titleLabel = new MarqueeLabel(m_card);
    m_titleLabel->setFixedWidth(400);
    { QFont f; f.setPixelSize(24); f.setBold(true);
      m_titleLabel->setTextStyle(f, QColor(255, 255, 255, 255)); }

    m_artistLabel = new MarqueeLabel(m_card);
    m_artistLabel->setFixedWidth(400);
    { QFont f; f.setPixelSize(16);
      m_artistLabel->setTextStyle(f, QColor(255, 255, 255, 175)); }

    m_currentTime = new QLabel("00:00", m_card);
    m_currentTime->setStyleSheet("color:rgba(255,255,255,150);font-size:11px;background:transparent;");
    m_currentTime->setFixedWidth(40);

    m_totalTime = new QLabel("00:00", m_card);
    m_totalTime->setStyleSheet("color:rgba(255,255,255,150);font-size:11px;background:transparent;");
    m_totalTime->setFixedWidth(40);
    m_totalTime->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QString sliderSS =
        "QSlider{min-height:16px}"
        "QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,55);border-radius:2px}"
        "QSlider::sub-page:horizontal{background:white;border-radius:2px}"
        "QSlider::handle:horizontal{background:white;width:12px;height:12px;margin:-4px 0;border-radius:6px}"
        "QSlider::handle:horizontal:hover{background:#0078d7}";

    m_seekSlider = new ClickableSlider(Qt::Horizontal, m_card);
    m_seekSlider->setRange(0, 0);
    m_seekSlider->setStyleSheet(sliderSS);
    m_seekSlider->setCursor(Qt::PointingHandCursor);
    connect(m_seekSlider, &QSlider::sliderPressed,  this, [this] { m_userSeeking = true; });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this] {
        m_userSeeking = false;
        emit seekRequested(m_seekSlider->value());
    });
    connect(m_seekSlider, &QSlider::sliderMoved, this, [this](int v) {
        m_currentTime->setText(fmt(v));
    });

    auto btn = [&](const QString &txt, int sz) {
        auto *b = new QPushButton(txt, m_card);
        b->setStyleSheet(QString("QPushButton{background:transparent;border:none;color:rgba(255,255,255,200);font-size:%1px;padding:4px 14px}QPushButton:hover{color:white}").arg(sz));
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };

    m_shuffleBtn = btn(QString::fromUtf8("\xF0\x9F\x94\x80"), 20);
    m_prevBtn    = btn(QString::fromUtf8("\xE2\x8F\xAE"), 24);
    m_playBtn    = btn(QString::fromUtf8("\xE2\x96\xB6"), 32);
    m_nextBtn    = btn(QString::fromUtf8("\xE2\x8F\xAD"), 24);
    m_repeatBtn  = btn(QString::fromUtf8("\xF0\x9F\x94\x81"), 20);
    m_likeBtn    = btn(QString::fromUtf8("\xE2\x99\xA1"), 20);
    m_textBtn    = btn(QString::fromUtf8("\xF0\x9F\x93\x9D"), 20);
    m_closeBtn   = btn(QString::fromUtf8("\xE2\x9C\x95"), 18);
    m_muteBtn    = btn(QString::fromUtf8("\xF0\x9F\x94\x8A"), 18);

    m_volumeSlider = new ClickableSlider(Qt::Horizontal, m_card);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(100);
    m_volumeSlider->setStyleSheet(sliderSS);
    m_volumeSlider->setCursor(Qt::PointingHandCursor);

    connect(m_shuffleBtn, &QPushButton::clicked, this, &FullscreenPlayer::shuffleToggleRequested);
    connect(m_prevBtn,    &QPushButton::clicked, this, &FullscreenPlayer::previousRequested);
    connect(m_playBtn,    &QPushButton::clicked, this, &FullscreenPlayer::playPauseRequested);
    connect(m_nextBtn,    &QPushButton::clicked, this, &FullscreenPlayer::nextRequested);
    connect(m_repeatBtn,  &QPushButton::clicked, this, &FullscreenPlayer::repeatToggleRequested);
    connect(m_likeBtn,    &QPushButton::clicked, this, &FullscreenPlayer::likeToggleRequested);
    connect(m_closeBtn,   &QPushButton::clicked, this, &FullscreenPlayer::closeOverlay);
    connect(m_muteBtn,    &QPushButton::clicked, this, &FullscreenPlayer::muteToggleRequested);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &FullscreenPlayer::volumeChangeRequested);

    QVBoxLayout *cl = new QVBoxLayout(m_card);
    cl->setContentsMargins(0, 0, 0, 0); cl->setSpacing(0);
    cl->addStretch();
    cl->addWidget(m_coverLabel, 0, Qt::AlignCenter); cl->addSpacing(30);
    cl->addWidget(m_titleLabel, 0, Qt::AlignCenter); cl->addSpacing(4);
    cl->addWidget(m_artistLabel, 0, Qt::AlignCenter); cl->addSpacing(30);

    QHBoxLayout *sr = new QHBoxLayout(); sr->setContentsMargins(20, 0, 20, 0); sr->setSpacing(10);
    sr->addWidget(m_currentTime); sr->addWidget(m_seekSlider, 1); sr->addWidget(m_totalTime);
    cl->addLayout(sr); cl->addSpacing(6);

    auto *br = new QHBoxLayout(); br->setAlignment(Qt::AlignHCenter); br->setSpacing(8);
    br->addWidget(m_shuffleBtn);
    br->addWidget(m_prevBtn);
    br->addWidget(m_playBtn);
    br->addWidget(m_nextBtn);
    br->addWidget(m_repeatBtn);
    cl->addLayout(br); cl->addSpacing(8);

    auto *ar = new QHBoxLayout(); ar->setAlignment(Qt::AlignHCenter); ar->setSpacing(8);
    ar->addWidget(m_likeBtn);
    ar->addWidget(m_textBtn);
    cl->addLayout(ar); cl->addSpacing(12);

    auto *vr = new QHBoxLayout(); vr->setAlignment(Qt::AlignHCenter); vr->setSpacing(6);
    vr->addWidget(m_muteBtn); vr->addWidget(m_volumeSlider); vr->addWidget(m_closeBtn);
    cl->addLayout(vr);
}

// ---------------------------------------------------------------------------

bool FullscreenPlayer::eventFilter(QObject *w, QEvent *e)
{
    if (w == m_coverLabel && e->type() == QEvent::MouseButtonRelease) {
        if (static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) { closeOverlay(); return true; }
    }
    return QWidget::eventFilter(w, e);
}

// ---------------------------------------------------------------------------

void FullscreenPlayer::openFor(const QPixmap &cover, const QString &title, const QString &artist,
                                int durationMs, int positionMs, bool isPlaying, int volume)
{
    if (m_closeAnim) { m_closeAnim->stop(); delete m_closeAnim; m_closeAnim = nullptr; }

    m_rawCover = cover; m_durationMs = durationMs;
    m_titleLabel->setText(title); m_artistLabel->setText(artist);
    m_seekSlider->setRange(0, durationMs); m_totalTime->setText(fmt(durationMs));
    updateCoverWidget(); updatePosition(positionMs); updatePlayState(isPlaying); updateVolume(volume);
    m_volumeSlider->blockSignals(true); m_volumeSlider->setValue(volume); m_volumeSlider->blockSignals(false);
    
    for (auto *w : { (QWidget*)m_muteBtn, (QWidget*)m_closeBtn, (QWidget*)m_shuffleBtn, 
                     (QWidget*)m_repeatBtn, (QWidget*)m_likeBtn, (QWidget*)m_textBtn, 
                     (QWidget*)m_prevBtn, (QWidget*)m_playBtn, (QWidget*)m_nextBtn }) {
        if (w) w->setFocusPolicy(Qt::NoFocus);
    }

    extractPalette(m_rawCover);
    if (m_bgWidget)
        m_bgWidget->setPalette(m_palette);

    setGeometry(parentWidget()->rect());
    raise(); show();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    grabKeyboard();

    layoutCard();
    m_cardOpacity->setOpacity(0.0);

    QPoint finalCardPos = m_card->pos();
    QPoint startCardPos = finalCardPos + QPoint(0, 50);
    m_card->move(startCardPos);

    auto *group = new QParallelAnimationGroup(this);

    auto *oa = new QPropertyAnimation(m_cardOpacity, "opacity", group);
    oa->setStartValue(0.0); oa->setEndValue(1.0);
    oa->setDuration(300); oa->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(oa);

    auto *pa = new QPropertyAnimation(m_card, "pos", group);
    pa->setStartValue(startCardPos); pa->setEndValue(finalCardPos);
    pa->setDuration(450); pa->setEasingCurve(QEasingCurve::OutBack);
    group->addAnimation(pa);

    group->start(QAbstractAnimation::DeleteWhenStopped);
    m_animTimer->start();
    m_isOpen = true;
}

// ---------------------------------------------------------------------------

void FullscreenPlayer::closeOverlay()
{
    if (!m_isOpen) return;
    m_isOpen = false;
    releaseKeyboard();
    if (m_closeAnim) { m_closeAnim->stop(); delete m_closeAnim; m_closeAnim = nullptr; }

    m_closeAnim = new QParallelAnimationGroup(this);
    
    auto *oa = new QPropertyAnimation(m_cardOpacity, "opacity", m_closeAnim);
    oa->setStartValue(m_cardOpacity->opacity()); oa->setEndValue(0.0);
    oa->setDuration(250); oa->setEasingCurve(QEasingCurve::InQuad);
    m_closeAnim->addAnimation(oa);

    auto *pa = new QPropertyAnimation(m_card, "pos", m_closeAnim);
    pa->setStartValue(m_card->pos()); 
    pa->setEndValue(m_card->pos() + QPoint(0, 30));
    pa->setDuration(250); pa->setEasingCurve(QEasingCurve::InQuad);
    m_closeAnim->addAnimation(pa);

    connect(m_closeAnim, &QParallelAnimationGroup::finished, this, [this] {
        hide();
        clearFocus();
        m_animTimer->stop();
        delete m_closeAnim; m_closeAnim = nullptr;
    });
    m_closeAnim->start();
}

// ---------------------------------------------------------------------------

void FullscreenPlayer::updateTrack(const QPixmap &cover, const QString &title,
                                    const QString &artist, int durationMs)
{
    m_rawCover = cover; m_durationMs = durationMs;
    m_titleLabel->setText(title); m_artistLabel->setText(artist);
    m_seekSlider->setRange(0, durationMs); m_totalTime->setText(fmt(durationMs));
    updateCoverWidget();
    layoutCard();
    if (isVisible()) {
        extractPalette(m_rawCover);
        if (m_bgWidget)
            m_bgWidget->setPalette(m_palette);
    }
}

void FullscreenPlayer::updatePosition(int ms)
{
    if (m_userSeeking) return;
    m_seekSlider->blockSignals(true); m_seekSlider->setValue(ms); m_seekSlider->blockSignals(false);
    m_currentTime->setText(fmt(ms));
}

void FullscreenPlayer::updatePlayState(bool playing)
{
    m_playBtn->setText(playing ? QString::fromUtf8("\xE2\x8F\xB8") : QString::fromUtf8("\xE2\x96\xB6"));
}

void FullscreenPlayer::updateVolume(int value)
{
    m_volumeValue = qBound(0, value, 100);
    m_volumeSlider->blockSignals(true);
    m_volumeSlider->setValue(m_volumeValue);
    m_volumeSlider->blockSignals(false);

    if (m_volumeValue == 0) {
        m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x87"));
    } else if (m_volumeValue < 50) {
        m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x89"));
    } else {
        m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x8A"));
    }
}

void FullscreenPlayer::updateLikeState(bool liked)
{
    m_likeBtn->setText(liked ? QString::fromUtf8("\xE2\x99\xA5") : QString::fromUtf8("\xE2\x99\xA1"));
    m_likeBtn->setStyleSheet(liked
        ? "QPushButton{background:transparent;border:none;color:#1db954;font-size:20px;padding:4px 14px}QPushButton:hover{color:#1ed760}"
        : "QPushButton{background:transparent;border:none;color:rgba(255,255,255,200);font-size:20px;padding:4px 14px}QPushButton:hover{color:white}");
}

void FullscreenPlayer::updateShuffleState(bool enabled, int mode)
{
    if (!enabled) {
        m_shuffleBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x80"));
        m_shuffleBtn->setStyleSheet("QPushButton{background:transparent;border:none;color:rgba(255,255,255,200);font-size:20px;padding:4px 14px}QPushButton:hover{color:white}");
        return;
    }

    if (mode == 1) {
        m_shuffleBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x80") + "S");
        m_shuffleBtn->setStyleSheet("QPushButton{background:transparent;border:none;color:#1db954;font-size:20px;padding:4px 14px}QPushButton:hover{color:#1ed760}");
        return;
    }

    m_shuffleBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x80") + "R");
    m_shuffleBtn->setStyleSheet("QPushButton{background:transparent;border:none;color:#00bcd4;font-size:20px;padding:4px 14px}QPushButton:hover{color:#38d6ea}");
}

void FullscreenPlayer::updateRepeatState(int mode)
{
    if (mode == 0) {
        m_repeatBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x81"));
        m_repeatBtn->setStyleSheet("QPushButton{background:transparent;border:none;color:rgba(255,255,255,200);font-size:20px;padding:4px 14px}QPushButton:hover{color:white}");
    } else if (mode == 1) {
        m_repeatBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x81"));
        m_repeatBtn->setStyleSheet("QPushButton{background:transparent;border:none;color:#1db954;font-size:20px;padding:4px 14px}QPushButton:hover{color:#1ed760}");
    } else {
        m_repeatBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x82"));
        m_repeatBtn->setStyleSheet("QPushButton{background:transparent;border:none;color:#1db954;font-size:20px;padding:4px 14px}QPushButton:hover{color:#1ed760}");
    }
}

void FullscreenPlayer::extractPalette(const QPixmap &albumArt)
{
    m_palette.clear();
    if (albumArt.isNull()) {
        m_palette = { {180, 60, 80}, {60, 80, 180}, {80, 160, 120} };
        return;
    }

    QImage img = albumArt.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .toImage()
                     .convertToFormat(QImage::Format_ARGB32);

    struct Bin {
        int count = 0;
        int rSum = 0;
        int gSum = 0;
        int bSum = 0;
    };

    QVector<Bin> bins(1 << 15);

    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]);
            if (c.alpha() < 16)
                continue;
            const int r5 = c.red() >> 3;
            const int g5 = c.green() >> 3;
            const int b5 = c.blue() >> 3;
            const int key = (r5 << 10) | (g5 << 5) | b5;
            Bin &bin = bins[key];
            bin.count++;
            bin.rSum += c.red();
            bin.gSum += c.green();
            bin.bSum += c.blue();
        }
    }

    struct BinColor {
        QColor color;
        int count = 0;
    };

    QVector<BinColor> colors;
    colors.reserve(512);
    for (const Bin &bin : bins) {
        if (bin.count <= 0)
            continue;

        colors.append({
            QColor(bin.rSum / bin.count, bin.gSum / bin.count, bin.bSum / bin.count),
            bin.count
        });
    }

    if (colors.isEmpty()) {
        m_palette = { {180, 60, 80}, {60, 80, 180}, {80, 160, 120} };
        return;
    }

    std::sort(colors.begin(), colors.end(), [](const BinColor &a, const BinColor &b) {
        return a.count > b.count;
    });

    QVector<QColor> finalPalette;
    for (const auto &bc : colors) {
        bool tooSimilar = false;
        for (const QColor &existing : finalPalette) {
            int dr = bc.color.red() - existing.red();
            int dg = bc.color.green() - existing.green();
            int db = bc.color.blue() - existing.blue();
            if (std::sqrt(dr*dr + dg*dg + db*db) < 45) {
                tooSimilar = true;
                break;
            }
        }
        if (!tooSimilar) {
            finalPalette.append(bc.color);
        }
        if (finalPalette.size() >= 3) break;
    }

    while (finalPalette.size() < 3) {
        if (finalPalette.isEmpty()) finalPalette.append(QColor(30, 30, 30));
        else finalPalette.append(finalPalette.last());
    }

    m_palette = finalPalette;
}

// ---------------------------------------------------------------------------

void FullscreenPlayer::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), QColor(18, 18, 18));
}

void FullscreenPlayer::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) closeOverlay();
    else QWidget::keyPressEvent(e);
}

void FullscreenPlayer::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (m_bgWidget)
        m_bgWidget->setGeometry(rect());
    if (m_isOpen)
        layoutCard();
}

void FullscreenPlayer::updateBassLevel(float level)
{
    // Fast attack (0.8) to catch the 'kick', smooth decay (0.92)
    if (level > m_lastLevel)
        m_lastLevel = m_lastLevel * 0.2f + level * 0.8f; 
    else
        m_lastLevel = m_lastLevel * 0.92f + level * 0.08f;
}

void FullscreenPlayer::animateTick()
{
    m_phase += 0.015f;
    if (m_bgWidget)
        m_bgWidget->setTime(m_phase);
}

void FullscreenPlayer::layoutCard()
{
    m_card->adjustSize();
    m_card->move((width() - m_card->width()) / 2, (height() - m_card->height()) / 2);
}

void FullscreenPlayer::updateCoverWidget()
{
    if (m_rawCover.isNull()) {
        m_coverLabel->clear();
        m_coverLabel->setFixedSize(320, 320);
        return;
    }

    QPixmap px = m_rawCover.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_coverLabel->setPixmap(rounded(px, 24));
    m_coverLabel->setFixedSize(px.size());
}
