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

// ══════════════════════════════════════════════════════════════════════
//  Noise utilities — gradient noise, no integer artifacts
// ══════════════════════════════════════════════════════════════════════

static inline float _mix(float a, float b, float t) { return a + (b-a)*t; }
static inline float _fade(float t) { return t*t*t*(t*(t*6.f-15.f)+10.f); }

// быстрый целочисленный hash → индекс градиента
static inline int _ihash(int x, int y) {
    unsigned h = (unsigned)(x * 1619 + y * 31337);
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return (int)(h & 0x7);
}

// 8 равномерно распределённых градиентов
static const float _GX[8] = { 1.f, 1.f, 0.f,-1.f,-1.f,-1.f, 0.f, 1.f };
static const float _GY[8] = { 0.f, 1.f, 1.f, 1.f, 0.f,-1.f,-1.f,-1.f };

static float gnoise(float x, float y) {
    int   ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix,         fy = y - iy;
    float ux = _fade(fx),      uy = _fade(fy);

    int i00 = _ihash(ix,   iy  ), i10 = _ihash(ix+1, iy  );
    int i01 = _ihash(ix,   iy+1), i11 = _ihash(ix+1, iy+1);

    float v00 = _GX[i00]*fx       + _GY[i00]*fy;
    float v10 = _GX[i10]*(fx-1.f) + _GY[i10]*fy;
    float v01 = _GX[i01]*fx       + _GY[i01]*(fy-1.f);
    float v11 = _GX[i11]*(fx-1.f) + _GY[i11]*(fy-1.f);

    return _mix(_mix(v00,v10,ux), _mix(v01,v11,ux), uy) * 0.5f + 0.5f;
}

// 3 октавы — мягко, без мелких деталей
static float fbm(float x, float y, float t) {
    float ox = t * 0.18f;
    float oy = t * 0.11f;
    return gnoise(x + ox, y + oy);
}

static void boxBlurRgb(QImage &img, int radius)
{
    if (radius <= 0 || img.isNull())
        return;

    const int w = img.width();
    const int h = img.height();
    if (w <= 0 || h <= 0)
        return;

    QImage temp(w, h, QImage::Format_RGB32);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, count = 0;
            for (int k = -radius; k <= radius; ++k) {
                const int sx = qBound(0, x + k, w - 1);
                const QRgb px = img.pixel(sx, y);
                r += qRed(px);
                g += qGreen(px);
                b += qBlue(px);
                ++count;
            }
            temp.setPixel(x, y, qRgb(r / count, g / count, b / count));
        }
    }

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, count = 0;
            for (int k = -radius; k <= radius; ++k) {
                const int sy = qBound(0, y + k, h - 1);
                const QRgb px = temp.pixel(x, sy);
                r += qRed(px);
                g += qGreen(px);
                b += qBlue(px);
                ++count;
            }
            img.setPixel(x, y, qRgb(r / count, g / count, b / count));
        }
    }
}


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
            "float noiseBlur(vec2 p) {\n"
            "  vec2 o = vec2(0.008, 0.008);\n"
            "  float n = 0.0;\n"
            "  n += noise(p);\n"
            "  n += noise(p + vec2(o.x, 0.0));\n"
            "  n += noise(p - vec2(o.x, 0.0));\n"
            "  n += noise(p + vec2(0.0, o.y));\n"
            "  n += noise(p - vec2(0.0, o.y));\n"
            "  return n * 0.2;\n"
            "}\n"
            "vec3 pickColor(int idx, vec3 c0, vec3 c1, vec3 c2) {\n"
            "  return idx == 0 ? c0 : (idx == 1 ? c1 : c2);\n"
            "}\n"
            "void main() {\n"
            "  vec2 p = v_uv * 1.6;\n"
            "  float t = u_time;\n"
            "  float n0 = noiseBlur(p + vec2(0.0, 0.0) + vec2(t * 0.10, t * 0.08));\n"
            "  float n1 = noiseBlur(p + vec2(4.6, 2.3) + vec2(-t * 0.07, t * 0.06));\n"
            "  float n2 = noiseBlur(p + vec2(8.4, 5.1) + vec2(t * 0.05, -t * 0.04));\n"
            "  n0 = n0 * n0 * (3.0 - 2.0 * n0);\n"
            "  n1 = n1 * n1 * (3.0 - 2.0 * n1);\n"
            "  n2 = n2 * n2 * (3.0 - 2.0 * n2);\n"
            "  float w0 = pow(n0, 2.2);\n"
            "  float w1 = pow(n1, 2.2);\n"
            "  float w2 = pow(n2, 2.2);\n"
            "  int maxIdx = 0;\n"
            "  int secondIdx = 1;\n"
            "  if (w1 > w0) { maxIdx = 1; secondIdx = 0; }\n"
            "  if (w2 > (maxIdx == 0 ? w0 : w1)) { secondIdx = maxIdx; maxIdx = 2; }\n"
            "  else if (w2 > (secondIdx == 0 ? w0 : w1)) { secondIdx = 2; }\n"
            "  float wa = (maxIdx == 0 ? w0 : (maxIdx == 1 ? w1 : w2));\n"
            "  float wb = (secondIdx == 0 ? w0 : (secondIdx == 1 ? w1 : w2));\n"
            "  float sum = wa + wb;\n"
            "  if (sum < 0.0001) sum = 1.0;\n"
            "  vec3 c0 = u_color0;\n"
            "  vec3 c1 = u_color1;\n"
            "  vec3 c2 = u_color2;\n"
            "  vec3 a = pickColor(maxIdx, c0, c1, c2);\n"
            "  vec3 b = pickColor(secondIdx, c0, c1, c2);\n"
            "  vec3 col = (a * wa + b * wb) / sum;\n"
            "  col *= 0.78;\n"
            "  fragColor = vec4(col, 1.0);\n"
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
};


// ---------------------------------------------------------------------------
// MarqueeLabel
// ---------------------------------------------------------------------------

MarqueeLabel::MarqueeLabel(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TranslucentBackground);
}

void MarqueeLabel::setText(const QString &text)
{
    m_text = text;
    QFontMetrics fm(m_font);
    m_textW = fm.horizontalAdvance(m_text);
    if (m_anim) { m_anim->stop(); m_anim->deleteLater(); m_anim = nullptr; }
    m_offset = 0;
    updateGeometry();
    update();
    restartScroll();
}

void MarqueeLabel::setTextStyle(const QFont &font, const QColor &color)
{
    m_font  = font;
    m_color = color;
    QFontMetrics fm(m_font);
    m_textW = fm.horizontalAdvance(m_text);
    update();
    restartScroll();
}

QSize MarqueeLabel::sizeHint() const
{
    QFontMetrics fm(m_font);
    return { width() > 0 ? width() : qMin(m_textW, 400), fm.height() + 4 };
}

QSize MarqueeLabel::minimumSizeHint() const { return sizeHint(); }

void MarqueeLabel::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    restartScroll();
}

void MarqueeLabel::restartScroll()
{
    if (m_anim) { m_anim->stop(); m_anim->deleteLater(); m_anim = nullptr; }
    m_offset = 0;
    update();
    if (m_textW <= width()) return;

    int travel = m_textW + kGap;
    int ms = travel * 1000 / kSpeed;

    m_anim = new QPropertyAnimation(this, "scrollOffset", this);
    m_anim->setStartValue(0);
    m_anim->setEndValue(-travel);
    m_anim->setDuration(ms);
    m_anim->setEasingCurve(QEasingCurve::Linear);
    m_anim->setLoopCount(-1);

    QTimer::singleShot(1500, this, [this] { if (m_anim) m_anim->start(); });
}

void MarqueeLabel::paintEvent(QPaintEvent *)
{
    const int w = width(), h = height();
    if (w <= 0 || h <= 0) return;
    const bool scrolling = m_textW > w;

    QPixmap buf(w, h);
    buf.fill(Qt::transparent);
    {
        QPainter p(&buf);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(m_font);
        QFontMetrics fm(m_font);
        int y = (h + fm.ascent() - fm.descent()) / 2;

        p.setPen(m_color);
        if (scrolling) {
            p.drawText(m_offset, y, m_text);
            p.drawText(m_offset + m_textW + kGap, y, m_text);

            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            QLinearGradient gl(0, 0, kFade, 0);
            gl.setColorAt(0, Qt::transparent); gl.setColorAt(1, Qt::white);
            p.fillRect(0, 0, kFade, h, gl);

            QLinearGradient gr(w - kFade, 0, w, 0);
            gr.setColorAt(0, Qt::white); gr.setColorAt(1, Qt::transparent);
            p.fillRect(w - kFade, 0, kFade, h, gr);
        } else {
            p.drawText(0, y, m_text);
        }
    }

    QPainter p(this);
    p.setOpacity(m_opacity);
    p.drawPixmap(0, 0, buf);
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

    QString ctrlSS = "QPushButton{background:transparent;border:none;color:rgba(255,255,255,200);font-size:20px;padding:4px 14px}QPushButton:hover{color:white}";
    QString playSS = "QPushButton{background:transparent;border:none;color:white;font-size:36px;padding:4px 18px}QPushButton:hover{color:#0078d7}";

    m_shuffleBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x80"), m_card);
    m_shuffleBtn->setStyleSheet(ctrlSS); m_shuffleBtn->setCursor(Qt::PointingHandCursor);

    m_prevBtn = new QPushButton(QString::fromUtf8("\xE2\x8F\xAE"), m_card);
    m_prevBtn->setStyleSheet(ctrlSS); m_prevBtn->setCursor(Qt::PointingHandCursor);

    m_playBtn = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"), m_card);
    m_playBtn->setStyleSheet(playSS); m_playBtn->setCursor(Qt::PointingHandCursor);

    m_nextBtn = new QPushButton(QString::fromUtf8("\xE2\x8F\xAD"), m_card);
    m_nextBtn->setStyleSheet(ctrlSS); m_nextBtn->setCursor(Qt::PointingHandCursor);

    m_repeatBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x81"), m_card);
    m_repeatBtn->setStyleSheet(ctrlSS); m_repeatBtn->setCursor(Qt::PointingHandCursor);

    m_likeBtn = new QPushButton(QString::fromUtf8("\xE2\x99\xA1"), m_card);
    m_likeBtn->setStyleSheet(ctrlSS); m_likeBtn->setCursor(Qt::PointingHandCursor);

    m_textBtn = new QPushButton(QStringLiteral("T"), m_card);
    m_textBtn->setStyleSheet(ctrlSS); m_textBtn->setCursor(Qt::PointingHandCursor);
    m_textBtn->setToolTip(QStringLiteral("Lyrics coming soon"));
    connect(m_textBtn, &QPushButton::clicked, this, [this]() {
        m_textBtn->setText(QStringLiteral("...") );
        QTimer::singleShot(900, this, [this]() {
            if (m_textBtn)
                m_textBtn->setText(QStringLiteral("T"));
        });
    });

    connect(m_shuffleBtn, &QPushButton::clicked, this, &FullscreenPlayer::shuffleToggleRequested);
    connect(m_prevBtn, &QPushButton::clicked, this, &FullscreenPlayer::previousRequested);
    connect(m_playBtn, &QPushButton::clicked, this, &FullscreenPlayer::playPauseRequested);
    connect(m_nextBtn, &QPushButton::clicked, this, &FullscreenPlayer::nextRequested);
    connect(m_repeatBtn, &QPushButton::clicked, this, &FullscreenPlayer::repeatToggleRequested);
    connect(m_likeBtn, &QPushButton::clicked, this, &FullscreenPlayer::likeToggleRequested);

    m_closeBtn = new QPushButton(QString::fromUtf8("\xE2\x9C\x95"), m_card);
    m_closeBtn->setStyleSheet(
        "QPushButton{background:transparent;border:none;color:rgba(255,255,255,140);font-size:20px;padding:4px 14px}"
        "QPushButton:hover{color:white}");
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_closeBtn, &QPushButton::clicked, this, &FullscreenPlayer::closeOverlay);

    m_muteBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x8A"), m_card);
    m_muteBtn->setStyleSheet(ctrlSS); m_muteBtn->setCursor(Qt::PointingHandCursor);
    connect(m_muteBtn, &QPushButton::clicked, this, &FullscreenPlayer::muteToggleRequested);

    m_volumeSlider = new ClickableSlider(Qt::Horizontal, m_card);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(130);
    m_volumeSlider->setStyleSheet(sliderSS);
    m_volumeSlider->setCursor(Qt::PointingHandCursor);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &FullscreenPlayer::volumeChangeRequested);

    QVBoxLayout *cl = new QVBoxLayout(m_card);
    cl->setSpacing(10); cl->setContentsMargins(10, 10, 10, 10); cl->setAlignment(Qt::AlignHCenter);

    auto addC = [&](QWidget *w) {
        auto *r = new QHBoxLayout(); r->setAlignment(Qt::AlignHCenter); r->addWidget(w); cl->addLayout(r);
    };

    addC(m_coverLabel); cl->addSpacing(14);
    addC(m_titleLabel); addC(m_artistLabel); cl->addSpacing(18);

    auto *sr = new QHBoxLayout(); sr->setSpacing(8);
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
    m_muteBtn->setFocusPolicy(Qt::NoFocus);
    m_closeBtn->setFocusPolicy(Qt::NoFocus);
    m_shuffleBtn->setFocusPolicy(Qt::NoFocus);
    m_repeatBtn->setFocusPolicy(Qt::NoFocus);
    m_likeBtn->setFocusPolicy(Qt::NoFocus);
    m_textBtn->setFocusPolicy(Qt::NoFocus);
    m_prevBtn->setFocusPolicy(Qt::NoFocus);
    m_playBtn->setFocusPolicy(Qt::NoFocus);
    m_nextBtn->setFocusPolicy(Qt::NoFocus);

    extractPalette(m_rawCover);
    if (m_bgWidget)
        m_bgWidget->setPalette(m_palette);

    setGeometry(parentWidget()->rect());
    raise(); show();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    grabKeyboard();
    update();

    m_card->adjustSize();
    layoutCard();
    m_cardOpacity->setOpacity(0.0);

    auto *oa = new QPropertyAnimation(m_cardOpacity, "opacity", this);
    oa->setStartValue(0.0); oa->setEndValue(1.0);
    oa->setDuration(400); oa->setEasingCurve(QEasingCurve::OutCubic);
    oa->start(QAbstractAnimation::DeleteWhenStopped);

    auto slideIn = [&](QWidget *w, int skipMs) {
        if (!w) return;
        if (w->graphicsEffect()) { delete w->graphicsEffect(); }
        if (auto *ml = qobject_cast<MarqueeLabel *>(w)) ml->setOpacity(1.0);
        QPoint orig = w->pos();
        w->move(orig.x() - 80, orig.y());
        auto *pa = new QPropertyAnimation(w, "pos", this);
        pa->setStartValue(w->pos()); pa->setEndValue(orig);
        pa->setDuration(600); pa->setEasingCurve(QEasingCurve::OutCubic);
        pa->start(QAbstractAnimation::DeleteWhenStopped);
        pa->setCurrentTime(skipMs);
    };

    slideIn(m_shuffleBtn,   300);
    slideIn(m_prevBtn,      300);
    slideIn(m_playBtn,      300);
    slideIn(m_nextBtn,      300);
    slideIn(m_repeatBtn,    300);
    slideIn(m_likeBtn,      260);
    slideIn(m_textBtn,      260);
    slideIn(m_muteBtn,      300);
    slideIn(m_volumeSlider, 300);
    slideIn(m_closeBtn,     300);
    slideIn(m_seekSlider,   300);
    slideIn(m_titleLabel,   200);
    slideIn(m_artistLabel,  170);
    slideIn(m_coverLabel,   100);

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

    for (QWidget *w : {(QWidget *)m_coverLabel, (QWidget *)m_titleLabel, (QWidget *)m_artistLabel,
                       (QWidget *)m_seekSlider, (QWidget *)m_shuffleBtn, (QWidget *)m_prevBtn,
                       (QWidget *)m_playBtn, (QWidget *)m_nextBtn, (QWidget *)m_repeatBtn,
                       (QWidget *)m_likeBtn, (QWidget *)m_textBtn, (QWidget *)m_muteBtn,
                       (QWidget *)m_volumeSlider, (QWidget *)m_closeBtn}) {
        if (!w) continue;
        if (w->graphicsEffect()) w->setGraphicsEffect(nullptr);
        if (auto *ml = qobject_cast<MarqueeLabel *>(w)) ml->setOpacity(1.0);
    }
    m_card->adjustSize();
    layoutCard();

    m_closeAnim = new QParallelAnimationGroup(this);
    auto *oa = new QPropertyAnimation(m_cardOpacity, "opacity", this);
    oa->setStartValue(m_cardOpacity->opacity()); oa->setEndValue(0.0);
    oa->setDuration(300); oa->setEasingCurve(QEasingCurve::InCubic);
    m_closeAnim->addAnimation(oa);
    m_closeAnim->start();

    auto slideOut = [&](QWidget *w, int delayMs) {
        if (!w) return;
        QPoint orig = w->pos();
        auto *pa = new QPropertyAnimation(w, "pos", this);
        pa->setStartValue(orig); pa->setEndValue(QPoint(orig.x() + 24, orig.y()));
        pa->setDuration(280); pa->setEasingCurve(QEasingCurve::InCubic);
        QTimer::singleShot(delayMs, this, [pa] {
            pa->start(QAbstractAnimation::DeleteWhenStopped);
        });
    };

    slideOut(m_coverLabel,   0);
    slideOut(m_titleLabel,   55);
    slideOut(m_artistLabel,  75);
    slideOut(m_shuffleBtn,   130);
    slideOut(m_prevBtn,      130);
    slideOut(m_playBtn,      130);
    slideOut(m_nextBtn,      130);
    slideOut(m_repeatBtn,    130);
    slideOut(m_likeBtn,      145);
    slideOut(m_textBtn,      145);
    slideOut(m_muteBtn,      130);
    slideOut(m_seekSlider,   130);
    slideOut(m_volumeSlider, 130);
    slideOut(m_closeBtn,     130);

    connect(m_closeAnim, &QParallelAnimationGroup::finished, this, [this] {
        hide();
        clearFocus();
        delete m_closeAnim; m_closeAnim = nullptr;
    });
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

// ---------------------------------------------------------------------------

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

    const QColor c1 = colors[0].color;
    const QColor c2 = (colors.size() > 1) ? colors[1].color : c1;

    auto colorDistance = [](const QColor &a, const QColor &b) -> float {
        const float dr = a.redF() - b.redF();
        const float dg = a.greenF() - b.greenF();
        const float db = a.blueF() - b.blueF();
        const float rgb = std::sqrt(dr * dr + dg * dg + db * db);
        const float lumA = a.redF() * 0.2126f + a.greenF() * 0.7152f + a.blueF() * 0.0722f;
        const float lumB = b.redF() * 0.2126f + b.greenF() * 0.7152f + b.blueF() * 0.0722f;
        const float lum = qAbs(lumA - lumB);
        return rgb * 0.75f + lum * 0.25f;
    };

    QColor c3 = c2;
    if (colors.size() > 2) {
        float bestScore = -1.0f;
        for (int i = 2; i < colors.size(); ++i) {
            const QColor c = colors[i].color;
            const float d1 = colorDistance(c, c1);
            const float d2 = colorDistance(c, c2);
            const float score = qMin(d1, d2);
            if (score > bestScore) {
                bestScore = score;
                c3 = c;
            }
        }
    }

    m_palette = { c1, c2, c3 };
}

// ---------------------------------------------------------------------------

void FullscreenPlayer::updateNoiseFrame()
{
    const int NW = 160, NH = 90;
    QImage frame(NW, NH, QImage::Format_RGB32);

    if (m_palette.size() < 3) { frame.fill(Qt::black); m_noiseFrame = frame; return; }

    float pr[3], pg[3], pb[3];
    for (int i = 0; i < 3; i++) {
        pr[i] = m_palette[i].redF();
        pg[i] = m_palette[i].greenF();
        pb[i] = m_palette[i].blueF();
    }

    const float t  = m_phase;
    const float sc = 1.6f;   // ← пространственная частота: меньше = крупнее пятна
    auto sm = [](float x) { return x*x*(3.f-2.f*x); };

    for (int y = 0; y < NH; y++) {
        QRgb *line = reinterpret_cast<QRgb*>(frame.scanLine(y));
        for (int x = 0; x < NW; x++) {

            float px = (float)x / NW * sc;
            float py = (float)y / NH * sc;

            // лёгкий warp одним слоем — даёт органичность без завитков
            float wx = gnoise(px + 0.0f, py + 0.0f + t * 0.10f) - 0.5f;
            float wy = gnoise(px + 3.7f, py + 1.9f + t * 0.08f) - 0.5f;

            float n0 = gnoise(px + wx * 0.8f + t * 0.10f,
                              py + wy * 0.8f + t * 0.08f);
            float n1 = gnoise(px + 4.6f + wx * 0.7f - t * 0.07f,
                              py + 2.3f + wy * 0.7f + t * 0.06f);
            float n2 = gnoise(px + 8.4f - wx * 0.6f + t * 0.05f,
                              py + 5.1f - wy * 0.6f - t * 0.04f);

            n0 = sm(n0);
            n1 = sm(n1);
            n2 = sm(n2);

            float w[3] = {
                std::pow(n0, 2.6f),
                std::pow(n1, 2.6f),
                std::pow(n2, 2.6f)
            };

            int maxIdx = 0;
            int secondIdx = 1;
            if (w[1] > w[0]) { maxIdx = 1; secondIdx = 0; }
            if (w[2] > w[maxIdx]) { secondIdx = maxIdx; maxIdx = 2; }
            else if (w[2] > w[secondIdx]) { secondIdx = 2; }

            float sum = w[maxIdx] + w[secondIdx];
            if (sum <= 0.0001f) {
                w[maxIdx] = 0.5f;
                w[secondIdx] = 0.5f;
                sum = 1.0f;
            }

            float R = (pr[maxIdx] * w[maxIdx] + pr[secondIdx] * w[secondIdx]) / sum;
            float G = (pg[maxIdx] * w[maxIdx] + pg[secondIdx] * w[secondIdx]) / sum;
            float B = (pb[maxIdx] * w[maxIdx] + pb[secondIdx] * w[secondIdx]) / sum;

            line[x] = qRgb(
                qBound(0, (int)(R * 200.f), 255),
                qBound(0, (int)(G * 200.f), 255),
                qBound(0, (int)(B * 200.f), 255)
            );
        }
    }

    boxBlurRgb(frame, 6);
    m_noiseFrame = frame;
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

void FullscreenPlayer::animateTick()
{
    m_phase += 0.05f;
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