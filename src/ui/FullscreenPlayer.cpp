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
#include <QSizePolicy>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QVector>
#include <QListWidget>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTextOption>
#include <QVariantAnimation>
#include <QHash>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <limits>
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

static QString normalizeLyricsKey(const QString &text)
{
    return text.simplified().toLower();
}

static int durationToSeconds(int ms)
{
    if (ms <= 0)
        return 0;
    // Manual rounding to avoid <cmath> dependency/conflicts
    return qMax(1, static_cast<int>((ms / 1000.0) + 0.5));
}

static constexpr int kCardWidth = 440;
static constexpr int kLyricsPanelWidth = 360;
static constexpr int kLyricsPanelPaddingLeft = 24;
static constexpr int kLyricsFontSize = 14;
static constexpr int kLyricsFontSizeActive = 18;

struct LyricsCacheEntry {
    QString synced;
    QString plain;
    bool instrumental = false;
    bool found = false;
};

QHash<QString, LyricsCacheEntry> s_lyricsCache;
bool s_lyricsCacheLoaded = false;
static constexpr int kLyricsCacheMaxEntries = 800;
static constexpr int kLyricsCacheVersion = 1;

QString lyricsCachePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return {};
    QDir().mkpath(dir);
    return dir + QStringLiteral("/lrc_cache.json");
}

void pruneLyricsCache()
{
    while (s_lyricsCache.size() > kLyricsCacheMaxEntries) {
        auto it = s_lyricsCache.begin();
        if (it == s_lyricsCache.end())
            break;
        s_lyricsCache.erase(it);
    }
}

void loadLyricsCache()
{
    if (s_lyricsCacheLoaded)
        return;
    s_lyricsCacheLoaded = true;

    const QString path = lyricsCachePath();
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;

    if (file.size() > 5 * 1024 * 1024) {
        file.close();
        return;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt() != kLyricsCacheVersion)
        return;

    const QJsonObject items = root.value(QStringLiteral("items")).toObject();
    for (auto it = items.begin(); it != items.end(); ++it) {
        const QJsonObject obj = it.value().toObject();
        LyricsCacheEntry entry;
        entry.synced = obj.value(QStringLiteral("synced")).toString();
        entry.plain = obj.value(QStringLiteral("plain")).toString();
        entry.instrumental = obj.value(QStringLiteral("instrumental")).toBool(false);
        entry.found = obj.value(QStringLiteral("found")).toBool(false);
        s_lyricsCache.insert(it.key(), entry);
    }
}

void saveLyricsCache()
{
    if (!s_lyricsCacheLoaded)
        return;

    const QString path = lyricsCachePath();
    if (path.isEmpty())
        return;

    QJsonObject items;
    for (auto it = s_lyricsCache.begin(); it != s_lyricsCache.end(); ++it) {
        QJsonObject obj;
        obj.insert(QStringLiteral("synced"), it->synced);
        obj.insert(QStringLiteral("plain"), it->plain);
        obj.insert(QStringLiteral("instrumental"), it->instrumental);
        obj.insert(QStringLiteral("found"), it->found);
        items.insert(it.key(), obj);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kLyricsCacheVersion);
    root.insert(QStringLiteral("items"), items);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

void cacheLyricsEntry(const QString &key, const LyricsCacheEntry &entry)
{
    if (key.isEmpty())
        return;
    s_lyricsCache.insert(key, entry);
    pruneLyricsCache();
    saveLyricsCache();
}

class LyricsItemDelegate : public QStyledItemDelegate
{
public:
    explicit LyricsItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void setView(QListWidget *view) { m_view = view; }

    void setIndices(int active, int previous)
    {
        m_activeIndex = active;
        m_prevIndex = previous;
    }

    void setProgress(qreal progress)
    {
        m_progress = progress;
        if (m_view) {
            // We no longer call doItemsLayout() here. 
            // Layout is completely static now, only visual rendering changes.
            // This guarantees 60fps buttery smoothness.
            m_view->viewport()->update();
        }
    }

    int getExtraTop(int row) const {
        if (!m_view || !m_view->viewport()) return 0;
        return (row == 0) ? qMax(0, m_view->viewport()->height() / 2 - 20) : 0;
    }

    int getExtraBottom(int row) const {
        if (!m_view || !m_view->viewport() || !m_view->model()) return 0;
        return (row == m_view->model()->rowCount() - 1) ? qMax(0, m_view->viewport()->height() / 2 - 20) : 0;
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        
        QFont font = opt.font;
        font.setPixelSize(kLyricsFontSizeActive);
        font.setBold(true);
        QFontMetrics fm(font);

        int width = opt.rect.width();
        if (width <= 0 && m_view)
            width = m_view->viewport()->width() - 24;
        if (width <= 0)
            width = 300;

        int flags = Qt::TextWordWrap | Qt::AlignCenter;
        QRect bounds = fm.boundingRect(QRect(0, 0, width, 10000), flags, opt.text);
        
        // Return generous container size so scaled text never clips.
        return QSize(width, bounds.height() + 24 + getExtraTop(index.row()) + getExtraBottom(index.row()));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing);

        const bool isActive = index.row() == m_activeIndex;
        const bool isPrev = index.row() == m_prevIndex;
        qreal blend = 0.0;
        if (isActive)
            blend = m_progress;
        else if (isPrev)
            blend = 1.0 - m_progress;

        const int baseSize = kLyricsFontSize;
        const int activeSize = kLyricsFontSizeActive;
        
        // Use painter scale rather than changing font size
        qreal minScale = (qreal)baseSize / (qreal)activeSize;
        qreal scale = minScale + (1.0 - minScale) * blend;

        QFont font = opt.font;
        font.setPixelSize(activeSize);
        font.setBold(true);
        painter->setFont(font);

        auto lerp = [&](int a, int b) { return a + (b - a) * blend; };
        QColor baseColor(140, 140, 140);
        QColor activeColor(255, 255, 255);
        QColor color(lerp(baseColor.red(), activeColor.red()),
                     lerp(baseColor.green(), activeColor.green()),
                     lerp(baseColor.blue(), activeColor.blue()));
        painter->setPen(color);

        QRect contentRect = opt.rect;
        contentRect.setTop(contentRect.top() + getExtraTop(index.row()));
        contentRect.setBottom(contentRect.bottom() - getExtraBottom(index.row()));

        // Scale from the exact center of the layout block
        painter->translate(contentRect.center());
        painter->scale(scale, scale);
        painter->translate(-contentRect.center());

        // Dynamic fade out near the edges of the viewport
        if (m_view && m_view->viewport()) {
            int viewH = m_view->viewport()->height();
            int cy = contentRect.center().y();
            float dist = qAbs(cy - viewH / 2.0f);
            float maxDist = viewH / 2.0f;
            float normalizedDist = dist / maxDist; // 0 at center, 1 at edge
            float alpha = 1.0f;
            if (normalizedDist > 0.35f) {
                alpha = 1.0f - (normalizedDist - 0.35f) / 0.65f;
            }
            // Apply easing for smoother fade
            alpha = alpha * alpha; 
            painter->setOpacity(qBound(0.0f, alpha, 1.0f));
        }

        QTextOption textOpt;
        textOpt.setWrapMode(QTextOption::WordWrap);
        textOpt.setAlignment(Qt::AlignCenter);
        
        // Text wrapper sees full box, painter scale fits it smoothly
        painter->drawText(contentRect, opt.text, textOpt);

        painter->restore();
    }

private:
    QListWidget *m_view { nullptr };
    int m_activeIndex { -1 };
    int m_prevIndex { -1 };
    qreal m_progress { 1.0 };
};

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

    ~FullscreenBackgroundGL()
    {
        makeCurrent();
        doneCurrent();
    }

    void setPalette(const QVector<QColor> &colors)
    {
        if (m_palette == colors) return;
        m_prevPalette = m_palette;
        m_palette = colors;
        m_progress = 0.0f;
        update();
    }

    void setPaletteInstant(const QVector<QColor> &colors)
    {
        m_palette = colors;
        m_prevPalette = colors;
        m_progress = 1.0f;
        update();
    }

    void setPaletteTransition(float progress)
    {
        m_progress = progress;
        update();
    }

    void setCover(const QPixmap &pixmap)
    {
        Q_UNUSED(pixmap)
        // For the new pure-liquid shader, we don't need a blurred texture.
        // We just need the palette, which is already handled by setPalette.
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
            "layout(location = 0) in vec2 a_pos;\n"
            "out vec2 v_uv;\n"
            "void main() {\n"
            "  v_uv = a_pos * 0.5 + 0.5;\n"
            "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
            "}\n";

        static const char *kFrag =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "out vec4 fragColor;\n"
            "uniform float u_time;\n"
            "uniform vec3 u_color0;\n"
            "uniform vec3 u_color1;\n"
            "uniform vec3 u_color2;\n"
            "\n"
            "vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }\n"
            "vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }\n"
            "vec3 permute(vec3 x) { return mod289(((x*34.0)+1.0)*x); }\n"
            "\n"
            "float snoise(vec2 v) {\n"
            "  const vec4 C = vec4(0.211324865405187, 0.366025403784439,\n"
            "                      -0.577350269189626, 0.024390243902439);\n"
            "  vec2 i  = floor(v + dot(v, C.yy));\n"
            "  vec2 x0 = v - i + dot(i, C.xx);\n"
            "  vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);\n"
            "  vec4 x12 = x0.xyxy + C.xxzz;\n"
            "  x12.xy -= i1;\n"
            "  i = mod289(i);\n"
            "  vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));\n"
            "  vec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy), dot(x12.zw,x12.zw)), 0.0);\n"
            "  m = m*m; m = m*m;\n"
            "  vec3 x = 2.0 * fract(p * C.www) - 1.0;\n"
            "  vec3 h = abs(x) - 0.5;\n"
            "  vec3 ox = floor(x + 0.5);\n"
            "  vec3 a0 = x - ox;\n"
            "  m *= 1.79284291400159 - 0.85373472095314 * (a0*a0 + h*h);\n"
            "  vec3 g;\n"
            "  g.x = a0.x * x0.x + h.x * x0.y;\n"
            "  g.yz = a0.yz * x12.xz + h.yz * x12.yw;\n"
            "  return 130.0 * dot(m, g);\n"
            "}\n"
            "\n"
            "void main() {\n"
            "  vec2 uv = v_uv;\n"
            "  float t = u_time * 0.1;\n"
            "\n"
            "  // Use domain warping to generate the liquid pattern\n"
            "  vec2 p = uv * 1.2;\n"
            "  \n"
            "  float n1 = snoise(p + vec2(t * 0.5, t * 0.3));\n"
            "  float n2 = snoise(p * 1.5 + vec2(n1, n1) + vec2(-t * 0.4, t * 0.6));\n"
            "  float n3 = snoise(p * 0.8 + vec2(n2, n1) + vec2(t * 0.3, -t * 0.2));\n"
            "  \n"
            "  // Map the noise to our 3 dominant colors\n"
            "  float weight1 = smoothstep(-0.6, 0.6, n2);\n"
            "  float weight2 = smoothstep(-0.5, 0.5, n3);\n"
            "  \n"
            "  vec3 liquid = mix(u_color0, u_color1, weight1);\n"
            "  liquid = mix(liquid, u_color2, weight2);\n"
            "  \n"
            "  // Add some internal flow highlight\n"
            "  liquid += (n2 * 0.05);\n"
            "  \n"
            "  // Final processing\n"
            "  liquid *= 0.6; // Darken for UI\n"
            "  \n"
            "  // Vignette\n"
            "  float d = length(uv - 0.5);\n"
            "  liquid *= smoothstep(1.2, 0.4, d);\n"
            "\n"
            "  fragColor = vec4(liquid, 1.0);\n"
            "}\n";

        m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
        m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
        m_program.link();

        float verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        m_vbo.create();
        m_vbo.bind();
        m_vbo.allocate(verts, sizeof(verts));
    }

    void paintGL() override
    {
        glViewport(0, 0, width(), height());
        glClearColor(0.07f, 0.07f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!m_program.isLinked())
            return;

        m_program.bind();
        m_program.setUniformValue("u_time", m_time);

        QVector<QColor> colors = m_palette;
        if (colors.size() < 3) colors = { QColor(180, 60, 80), QColor(60, 80, 180), QColor(80, 160, 120) };
        
        QVector<QColor> prevColors = m_prevPalette;
        if (prevColors.size() < 3) prevColors = colors;

        for (int i = 0; i < 3; ++i) {
            float r = prevColors[i].redF()   + (colors[i].redF()   - prevColors[i].redF())   * m_progress;
            float g = prevColors[i].greenF() + (colors[i].greenF() - prevColors[i].greenF()) * m_progress;
            float b = prevColors[i].blueF()  + (colors[i].blueF()  - prevColors[i].blueF())  * m_progress;
            m_program.setUniformValue(QString("u_color%1").arg(i).toLatin1().constData(), r, g, b);
        }

        m_vbo.bind();
        m_program.enableAttributeArray(0);
        m_program.setAttributeBuffer(0, GL_FLOAT, 0, 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_program.release();
    }

private:
    QOpenGLShaderProgram m_program;
    QOpenGLBuffer m_vbo;
    QVector<QColor> m_palette;
    QVector<QColor> m_prevPalette;
    float m_progress = 1.0f;
    float m_time = 0.0f;
};

// ---------------------------------------------------------------------------
// MarqueeLabel
// ---------------------------------------------------------------------------

MarqueeLabel::MarqueeLabel(QWidget *parent) : QWidget(parent)
{
    m_anim = new QPropertyAnimation(this, "scrollOffset", this);
}

void MarqueeLabel::setText(const QString &text)
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
    m_card->setMinimumWidth(kCardWidth);
    m_card->setMaximumWidth(kCardWidth + kLyricsPanelWidth);
    m_card->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_cardOpacity = new QGraphicsOpacityEffect(m_card);
    m_cardOpacity->setOpacity(0.0);
    m_card->setGraphicsEffect(m_cardOpacity);

    m_mainPanel = new QWidget(m_card);
    m_mainPanel->setFixedWidth(kCardWidth);
    m_mainPanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    m_lyricsPanel = new QWidget(m_card);
    m_lyricsPanel->setMaximumWidth(0);
    m_lyricsPanel->setMinimumWidth(0);
    m_lyricsPanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_lyricsPanel->setStyleSheet("background:transparent;");

    m_bgWidget = new FullscreenBackgroundGL(this);
    m_bgWidget->setGeometry(rect());
    m_bgWidget->lower();

    m_coverLabel = new QLabel(m_mainPanel);
    m_coverLabel->setFixedSize(320, 320);
    m_coverLabel->setAlignment(Qt::AlignCenter);

    m_titleLabel = new MarqueeLabel(m_mainPanel);
    m_titleLabel->setFixedWidth(400);
    { QFont f; f.setPixelSize(24); f.setBold(true);
      m_titleLabel->setTextStyle(f, QColor(255, 255, 255, 255)); }

    m_artistLabel = new MarqueeLabel(m_mainPanel);
    m_artistLabel->setFixedWidth(400);
    { QFont f; f.setPixelSize(16);
      m_artistLabel->setTextStyle(f, QColor(255, 255, 255, 175)); }

    m_currentTime = new QLabel("00:00", m_mainPanel);
    m_currentTime->setStyleSheet("color:rgba(255,255,255,150);font-size:11px;background:transparent;");
    m_currentTime->setFixedWidth(40);

    m_totalTime = new QLabel("00:00", m_mainPanel);
    m_totalTime->setStyleSheet("color:rgba(255,255,255,150);font-size:11px;background:transparent;");
    m_totalTime->setFixedWidth(40);
    m_totalTime->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QString sliderSS =
        "QSlider{min-height:16px}"
        "QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,55);border-radius:2px}"
        "QSlider::sub-page:horizontal{background:white;border-radius:2px}"
        "QSlider::handle:horizontal{background:white;width:12px;height:12px;margin:-4px 0;border-radius:6px}"
        "QSlider::handle:horizontal:hover{background:#0078d7}";

    m_seekSlider = new ClickableSlider(Qt::Horizontal, m_mainPanel);
    m_seekSlider->setRange(0, 0);
    m_seekSlider->setSingleStep(5000);
    m_seekSlider->setPageStep(15000);
    m_seekSlider->setStyleSheet(sliderSS);
    m_seekSlider->setCursor(Qt::PointingHandCursor);
    connect(m_seekSlider, &QSlider::sliderPressed,  this, [this] { m_userSeeking = true; });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this] {
        m_userSeeking = false;
        const int target = m_seekSlider->value();
        // Pre-anchor the interpolator at the new position so the lyric
        // highlight doesn't lag waiting for the engine's first post-seek
        // positionChanged. Also clear any stale lyric-click ignore window so
        // the next position update isn't blocked by tolerance check.
        m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch();
        m_positionAnchorAudioMs = target;
        m_lastPositionMs = target;
        m_seekIgnoreUntilMs = 0;
        emit seekRequested(target);
        if (m_lyricsSyncedAvailable && m_lyricsVisible)
            updateLyricsHighlight(target);
    });
    connect(m_seekSlider, &QSlider::sliderMoved, this, [this](int v) {
        m_currentTime->setText(fmt(v));
    });

    auto btn = [&](const QString &txt, int sz) {
        auto *b = new QPushButton(txt, m_mainPanel);
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

    m_textBtn->setStyleSheet(
        "QPushButton{background:transparent;border:none;color:rgba(255,255,255,200);font-size:20px;padding:4px 14px}"
        "QPushButton:hover{color:white}"
        "QPushButton:disabled{color:rgba(255,255,255,90);}");
    m_textBtn->setEnabled(false);

    m_volumeSlider = new ClickableSlider(Qt::Horizontal, m_mainPanel);
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
    connect(m_textBtn,    &QPushButton::clicked, this, &FullscreenPlayer::toggleLyrics);
    connect(m_closeBtn,   &QPushButton::clicked, this, &FullscreenPlayer::closeOverlay);
    connect(m_muteBtn,    &QPushButton::clicked, this, &FullscreenPlayer::muteToggleRequested);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &FullscreenPlayer::volumeChangeRequested);

    QVBoxLayout *cl = new QVBoxLayout(m_mainPanel);
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

    auto *lyricsLayout = new QVBoxLayout(m_lyricsPanel);
    lyricsLayout->setContentsMargins(kLyricsPanelPaddingLeft, 12, 12, 12);
    lyricsLayout->setSpacing(10);

    m_lyricsList = new QListWidget(m_lyricsPanel);
    m_lyricsList->setWordWrap(true);
    m_lyricsList->setUniformItemSizes(false);
    m_lyricsList->setSpacing(6);
    m_lyricsList->setFocusPolicy(Qt::NoFocus);
    m_lyricsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_lyricsList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lyricsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lyricsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_lyricsList->viewport()->setCursor(Qt::PointingHandCursor);
    m_lyricsList->setStyleSheet(
        "QListWidget{background:transparent;border:none;color:rgba(255,255,255,180);}"
        "QListWidget::item{padding:2px 6px;}"
    );

    m_lyricsDelegate = new LyricsItemDelegate(m_lyricsList);
    m_lyricsDelegate->setView(m_lyricsList);
    m_lyricsList->setItemDelegate(m_lyricsDelegate);

    m_lyricsHighlightAnim = new QVariantAnimation(this);
    // 280ms OutCubic feels right for lyric transitions — slow enough to read
    // the morph but fast enough that line N+1 visibly takes over before the
    // user starts wondering. Spotify's transition is ~250ms.
    m_lyricsHighlightAnim->setDuration(280);
    m_lyricsHighlightAnim->setStartValue(0.0);
    m_lyricsHighlightAnim->setEndValue(1.0);
    m_lyricsHighlightAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_lyricsHighlightAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_lyricsHighlightProgress = v.toReal();
        if (m_lyricsDelegate)
            m_lyricsDelegate->setProgress(m_lyricsHighlightProgress);
    });
    connect(m_lyricsHighlightAnim, &QVariantAnimation::finished, this, [this] {
        m_lyricsPrevIndex = -1;
    });
    lyricsLayout->addWidget(m_lyricsList, 1);

    m_paletteTransitionAnim = new QVariantAnimation(this);
    m_paletteTransitionAnim->setDuration(1200);
    m_paletteTransitionAnim->setStartValue(0.0);
    m_paletteTransitionAnim->setEndValue(1.0);
    m_paletteTransitionAnim->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_paletteTransitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        if (m_bgWidget) m_bgWidget->setPaletteTransition(v.toFloat());
    });

    m_speedPulseAnim = new QVariantAnimation(this);
    m_speedPulseAnim->setDuration(800);
    m_speedPulseAnim->setStartValue(0.0);
    m_speedPulseAnim->setEndValue(1.0);
    m_speedPulseAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_speedPulseAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        float progress = v.toFloat();
        // Sine-like speed pulse: fast at start, slow at end
        float pulse = std::sin(progress * 3.14159f);
        m_animationSpeed = 1.0 + 4.0 * pulse;
    });
    connect(m_speedPulseAnim, &QVariantAnimation::finished, this, [this] {
        m_animationSpeed = 1.0;
    });

    // Tie palette transition to a speed pulse as well
    connect(m_paletteTransitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        float progress = v.toFloat();
        float pulse = std::sin(progress * 3.14159f);
        m_animationSpeed = 1.0 + 4.0 * pulse;
    });

    connect(m_lyricsList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const int row = m_lyricsList->row(item);
        if (row < 0 || row >= m_lyricsSyncedTimes.size())
            return;
        const int targetMs = m_lyricsSyncedTimes[row];
        m_expectedSeekPositionMs = targetMs;
        // Short ignore window: engine usually catches up within 100-300ms.
        // 600ms is plenty without freezing the UI for seconds on slow seeks.
        m_seekIgnoreUntilMs = QDateTime::currentMSecsSinceEpoch() + 600;
        // Kill any in-flight scroll/highlight anim so we don't compound
        // motions on top of the snap.
        if (m_lyricsScrollAnim) m_lyricsScrollAnim->stop();
        if (m_lyricsHighlightAnim) m_lyricsHighlightAnim->stop();
        startLyricsHighlightAnimation(m_lyricsCurrentIndex, row);
        m_lyricsCurrentIndex = row;
        // Re-anchor interpolation so updateLyricsHighlight doesn't bounce away
        // before the engine's first post-seek position arrives.
        m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch();
        m_positionAnchorAudioMs = targetMs;
        m_lastPositionMs = targetMs;
        emit seekRequested(targetMs);
        // INSTANT snap on click — matches Spotify/Apple Music feel and
        // prevents the visible "double scroll" where the row first appeared
        // to scroll one distance and then continue another.
        animateLyricsScrollTo(row, true, true);
    });

    m_lyricsList->viewport()->installEventFilter(this);
    // Force native viewport repaint when scrolling so our dynamic opacity works perfectly
    connect(m_lyricsList->verticalScrollBar(), &QScrollBar::valueChanged, m_lyricsList->viewport(), [this] {
        m_lyricsList->viewport()->update();
    });
    // No visible scrollbar; suspend auto-scroll via wheel/drag only.

    auto *root = new QHBoxLayout(m_card);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_mainPanel);
    root->addWidget(m_lyricsPanel);
}

FullscreenPlayer::~FullscreenPlayer()
{
    if (m_animTimer) m_animTimer->stop();
}

// ---------------------------------------------------------------------------

bool FullscreenPlayer::eventFilter(QObject *w, QEvent *e)
{
    if (w == m_coverLabel && e->type() == QEvent::MouseButtonRelease) {
        if (static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) { closeOverlay(); return true; }
    }
    if (m_lyricsList &&
        w == m_lyricsList->viewport() &&
        (e->type() == QEvent::Wheel || e->type() == QEvent::MouseButtonPress ||
         e->type() == QEvent::MouseMove || e->type() == QEvent::KeyPress)) {
        suspendLyricsAutoScroll();
    }
    return QWidget::eventFilter(w, e);
}

// ---------------------------------------------------------------------------

void FullscreenPlayer::openFor(const QPixmap &cover, const QString &title, const QString &artist,
                                const QString &album, int durationMs, int positionMs, bool isPlaying, int volume)
{
    if (m_closeAnim) { m_closeAnim->stop(); delete m_closeAnim; m_closeAnim = nullptr; }

    m_rawCover = cover; m_durationMs = durationMs;
    m_trackTitle = title;
    m_trackArtist = artist;
    m_trackAlbum = album;
    m_trackDurationSec = durationToSeconds(durationMs);
    m_titleLabel->setText(title); m_artistLabel->setText(artist);
    m_seekSlider->setRange(0, durationMs); m_totalTime->setText(fmt(durationMs));
    updateCoverWidget(); updatePosition(positionMs); updatePlayState(isPlaying); updateVolume(volume);
    m_volumeSlider->blockSignals(true); m_volumeSlider->setValue(volume); m_volumeSlider->blockSignals(false);
    requestLyrics();
    
    for (auto *w : { (QWidget*)m_muteBtn, (QWidget*)m_closeBtn, (QWidget*)m_shuffleBtn, 
                     (QWidget*)m_repeatBtn, (QWidget*)m_likeBtn, (QWidget*)m_textBtn, 
                     (QWidget*)m_prevBtn, (QWidget*)m_playBtn, (QWidget*)m_nextBtn }) {
        if (w) w->setFocusPolicy(Qt::NoFocus);
    }

    extractPalette(m_rawCover);
    if (m_bgWidget) {
        m_bgWidget->setPaletteInstant(m_palette);
        if (m_speedPulseAnim) {
            m_speedPulseAnim->stop();
            m_speedPulseAnim->start();
        }
        m_bgWidget->setCover(m_rawCover);
    }

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
    pa->setDuration(450); pa->setEasingCurve(QEasingCurve::OutQuint);
    group->addAnimation(pa);

    connect(group, &QParallelAnimationGroup::finished, this, [this] {
        if (m_cardOpacity) m_cardOpacity->setEnabled(false);
    });

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
    setLyricsVisible(false, false);
    if (m_closeAnim) { m_closeAnim->stop(); delete m_closeAnim; m_closeAnim = nullptr; }

    if (m_speedPulseAnim) {
        m_speedPulseAnim->stop();
        m_speedPulseAnim->start();
    }

    if (m_cardOpacity) m_cardOpacity->setEnabled(true);

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
                                    const QString &artist, const QString &album, int durationMs)
{
    m_rawCover = cover; m_durationMs = durationMs;
    m_trackTitle = title;
    m_trackArtist = artist;
    m_trackAlbum = album;
    m_trackDurationSec = durationToSeconds(durationMs);
    m_titleLabel->setText(title); m_artistLabel->setText(artist);
    m_seekSlider->setRange(0, durationMs); m_totalTime->setText(fmt(durationMs));
    updateCoverWidget();
    layoutCard();
    if (isVisible()) {
        extractPalette(m_rawCover);
        if (m_bgWidget) {
            m_bgWidget->setPalette(m_palette);
            if (m_paletteTransitionAnim) {
                m_paletteTransitionAnim->stop();
                m_paletteTransitionAnim->start();
            }
            m_bgWidget->setCover(m_rawCover);
        }
    }
    if (m_isOpen)
        requestLyrics();
}

void FullscreenPlayer::updatePosition(int ms)
{
    if (m_userSeeking) {
        m_lastPositionMs = ms;
        // Re-anchor so when sliderReleased fires the interpolator picks up
        // from the actual playback point, not a stale one.
        m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch();
        m_positionAnchorAudioMs = ms;
        return;
    }

    if (QDateTime::currentMSecsSinceEpoch() < m_seekIgnoreUntilMs) {
        const int delta = std::abs(ms - m_expectedSeekPositionMs);
        if (delta < 500) {
            // Engine settled at the click-to-seek target — release the lock.
            m_seekIgnoreUntilMs = 0;
        } else if (delta > 5000) {
            // The user has clearly moved on (e.g. dragged the main seek bar
            // to a totally different point). Drop the click-to-seek lock so
            // the lyric highlight follows the new position immediately.
            // Without this branch the lyric panel could appear "frozen" for
            // up to 600ms after every fast seek combo.
            m_seekIgnoreUntilMs = 0;
        } else {
            // Engine is settling within a few hundred ms of the original
            // target — keep blocking briefly to prevent the highlight
            // bouncing to a neighboring lyric and back.
            return;
        }
    }

    m_lastPositionMs = ms;
    // Anchor for sub-frame interpolation. The engine emits at ~20Hz; we want
    // lyric highlight transitions to land within one display frame of the true
    // audio position, so animateTick() extrapolates from this anchor.
    m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch();
    m_positionAnchorAudioMs = ms;
    m_seekSlider->blockSignals(true); m_seekSlider->setValue(ms); m_seekSlider->blockSignals(false);
    m_currentTime->setText(fmt(ms));
    updateLyricsHighlight(ms);
}

void FullscreenPlayer::updatePlayState(bool playing)
{
    m_playBtn->setText(playing ? QString::fromUtf8("\xE2\x8F\xB8") : QString::fromUtf8("\xE2\x96\xB6"));
    // Re-anchor the interpolator at the play/pause transition. While paused
    // we freeze the extrapolator so the highlight doesn't run away from the
    // real (frozen) audio position.
    m_positionAnchorPlaying = playing;
    m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch();
    m_positionAnchorAudioMs = m_lastPositionMs;
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
// Lyrics
// ---------------------------------------------------------------------------

bool FullscreenPlayer::hasLyrics() const
{
    return m_lyricsSyncedAvailable || !m_lyricsPlainLines.isEmpty();
}

void FullscreenPlayer::toggleLyrics()
{
    if (!hasLyrics())
        return;
    setLyricsVisible(!m_lyricsVisible, true);
}

void FullscreenPlayer::requestLyrics()
{
    loadLyricsCache();

    if (m_lyricsReply) {
        m_lyricsReply->abort();
        m_lyricsReply->deleteLater();
        m_lyricsReply = nullptr;
    }
    m_lyricsRequestInFlight = false;

    const QString title = m_trackTitle.trimmed();
    const QString artist = m_trackArtist.trimmed();
    if (title.isEmpty() || title == QStringLiteral("No track playing")) {
        m_lyricsSyncedAvailable = false;
        m_lyricsPlainLines.clear();
        m_lyricsSyncedLines.clear();
        m_lyricsSyncedTimes.clear();
        m_lyricsCurrentIndex = -1;
        rebuildLyricsList();
        updateLyricsButtonState();
        return;
    }

    const QString key = normalizeLyricsKey(title) + "|" +
                        normalizeLyricsKey(artist) + "|" +
                        QString::number(m_trackDurationSec);
    if (key == m_lyricsKey && (m_lyricsRequestInFlight || hasLyrics()))
        return;

    m_lyricsKey = key;

    const auto cached = s_lyricsCache.constFind(key);
    if (cached != s_lyricsCache.constEnd()) {
        if (cached->found)
            applyLyrics(cached->synced, cached->plain, cached->instrumental);
        else
            applyLyrics(QString(), QString(), false);
        return;
    }

    m_lyricsSyncedAvailable = false;
    m_lyricsPlainLines.clear();
    m_lyricsSyncedLines.clear();
    m_lyricsSyncedTimes.clear();
    m_lyricsCurrentIndex = -1;
    m_lyricsRequestInFlight = true;
    rebuildLyricsList();
    updateLyricsButtonState();

    if (!m_lyricsNet)
        m_lyricsNet = new QNetworkAccessManager(this);

    QUrl url(QStringLiteral("https://lrclib.net/api/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("track_name"), title);
    if (!artist.isEmpty())
        query.addQueryItem(QStringLiteral("artist_name"), artist);
    url.setQuery(query);

    sendLyricsRequest(url);
}

void FullscreenPlayer::sendLyricsRequest(const QUrl &url)
{
    if (m_lyricsReply) {
        m_lyricsReply->abort();
        m_lyricsReply->deleteLater();
        m_lyricsReply = nullptr;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("MusicPlayer"));
    req.setTransferTimeout(8000);

    m_lyricsRequestInFlight = true;
    rebuildLyricsList();
    m_lyricsReply = m_lyricsNet->get(req);
    const int token = ++m_lyricsRequestToken;
    m_lyricsReply->setProperty("lyricsToken", token);
    connect(m_lyricsReply, &QNetworkReply::finished, this, &FullscreenPlayer::onLyricsReplyFinished);
}

void FullscreenPlayer::onLyricsReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;

    const int token = reply->property("lyricsToken").toInt();
    if (token != m_lyricsRequestToken) {
        reply->deleteLater();
        return;
    }

    const QNetworkReply::NetworkError netError = reply->error();
    const bool replyOpen = reply->isOpen();
    if (m_lyricsReply == reply)
        m_lyricsReply = nullptr;
    reply->deleteLater();

    if (netError != QNetworkReply::NoError || !replyOpen) {
        m_lyricsRequestInFlight = false;
        rebuildLyricsList();
        updateLyricsButtonState();
        return;
    }

    m_lyricsRequestInFlight = false;
    const QByteArray payload = reply->readAll();

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(payload, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isArray()) {
        rebuildLyricsList();
        updateLyricsButtonState();
        return;
    }

    const QJsonArray arr = doc.array();
    if (arr.isEmpty()) {
        cacheLyricsEntry(m_lyricsKey, { QString(), QString(), false, false });
        rebuildLyricsList();
        updateLyricsButtonState();
        return;
    }

    int bestIndex = -1;
    int bestScore = std::numeric_limits<int>::min();
    int bestDiff = std::numeric_limits<int>::max();
    const QString wantTitle = normalizeLyricsKey(m_trackTitle);
    const QString wantArtist = normalizeLyricsKey(m_trackArtist);

    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject obj = arr.at(i).toObject();
        if (obj.isEmpty())
            continue;

        const QString gotTitle = normalizeLyricsKey(obj.value(QStringLiteral("trackName")).toString());
        const QString gotArtist = normalizeLyricsKey(obj.value(QStringLiteral("artistName")).toString());
        const int duration = obj.value(QStringLiteral("duration")).toInt();
        const int diff = (m_trackDurationSec > 0 && duration > 0)
            ? qAbs(duration - m_trackDurationSec)
            : std::numeric_limits<int>::max();

        int score = 0;
        if (!wantTitle.isEmpty() && gotTitle == wantTitle)
            score += 6;
        if (!wantArtist.isEmpty() && gotArtist == wantArtist)
            score += 5;
        if (diff <= 2)
            score += 3;
        else if (diff <= 5)
            score += 1;
        else if (diff != std::numeric_limits<int>::max())
            score -= diff / 5;

        const QString synced = obj.value(QStringLiteral("syncedLyrics")).toString().trimmed();
        const QString plain = obj.value(QStringLiteral("plainLyrics")).toString().trimmed();
        if (!synced.isEmpty())
            score += 2;
        else if (!plain.isEmpty())
            score += 1;

        if (score > bestScore || (score == bestScore && diff < bestDiff)) {
            bestScore = score;
            bestDiff = diff;
            bestIndex = i;
        }
    }

    if (bestIndex < 0) {
        cacheLyricsEntry(m_lyricsKey, { QString(), QString(), false, false });
        rebuildLyricsList();
        updateLyricsButtonState();
        return;
    }

    const QJsonObject best = arr.at(bestIndex).toObject();
    const QString synced = best.value(QStringLiteral("syncedLyrics")).toString();
    const QString plain = best.value(QStringLiteral("plainLyrics")).toString();
    const bool instrumental = best.value(QStringLiteral("instrumental")).toBool(false);
    cacheLyricsEntry(m_lyricsKey, { synced, plain, instrumental, true });
    applyLyrics(synced, plain, instrumental);
}

void FullscreenPlayer::applyLyrics(const QString &synced, const QString &plain, bool instrumental)
{
    m_lyricsSyncedAvailable = false;
    m_lyricsPlainLines.clear();
    m_lyricsSyncedLines.clear();
    m_lyricsSyncedTimes.clear();
    m_lyricsCurrentIndex = -1;

    const QString syncedTrim = synced.trimmed();
    const QString plainTrim = plain.trimmed();

    if (!syncedTrim.isEmpty())
        parseSyncedLyrics(syncedTrim);

    if (!m_lyricsSyncedAvailable && !plainTrim.isEmpty())
        parsePlainLyrics(plainTrim);

    rebuildLyricsList();
    updateLyricsButtonState();
    if (instrumental && !hasLyrics())
        return;
    updateLyricsHighlight(m_lastPositionMs);
}

void FullscreenPlayer::parseSyncedLyrics(const QString &text)
{
    struct Entry { int timeMs; QString line; };
    QVector<Entry> entries;

    QRegularExpression tag(QStringLiteral("\\[(\\d{1,2}):(\\d{2})(?:\\.(\\d{1,3}))?\\]"));
    const QStringList rawLines = text.split('\n');
    for (const QString &raw : rawLines) {
        QRegularExpressionMatchIterator it = tag.globalMatch(raw);
        if (!it.hasNext())
            continue;

        QString line = raw;
        line.remove(tag);
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int minutes = m.captured(1).toInt();
            const int seconds = m.captured(2).toInt();
            const QString fracText = m.captured(3);
            int fracMs = 0;
            if (!fracText.isEmpty()) {
                const int frac = fracText.toInt();
                if (fracText.size() == 1)
                    fracMs = frac * 100;
                else if (fracText.size() == 2)
                    fracMs = frac * 10;
                else
                    fracMs = frac;
            }
            const int timeMs = minutes * 60000 + seconds * 1000 + fracMs;
            entries.push_back({ timeMs, line });
        }
    }

    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.timeMs < b.timeMs;
    });

    m_lyricsSyncedLines.clear();
    m_lyricsSyncedTimes.clear();
    for (const Entry &e : entries) {
        m_lyricsSyncedTimes.append(e.timeMs);
        m_lyricsSyncedLines.append(e.line);
    }

    m_lyricsSyncedAvailable = !m_lyricsSyncedLines.isEmpty();
}

void FullscreenPlayer::parsePlainLyrics(const QString &text)
{
    QStringList lines = text.split('\n');
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
        lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    m_lyricsPlainLines = lines;
}

void FullscreenPlayer::updateLyricsButtonState()
{
    const bool enabled = hasLyrics();
    if (!m_textBtn)
        return;

    m_textBtn->setEnabled(enabled);
    const bool active = enabled && m_lyricsVisible;
    m_textBtn->setStyleSheet(active
        ? "QPushButton{background:transparent;border:none;color:#1db954;font-size:20px;padding:4px 14px}QPushButton:hover{color:#1ed760}QPushButton:disabled{color:rgba(255,255,255,90);}"
        : "QPushButton{background:transparent;border:none;color:rgba(255,255,255,200);font-size:20px;padding:4px 14px}QPushButton:hover{color:white}QPushButton:disabled{color:rgba(255,255,255,90);}");

    if (!enabled && !m_lyricsRequestInFlight && m_lyricsVisible)
        setLyricsVisible(false, true);
}

void FullscreenPlayer::updateLyricsHighlight(int ms)
{
    if (!m_lyricsSyncedAvailable || m_lyricsSyncedTimes.isEmpty() || !m_lyricsList)
        return;

    auto it = std::upper_bound(m_lyricsSyncedTimes.begin(), m_lyricsSyncedTimes.end(), ms);
    const int idx = it == m_lyricsSyncedTimes.begin()
        ? -1
        : static_cast<int>(it - m_lyricsSyncedTimes.begin()) - 1;

    if (QDateTime::currentMSecsSinceEpoch() < m_seekIgnoreUntilMs) {
        if (idx != m_lyricsCurrentIndex) {
            // We recently seeked, and the engine is sending a time that maps to a DIFFERENT
            // lyric line than the one we actively clicked. It's likely a slightly early keyframe.
            // Block it so the UI doesn't bounce away from the clicked line.
            return;
        } else {
            // The engine's time now naturally falls exactly into the lyric we clicked!
            // We can safely lift the block early.
            m_seekIgnoreUntilMs = 0;
        }
    }

    if (idx == m_lyricsCurrentIndex)
        return;

    const int prevIndex = m_lyricsCurrentIndex;
    m_lyricsCurrentIndex = idx;

    QString currentText = (idx >= 0 && idx < m_lyricsList->count()) ? m_lyricsList->item(idx)->text() : "N/A";
    int lyricTimeMs = (idx >= 0 && idx < m_lyricsSyncedTimes.size()) ? m_lyricsSyncedTimes[idx] : 0;
    qDebug() << "[SYNC DEBUG] Text:" << currentText 
             << "| Text time(sec):" << (lyricTimeMs / 1000) 
             << "| Playback time(sec):" << (ms / 1000);

    startLyricsHighlightAnimation(prevIndex, m_lyricsCurrentIndex);

    const bool suspended = lyricsAutoScrollSuspended();
    if (suspended) {
        m_lyricsAutoScrollSuppressed = true;
        return;
    }

    if (idx < 0) {
        // We're before the first synced line (intro / instrumental at the
        // start of the track). The correct visual is the "rest state" of the
        // list: scrollbar at the very top, so the inflated row-0 padding
        // (kLyricsPanelHalfHeight) appears at the viewport top, with the
        // first lyric naturally sitting at viewport center but NOT yet
        // highlighted. animateLyricsScrollTo(0) would compute a scroll that
        // centres row 0's CONTENT — and because row 0's sizeHint already
        // includes top padding, that target shifted the whole text upward,
        // making it look like row 0 was already playing. Scrolling the bar
        // to 0 instead gives the proper "lyrics about to start" framing.
        if (auto *bar = m_lyricsList->verticalScrollBar()) {
            if (m_lyricsScrollAnim) m_lyricsScrollAnim->stop();
            bar->setValue(bar->minimum());
        }
        return;
    }

    // Jumps of more than a few lines are almost always the result of a seek;
    // a smooth 380ms scroll across many rows looks like a fast jittery scroll
    // (and never lands on the right place because the line keeps changing).
    // Snap instantly for big jumps, animate only for natural line-by-line
    // playback advancement.
    const int jump = std::abs(idx - prevIndex);
    const bool isBigJump = (prevIndex < 0) || (jump > 3);
    animateLyricsScrollTo(m_lyricsCurrentIndex, false, isBigJump);
}

void FullscreenPlayer::setLyricsVisible(bool visible, bool animate)
{
    if (visible == m_lyricsVisible)
        return;

    m_lyricsVisible = visible;
    const int targetWidth = visible ? kLyricsPanelWidth : 0;

    if (m_card)
        m_card->adjustSize();

    if (!animate || !m_lyricsPanel) {
        if (m_lyricsPanel)
            m_lyricsPanel->setMinimumWidth(targetWidth);
        if (m_lyricsPanel)
            m_lyricsPanel->setMaximumWidth(targetWidth);
        if (m_card) {
            const int cardHeight = m_card->height() > 0 ? m_card->height() : m_card->sizeHint().height();
            m_card->resize(kCardWidth + targetWidth, cardHeight);
            m_card->move(cardPosForWidth(kCardWidth + targetWidth));
        }
        updateLyricsButtonState();
        if (visible && m_lyricsSyncedAvailable)
            animateLyricsScrollTo(m_lyricsCurrentIndex);
        return;
    }

    auto *group = new QParallelAnimationGroup(this);

    auto *minWidthAnim = new QPropertyAnimation(m_lyricsPanel, "minimumWidth", group);
    minWidthAnim->setStartValue(m_lyricsPanel->minimumWidth());
    minWidthAnim->setEndValue(targetWidth);
    minWidthAnim->setDuration(320);
    minWidthAnim->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(minWidthAnim);

    auto *maxWidthAnim = new QPropertyAnimation(m_lyricsPanel, "maximumWidth", group);
    maxWidthAnim->setStartValue(m_lyricsPanel->maximumWidth());
    maxWidthAnim->setEndValue(targetWidth);
    maxWidthAnim->setDuration(320);
    maxWidthAnim->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(maxWidthAnim);

    const int cardHeight = m_card->height() > 0 ? m_card->height() : m_card->sizeHint().height();
    const QPoint targetPos = cardPosForWidth(kCardWidth + targetWidth);
    const QRect targetGeom(targetPos, QSize(kCardWidth + targetWidth, cardHeight));
    auto *geomAnim = new QPropertyAnimation(m_card, "geometry", group);
    geomAnim->setStartValue(m_card->geometry());
    geomAnim->setEndValue(targetGeom);
    geomAnim->setDuration(320);
    geomAnim->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(geomAnim);

    connect(group, &QParallelAnimationGroup::finished, this, [this, targetWidth, visible] {
        if (m_lyricsPanel) {
            m_lyricsPanel->setMinimumWidth(targetWidth);
            m_lyricsPanel->setMaximumWidth(targetWidth);
        }
        if (m_lyricsList)
            m_lyricsList->doItemsLayout();
        if (visible && m_lyricsSyncedAvailable) {
            updateLyricsHighlight(m_lastPositionMs);
            animateLyricsScrollTo(m_lyricsCurrentIndex, true, true);
        }
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
    updateLyricsButtonState();
}

void FullscreenPlayer::rebuildLyricsList()
{
    if (!m_lyricsList)
        return;

    m_lyricsList->clear();
    const QStringList lines = m_lyricsSyncedAvailable ? m_lyricsSyncedLines : m_lyricsPlainLines;
    if (lines.isEmpty()) {
        if (m_lyricsRequestInFlight) {
            auto *item = new QListWidgetItem("", m_lyricsList);
            item->setTextAlignment(Qt::AlignCenter);
            QFont font = item->font();
            font.setPixelSize(kLyricsFontSize);
            item->setFont(font);
            item->setForeground(QColor(200, 200, 200));
        }
        m_lyricsList->setCurrentRow(-1);
        return;
    }

    QFont baseFont = m_lyricsList->font();
    baseFont.setPixelSize(kLyricsFontSize);
    baseFont.setBold(false);

    for (const QString &line : lines) {
        const QString shown = line.isEmpty() ? QStringLiteral(" ") : line;
        auto *item = new QListWidgetItem(shown, m_lyricsList);
        item->setTextAlignment(Qt::AlignCenter);
        item->setFont(baseFont);
        item->setForeground(QColor(230, 230, 230));
    }

    m_lyricsCurrentIndex = -1;
    m_lyricsList->setCurrentRow(-1);

    if (m_lyricsDelegate)
    {
        m_lyricsDelegate->setIndices(-1, -1);
        m_lyricsDelegate->setProgress(1.0);
    }

    // Pre-warm geometry: force Qt to lay out every row's sizeHint now so the
    // first lyric-line activation doesn't pay the lazy layout cost (which used
    // to manifest as a visible stutter on the first transition of every song).
    m_lyricsList->doItemsLayout();
}

void FullscreenPlayer::animateLyricsScrollTo(int logicalIndex, bool force, bool instant)
{
    if (!m_lyricsVisible || !m_lyricsList)
        return;

    if (force) {
        m_lyricsHoldUntilMs = 0;
        m_lyricsAutoScrollSuppressed = false;
    } else if (lyricsAutoScrollSuspended()) {
        return;
    }

    const int target = lyricsScrollTargetForIndex(logicalIndex);
    if (target < 0)
        return;

    auto *bar = m_lyricsList->verticalScrollBar();

    if (instant) {
        if (m_lyricsScrollAnim) m_lyricsScrollAnim->stop();
        bar->setValue(target);
        return;
    }

    int currentVal = bar->value();
    if (target == currentVal)
        return;

    if (!m_lyricsScrollAnim)
        m_lyricsScrollAnim = new QPropertyAnimation(bar, "value", this);

    m_lyricsScrollAnim->stop();
    m_lyricsScrollAnim->setStartValue(currentVal);
    m_lyricsScrollAnim->setEndValue(target);
    // Match the highlight animation duration and easing so the line "rises"
    // and "lights up" as a single coherent motion instead of two stuttery
    // independent timelines (which is what made the old behavior feel laggy).
    m_lyricsScrollAnim->setDuration(380);
    m_lyricsScrollAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_lyricsScrollAnim->start();
}
void FullscreenPlayer::startLyricsHighlightAnimation(int prevIndex, int nextIndex)
{
    if (!m_lyricsList || !m_lyricsDelegate)
        return;

    m_lyricsPrevIndex = prevIndex;
    m_lyricsDelegate->setIndices(nextIndex, prevIndex);

    if (m_lyricsHighlightAnim) {
        m_lyricsHighlightAnim->stop();
        m_lyricsHighlightAnim->setStartValue(0.0);
        m_lyricsHighlightAnim->setEndValue(1.0);
        m_lyricsDelegate->setProgress(0.0);
        m_lyricsHighlightAnim->start();
    }
}

int FullscreenPlayer::lyricsScrollTargetForIndex(int index) const
{
    if (!m_lyricsList) return -1;
    if (index < 0 || index >= m_lyricsList->count()) return -1;

    QListWidgetItem *item = m_lyricsList->item(index);
    if (!item) return -1;

    auto *bar = m_lyricsList->verticalScrollBar();
    int currentBarVal = bar ? bar->value() : 0;

    QRect rect = m_lyricsList->visualItemRect(item);
    if (!rect.isValid() && bar) {
        // If the item is off-screen, visualItemRect returns an empty rect.
        // We can temporarily scroll to it instantly to force layout visibility, 
        // read the rect, and then instantly restore the scrollbar.
        // Since this happens synchronously before the next paint event,
        // it causes no visual flicker.
        const bool wasBlocked = bar->blockSignals(true);
        m_lyricsList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        rect = m_lyricsList->visualItemRect(item);
        bar->setValue(currentBarVal);
        bar->blockSignals(wasBlocked);
    }

    if (!rect.isValid()) return -1;

    int extraTop = (index == 0) ? qMax(0, m_lyricsList->viewport()->height() / 2 - 20) : 0;
    int extraBottom = (index == m_lyricsList->count() - 1) ? qMax(0, m_lyricsList->viewport()->height() / 2 - 20) : 0;

    QRect contentRect = rect;
    contentRect.setTop(contentRect.top() + extraTop);
    contentRect.setBottom(contentRect.bottom() - extraBottom);

    int absoluteCenterY = (bar ? bar->value() : 0) + contentRect.center().y();
    int target = absoluteCenterY - m_lyricsList->viewport()->height() / 2;

    return bar ? qBound(bar->minimum(), target, bar->maximum()) : target;
}

QPoint FullscreenPlayer::cardPosForWidth(int cardWidth) const
{
    const int cardHeight = m_card ? (m_card->height() > 0 ? m_card->height() : m_card->sizeHint().height()) : 0;
    const int x = (width() - cardWidth) / 2;
    const int y = (height() - cardHeight) / 2;
    return { x, y };
}

void FullscreenPlayer::suspendLyricsAutoScroll()
{
    // 700ms hold after the last user wheel/touch — long enough that a single
    // scroll gesture isn't yanked back instantly, short enough that the
    // auto-follow feels responsive again once the user stops. (Was 2000.)
    m_lyricsHoldUntilMs = QDateTime::currentMSecsSinceEpoch() + 700;
    m_lyricsAutoScrollSuppressed = true;
}

bool FullscreenPlayer::lyricsAutoScrollSuspended() const
{
    return QDateTime::currentMSecsSinceEpoch() < m_lyricsHoldUntilMs;
}

void FullscreenPlayer::maybeResumeLyricsAutoScroll()
{
    if (!m_lyricsAutoScrollSuppressed)
        return;
    if (!m_lyricsVisible || !m_lyricsSyncedAvailable)
        return;
    if (lyricsAutoScrollSuspended())
        return;
    if (m_lyricsCurrentIndex < 0)
        return;

    m_lyricsAutoScrollSuppressed = false;
    animateLyricsScrollTo(m_lyricsCurrentIndex);
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
            // Use squared distance (45^2 = 2025) to avoid sqrt/cmath
            if ((dr*dr + dg*dg + db*db) < 2025) {
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
    m_phase += 0.015f * m_animationSpeed; 
    if (m_bgWidget)
        m_bgWidget->setTime(m_phase);
    maybeResumeLyricsAutoScroll();

    // Sub-frame lyric sync: between 50ms engine updates, extrapolate audio
    // position assuming playback is steady. This brings the visible lyric
    // transition latency down from ~50ms (worst case) to one display frame.
    // We only extrapolate forward — if the engine sends a corrected position
    // updatePosition() will re-anchor.
    if (m_positionAnchorPlaying && m_lyricsSyncedAvailable && m_lyricsVisible &&
        QDateTime::currentMSecsSinceEpoch() >= m_seekIgnoreUntilMs && !m_userSeeking)
    {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_positionAnchorWallMs;
        // Clamp to a sane bound — if we somehow haven't received a position
        // update in a while, don't extrapolate into the next song.
        const qint64 capped = elapsed < qint64(0) ? qint64(0)
                            : elapsed > qint64(500) ? qint64(500)
                            : elapsed;
        const int extrapolated = m_positionAnchorAudioMs + static_cast<int>(capped);
        updateLyricsHighlight(extrapolated);
    }
}

void FullscreenPlayer::layoutCard()
{
    m_card->adjustSize();
    m_card->move(cardPosForWidth(m_card->width()));
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
