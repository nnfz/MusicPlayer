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
#include <cmath>
#include <QDateTime>
#include <QDebug>
#include <QApplication>

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
    if (ms <= 0) return 0;
    return qMax(1, static_cast<int>((ms / 1000.0) + 0.5));
}

static constexpr int kCardWidth = 380;
static constexpr int kLyricsPanelWidth = 600; 
static constexpr int kLyricsPanelPaddingLeft = 20;
static constexpr int kLyricsFontSize = 20;
static constexpr int kLyricsFontSizeActive = 23;

// ---------------------------------------------------------------------------
// Lyrics Cache logic
// ---------------------------------------------------------------------------

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
    if (dir.isEmpty()) return {};
    QDir().mkpath(dir);
    return dir + QStringLiteral("/lrc_cache.json");
}

void loadLyricsCache()
{
    if (s_lyricsCacheLoaded) return;
    s_lyricsCacheLoaded = true;
    const QString path = lyricsCachePath();
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;
    const QByteArray data = file.readAll();
    file.close();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject root = doc.object();
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

void cacheLyricsEntry(const QString &key, const LyricsCacheEntry &entry)
{
    if (key.isEmpty()) return;
    s_lyricsCache.insert(key, entry);
    if (s_lyricsCache.size() > kLyricsCacheMaxEntries) s_lyricsCache.erase(s_lyricsCache.begin());
    const QString path = lyricsCachePath();
    if (path.isEmpty()) return;
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
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

// ---------------------------------------------------------------------------
// LyricsItemDelegate
// ---------------------------------------------------------------------------

class LyricsItemDelegate : public QStyledItemDelegate
{
public:
    explicit LyricsItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void setView(QListWidget *view) { m_view = view; }
    void setIndices(int active, int previous) { m_activeIndex = active; m_prevIndex = previous; }
    void setProgress(qreal progress) { m_progress = progress; if (m_view) m_view->viewport()->update(); }

    int getExtraTop(int row) const {
        if (!m_view || !m_view->viewport()) return 0;
        return (row == 0) ? qMax(0, m_view->viewport()->height() / 2 - 20) : 0;
    }
    int getExtraBottom(int row) const {
        if (!m_view || !m_view->viewport() || !m_view->model()) return 0;
        return (row == m_view->model()->rowCount() - 1) ? qMax(0, m_view->viewport()->height() / 2 - 20) : 0;
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        QFont font = opt.font;
        font.setPixelSize(kLyricsFontSizeActive);
        font.setBold(true);
        font.setFamily("-apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif");
        QFontMetrics fm(font);
        int width = opt.rect.width();
        if (width <= 0 && m_view) width = m_view->viewport()->width() - 24;
        if (width <= 0) width = 300;
        int flags = Qt::TextWordWrap | Qt::AlignLeft;
        QRect bounds = fm.boundingRect(QRect(0, 0, width, 10000), flags, opt.text);
        return QSize(width, bounds.height() + 24 + getExtraTop(index.row()) + getExtraBottom(index.row()));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing);
        const bool isActive = index.row() == m_activeIndex;
        const bool isPrev = index.row() == m_prevIndex;
        qreal blend = 0.0;
        if (isActive) blend = m_progress;
        else if (isPrev) blend = 1.0 - m_progress;
        const int baseSize = kLyricsFontSize;
        const int activeSize = kLyricsFontSizeActive;
        qreal minScale = (qreal)baseSize / (qreal)activeSize;
        qreal scale = minScale + (1.0 - minScale) * blend;
        QFont font = opt.font;
        font.setPixelSize(activeSize);
        font.setBold(isActive || (isPrev && blend > 0.5));
        font.setFamily("-apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif");
        painter->setFont(font);
        auto lerp = [&](int a, int b) { return a + (b - a) * blend; };
        QColor baseColor(255, 255, 255, 255 * 0.38);
        QColor activeColor(255, 255, 255, 255 * 0.95);
        QColor color(lerp(baseColor.red(), activeColor.red()), lerp(baseColor.green(), activeColor.green()), lerp(baseColor.blue(), activeColor.blue()), lerp(baseColor.alpha(), activeColor.alpha()));
        painter->setPen(color);
        QRect contentRect = opt.rect;
        contentRect.setTop(contentRect.top() + getExtraTop(index.row()));
        contentRect.setBottom(contentRect.bottom() - getExtraBottom(index.row()));
        painter->translate(contentRect.topLeft() + QPoint(0, contentRect.height()/2));
        painter->scale(scale, scale);
        painter->translate(-(contentRect.topLeft() + QPoint(0, contentRect.height()/2)));
        if (m_view && m_view->viewport()) {
            int viewH = m_view->viewport()->height();
            int cy = contentRect.center().y();
            float dist = qAbs(cy - viewH / 2.0f);
            float maxDist = viewH / 2.0f;
            float normalizedDist = dist / maxDist;
            float alpha = 1.0f;
            if (normalizedDist > 0.35f) alpha = 1.0f - (normalizedDist - 0.35f) / 0.65f;
            alpha = alpha * alpha; 
            painter->setOpacity(qBound(0.0f, alpha, 1.0f));
        }
        QTextOption textOpt;
        textOpt.setWrapMode(QTextOption::WordWrap);
        textOpt.setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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
// FullscreenBackgroundGL
// ---------------------------------------------------------------------------

class FullscreenPlayer::FullscreenBackgroundGL
    : public QOpenGLWidget
    , protected QOpenGLFunctions
{
public:
    explicit FullscreenBackgroundGL(QWidget *parent = nullptr) : QOpenGLWidget(parent) {
        setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }
    ~FullscreenBackgroundGL() { makeCurrent(); doneCurrent(); }
    void setPalette(const QVector<QColor> &colors) { if (m_palette == colors) return; m_prevPalette = m_palette; m_palette = colors; m_progress = 0.0f; update(); }
    void setPaletteInstant(const QVector<QColor> &colors) { m_palette = colors; m_prevPalette = colors; m_progress = 1.0f; update(); }
    void setPaletteTransition(float progress) { m_progress = progress; update(); }
    void setTime(float t) { m_time = t; update(); }
    void setOpacity(float opacity) { m_opacity = opacity; update(); }
    float opacity() const { return m_opacity; }

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        static const char *kVert = "#version 330 core\nlayout(location = 0) in vec2 a_pos;\nout vec2 v_uv;\nvoid main() {\nv_uv = a_pos * 0.5 + 0.5;\ngl_Position = vec4(a_pos, 0.0, 1.0);\n}\n";
        static const char *kFrag = "#version 330 core\nin vec2 v_uv;\nout vec4 fragColor;\nuniform float u_time;\nuniform float u_opacity;\nuniform vec3 u_color0;\nuniform vec3 u_color1;\nuniform vec3 u_color2;\nvec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }\nvec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }\nvec3 permute(vec3 x) { return mod289(((x*34.0)+1.0)*x); }\nfloat snoise(vec2 v) {\nconst vec4 C = vec4(0.211324865405187, 0.366025403784439,-0.577350269189626, 0.024390243902439);\nvec2 i = floor(v + dot(v, C.yy));\nvec2 x0 = v - i + dot(i, C.xx);\nvec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);\nvec4 x12 = x0.xyxy + C.xxzz;\nx12.xy -= i1;\ni = mod289(i);\nvec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));\nvec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy), dot(x12.zw,x12.zw)), 0.0);\nm = m*m; m = m*m;\nvec3 x = 2.0 * fract(p * C.www) - 1.0;\nvec3 h = abs(x) - 0.5;\nvec3 ox = floor(x + 0.5);\nvec3 a0 = x - ox;\nm *= 1.79284291400159 - 0.85373472095314 * (a0*a0 + h*h);\nvec3 g;\ng.x = a0.x * x0.x + h.x * x0.y;\ng.yz = a0.yz * x12.xz + h.yz * x12.yw;\nreturn 130.0 * dot(m, g);\n}\nvoid main() {\nvec2 uv = v_uv;\nfloat t = u_time * 0.1;\nvec2 p = uv * 1.2;\nfloat n1 = snoise(p + vec2(t * 0.5, t * 0.3));\nfloat n2 = snoise(p * 1.5 + vec2(n1, n1) + vec2(-t * 0.4, t * 0.6));\nfloat n3 = snoise(p * 0.8 + vec2(n2, n1) + vec2(t * 0.3, -t * 0.2));\nfloat weight1 = smoothstep(-0.6, 0.6, n2);\nfloat weight2 = smoothstep(-0.5, 0.5, n3);\nvec3 liquid = mix(u_color0, u_color1, weight1);\nliquid = mix(liquid, u_color2, weight2);\nliquid += (n2 * 0.05);\nfragColor = vec4(liquid, 1.0);\n}\n";
        m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
        m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
        m_program.link();
        float verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        m_vbo.create();
        m_vbo.bind();
        m_vbo.allocate(verts, sizeof(verts));
    }
    void paintGL() override {
        glViewport(0, 0, width(), height());
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!m_program.isLinked()) return;
        m_program.bind();
        m_program.setUniformValue("u_time", m_time);
        m_program.setUniformValue("u_opacity", m_opacity);
        QVector<QColor> colors = m_palette;
        if (colors.size() < 3) colors = { QColor(180, 60, 80), QColor(60, 80, 180), QColor(80, 160, 120) };
        QVector<QColor> prevColors = m_prevPalette;
        if (prevColors.size() < 3) prevColors = colors;
        for (int i = 0; i < 3; ++i) {
            float r = prevColors[i].redF() + (colors[i].redF() - prevColors[i].redF()) * m_progress;
            float g = prevColors[i].greenF() + (colors[i].greenF() - prevColors[i].greenF()) * m_progress;
            float b = prevColors[i].blueF() + (colors[i].blueF() - prevColors[i].blueF()) * m_progress;
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
    float m_opacity = 1.0f;
};

// ---------------------------------------------------------------------------
// MarqueeLabel
// ---------------------------------------------------------------------------

MarqueeLabel::MarqueeLabel(QWidget *parent) : QWidget(parent) {
    m_anim = new QPropertyAnimation(this, "scrollOffset", this);
}
void MarqueeLabel::setText(const QString &text) {
    if (m_text == text) return;
    m_text = text; m_offset = 0; m_anim->stop();
    m_textW = fontMetrics().horizontalAdvance(m_text);
    restartScroll();
}
void MarqueeLabel::setTextStyle(const QFont &font, const QColor &color) {
    m_font = font; m_color = color;
    m_textW = QFontMetrics(m_font).horizontalAdvance(m_text);
    restartScroll();
}
QSize MarqueeLabel::sizeHint() const { return {200, QFontMetrics(m_font).height() + 4}; }
QSize MarqueeLabel::minimumSizeHint() const { return sizeHint(); }
void MarqueeLabel::restartScroll() {
    m_anim->stop(); m_offset = 0;
    if (m_textW <= width()) { update(); return; }
    int dist = m_textW + kGap;
    m_anim->setDuration(dist * 1000 / kSpeed);
    m_anim->setStartValue(0); m_anim->setEndValue(dist);
    m_anim->setLoopCount(-1); m_anim->start();
}
void MarqueeLabel::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); restartScroll(); }
void MarqueeLabel::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity); p.setFont(m_font); p.setPen(m_color);
    if (m_textW <= width()) p.drawText(rect(), Qt::AlignCenter, m_text);
    else {
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
    setMouseTracking(true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    hide();

    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(16);
    connect(m_animTimer, &QTimer::timeout, this, &FullscreenPlayer::animateTick);

    m_hideControlsTimer = new QTimer(this);
    m_hideControlsTimer->setSingleShot(true);
    connect(m_hideControlsTimer, &QTimer::timeout, this, &FullscreenPlayer::hideControls);

    m_bgWidget = new FullscreenBackgroundGL(this);
    m_bgWidget->setGeometry(rect());
    m_bgWidget->show(); 
    m_bgWidget->lower();

    m_rootLayout = new QWidget(this);
    m_rootLayout->setStyleSheet("background:transparent;");
    m_rootLayout->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    m_titleBar = new QWidget(m_rootLayout);
    m_titleBar->setFixedHeight(60);
    QHBoxLayout *tbl = new QHBoxLayout(m_titleBar);
    tbl->setContentsMargins(24, 0, 24, 0);

    auto makeTitleBtn = [&](const QString &svg) {
        auto *b = new QPushButton(m_titleBar);
        b->setFixedSize(48, 48);
        b->setStyleSheet("QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.75);font-size:26px;}QPushButton:hover{color:white;}");
        b->setCursor(Qt::PointingHandCursor);
        b->setText(svg);
        return b;
    };

    QPushButton *downBtn = makeTitleBtn(QString::fromUtf8("\xE2\x9C\x95")); 
    connect(downBtn, &QPushButton::clicked, this, &FullscreenPlayer::closeOverlay);
    tbl->addWidget(downBtn);
    tbl->addStretch();
    
    m_centerArea = new QWidget(m_rootLayout);
    QVBoxLayout *cal = new QVBoxLayout(m_centerArea);
    cal->setContentsMargins(0, 0, 0, 0);
    cal->setSpacing(24);
    cal->setAlignment(Qt::AlignCenter);

    m_coverLabel = new QLabel(m_centerArea);
    m_coverLabel->setFixedSize(380, 380);
    m_coverLabel->setAlignment(Qt::AlignCenter);
    cal->addWidget(m_coverLabel, 0, Qt::AlignCenter);

    QWidget *info = new QWidget(m_centerArea);
    QVBoxLayout *il = new QVBoxLayout(info);
    il->setContentsMargins(0, 0, 0, 0);
    il->setSpacing(4);
    il->setAlignment(Qt::AlignCenter);

    m_titleLabel = new MarqueeLabel(info);
    m_titleLabel->setFixedWidth(500);
    { QFont f("-apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif"); f.setPixelSize(18); f.setBold(true);
      m_titleLabel->setTextStyle(f, QColor(255, 255, 255)); }
    il->addWidget(m_titleLabel);

    m_artistLabel = new MarqueeLabel(info);
    m_artistLabel->setFixedWidth(500);
    { QFont f("-apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif"); f.setPixelSize(14);
      m_artistLabel->setTextStyle(f, QColor(255, 255, 255, 165)); }
    il->addWidget(m_artistLabel);
    cal->addWidget(info);

    m_lyricsHint = new QPushButton(m_rootLayout);
    m_lyricsHint->setFixedSize(44, 44);
    m_lyricsHint->setStyleSheet("QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.75);font-size:28px;}");
    m_lyricsHint->setText(QString::fromUtf8("\xE2\x98\xB0"));
    m_lyricsHint->setCursor(Qt::PointingHandCursor);
    m_lyricsHintOpacityEffect = new QGraphicsOpacityEffect(m_lyricsHint);
    m_lyricsHintOpacityEffect->setOpacity(0.0);
    m_lyricsHint->setGraphicsEffect(m_lyricsHintOpacityEffect);
    connect(m_lyricsHint, &QPushButton::clicked, this, &FullscreenPlayer::toggleLyrics);

    m_lyricsPanel = new QWidget(m_rootLayout);
    m_lyricsPanel->setFixedWidth(kLyricsPanelWidth);
    m_lyricsPanel->setStyleSheet("background:transparent;");
    QVBoxLayout *lyricsLayout = new QVBoxLayout(m_lyricsPanel);
    lyricsLayout->setContentsMargins(40, 60, 40, 60);

    m_lyricsList = new QListWidget(m_lyricsPanel);
    m_lyricsList->setWordWrap(true);
    m_lyricsList->setUniformItemSizes(false);
    m_lyricsList->setSpacing(18);
    m_lyricsList->setFocusPolicy(Qt::NoFocus);
    m_lyricsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_lyricsList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lyricsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lyricsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_lyricsList->viewport()->setCursor(Qt::PointingHandCursor);
    m_lyricsList->setStyleSheet("QListWidget{background:transparent;border:none;}");
    m_lyricsList->installEventFilter(this);
    m_lyricsList->viewport()->installEventFilter(this);

    m_lyricsDelegate = new LyricsItemDelegate(m_lyricsList);
    m_lyricsDelegate->setView(m_lyricsList);
    m_lyricsList->setItemDelegate(m_lyricsDelegate);
    lyricsLayout->addWidget(m_lyricsList);

    m_seekBarArea = new QWidget(m_rootLayout);
    m_seekBarArea->setStyleSheet("background:transparent;");
    QVBoxLayout *pl = new QVBoxLayout(m_seekBarArea);
    pl->setContentsMargins(24, 0, 24, 24);
    pl->setSpacing(12);
    QHBoxLayout *tl = new QHBoxLayout();
    m_currentTime = new QLabel("01:15", m_seekBarArea);
    m_currentTime->setStyleSheet("color:rgba(255,255,255,0.6);font-size:12px;font-family:monospace;");
    m_totalTime = new QLabel("02:15", m_seekBarArea);
    m_totalTime->setStyleSheet("color:rgba(255,255,255,0.6);font-size:12px;font-family:monospace;");
    tl->addWidget(m_currentTime); tl->addStretch(); tl->addWidget(m_totalTime);
    pl->addLayout(tl);
    m_seekSlider = new ClickableSlider(Qt::Horizontal, m_seekBarArea);
    m_seekSlider->setStyleSheet("QSlider::groove:horizontal{height:6px;background:rgba(255,255,255,0.25);border-radius:3px;}QSlider::sub-page:horizontal{background:rgba(255,255,255,0.9);border-radius:3px;}QSlider::handle:horizontal{width:0px;height:0px;}");
    m_seekSlider->setCursor(Qt::PointingHandCursor);
    pl->addWidget(m_seekSlider);

    m_playbackControls = new QWidget(m_rootLayout);
    m_playbackControlsOpacityEffect = new QGraphicsOpacityEffect(m_playbackControls);
    m_playbackControlsOpacityEffect->setOpacity(0.0);
    m_playbackControls->setGraphicsEffect(m_playbackControlsOpacityEffect);
    QHBoxLayout *bl = new QHBoxLayout(m_playbackControls);
    bl->setContentsMargins(24, 0, 24, 10);

    auto makeCtrlBtn = [&](const QString &svg, int sz, bool play = false) {
        auto *b = new QPushButton(m_playbackControls);
        b->setStyleSheet(QString("QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.85);font-size:%1px;}QPushButton:hover{color:white;}").arg(sz));
        b->setCursor(Qt::PointingHandCursor);
        b->setText(svg);
        if (play) b->setFixedSize(48, 48);
        return b;
    };

    m_shuffleBtn = makeCtrlBtn(QString::fromUtf8("\xE2\xBF\x80"), 20); 
    m_prevBtn    = makeCtrlBtn(QString::fromUtf8("\xE2\x8F\xAE"), 22);
    m_playBtn    = makeCtrlBtn(QString::fromUtf8("\xE2\x96\xB6"), 32, true);
    m_nextBtn    = makeCtrlBtn(QString::fromUtf8("\xE2\x8F\xAD"), 22);
    m_repeatBtn  = makeCtrlBtn(QString::fromUtf8("\xE2\xBF\x81"), 20); 
    
    QWidget *leftSpacer = new QWidget(m_playbackControls);
    leftSpacer->setFixedWidth(120);
    bl->addWidget(leftSpacer);

    bl->addStretch(1);
    QHBoxLayout *cntr = new QHBoxLayout();
    cntr->setSpacing(28);
    cntr->addWidget(m_shuffleBtn);
    cntr->addWidget(m_prevBtn);
    cntr->addWidget(m_playBtn);
    cntr->addWidget(m_nextBtn);
    cntr->addWidget(m_repeatBtn);
    bl->addLayout(cntr);
    bl->addStretch(1);

    QHBoxLayout *volRow = new QHBoxLayout();
    volRow->setContentsMargins(0, 0, 0, 0);
    volRow->setSpacing(10);
    m_muteBtn = makeCtrlBtn(QString::fromUtf8("\xF0\x9F\x94\x8A"), 16);
    m_volumeSlider = new ClickableSlider(Qt::Horizontal, m_playbackControls);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(80);
    m_volumeSlider->setStyleSheet("QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,0.25);border-radius:2px;}QSlider::sub-page:horizontal{background:rgba(255,255,255,0.85);border-radius:2px;}QSlider::handle:horizontal{width:0px;height:0px;}");
    volRow->addWidget(m_muteBtn);
    volRow->addWidget(m_volumeSlider);
    
    QWidget *rightBtns = new QWidget(m_playbackControls);
    QHBoxLayout *rbl = new QHBoxLayout(rightBtns);
    rbl->setContentsMargins(0, 0, 0, 0);
    rbl->addStretch();
    rbl->addLayout(volRow);
    rightBtns->setFixedWidth(120);
    bl->addWidget(rightBtns);

    m_centerOffsetAnim = new QPropertyAnimation(this, "centerOffset", this);
    m_centerOffsetAnim->setDuration(600); 
    m_centerOffsetAnim->setEasingCurve(QEasingCurve::OutExpo);

    m_playbackControlsOpacityAnim = new QPropertyAnimation(this, "controlsOpacity", this);
    m_playbackControlsOpacityAnim->setDuration(400);
    m_playbackControlsOpacityAnim->setEasingCurve(QEasingCurve::OutExpo);

    m_playbackControlsSlideAnim = new QPropertyAnimation(m_playbackControls, "pos", this);
    m_playbackControlsSlideAnim->setDuration(400);
    m_playbackControlsSlideAnim->setEasingCurve(QEasingCurve::OutExpo);

    m_lyricsSlideAnim = new QPropertyAnimation(this, "lyricsPanelX", this);
    m_lyricsSlideAnim->setDuration(600);
    m_lyricsSlideAnim->setEasingCurve(QEasingCurve::OutExpo);

    m_lyricsHintOpacityAnim = new QPropertyAnimation(m_lyricsHintOpacityEffect, "opacity", this);
    m_lyricsHintOpacityAnim->setDuration(400);
    m_lyricsHintOpacityAnim->setEasingCurve(QEasingCurve::OutExpo);

    m_lyricsHintSlideAnim = new QPropertyAnimation(m_lyricsHint, "pos", this);
    m_lyricsHintSlideAnim->setDuration(400);
    m_lyricsHintSlideAnim->setEasingCurve(QEasingCurve::OutExpo);
    
    m_centerAreaOpacityEffect = nullptr;
    m_titleBarOpacityEffect = nullptr;
    m_seekBarOpacityEffect = nullptr;
    m_rootOpacityEffect = nullptr;

    setMainUiOpacity(1.0);

    connect(m_shuffleBtn, &QPushButton::clicked, this, &FullscreenPlayer::shuffleToggleRequested);
    connect(m_prevBtn,    &QPushButton::clicked, this, &FullscreenPlayer::previousRequested);
    connect(m_playBtn,    &QPushButton::clicked, this, &FullscreenPlayer::playPauseRequested);
    connect(m_nextBtn,    &QPushButton::clicked, this, &FullscreenPlayer::nextRequested);
    connect(m_repeatBtn,  &QPushButton::clicked, this, &FullscreenPlayer::repeatToggleRequested);
    connect(m_muteBtn,    &QPushButton::clicked, this, &FullscreenPlayer::muteToggleRequested);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &FullscreenPlayer::volumeChangeRequested);
    connect(m_seekSlider, &QSlider::sliderPressed, this, [this]{ m_userSeeking = true; });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]{
        m_userSeeking = false;
        emit seekRequested(m_seekSlider->value());
    });
    
    m_paletteTransitionAnim = new QVariantAnimation(this);
    m_paletteTransitionAnim->setDuration(800);
    m_paletteTransitionAnim->setStartValue(0.0f);
    m_paletteTransitionAnim->setEndValue(1.0f);
    connect(m_paletteTransitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){ if (m_bgWidget) m_bgWidget->setPaletteTransition(v.toFloat()); });
    
    m_speedPulseAnim = new QVariantAnimation(this);
    m_speedPulseAnim->setDuration(800);
    m_speedPulseAnim->setStartValue(0.0f);
    m_speedPulseAnim->setEndValue(1.0f);
    m_speedPulseAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_speedPulseAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){ float pulse = std::sin(v.toFloat() * 3.14159f); m_animationSpeed = 1.0 + 4.0 * pulse; });
    
    m_lyricsHighlightAnim = new QVariantAnimation(this);
    m_lyricsHighlightAnim->setDuration(280);
    m_lyricsHighlightAnim->setStartValue(0.0f);
    m_lyricsHighlightAnim->setEndValue(1.0f);
    m_lyricsHighlightAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_lyricsHighlightAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){ if (m_lyricsDelegate) m_lyricsDelegate->setProgress(v.toReal()); });
}

FullscreenPlayer::~FullscreenPlayer() { if (m_animTimer) m_animTimer->stop(); }

bool FullscreenPlayer::eventFilter(QObject *w, QEvent *e) {
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            if (m_lyricsVisible) setLyricsVisible(false, true);
            else closeOverlay();
            return true;
        }
    }
    if (m_lyricsList && (w == m_lyricsList || w == m_lyricsList->viewport())) {
        if (e->type() == QEvent::Wheel || e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseMove) { 
            suspendLyricsAutoScroll(); 
        }
        // Закрытие при клике по тексту (если пользователь кликнул не по активной строке, а просто по области)
        if (e->type() == QEvent::MouseButtonPress && m_lyricsVisible) {
            QMouseEvent *me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton) {
                setLyricsVisible(false, true);
                return true;
            }
        }
    }
    return QWidget::eventFilter(w, e);
}

void FullscreenPlayer::keyPressEvent(QKeyEvent *e) { 
    if (e->key() == Qt::Key_Escape) {
        if (m_lyricsVisible) setLyricsVisible(false, true);
        else closeOverlay();
    } else {
        QWidget::keyPressEvent(e); 
    }
}

void FullscreenPlayer::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        int w = width();
        bool inRight = (w - e->pos().x()) <= w * 0.20;
        if (inRight) {
            toggleLyrics();
            return;
        }
    }
    QWidget::mousePressEvent(e);
}

void FullscreenPlayer::setMainUiOpacity(qreal v) {
    m_mainUiOpacity = v;
    if (m_rootOpacityEffect) m_rootOpacityEffect->setOpacity(v);
}

void FullscreenPlayer::openFor(const QPixmap &cover, const QString &title, const QString &artist, const QString &album, int durationMs, int positionMs, bool isPlaying, int volume) {
    m_rawCover = cover; m_durationMs = durationMs; m_trackTitle = title; m_trackArtist = artist; m_trackAlbum = album; m_trackDurationSec = durationToSeconds(durationMs);
    m_titleLabel->setText(title); m_artistLabel->setText(artist); m_seekSlider->setRange(0, durationMs); m_totalTime->setText(fmt(durationMs));
    updateCoverWidget(); updatePosition(positionMs); updatePlayState(isPlaying); updateVolume(volume);
    m_volumeSlider->blockSignals(true); m_volumeSlider->setValue(volume); m_volumeSlider->blockSignals(false);
    requestLyrics(); extractPalette(m_rawCover);
    if (m_bgWidget) { m_bgWidget->setPaletteInstant(m_palette); if (m_speedPulseAnim) { m_speedPulseAnim->stop(); m_speedPulseAnim->start(); } }
    
    setGeometry(parentWidget()->rect()); 
    raise(); show(); activateWindow(); setFocus(Qt::ActiveWindowFocusReason); grabKeyboard();
    
    m_lyricsVisible = false;
    m_lyricsPanelX = width();
    m_stateLifted = false;
    m_stateHinted = false;
    m_controlsOpacity = 0.0;
    m_centerOffset = QPointF(0, 50);

    m_rootLayout->show();
    updateLayout();
    
    if (m_openCloseAnim) {
        m_openCloseAnim->stop();
        m_openCloseAnim->deleteLater();
    }
    
    setMainUiOpacity(1.0);
    if (m_bgWidget) m_bgWidget->setOpacity(1.0f);
    
    m_openCloseAnim = new QParallelAnimationGroup(this);
    
    auto *oa = new QPropertyAnimation(this, "mainUiOpacity", m_openCloseAnim);
    oa->setStartValue(1.0); oa->setEndValue(1.0); oa->setDuration(350); oa->setEasingCurve(QEasingCurve::OutCubic);
    m_openCloseAnim->addAnimation(oa);

    auto *pa = new QPropertyAnimation(this, "centerOffset", m_openCloseAnim);
    pa->setStartValue(QPointF(0, 50)); pa->setEndValue(QPointF(0, 0)); pa->setDuration(450); pa->setEasingCurve(QEasingCurve::OutQuint);
    m_openCloseAnim->addAnimation(pa);

    m_openCloseAnim->start(); 
    m_animTimer->start(); 
    m_isOpen = true;
}

void FullscreenPlayer::closeOverlay() {
    if (!m_isOpen) return;
    m_isOpen = false; releaseKeyboard(); setLyricsVisible(false, false);
    
    if (m_openCloseAnim) {
        m_openCloseAnim->stop();
        m_openCloseAnim->deleteLater();
    }
    
    m_openCloseAnim = new QParallelAnimationGroup(this);
    
    auto *oa = new QPropertyAnimation(this, "mainUiOpacity", m_openCloseAnim);
    oa->setStartValue(m_mainUiOpacity); oa->setEndValue(0.0); oa->setDuration(250); oa->setEasingCurve(QEasingCurve::InQuad);
    m_openCloseAnim->addAnimation(oa);

    auto *pa = new QPropertyAnimation(this, "centerOffset", m_openCloseAnim);
    pa->setStartValue(m_centerOffset); pa->setEndValue(m_centerOffset + QPointF(0, 30)); pa->setDuration(250); pa->setEasingCurve(QEasingCurve::InQuad);
    m_openCloseAnim->addAnimation(pa);

    connect(m_openCloseAnim, &QParallelAnimationGroup::finished, this, [this] { hide(); clearFocus(); m_animTimer->stop(); });
    m_openCloseAnim->start();
}

void FullscreenPlayer::updateTrack(const QPixmap &cover, const QString &title, const QString &artist, const QString &album, int durationMs) {
    m_rawCover = cover; m_durationMs = durationMs; m_trackTitle = title; m_trackArtist = artist; m_trackAlbum = album; m_trackDurationSec = durationToSeconds(durationMs);
    m_titleLabel->setText(title); m_artistLabel->setText(artist); m_seekSlider->setRange(0, durationMs); m_totalTime->setText(fmt(durationMs));
    updateCoverWidget(); updateLayout();
    if (isVisible()) { 
        extractPalette(m_rawCover); 
        if (m_bgWidget) { 
            m_bgWidget->setPalette(m_palette); 
            if (m_paletteTransitionAnim) { m_paletteTransitionAnim->stop(); m_paletteTransitionAnim->start(); }
            if (m_speedPulseAnim) { m_speedPulseAnim->stop(); m_speedPulseAnim->start(); }
        } 
    }
    if (m_isOpen) requestLyrics();
}

void FullscreenPlayer::updatePosition(int ms) {
    if (m_userSeeking) { m_lastPositionMs = ms; m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch(); m_positionAnchorAudioMs = ms; return; }
    if (QDateTime::currentMSecsSinceEpoch() < m_seekIgnoreUntilMs) {
        const int delta = std::abs(ms - m_expectedSeekPositionMs);
        if (delta < 500) m_seekIgnoreUntilMs = 0; else if (delta > 5000) m_seekIgnoreUntilMs = 0; else return;
    }
    m_lastPositionMs = ms; m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch(); m_positionAnchorAudioMs = ms;
    m_seekSlider->blockSignals(true); m_seekSlider->setValue(ms); m_seekSlider->blockSignals(false);
    m_currentTime->setText(fmt(ms)); updateLyricsHighlight(ms);
}

void FullscreenPlayer::updatePlayState(bool playing) {
    m_playBtn->setText(playing ? QString::fromUtf8("\xE2\x8F\xB8") : QString::fromUtf8("\xE2\x96\xB6"));
    m_positionAnchorPlaying = playing; m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch(); m_positionAnchorAudioMs = m_lastPositionMs;
}

void FullscreenPlayer::updateVolume(int value) {
    m_volumeValue = qBound(0, value, 100); m_volumeSlider->blockSignals(true); m_volumeSlider->setValue(m_volumeValue); m_volumeSlider->blockSignals(false);
    if (m_volumeValue == 0) m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x87"));
    else if (m_volumeValue < 50) m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x89"));
    else m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x8A"));
}

void FullscreenPlayer::updateLikeState(bool liked) { Q_UNUSED(liked); }
void FullscreenPlayer::updateShuffleState(bool enabled, int mode) { Q_UNUSED(mode); m_shuffleBtn->setStyleSheet(enabled ? "QPushButton{background:transparent;border:none;color:white;font-size:20px;}" : "QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.85);font-size:20px;}QPushButton:hover{color:white;}"); }
void FullscreenPlayer::updateRepeatState(int mode) { m_repeatBtn->setStyleSheet(mode > 0 ? "QPushButton{background:transparent;border:none;color:white;font-size:20px;}" : "QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.85);font-size:20px;}QPushButton:hover{color:white;}"); }

bool FullscreenPlayer::hasLyrics() const { return m_lyricsSyncedAvailable || !m_lyricsPlainLines.isEmpty(); }
void FullscreenPlayer::toggleLyrics() { if (!hasLyrics()) return; setLyricsVisible(!m_lyricsVisible, true); }

void FullscreenPlayer::requestLyrics() {
    loadLyricsCache();
    if (m_lyricsReply) {
        QNetworkReply *reply = m_lyricsReply;
        m_lyricsReply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    m_lyricsRequestInFlight = false;
    const QString title = m_trackTitle.trimmed(); const QString artist = m_trackArtist.trimmed();
    if (title.isEmpty() || title == QStringLiteral("No track playing")) {
        m_lyricsSyncedAvailable = false; m_lyricsPlainLines.clear(); m_lyricsSyncedLines.clear(); m_lyricsSyncedTimes.clear(); m_lyricsCurrentIndex = -1;
        rebuildLyricsList(); updateLyricsButtonState(); return;
    }
    const QString key = normalizeLyricsKey(title) + "|" + normalizeLyricsKey(artist) + "|" + QString::number(m_trackDurationSec);
    if (key == m_lyricsKey && (m_lyricsRequestInFlight || hasLyrics())) return;
    m_lyricsKey = key;
    const auto cached = s_lyricsCache.constFind(key);
    if (cached != s_lyricsCache.constEnd()) {
        if (cached->found) applyLyrics(cached->synced, cached->plain, cached->instrumental);
        else applyLyrics(QString(), QString(), false);
        return;
    }
    m_lyricsSyncedAvailable = false; m_lyricsPlainLines.clear(); m_lyricsSyncedLines.clear(); m_lyricsSyncedTimes.clear(); m_lyricsCurrentIndex = -1;
    m_lyricsRequestInFlight = true; rebuildLyricsList(); updateLyricsButtonState();
    if (!m_lyricsNet) m_lyricsNet = new QNetworkAccessManager(this);
    QUrl url(QStringLiteral("https://lrclib.net/api/search")); QUrlQuery query; query.addQueryItem(QStringLiteral("track_name"), title);
    if (!artist.isEmpty()) query.addQueryItem(QStringLiteral("artist_name"), artist);
    url.setQuery(query); sendLyricsRequest(url);
}

void FullscreenPlayer::sendLyricsRequest(const QUrl &url) {
    if (m_lyricsReply) { m_lyricsReply->abort(); m_lyricsReply->deleteLater(); m_lyricsReply = nullptr; }
    QNetworkRequest req(url); req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("MusicPlayer")); req.setTransferTimeout(8000);
    m_lyricsRequestInFlight = true; rebuildLyricsList(); m_lyricsReply = m_lyricsNet->get(req);
    const int token = ++m_lyricsRequestToken; m_lyricsReply->setProperty("lyricsToken", token);
    connect(m_lyricsReply, &QNetworkReply::finished, this, &FullscreenPlayer::onLyricsReplyFinished);
}

void FullscreenPlayer::onLyricsReplyFinished() {
    auto *reply = qobject_cast<QNetworkReply *>(sender()); 
    if (!reply) return;
    
    const int token = reply->property("lyricsToken").toInt(); 
    if (token != m_lyricsRequestToken) { 
        reply->deleteLater(); 
        return; 
    }
    
    const QNetworkReply::NetworkError netError = reply->error(); 
    const bool replyOpen = reply->isOpen();
    
    if (m_lyricsReply == reply) {
        m_lyricsReply = nullptr; 
    }
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
        if (obj.isEmpty()) continue;
        
        const QString gotTitle = normalizeLyricsKey(obj.value(QStringLiteral("trackName")).toString());
        const QString gotArtist = normalizeLyricsKey(obj.value(QStringLiteral("artistName")).toString());
        const int duration = obj.value(QStringLiteral("duration")).toInt();
        const int diff = (m_trackDurationSec > 0 && duration > 0) ? qAbs(duration - m_trackDurationSec) : std::numeric_limits<int>::max();
        int score = 0; 
        if (!wantTitle.isEmpty() && gotTitle == wantTitle) score += 6; 
        if (!wantArtist.isEmpty() && gotArtist == wantArtist) score += 5;
        if (diff <= 2) score += 3; 
        else if (diff <= 5) score += 1;
        
        const QString synced = obj.value(QStringLiteral("syncedLyrics")).toString().trimmed();
        const QString plain = obj.value(QStringLiteral("plainLyrics")).toString().trimmed();
        if (!synced.isEmpty()) score += 2; 
        else if (!plain.isEmpty()) score += 1;
        
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

void FullscreenPlayer::applyLyrics(const QString &synced, const QString &plain, bool instrumental) {
    (void)instrumental; 
    m_lyricsSyncedAvailable = false; 
    m_lyricsPlainLines.clear(); 
    m_lyricsSyncedLines.clear(); 
    m_lyricsSyncedTimes.clear(); 
    m_lyricsCurrentIndex = -1;
    
    const QString syncedTrim = synced.trimmed(); 
    const QString plainTrim = plain.trimmed();
    
    if (!syncedTrim.isEmpty()) parseSyncedLyrics(syncedTrim);
    if (!m_lyricsSyncedAvailable && !plainTrim.isEmpty()) parsePlainLyrics(plainTrim);
    
    rebuildLyricsList(); 
    updateLyricsButtonState(); 
    updateLyricsHighlight(m_lastPositionMs);
}

void FullscreenPlayer::parseSyncedLyrics(const QString &text) {
    struct Entry { int timeMs; QString line; }; QVector<Entry> entries;
    QRegularExpression tag(QStringLiteral("\\[(\\d{1,2}):(\\d{2})(?:\\.(\\d{1,3}))?\\]")); const QStringList rawLines = text.split('\n');
    for (const QString &raw : rawLines) {
        QRegularExpressionMatchIterator it = tag.globalMatch(raw); if (!it.hasNext()) continue;
        QString line = raw; line.remove(tag); line = line.trimmed(); if (line.isEmpty()) continue;
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int minutes = m.captured(1).toInt(); const int seconds = m.captured(2).toInt(); const QString fracText = m.captured(3);
            int fracMs = 0; if (!fracText.isEmpty()) { int frac = fracText.toInt(); if (fracText.size() == 1) fracMs = frac * 100; else if (fracText.size() == 2) fracMs = frac * 10; else fracMs = frac; }
            const int timeMs = minutes * 60000 + seconds * 1000 + fracMs; entries.push_back({ timeMs, line });
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.timeMs < b.timeMs; });
    m_lyricsSyncedLines.clear(); m_lyricsSyncedTimes.clear();
    for (const Entry &e : entries) { m_lyricsSyncedTimes.append(e.timeMs); m_lyricsSyncedLines.append(e.line); }
    m_lyricsSyncedAvailable = !m_lyricsSyncedLines.isEmpty();
}

void FullscreenPlayer::parsePlainLyrics(const QString &text) {
    QStringList lines = text.split('\n'); while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst(); while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    m_lyricsPlainLines = lines;
}

void FullscreenPlayer::updateLyricsButtonState() { const bool enabled = hasLyrics(); if (!m_textBtn) return; m_textBtn->setEnabled(enabled); }

void FullscreenPlayer::updateLyricsHighlight(int ms) {
    if (!m_lyricsSyncedAvailable || m_lyricsSyncedTimes.isEmpty() || !m_lyricsList) return;
    auto it = std::upper_bound(m_lyricsSyncedTimes.begin(), m_lyricsSyncedTimes.end(), ms);
    const int idx = it == m_lyricsSyncedTimes.begin() ? -1 : static_cast<int>(it - m_lyricsSyncedTimes.begin()) - 1;
    if (QDateTime::currentMSecsSinceEpoch() < m_seekIgnoreUntilMs) { if (idx != m_lyricsCurrentIndex) return; else m_seekIgnoreUntilMs = 0; }
    if (idx == m_lyricsCurrentIndex) return;
    const int prevIndex = m_lyricsCurrentIndex; m_lyricsCurrentIndex = idx;
    startLyricsHighlightAnimation(prevIndex, m_lyricsCurrentIndex);
    if (lyricsAutoScrollSuspended()) { m_lyricsAutoScrollSuppressed = true; return; }
    if (idx < 0) { if (auto *bar = m_lyricsList->verticalScrollBar()) { if (m_lyricsScrollAnim) m_lyricsScrollAnim->stop(); bar->setValue(bar->minimum()); } return; }
    const int jump = std::abs(idx - prevIndex); const bool isBigJump = (prevIndex < 0) || (jump > 3);
    animateLyricsScrollTo(m_lyricsCurrentIndex, false, isBigJump);
}

void FullscreenPlayer::setLyricsVisible(bool visible, bool animate) {
    if (visible == m_lyricsVisible) return;
    m_lyricsVisible = visible;
    updateState();

    m_lyricsSlideAnim->stop();
    m_lyricsSlideAnim->setStartValue(m_lyricsPanelX);
    m_lyricsSlideAnim->setEndValue(visible ? (qreal)(width() - kLyricsPanelWidth) : (qreal)width());
    if (animate) m_lyricsSlideAnim->start(); else setLyricsPanelX(m_lyricsSlideAnim->endValue().toReal());

    // Hide hint icon when lyrics are visible
    if (m_lyricsHintOpacityEffect) {
        m_lyricsHintOpacityEffect->setOpacity(visible ? 0.0 : (m_stateHinted ? 1.0 : 0.0));
    }

    if (visible && m_lyricsSyncedAvailable) { updateLyricsHighlight(m_lastPositionMs); animateLyricsScrollTo(m_lyricsCurrentIndex, true, true); }
}

void FullscreenPlayer::rebuildLyricsList() {
    if (!m_lyricsList) return; 
    m_lyricsList->clear();
    
    const QStringList lines = m_lyricsSyncedAvailable ? m_lyricsSyncedLines : m_lyricsPlainLines;
    if (lines.isEmpty()) { 
        m_lyricsList->setCurrentRow(-1); 
        return; 
    }
    
    for (const QString &line : lines) {
        const QString shown = line.isEmpty() ? QStringLiteral(" ") : line;
        auto *item = new QListWidgetItem(shown, m_lyricsList); 
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    m_lyricsCurrentIndex = -1; 
    m_lyricsList->setCurrentRow(-1);
    
    if (m_lyricsDelegate) { 
        m_lyricsDelegate->setIndices(-1, -1); 
        m_lyricsDelegate->setProgress(1.0); 
    }
    m_lyricsList->doItemsLayout();
}

void FullscreenPlayer::animateLyricsScrollTo(int logicalIndex, bool force, bool instant) {
    if (!m_lyricsVisible || !m_lyricsList) return; 
    
    if (force) {
        m_lyricsHoldUntilMs = 0; 
    } else if (lyricsAutoScrollSuspended()) {
        return;
    }
    
    const int target = lyricsScrollTargetForIndex(logicalIndex); 
    if (target < 0) return;
    
    auto *bar = m_lyricsList->verticalScrollBar();
    if (instant) { 
        if (m_lyricsScrollAnim) m_lyricsScrollAnim->stop(); 
        bar->setValue(target); 
        return; 
    }
    
    int currentVal = bar->value(); 
    if (target == currentVal) return;
    
    if (!m_lyricsScrollAnim) m_lyricsScrollAnim = new QPropertyAnimation(bar, "value", this);
    m_lyricsScrollAnim->stop(); 
    m_lyricsScrollAnim->setStartValue(currentVal); 
    m_lyricsScrollAnim->setEndValue(target); 
    m_lyricsScrollAnim->setDuration(380); 
    m_lyricsScrollAnim->setEasingCurve(QEasingCurve::OutCubic); 
    m_lyricsScrollAnim->start();
}

void FullscreenPlayer::startLyricsHighlightAnimation(int prevIndex, int nextIndex) {
    if (!m_lyricsList || !m_lyricsDelegate) return; 
    
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

int FullscreenPlayer::lyricsScrollTargetForIndex(int index) const {
    if (!m_lyricsList || index < 0 || index >= m_lyricsList->count()) return -1;
    QListWidgetItem *item = m_lyricsList->item(index); if (!item) return -1;
    auto *bar = m_lyricsList->verticalScrollBar(); int currentBarVal = bar ? bar->value() : 0;
    QRect rect = m_lyricsList->visualItemRect(item);
    if (!rect.isValid() && bar) { const bool wasBlocked = bar->blockSignals(true); m_lyricsList->scrollToItem(item, QAbstractItemView::PositionAtCenter); rect = m_lyricsList->visualItemRect(item); bar->setValue(currentBarVal); bar->blockSignals(wasBlocked); }
    if (!rect.isValid()) return -1;
    int extraTop = (index == 0) ? qMax(0, m_lyricsList->viewport()->height() / 2 - 20) : 0;
    int extraBottom = (index == m_lyricsList->count() - 1) ? qMax(0, m_lyricsList->viewport()->height() / 2 - 20) : 0;
    QRect contentRect = rect; contentRect.setTop(contentRect.top() + extraTop); contentRect.setBottom(contentRect.bottom() - extraBottom);
    int absoluteCenterY = (bar ? bar->value() : 0) + contentRect.center().y(); int target = absoluteCenterY - m_lyricsList->viewport()->height() / 2;
    return bar ? qBound(bar->minimum(), target, bar->maximum()) : target;
}

void FullscreenPlayer::suspendLyricsAutoScroll() { m_lyricsHoldUntilMs = QDateTime::currentMSecsSinceEpoch() + 700; m_lyricsAutoScrollSuppressed = true; }
bool FullscreenPlayer::lyricsAutoScrollSuspended() const { return QDateTime::currentMSecsSinceEpoch() < m_lyricsHoldUntilMs; }
void FullscreenPlayer::maybeResumeLyricsAutoScroll() { if (!m_lyricsAutoScrollSuppressed || !m_lyricsVisible || !m_lyricsSyncedAvailable || lyricsAutoScrollSuspended() || m_lyricsCurrentIndex < 0) return; m_lyricsAutoScrollSuppressed = false; animateLyricsScrollTo(m_lyricsCurrentIndex); }

void FullscreenPlayer::extractPalette(const QPixmap &albumArt) {
    m_palette.clear(); 
    if (albumArt.isNull()) { 
        m_palette = { {180, 60, 80}, {60, 80, 180}, {80, 160, 120} }; 
        return; 
    }
    
    QImage img = albumArt.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).toImage().convertToFormat(QImage::Format_ARGB32);
    struct Bin { int count = 0; int rSum = 0; int gSum = 0; int bSum = 0; }; 
    QVector<Bin> bins(1 << 15);
    
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]); 
            if (c.alpha() < 16) continue;
            
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
    
    struct BinColor { QColor color; int count = 0; }; 
    QVector<BinColor> colors;
    
    for (const Bin &bin : bins) { 
        if (bin.count <= 0) continue; 
        colors.append({ QColor(bin.rSum / bin.count, bin.gSum / bin.count, bin.bSum / bin.count), bin.count }); 
    }
    
    if (colors.isEmpty()) { 
        m_palette = { {180, 60, 80}, {60, 80, 180}, {80, 160, 120} }; 
        return; 
    }
    
    std::sort(colors.begin(), colors.end(), [](const BinColor &a, const BinColor &b) { return a.count > b.count; });
    QVector<QColor> finalPalette;
    
    for (const auto &bc : colors) {
        bool tooSimilar = false; 
        for (const QColor &existing : finalPalette) { 
            int dr = bc.color.red() - existing.red(); 
            int dg = bc.color.green() - existing.green(); 
            int db = bc.color.blue() - existing.blue(); 
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
        if (finalPalette.isEmpty()) {
            finalPalette.append(QColor(30, 30, 30)); 
        } else {
            finalPalette.append(finalPalette.last()); 
        }
    }
    m_palette = finalPalette;
}

void FullscreenPlayer::paintEvent(QPaintEvent *) { 
    QPainter painter(this); 
    painter.setRenderHint(QPainter::SmoothPixmapTransform); 
    
    // SINGLE GLOBAL BOTTOM SHADOW
    QLinearGradient grad(0, height(), 0, height() - 150);
    grad.setColorAt(0, QColor(0,0,0,140));
    grad.setColorAt(1, Qt::transparent);
    painter.fillRect(0, height()-150, width(), 150, grad);
}

void FullscreenPlayer::resizeEvent(QResizeEvent *e) { 
    QWidget::resizeEvent(e); 
    if (m_bgWidget) m_bgWidget->setGeometry(rect()); 
    if (m_isOpen) updateLayout(); 
}

void FullscreenPlayer::updateBassLevel(float level) { if (level > m_lastLevel) m_lastLevel = m_lastLevel * 0.2f + level * 0.8f; else m_lastLevel = m_lastLevel * 0.92f + level * 0.08f; }

void FullscreenPlayer::setControlsOpacity(qreal v) { 
    m_controlsOpacity = v; 
    if (m_playbackControlsOpacityEffect) m_playbackControlsOpacityEffect->setOpacity(v); 
    m_playbackControls->setEnabled(v > 0.1); 
}

void FullscreenPlayer::updateLayout() {
    m_rootLayout->setGeometry(rect());
    int w = width(), h = height();
    m_titleBar->setGeometry(0, 0, w, 60);
    m_centerArea->adjustSize();
    
    // Текст ближе к центру: tx анимируется через m_centerOffset.x()
    double tx = m_centerOffset.x();
    QPoint centerPos((w - m_centerArea->width())/2, (h - m_centerArea->height())/2);
    centerPos += QPoint(tx, m_centerOffset.y());
    m_centerArea->move(centerPos);
    
    if (m_lyricsHintSlideAnim && m_lyricsHintSlideAnim->state() == QAbstractAnimation::Stopped) {
        int hintX = w - m_lyricsHint->width() - 24;
        if (!m_stateHinted) hintX += 20;
        m_lyricsHint->move(hintX, (h - m_lyricsHint->height())/2);
    }
    
    if (m_lyricsSlideAnim && m_lyricsSlideAnim->state() == QAbstractAnimation::Stopped) {
        m_lyricsPanel->setGeometry(m_lyricsVisible ? (w - kLyricsPanelWidth) : w, 0, kLyricsPanelWidth, h);
    } else {
        m_lyricsPanel->setGeometry(m_lyricsPanelX, 0, kLyricsPanelWidth, h);
    }

    int pcH = m_playbackControls->sizeHint().height();
    int sbH = m_seekBarArea->sizeHint().height();
    m_seekBarArea->setGeometry(0, h - sbH, w, sbH);
    
    if (m_playbackControlsSlideAnim && m_playbackControlsSlideAnim->state() == QAbstractAnimation::Stopped) {
        int baseY = h - sbH - pcH;
        m_playbackControls->setGeometry(0, m_stateLifted ? baseY : (baseY + 40), w, pcH);
    }
}

void FullscreenPlayer::showControls() {
    m_hideControlsTimer->stop();
    if (!m_stateLifted) {
        m_stateLifted = true;
        m_playbackControlsOpacityAnim->stop();
        m_playbackControlsOpacityAnim->setStartValue(m_controlsOpacity);
        m_playbackControlsOpacityAnim->setEndValue(1.0);
        m_playbackControlsOpacityAnim->start();

        int pcH = m_playbackControls->sizeHint().height();
        int sbH = m_seekBarArea->sizeHint().height();
        int baseY = height() - sbH - pcH;

        m_playbackControlsSlideAnim->stop();
        m_playbackControlsSlideAnim->setStartValue(m_playbackControls->pos());
        m_playbackControlsSlideAnim->setEndValue(QPoint(0, baseY));
        m_playbackControlsSlideAnim->start();

        updateState();
    }
}

void FullscreenPlayer::hideControls() {
    if (m_stateLifted) {
        m_stateLifted = false;
        m_playbackControlsOpacityAnim->stop();
        m_playbackControlsOpacityAnim->setStartValue(m_controlsOpacity);
        m_playbackControlsOpacityAnim->setEndValue(0.0);
        m_playbackControlsOpacityAnim->start();

        int pcH = m_playbackControls->sizeHint().height();
        int sbH = m_seekBarArea->sizeHint().height();
        int baseY = height() - sbH - pcH;

        m_playbackControlsSlideAnim->stop();
        m_playbackControlsSlideAnim->setStartValue(m_playbackControls->pos());
        m_playbackControlsSlideAnim->setEndValue(QPoint(0, baseY + 40));
        m_playbackControlsSlideAnim->start();

        updateState();
    }
}

void FullscreenPlayer::updateState() {
    // Уменьшил сдвиг влево (-140 вместо -220), чтобы текст был ближе к центру
    double tx = m_lyricsVisible ? -140 : (m_stateHinted ? -50 : 0);
    double ty = m_stateLifted ? -28 : 0;
    m_centerOffsetAnim->stop();
    m_centerOffsetAnim->setStartValue(m_centerOffset);
    m_centerOffsetAnim->setEndValue(QPointF(tx, ty));
    m_centerOffsetAnim->start();
}

void FullscreenPlayer::mouseMoveEvent(QMouseEvent *e) { QWidget::mouseMoveEvent(e); }
void FullscreenPlayer::leaveEvent(QEvent *e) { QWidget::leaveEvent(e); }

void FullscreenPlayer::updateCoverWidget() {
    if (m_rawCover.isNull()) { m_coverLabel->clear(); m_coverLabel->setFixedSize(380, 380); return; }
    QPixmap px = m_rawCover.scaled(380, 380, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_coverLabel->setPixmap(rounded(px, 10)); m_coverLabel->setFixedSize(px.size());
}

void FullscreenPlayer::animateTick() {
    m_phase += 0.015f * m_animationSpeed; if (m_bgWidget) m_bgWidget->setTime(m_phase); maybeResumeLyricsAutoScroll();
    if (m_positionAnchorPlaying && m_lyricsSyncedAvailable && m_lyricsVisible && QDateTime::currentMSecsSinceEpoch() >= m_seekIgnoreUntilMs && !m_userSeeking) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_positionAnchorWallMs;
        const qint64 capped = elapsed < qint64(0) ? qint64(0) : elapsed > qint64(500) ? qint64(500) : elapsed;
        updateLyricsHighlight(m_positionAnchorAudioMs + static_cast<int>(capped));
    }

    if (isVisible()) {
        QPoint pos = mapFromGlobal(QCursor::pos());
        int w = width(), h = height();
        bool isMouseInside = (pos.x() >= -2 && pos.x() <= w + 2 && pos.y() >= -2 && pos.y() <= h + 2);
        
        if (isMouseInside && !m_userSeeking) {
            bool inBottom = (h - pos.y()) <= h * 0.20;
            bool inRight = (w - pos.x()) <= w * 0.20;

            if (inBottom) {
                showControls();
            } else if (m_stateLifted && !m_hideControlsTimer->isActive()) {
                m_hideControlsTimer->start(400);
            }

            if (!m_lyricsVisible) {
                if (inRight != m_stateHinted) {
                    m_stateHinted = inRight;
                    
                    m_lyricsHintOpacityAnim->stop();
                    m_lyricsHintOpacityAnim->setStartValue(m_lyricsHintOpacityEffect->opacity());
                    m_lyricsHintOpacityAnim->setEndValue(m_stateHinted ? 1.0 : 0.0);
                    m_lyricsHintOpacityAnim->start();
                    
                    m_lyricsHintSlideAnim->stop();
                    m_lyricsHintSlideAnim->setStartValue(m_lyricsHint->pos());
                    int targetX = w - m_lyricsHint->width() - 24;
                    if (!m_stateHinted) targetX += 20;
                    m_lyricsHintSlideAnim->setEndValue(QPoint(targetX, (h - m_lyricsHint->height()) / 2));
                    m_lyricsHintSlideAnim->start();
                    
                    updateState();
                }
            }
        } else if (!isMouseInside && !m_userSeeking) {
            if (m_stateLifted && !m_hideControlsTimer->isActive()) m_hideControlsTimer->start(200);
            if (m_stateHinted && !m_lyricsVisible) {
                m_stateHinted = false;
                
                m_lyricsHintOpacityAnim->stop();
                m_lyricsHintOpacityAnim->setStartValue(m_lyricsHintOpacityEffect->opacity());
                m_lyricsHintOpacityAnim->setEndValue(0.0);
                m_lyricsHintOpacityAnim->start();
                
                m_lyricsHintSlideAnim->stop();
                m_lyricsHintSlideAnim->setStartValue(m_lyricsHint->pos());
                int targetX = width() - m_lyricsHint->width() - 24 + 20;
                m_lyricsHintSlideAnim->setEndValue(QPoint(targetX, (height() - m_lyricsHint->height()) / 2));
                m_lyricsHintSlideAnim->start();
                
                updateState();
            }
        }
    }
}
