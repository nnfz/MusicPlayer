#include "FullscreenPlayer.h"

#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QEasingCurve>
#include <QSizePolicy>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
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
#include <QSurfaceFormat>
#include <algorithm>
#include <limits>
#include <cmath>
#include <QDateTime>
#include <QDebug>
#include <QApplication>

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

static QString normalizeLyricsKey(const QString &text) { return text.simplified().toLower(); }

static int durationToSeconds(int ms)
{
    if (ms <= 0) return 0;
    return qMax(1, static_cast<int>((ms / 1000.0) + 0.5));
}

static constexpr int kCardWidth             = 420;
static constexpr int kLyricsPanelWidth      = 600;
static constexpr int kLyricsFontSize        = 20;
static constexpr int kLyricsFontSizeActive  = 23;

struct LyricsCacheEntry {
    QString synced;
    QString plain;
    bool instrumental = false;
    bool found = false;
};

QHash<QString, LyricsCacheEntry> s_lyricsCache;
bool s_lyricsCacheLoaded = false;
static constexpr int kLyricsCacheMaxEntries = 800;
static constexpr int kLyricsCacheVersion    = 1;

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
        entry.synced       = obj.value(QStringLiteral("synced")).toString();
        entry.plain        = obj.value(QStringLiteral("plain")).toString();
        entry.instrumental = obj.value(QStringLiteral("instrumental")).toBool(false);
        entry.found        = obj.value(QStringLiteral("found")).toBool(false);
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
        obj.insert(QStringLiteral("synced"),        it->synced);
        obj.insert(QStringLiteral("plain"),         it->plain);
        obj.insert(QStringLiteral("instrumental"),  it->instrumental);
        obj.insert(QStringLiteral("found"),         it->found);
        items.insert(it.key(), obj);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kLyricsCacheVersion);
    root.insert(QStringLiteral("items"),   items);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

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
        if (width <= 0) width = kLyricsPanelWidth - 80; // ← было 300, поставь реальную ширину
        QRect bounds = fm.boundingRect(QRect(0, 0, width, 10000), Qt::TextWordWrap | Qt::AlignLeft, opt.text);
        return QSize(width, bounds.height() + 24 + getExtraTop(index.row()) + getExtraBottom(index.row()));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing);

        const bool isActive = index.row() == m_activeIndex;
        const bool isPrev   = index.row() == m_prevIndex;
        const qreal t = qBound(0.0, m_progress, 1.0);

        // Spring-like easing with subtle overshoot — iOS/Apple Music feel
        const qreal eased = (t >= 1.0)
            ? 1.0
            : 1.0 - std::pow(1.0 - t, 3.0) * std::cos(t * 7.5);

        qreal blendRaw = 0.0;
        if (isActive) blendRaw = eased;
        else if (isPrev) blendRaw = 1.0 - eased;

        // Keep color/weight stable during the first frame of a line switch.
        qreal blend = qBound(0.0, blendRaw, 1.0);
        if (isActive) blend = qMax(0.12, blend);

        // Scale: inactive rows are slightly smaller, active one "pops" forward
        const qreal minScale = 0.92;
        const qreal scaleBlend = qBound(0.0, blendRaw, 1.15);
        const qreal scale = minScale + (1.0 - minScale) * scaleBlend;

        QFont font = opt.font;
        font.setPixelSize(kLyricsFontSizeActive);
        const int weightInt = QFont::Normal + (int)((QFont::DemiBold - QFont::Normal) * blend);
        const int clamped = qBound((int)QFont::Normal, weightInt, (int)QFont::DemiBold);
        font.setWeight(static_cast<QFont::Weight>(clamped));
        font.setFamily("-apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif");
        painter->setFont(font);

        auto lerp = [&](int a, int b) { return a + (int)((b - a) * blend); };
        QColor baseColor(255, 255, 255, (int)(255 * 0.35));
        QColor activeColor(255, 255, 255, (int)(255 * 0.95));
        painter->setPen(QColor(lerp(baseColor.red(),   activeColor.red()),
                               lerp(baseColor.green(), activeColor.green()),
                               lerp(baseColor.blue(),  activeColor.blue()),
                               lerp(baseColor.alpha(), activeColor.alpha())));

        QRect contentRect = opt.rect;
        contentRect.setTop(contentRect.top()       + getExtraTop(index.row()));
        contentRect.setBottom(contentRect.bottom() - getExtraBottom(index.row()));

        painter->translate(contentRect.topLeft() + QPoint(0, contentRect.height()/2));
        painter->scale(scale, scale);
        painter->translate(-(contentRect.topLeft() + QPoint(0, contentRect.height()/2)));

        // Cubic fade by distance from center — sharp near center, fades to near-invisible at edges
        if (m_view && m_view->viewport()) {
            int viewH  = m_view->viewport()->height();
            int cy     = contentRect.center().y();
            float dist = qAbs(cy - viewH * 0.5f) / (viewH * 0.5f);
            float fade = 1.0f - qBound(0.0f, (dist - 0.25f) / 0.6f, 1.0f);
            fade       = fade * fade * fade;
            const float minFade = isActive ? 0.2f : 0.05f;
            painter->setOpacity(qBound(minFade, fade, 1.0f));
        }

        QTextOption textOpt;
        textOpt.setWrapMode(QTextOption::WordWrap);
        textOpt.setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        painter->drawText(contentRect, opt.text, textOpt);
        painter->restore();
    }

private:
    QListWidget *m_view        { nullptr };
    int          m_activeIndex { -1 };
    int          m_prevIndex   { -1 };
    qreal        m_progress    { 1.0 };
};

class FullscreenPlayer::FullscreenBackgroundGL
    : public QOpenGLWidget
    , protected QOpenGLFunctions
{
public:
    explicit FullscreenBackgroundGL(QWidget *parent = nullptr) : QOpenGLWidget(parent) {
        QSurfaceFormat fmt;
        fmt.setSwapInterval(1);
        setFormat(fmt);
        setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }
    ~FullscreenBackgroundGL() { makeCurrent(); doneCurrent(); }

    QVector<QColor> palette() const { return m_palette; }

    void setPalette(const QVector<QColor> &colors) {
        if (m_palette == colors) return;
        if (m_progress > 0.0f && m_progress < 1.0f && m_prevPalette.size() >= 3 && m_palette.size() >= 3) {
            QVector<QColor> cur;
            for (int i = 0; i < 3; ++i) {
                float r = m_prevPalette[i].redF()   + (m_palette[i].redF()   - m_prevPalette[i].redF())   * m_progress;
                float g = m_prevPalette[i].greenF() + (m_palette[i].greenF() - m_prevPalette[i].greenF()) * m_progress;
                float b = m_prevPalette[i].blueF()  + (m_palette[i].blueF()  - m_prevPalette[i].blueF())  * m_progress;
                cur.append(QColor::fromRgbF(r, g, b));
            }
            m_prevPalette = cur;
        } else {
            m_prevPalette = m_palette;
        }
        m_palette  = colors;
        m_progress = 0.0f;
        update();
    }

    void setPaletteInstant(const QVector<QColor> &colors) {
        m_palette = colors; m_prevPalette = colors; m_progress = 1.0f; update();
    }
    void setPaletteTransition(float p) { m_progress = p; update(); }
    void setTime(float t)              { m_time = t; update(); }
    void setOpacity(float o)           { m_opacity = o; update(); }
    float opacity() const              { return m_opacity; }

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        static const char *kVert =
            "#version 330 core\n"
            "layout(location=0) in vec2 a_pos;\n"
            "out vec2 v_uv;\n"
            "void main(){\n"
            "v_uv=a_pos*0.5+0.5;\n"
            "gl_Position=vec4(a_pos,0.0,1.0);\n"
            "}\n";
        static const char *kFrag =
            "#version 330 core\n"
            "in vec2 v_uv;\nout vec4 fragColor;\n"
            "uniform float u_time;\nuniform float u_opacity;\n"
            "uniform vec3 u_color0;\nuniform vec3 u_color1;\nuniform vec3 u_color2;\n"
            "vec3 mod289(vec3 x){return x-floor(x*(1.0/289.0))*289.0;}\n"
            "vec2 mod289(vec2 x){return x-floor(x*(1.0/289.0))*289.0;}\n"
            "vec3 permute(vec3 x){return mod289(((x*34.0)+1.0)*x);}\n"
            "float snoise(vec2 v){\n"
            "const vec4 C=vec4(0.211324865405187,0.366025403784439,-0.577350269189626,0.024390243902439);\n"
            "vec2 i=floor(v+dot(v,C.yy));\nvec2 x0=v-i+dot(i,C.xx);\n"
            "vec2 i1=(x0.x>x0.y)?vec2(1.0,0.0):vec2(0.0,1.0);\n"
            "vec4 x12=x0.xyxy+C.xxzz;\nx12.xy-=i1;\ni=mod289(i);\n"
            "vec3 p=permute(permute(i.y+vec3(0.0,i1.y,1.0))+i.x+vec3(0.0,i1.x,1.0));\n"
            "vec3 m=max(0.5-vec3(dot(x0,x0),dot(x12.xy,x12.xy),dot(x12.zw,x12.zw)),0.0);\n"
            "m=m*m;m=m*m;\n"
            "vec3 x=2.0*fract(p*C.www)-1.0;\nvec3 h=abs(x)-0.5;\n"
            "vec3 ox=floor(x+0.5);vec3 a0=x-ox;\n"
            "m*=1.79284291400159-0.85373472095314*(a0*a0+h*h);\n"
            "vec3 g;\ng.x=a0.x*x0.x+h.x*x0.y;\ng.yz=a0.yz*x12.xz+h.yz*x12.yw;\n"
            "return 130.0*dot(m,g);}\n"
            "void main(){\n"
            "vec2 uv=v_uv;float t=u_time*0.1;\nvec2 p=uv*1.2;\n"
            "float n1=snoise(p+vec2(t*0.5,t*0.3));\n"
            "float n2=snoise(p*1.5+vec2(n1,n1)+vec2(-t*0.4,t*0.6));\n"
            "float n3=snoise(p*0.8+vec2(n2,n1)+vec2(t*0.3,-t*0.2));\n"
            "float w1=smoothstep(-0.6,0.6,n2);float w2=smoothstep(-0.5,0.5,n3);\n"
            "vec3 liq=mix(u_color0,u_color1,w1);liq=mix(liq,u_color2,w2);\n"
            "liq+=(n2*0.05);\nfragColor=vec4(liq,1.0);\n}\n";
        m_program.addShaderFromSourceCode(QOpenGLShader::Vertex,   kVert);
        m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
        m_program.link();
        float verts[] = { -1,-1, 1,-1, -1,1, 1,1 };
        m_vbo.create();
        m_vbo.bind();
        m_vbo.allocate(verts, sizeof(verts));
    }

    void paintGL() override {
        glViewport(0, 0, width(), height());
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!m_program.isLinked()) return;
        m_program.bind();
        m_program.setUniformValue("u_time",    m_time);
        m_program.setUniformValue("u_opacity", m_opacity);
        QVector<QColor> colors     = (m_palette.size()     >= 3) ? m_palette     : QVector<QColor>{{ {180,60,80},{60,80,180},{80,160,120} }};
        QVector<QColor> prevColors = (m_prevPalette.size() >= 3) ? m_prevPalette : colors;
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

    void resizeGL(int w, int h) override {
        glViewport(0, 0, w, h);
    }
private:
    QOpenGLShaderProgram m_program;
    QOpenGLBuffer        m_vbo;
    QVector<QColor>      m_palette;
    QVector<QColor>      m_prevPalette;
    float                m_progress { 1.f };
    float                m_time     { 0.f };
    float                m_opacity  { 1.f };
};

MarqueeLabel::MarqueeLabel(QWidget *parent) : QWidget(parent) {
    m_anim = new QPropertyAnimation(this, "scrollOffset", this);
    m_anim->setEasingCurve(QEasingCurve::Linear);
}
void MarqueeLabel::setText(const QString &text) {
    if (m_text == text) return;
    m_text = text; m_offset = 0.0; m_anim->stop();
    m_textW = fontMetrics().horizontalAdvance(m_text);
    restartScroll();
}
void MarqueeLabel::setTextStyle(const QFont &font, const QColor &color) {
    m_font = font; m_color = color;
    m_textW = QFontMetrics(m_font).horizontalAdvance(m_text);
    restartScroll();
}
QSize MarqueeLabel::sizeHint() const        { return {200, QFontMetrics(m_font).height() + 4}; }
QSize MarqueeLabel::minimumSizeHint() const { return sizeHint(); }
void MarqueeLabel::restartScroll() {
    m_anim->stop(); m_offset = 0.0;
    if (m_textW <= width()) { update(); return; }
    qreal dist = m_textW + kGap;
    m_anim->setDuration(dist * 1000 / kSpeed);
    m_anim->setStartValue(0.0); m_anim->setEndValue(dist);
    m_anim->setLoopCount(-1); m_anim->start();
}
void MarqueeLabel::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); restartScroll(); }
void MarqueeLabel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setOpacity(m_opacity);
    p.setFont(m_font);
    p.setPen(m_color);
    if (m_textW <= width()) {
        p.drawText(rect(), Qt::AlignCenter, m_text);
    } else {
        const qreal dist = m_textW + kGap;
        p.drawText(rect().translated(-m_offset, 0.0),        Qt::AlignLeft | Qt::AlignVCenter, m_text);
        p.drawText(rect().translated(-m_offset + dist, 0.0), Qt::AlignLeft | Qt::AlignVCenter, m_text);
    }
}

QPointF FullscreenPlayer::springStep(QPointF current, QPointF target, QPointF &velocity, float dt, float stiffness, float damping)
{
    QPointF force = (target - current) * stiffness - velocity * damping;
    velocity += force * dt;
    return current + velocity * dt;
}

float FullscreenPlayer::springStep1D(float current, float target, float &velocity, float dt, float stiffness, float damping)
{
    float force = (target - current) * stiffness - velocity * damping;
    velocity += force * dt;
    return current + velocity * dt;
}

FullscreenPlayer::FullscreenPlayer(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    hide();

    m_animTimer = new QTimer(this);
    m_animTimer->setTimerType(Qt::PreciseTimer);
    m_animTimer->setInterval(1);
    connect(m_animTimer, &QTimer::timeout, this, &FullscreenPlayer::animateTick);

    m_hideControlsTimer = new QTimer(this);
    m_hideControlsTimer->setSingleShot(true);
    connect(m_hideControlsTimer, &QTimer::timeout, this, &FullscreenPlayer::hideControls);

    m_lyricsScrollTimer = new QTimer(this);
    m_lyricsScrollTimer->setTimerType(Qt::PreciseTimer);
    m_lyricsScrollTimer->setInterval(8);
    connect(m_lyricsScrollTimer, &QTimer::timeout, this, &FullscreenPlayer::tickLyricsSmoothScroll);

    m_bgWidget = new FullscreenBackgroundGL(this);
    m_bgWidget->setGeometry(rect());
    m_bgWidget->show();
    m_bgWidget->lower();

    m_dimOverlay = new QWidget(this);
    m_dimOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dimOverlay->setAutoFillBackground(false);
    m_dimOverlay->setStyleSheet("background:rgba(0,0,0,90);");
    m_dimOverlay->setGeometry(rect());
    m_dimOverlay->show();

    m_rootLayout = new QWidget(this);
    m_rootLayout->setStyleSheet("background:transparent;");
    m_rootLayout->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    m_titleBar = new QWidget(m_rootLayout);
    m_titleBar->setFixedHeight(60);
    QHBoxLayout *tbl = new QHBoxLayout(m_titleBar);
    tbl->setContentsMargins(24, 0, 24, 0);

    auto makeTitleBtn = [&](const QString &txt) {
        auto *b = new QPushButton(m_titleBar);
        b->setFixedSize(48, 48);
        b->setStyleSheet("QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.75);font-size:26px;}QPushButton:hover{color:white;}");
        b->setCursor(Qt::PointingHandCursor);
        b->setText(txt);
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
    m_coverLabel->setFixedSize(kCardWidth, kCardWidth);
    m_coverLabel->setAlignment(Qt::AlignCenter);
    cal->addWidget(m_coverLabel, 0, Qt::AlignCenter);

    QWidget *info = new QWidget(m_centerArea);
    QVBoxLayout *il = new QVBoxLayout(info);
    il->setContentsMargins(0, 0, 0, 0);
    il->setSpacing(4);
    il->setAlignment(Qt::AlignCenter);

    m_titleLabel = new MarqueeLabel(info);
    m_titleLabel->setFixedWidth(500);
    { QFont f("-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif"); f.setPixelSize(18); f.setBold(true);
      m_titleLabel->setTextStyle(f, QColor(255,255,255)); }
    il->addWidget(m_titleLabel);

    m_artistLabel = new MarqueeLabel(info);
    m_artistLabel->setFixedWidth(500);
    { QFont f("-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif"); f.setPixelSize(14);
      m_artistLabel->setTextStyle(f, QColor(255,255,255,165)); }
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
    m_lyricsList->setSpacing(10);
    m_lyricsList->setFocusPolicy(Qt::NoFocus);
    m_lyricsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_lyricsList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lyricsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lyricsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_lyricsList->viewport()->setCursor(Qt::PointingHandCursor);
    m_lyricsList->setStyleSheet("QListWidget{background:transparent;border:none;}");
    m_lyricsList->installEventFilter(this);
    m_lyricsList->viewport()->installEventFilter(this);
    m_lyricsList->setResizeMode(QListView::Fixed);
    
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
    m_currentTime = new QLabel("0:00", m_seekBarArea);
    m_currentTime->setStyleSheet("color:rgba(255,255,255,0.6);font-size:12px;font-family:monospace;");
    m_totalTime = new QLabel("0:00", m_seekBarArea);
    m_totalTime->setStyleSheet("color:rgba(255,255,255,0.6);font-size:12px;font-family:monospace;");
    tl->addWidget(m_currentTime); tl->addStretch(); tl->addWidget(m_totalTime);
    pl->addLayout(tl);
    m_seekSlider = new ClickableSlider(Qt::Horizontal, m_seekBarArea);
    m_seekSlider->setStyleSheet(
        "QSlider::groove:horizontal{height:6px;background:rgba(255,255,255,0.25);border-radius:3px;}"
        "QSlider::sub-page:horizontal{background:rgba(255,255,255,0.9);border-radius:3px;}"
        "QSlider::handle:horizontal{width:0px;height:0px;}");
    m_seekSlider->setCursor(Qt::PointingHandCursor);
    pl->addWidget(m_seekSlider);

    m_playbackControls = new QWidget(m_rootLayout);
    m_playbackControlsOpacityEffect = new QGraphicsOpacityEffect(m_playbackControls);
    m_playbackControlsOpacityEffect->setOpacity(0.0);
    m_playbackControls->setGraphicsEffect(m_playbackControlsOpacityEffect);
    QHBoxLayout *bl = new QHBoxLayout(m_playbackControls);
    bl->setContentsMargins(24, 0, 24, 10);

    auto makeCtrlBtn = [&](const QString &txt, int sz, bool fixedSz = false) {
        auto *b = new QPushButton(m_playbackControls);
        b->setStyleSheet(QString("QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.85);font-size:%1px;}QPushButton:hover{color:white;}").arg(sz));
        b->setCursor(Qt::PointingHandCursor);
        b->setText(txt);
        if (fixedSz) b->setFixedSize(48, 48);
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
    volRow->setContentsMargins(0,0,0,0);
    volRow->setSpacing(10);
    m_muteBtn = makeCtrlBtn(QString::fromUtf8("\xF0\x9F\x94\x8A"), 16);
    m_volumeSlider = new ClickableSlider(Qt::Horizontal, m_playbackControls);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(80);
    m_volumeSlider->setStyleSheet(
        "QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,0.25);border-radius:2px;}"
        "QSlider::sub-page:horizontal{background:rgba(255,255,255,0.85);border-radius:2px;}"
        "QSlider::handle:horizontal{width:0px;height:0px;}");
    volRow->addWidget(m_muteBtn);
    volRow->addWidget(m_volumeSlider);

    QWidget *rightBtns = new QWidget(m_playbackControls);
    QHBoxLayout *rbl = new QHBoxLayout(rightBtns);
    rbl->setContentsMargins(0,0,0,0);
    rbl->addStretch();
    rbl->addLayout(volRow);
    rightBtns->setFixedWidth(120);
    bl->addWidget(rightBtns);

    m_paletteTransitionAnim = new QVariantAnimation(this);
    m_paletteTransitionAnim->setDuration(800);
    m_paletteTransitionAnim->setStartValue(0.0f);
    m_paletteTransitionAnim->setEndValue(1.0f);
    connect(m_paletteTransitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        if (m_bgWidget) m_bgWidget->setPaletteTransition(v.toFloat());
    });

    m_speedPulseAnim = new QVariantAnimation(this);
    m_speedPulseAnim->setDuration(800);
    m_speedPulseAnim->setStartValue(0.0f);
    m_speedPulseAnim->setEndValue(1.0f);
    m_speedPulseAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_speedPulseAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        float pulse = std::sin(v.toFloat() * 3.14159f);
        m_animationSpeed = 1.0f + 4.0f * pulse;
    });

    // 380ms OutCubic — строки плавно "выплывают", с чуть заметным overshoot в делегате
    m_lyricsHighlightAnim = new QVariantAnimation(this);
    m_lyricsHighlightAnim->setDuration(380);
    m_lyricsHighlightAnim->setStartValue(0.0f);
    m_lyricsHighlightAnim->setEndValue(1.0f);
    m_lyricsHighlightAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_lyricsHighlightAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v){
        if (m_lyricsDelegate) m_lyricsDelegate->setProgress(v.toReal());
    });

    m_centerAreaOpacityEffect  = nullptr;
    m_titleBarOpacityEffect    = nullptr;
    m_seekBarOpacityEffect     = nullptr;
    m_rootOpacityEffect        = nullptr;

    connect(m_shuffleBtn,   &QPushButton::clicked, this, &FullscreenPlayer::shuffleToggleRequested);
    connect(m_prevBtn,      &QPushButton::clicked, this, &FullscreenPlayer::previousRequested);
    connect(m_playBtn,      &QPushButton::clicked, this, &FullscreenPlayer::playPauseRequested);
    connect(m_nextBtn,      &QPushButton::clicked, this, &FullscreenPlayer::nextRequested);
    connect(m_repeatBtn,    &QPushButton::clicked, this, &FullscreenPlayer::repeatToggleRequested);
    connect(m_muteBtn,      &QPushButton::clicked, this, &FullscreenPlayer::muteToggleRequested);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &FullscreenPlayer::volumeChangeRequested);
    connect(m_volumeSlider, &QSlider::sliderPressed, this, [this]{
        if (m_lyricsVisible) suspendLyricsAutoScroll();
    });
    connect(m_seekSlider, &QSlider::sliderPressed, this, [this]{
        m_userSeeking = true;
        if (m_lyricsVisible) suspendLyricsAutoScroll();
    });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]{
        m_userSeeking = false;
        emit seekRequested(m_seekSlider->value());
        if (m_lyricsVisible && m_lyricsSyncedAvailable) {
            m_seekIgnoreUntilMs      = QDateTime::currentMSecsSinceEpoch() + 1500;
            m_expectedSeekPositionMs = m_seekSlider->value();
            updateLyricsHighlight(m_seekSlider->value());
            animateLyricsScrollTo(m_lyricsCurrentIndex, true, false);
        }
    });
}

FullscreenPlayer::~FullscreenPlayer()
{
    if (m_animTimer) m_animTimer->stop();
    if (m_lyricsScrollTimer) m_lyricsScrollTimer->stop();
}

bool FullscreenPlayer::eventFilter(QObject *w, QEvent *e)
{
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            if (m_lyricsVisible) setLyricsVisible(false, true);
            else closeOverlay();
            return true;
        }
    }
    if (m_lyricsList && (w == m_lyricsList || w == m_lyricsList->viewport())) {
        if (e->type() == QEvent::Wheel || e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseMove)
            suspendLyricsAutoScroll();
        if (e->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent*>(e);
            auto *bar = m_lyricsList->verticalScrollBar();
            if (bar && !(we->modifiers() & Qt::ControlModifier)) {
                const QPoint angleDelta = we->angleDelta();
                const QPoint pixelDelta = we->pixelDelta();
                if (!pixelDelta.isNull() || !angleDelta.isNull()) {
                    if (angleDelta.y() != 0 || !pixelDelta.isNull()) {
                        int rowH = m_lyricsList->count() > 0 ? m_lyricsList->sizeHintForRow(0) : 0;
                        if (rowH <= 0) rowH = 36;

                        int delta = 0;
                        if (!pixelDelta.isNull()) {
                            delta = pixelDelta.y();
                        } else {
                            const float steps = angleDelta.y() / 120.0f;
                            delta = static_cast<int>(steps * rowH * 0.6f);
                        }

                        const int maxStep = rowH * 3;
                        delta = qBound(-maxStep, delta, maxStep);

                        if (delta != 0) {
                            const int current = bar->value();
                            const int baseTarget = (m_lyricsScrollTimer && m_lyricsScrollTimer->isActive())
                                ? m_lyricsScrollTarget
                                : current;
                            int target = baseTarget - delta;
                            target = qBound(bar->minimum(), target, bar->maximum());
                            m_lyricsScrollTarget = target;
                            m_lyricsScrollVelocity = 0.f;

                            if (m_lyricsScrollTimer && !m_lyricsScrollTimer->isActive()) {
                                m_lyricsScrollClock.restart();
                                m_lyricsScrollTimer->start();
                            }

                            we->accept();
                            return true;
                        }
                    }
                }
            }
        }
        if (e->type() == QEvent::MouseButtonPress && m_lyricsVisible) {
            QMouseEvent *me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton) {
                QListWidgetItem *item = m_lyricsList->itemAt(me->pos());
                if (item && m_lyricsSyncedAvailable) {
                    int row = m_lyricsList->row(item);
                    if (row >= 0 && row < m_lyricsSyncedTimes.size()) {
                        const int seekMs = m_lyricsSyncedTimes.at(row);
                        m_seekIgnoreUntilMs      = QDateTime::currentMSecsSinceEpoch() + 2000;
                        m_expectedSeekPositionMs = seekMs;
                        emit seekRequested(seekMs);
                        if (!m_isPlaying) emit playPauseRequested();
                        m_lyricsCurrentIndex = row;
                        startLyricsHighlightAnimation(m_lyricsPrevIndex, row);
                        return true;
                    }
                }
                setLyricsVisible(false, true);
                return true;
            }
        }
    }
    return QWidget::eventFilter(w, e);
}

void FullscreenPlayer::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        if (m_lyricsVisible) setLyricsVisible(false, true);
        else closeOverlay();
    } else {
        QWidget::keyPressEvent(e);
    }
}

void FullscreenPlayer::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        bool inRight = (width() - e->pos().x()) <= width() * 0.20;
        if (inRight) { toggleLyrics(); return; }
    }
    QWidget::mousePressEvent(e);
}

void FullscreenPlayer::setMainUiOpacity(qreal v)
{
    m_mainUiOpacity = v;
    if (m_rootOpacityEffect) m_rootOpacityEffect->setOpacity(v);
}

void FullscreenPlayer::setControlsOpacity(qreal v)
{
    m_controlsOpacity = v;
    if (m_playbackControlsOpacityEffect) m_playbackControlsOpacityEffect->setOpacity(v);
    m_playbackControls->setEnabled(v > 0.1);
}

void FullscreenPlayer::openFor(const QPixmap &cover, const QString &title, const QString &artist,
                                const QString &album, int durationMs, int positionMs,
                                bool isPlaying, int volume)
{
    m_rawCover        = cover;
    m_durationMs      = durationMs;
    m_trackTitle      = title;
    m_trackArtist     = artist;
    m_trackAlbum      = album;
    m_trackDurationSec = durationToSeconds(durationMs);

    m_titleLabel->setText(title);
    m_artistLabel->setText(artist);
    m_seekSlider->setRange(0, durationMs);
    m_totalTime->setText(fmt(durationMs));

    updateCoverWidget();
    updatePosition(positionMs);
    updatePlayState(isPlaying);
    updateVolume(volume);
    m_volumeSlider->blockSignals(true);
    m_volumeSlider->setValue(volume);
    m_volumeSlider->blockSignals(false);

    requestLyrics();
    extractPalette(m_rawCover);
    if (m_bgWidget) {
        m_bgWidget->setPaletteInstant(m_palette);
        if (m_speedPulseAnim) { m_speedPulseAnim->stop(); m_speedPulseAnim->start(); }
    }

    setGeometry(parentWidget()->rect());
    raise(); show(); activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    grabKeyboard();

    m_lyricsVisible  = false;
    m_stateLifted    = false;
    m_stateHinted    = false;

    m_centerOffset        = QPointF(0, 50);
    m_centerOffsetTarget  = QPointF(0, 0);
    m_centerOffsetVelocity = QPointF(0, 0);

    m_lyricsPanelX         = (float)width();
    m_lyricsPanelXTarget   = (float)width();
    m_lyricsPanelXVelocity = 0.f;

    m_controlsAlpha         = 0.f;
    m_controlsAlphaTarget   = 0.f;
    m_controlsAlphaVelocity = 0.f;

    m_hintAlpha         = 0.f;
    m_hintAlphaTarget   = 0.f;
    m_hintAlphaVelocity = 0.f;

    m_hintX         = (float)(width() - m_lyricsHint->width() - 24 + 20);
    m_hintXTarget   = m_hintX;
    m_hintXVelocity = 0.f;

    m_openAlpha         = 0.f;
    m_openAlphaTarget   = 1.f;
    m_openAlphaVelocity = 0.f;

    int pcH = m_playbackControls->sizeHint().height();
    int sbH = m_seekBarArea->sizeHint().height();
    m_controlsY         = (float)(height() - sbH - pcH + 40);
    m_controlsYTarget   = m_controlsY;
    m_controlsYVelocity = 0.f;

    if (m_lyricsScrollTimer) m_lyricsScrollTimer->stop();
    m_lyricsScrollTarget   = 0;
    m_lyricsScrollVelocity = 0.f;

    m_rootLayout->show();
    updateLayout();

    m_frameTimer.start();
    m_lastFrameMs = 0;
    m_animTimer->start();
    m_isOpen = true;
}

void FullscreenPlayer::closeOverlay()
{
    if (!m_isOpen) return;
    m_isOpen = false;
    releaseKeyboard();
    setLyricsVisible(false, false);

    if (m_lyricsScrollTimer) m_lyricsScrollTimer->stop();

    m_openAlphaTarget        = 0.f;
    m_centerOffsetTarget     = m_centerOffset + QPointF(0, 30);
    m_openAlphaVelocity      = 0.f;

    QTimer::singleShot(300, this, [this]{
        hide();
        clearFocus();
        m_animTimer->stop();
    });
}

void FullscreenPlayer::updateTrack(const QPixmap &cover, const QString &title, const QString &artist,
                                    const QString &album, int durationMs)
{
    m_rawCover         = cover;
    m_durationMs       = durationMs;
    m_trackTitle       = title;
    m_trackArtist      = artist;
    m_trackAlbum       = album;
    m_trackDurationSec = durationToSeconds(durationMs);

    m_titleLabel->setText(title);
    m_artistLabel->setText(artist);
    m_seekSlider->setRange(0, durationMs);
    m_totalTime->setText(fmt(durationMs));

    updateCoverWidget();
    updateLayout();

    if (isVisible()) {
        extractPalette(m_rawCover);
        if (m_bgWidget) {
            if (m_bgWidget->palette() != m_palette) {
                m_bgWidget->setPalette(m_palette);
                if (m_paletteTransitionAnim) { m_paletteTransitionAnim->stop(); m_paletteTransitionAnim->start(); }
            }
            if (m_speedPulseAnim) { m_speedPulseAnim->stop(); m_speedPulseAnim->start(); }
        }
    }
    if (m_isOpen) requestLyrics();
}

void FullscreenPlayer::updatePosition(int ms)
{
    if (m_userSeeking) {
        m_lastPositionMs       = ms;
        m_positionAnchorWallMs = QDateTime::currentMSecsSinceEpoch();
        m_positionAnchorAudioMs = ms;
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() < m_seekIgnoreUntilMs) {
        const int delta = std::abs(ms - m_expectedSeekPositionMs);
        if (delta < 500 || delta > 5000) m_seekIgnoreUntilMs = 0;
        else return;
    }
    m_lastPositionMs        = ms;
    m_positionAnchorWallMs  = QDateTime::currentMSecsSinceEpoch();
    m_positionAnchorAudioMs = ms;
    m_seekSlider->blockSignals(true);
    m_seekSlider->setValue(ms);
    m_seekSlider->blockSignals(false);
    m_currentTime->setText(fmt(ms));
    updateLyricsHighlight(ms);
}

void FullscreenPlayer::updatePlayState(bool playing)
{
    m_isPlaying = playing;
    m_playBtn->setText(playing ? QString::fromUtf8("\xE2\x8F\xB8") : QString::fromUtf8("\xE2\x96\xB6"));
    m_positionAnchorPlaying  = playing;
    m_positionAnchorWallMs   = QDateTime::currentMSecsSinceEpoch();
    m_positionAnchorAudioMs  = m_lastPositionMs;
}

void FullscreenPlayer::updateVolume(int value)
{
    m_volumeValue = qBound(0, value, 100);
    m_volumeSlider->blockSignals(true);
    m_volumeSlider->setValue(m_volumeValue);
    m_volumeSlider->blockSignals(false);
    if      (m_volumeValue == 0)  m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x87"));
    else if (m_volumeValue < 50)  m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x89"));
    else                           m_muteBtn->setText(QString::fromUtf8("\xF0\x9F\x94\x8A"));
}

void FullscreenPlayer::updateLikeState(bool liked)      { Q_UNUSED(liked); }
void FullscreenPlayer::updateShuffleState(bool enabled, int mode) {
    Q_UNUSED(mode);
    m_shuffleBtn->setStyleSheet(enabled
        ? "QPushButton{background:transparent;border:none;color:white;font-size:20px;}"
        : "QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.85);font-size:20px;}QPushButton:hover{color:white;}");
}
void FullscreenPlayer::updateRepeatState(int mode) {
    m_repeatBtn->setStyleSheet(mode > 0
        ? "QPushButton{background:transparent;border:none;color:white;font-size:20px;}"
        : "QPushButton{background:transparent;border:none;color:rgba(255,255,255,0.85);font-size:20px;}QPushButton:hover{color:white;}");
}

bool FullscreenPlayer::hasLyrics() const { return m_lyricsSyncedAvailable || !m_lyricsPlainLines.isEmpty(); }
void FullscreenPlayer::toggleLyrics() { if (!hasLyrics()) return; setLyricsVisible(!m_lyricsVisible, true); }

void FullscreenPlayer::requestLyrics()
{
    loadLyricsCache();
    if (m_lyricsReply) {
        QNetworkReply *reply = m_lyricsReply;
        m_lyricsReply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    m_lyricsRequestInFlight = false;
    const QString title  = m_trackTitle.trimmed();
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
    const QString key = normalizeLyricsKey(title) + "|" + normalizeLyricsKey(artist) + "|" + QString::number(m_trackDurationSec);
    if (key == m_lyricsKey && (m_lyricsRequestInFlight || hasLyrics())) return;
    m_lyricsKey = key;
    const auto cached = s_lyricsCache.constFind(key);
    if (cached != s_lyricsCache.constEnd()) {
        if (cached->found) applyLyrics(cached->synced, cached->plain, cached->instrumental);
        else               applyLyrics(QString(), QString(), false);
        return;
    }
    m_lyricsSyncedAvailable = false;
    m_lyricsPlainLines.clear();
    m_lyricsSyncedLines.clear();
    m_lyricsSyncedTimes.clear();
    m_lyricsCurrentIndex    = -1;
    m_lyricsRequestInFlight = true;
    rebuildLyricsList();
    updateLyricsButtonState();
    if (!m_lyricsNet) m_lyricsNet = new QNetworkAccessManager(this);
    QUrl url(QStringLiteral("https://lrclib.net/api/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("track_name"), title);
    if (!artist.isEmpty()) query.addQueryItem(QStringLiteral("artist_name"), artist);
    url.setQuery(query);
    sendLyricsRequest(url);
}

void FullscreenPlayer::sendLyricsRequest(const QUrl &url)
{
    if (m_lyricsReply) { m_lyricsReply->abort(); m_lyricsReply->deleteLater(); m_lyricsReply = nullptr; }
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
    auto *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    const int token = reply->property("lyricsToken").toInt();
    if (token != m_lyricsRequestToken) { reply->deleteLater(); return; }
    const QNetworkReply::NetworkError netError = reply->error();
    const bool replyOpen = reply->isOpen();
    if (m_lyricsReply == reply) m_lyricsReply = nullptr;
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
        rebuildLyricsList(); updateLyricsButtonState(); return;
    }
    const QJsonArray arr = doc.array();
    if (arr.isEmpty()) {
        cacheLyricsEntry(m_lyricsKey, {QString(), QString(), false, false});
        rebuildLyricsList(); updateLyricsButtonState(); return;
    }
    int bestIndex = -1;
    int bestScore = std::numeric_limits<int>::min();
    int bestDiff  = std::numeric_limits<int>::max();
    const QString wantTitle  = normalizeLyricsKey(m_trackTitle);
    const QString wantArtist = normalizeLyricsKey(m_trackArtist);
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject obj = arr.at(i).toObject();
        if (obj.isEmpty()) continue;
        const QString gotTitle  = normalizeLyricsKey(obj.value(QStringLiteral("trackName")).toString());
        const QString gotArtist = normalizeLyricsKey(obj.value(QStringLiteral("artistName")).toString());
        const int duration = obj.value(QStringLiteral("duration")).toInt();
        const int diff = (m_trackDurationSec > 0 && duration > 0) ? qAbs(duration - m_trackDurationSec) : std::numeric_limits<int>::max();
        int score = 0;
        if (!wantTitle.isEmpty()  && gotTitle  == wantTitle)  score += 6;
        if (!wantArtist.isEmpty() && gotArtist == wantArtist) score += 5;
        if (diff <= 2) score += 3; else if (diff <= 5) score += 1;
        const QString synced = obj.value(QStringLiteral("syncedLyrics")).toString().trimmed();
        const QString plain  = obj.value(QStringLiteral("plainLyrics")).toString().trimmed();
        if (!synced.isEmpty()) score += 2; else if (!plain.isEmpty()) score += 1;
        if (score > bestScore || (score == bestScore && diff < bestDiff)) {
            bestScore = score; bestDiff = diff; bestIndex = i;
        }
    }
    if (bestIndex < 0) {
        cacheLyricsEntry(m_lyricsKey, {QString(), QString(), false, false});
        rebuildLyricsList(); updateLyricsButtonState(); return;
    }
    const QJsonObject best = arr.at(bestIndex).toObject();
    const QString synced       = best.value(QStringLiteral("syncedLyrics")).toString();
    const QString plain        = best.value(QStringLiteral("plainLyrics")).toString();
    const bool    instrumental = best.value(QStringLiteral("instrumental")).toBool(false);
    cacheLyricsEntry(m_lyricsKey, {synced, plain, instrumental, true});
    applyLyrics(synced, plain, instrumental);
}

void FullscreenPlayer::applyLyrics(const QString &synced, const QString &plain, bool instrumental)
{
    (void)instrumental;
    m_lyricsSyncedAvailable = false;
    m_lyricsPlainLines.clear();
    m_lyricsSyncedLines.clear();
    m_lyricsSyncedTimes.clear();
    m_lyricsCurrentIndex = -1;
    const QString syncedTrim = synced.trimmed();
    const QString plainTrim  = plain.trimmed();
    if (!syncedTrim.isEmpty()) parseSyncedLyrics(syncedTrim);
    if (!m_lyricsSyncedAvailable && !plainTrim.isEmpty()) parsePlainLyrics(plainTrim);
    rebuildLyricsList();
    updateLyricsButtonState();
    updateLyricsHighlight(m_lastPositionMs);
}

void FullscreenPlayer::parseSyncedLyrics(const QString &text)
{
    struct Entry { int timeMs; QString line; };
    QVector<Entry> entries;
    QRegularExpression tag(QStringLiteral("\\[(\\d{1,2}):(\\d{2})(?:\\.(\\d{1,3}))?\\]"));
    for (const QString &raw : text.split('\n')) {
        auto it = tag.globalMatch(raw);
        if (!it.hasNext()) continue;
        QString line = raw;
        line.remove(tag);
        line = line.trimmed();
        if (line.isEmpty()) continue;
        while (it.hasNext()) {
            const auto m = it.next();
            int fracMs = 0;
            const QString fracText = m.captured(3);
            if (!fracText.isEmpty()) {
                int frac = fracText.toInt();
                if      (fracText.size() == 1) fracMs = frac * 100;
                else if (fracText.size() == 2) fracMs = frac * 10;
                else                            fracMs = frac;
            }
            entries.push_back({ m.captured(1).toInt()*60000 + m.captured(2).toInt()*1000 + fracMs, line });
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b){ return a.timeMs < b.timeMs; });
    for (const Entry &e : entries) { m_lyricsSyncedTimes.append(e.timeMs); m_lyricsSyncedLines.append(e.line); }
    m_lyricsSyncedAvailable = !m_lyricsSyncedLines.isEmpty();
}

void FullscreenPlayer::parsePlainLyrics(const QString &text)
{
    QStringList lines = text.split('\n');
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())  lines.removeLast();
    m_lyricsPlainLines = lines;
}

void FullscreenPlayer::updateLyricsButtonState()
{
    if (!m_textBtn) return;
    m_textBtn->setEnabled(hasLyrics());
}

void FullscreenPlayer::updateLyricsHighlight(int ms)
{
    if (!m_lyricsSyncedAvailable || m_lyricsSyncedTimes.isEmpty() || !m_lyricsList) return;
    auto it = std::upper_bound(m_lyricsSyncedTimes.begin(), m_lyricsSyncedTimes.end(), ms);
    const int idx = (it == m_lyricsSyncedTimes.begin()) ? -1 : (int)(it - m_lyricsSyncedTimes.begin()) - 1;
    if (QDateTime::currentMSecsSinceEpoch() < m_seekIgnoreUntilMs) {
        if (idx != m_lyricsCurrentIndex) return;
        else m_seekIgnoreUntilMs = 0;
    }
    if (idx == m_lyricsCurrentIndex) return;
    const int prevIndex  = m_lyricsCurrentIndex;
    m_lyricsCurrentIndex = idx;
    startLyricsHighlightAnimation(prevIndex, m_lyricsCurrentIndex);
    if (lyricsAutoScrollSuspended()) { m_lyricsAutoScrollSuppressed = true; return; }
    if (idx < 0) {
        if (auto *bar = m_lyricsList->verticalScrollBar()) {
            if (m_lyricsScrollTimer) m_lyricsScrollTimer->stop();
            m_lyricsScrollVelocity = 0.f;
            bar->setValue(bar->minimum());
            m_lyricsScrollTarget = bar->value();
        }
        return;
    }
    const bool isBigJump = (prevIndex < 0) || (std::abs(idx - prevIndex) > 3);
    animateLyricsScrollTo(m_lyricsCurrentIndex, false, isBigJump);
}

void FullscreenPlayer::setLyricsVisible(bool visible, bool animate)
{
    if (visible == m_lyricsVisible) return;
    m_lyricsVisible = visible;
    updateState();

    m_centerArea->adjustSize();
    QPoint centerPos((width() - m_centerArea->width())/2, (height() - m_centerArea->height())/2);
    centerPos += QPoint((int)m_centerOffsetTarget.x(), (int)m_centerOffsetTarget.y());
    float lyricsPanelVisibleX = (float)(centerPos.x() + m_centerArea->width() + 10);

    m_lyricsPanelXTarget = visible ? lyricsPanelVisibleX : (float)width();

    if (!animate) {
        m_lyricsPanelX         = m_lyricsPanelXTarget;
        m_lyricsPanelXVelocity = 0.f;
        updateLayout();
    }

    if (visible) {
        m_stateHinted        = false;
        m_hintAlphaTarget    = 0.f;
        m_hintXTarget        = (float)(width() - m_lyricsHint->width() - 24 + 20);
        m_lyricsHintOpacityEffect->setOpacity(0.0);
    } else {
        m_lyricsHintOpacityEffect->setOpacity(m_stateHinted ? 1.0 : 0.0);
    }

    if (visible && m_lyricsSyncedAvailable) {
        updateLyricsHighlight(m_lastPositionMs);
        animateLyricsScrollTo(m_lyricsCurrentIndex, true, true);
    }
}

void FullscreenPlayer::rebuildLyricsList()
{
    if (!m_lyricsList) return;
    m_lyricsList->clear();
    const QStringList lines = m_lyricsSyncedAvailable ? m_lyricsSyncedLines : m_lyricsPlainLines;
    if (lines.isEmpty()) { m_lyricsList->setCurrentRow(-1); return; }
    for (const QString &line : lines) {
        auto *item = new QListWidgetItem(line.isEmpty() ? QStringLiteral(" ") : line, m_lyricsList);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    m_lyricsCurrentIndex = -1;
    m_lyricsList->setCurrentRow(-1);
    if (m_lyricsDelegate) { m_lyricsDelegate->setIndices(-1,-1); m_lyricsDelegate->setProgress(1.0); }
    m_lyricsList->doItemsLayout();
}

void FullscreenPlayer::animateLyricsScrollTo(int logicalIndex, bool force, bool instant)
{
    if (!m_lyricsVisible || !m_lyricsList) return;
    if (force) m_lyricsHoldUntilMs = 0;
    else if (lyricsAutoScrollSuspended()) return;
    const int target = lyricsScrollTargetForIndex(logicalIndex);
    if (target < 0) return;
    auto *bar = m_lyricsList->verticalScrollBar();
    if (instant) {
        if (m_lyricsScrollTimer) m_lyricsScrollTimer->stop();
        m_lyricsScrollVelocity = 0.f;
        if (bar) bar->setValue(target);
        m_lyricsScrollTarget = target;
        return;
    }
    m_lyricsScrollTarget = target;
    if (m_lyricsScrollTimer && !m_lyricsScrollTimer->isActive()) {
        m_lyricsScrollClock.restart();
        m_lyricsScrollTimer->start();
    }
}

void FullscreenPlayer::tickLyricsSmoothScroll()
{
    if (!m_lyricsList || !m_lyricsScrollTimer) return;
    auto *bar = m_lyricsList->verticalScrollBar();
    if (!bar) { m_lyricsScrollTimer->stop(); return; }

    const int current = bar->value();
    const int dist    = m_lyricsScrollTarget - current;

    if (qAbs(dist) <= 1) {
        bar->setValue(m_lyricsScrollTarget);
        m_lyricsScrollVelocity = 0.f;
        m_lyricsScrollTimer->stop();
        return;
    }

    qint64 dt = m_lyricsScrollClock.isValid() ? m_lyricsScrollClock.restart() : 16;
    dt = qMax<qint64>(1, qMin<qint64>(dt, 40));
    const float dtSec = (float)dt / 1000.f;

    // Spring physics — stiffness/damping tuned for Apple Music feel
    const float stiffness = 180.f;
    const float damping   = 22.f;
    const float force     = (float)dist * stiffness - m_lyricsScrollVelocity * damping;
    m_lyricsScrollVelocity += force * dtSec;
    m_lyricsScrollVelocity  = qBound(-4000.f, m_lyricsScrollVelocity, 4000.f);

    int step = (int)(m_lyricsScrollVelocity * dtSec);
    if (step == 0) step = (dist > 0) ? 1 : -1;

    bar->setValue(current + step);
}

void FullscreenPlayer::startLyricsHighlightAnimation(int prevIndex, int nextIndex)
{
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

int FullscreenPlayer::lyricsScrollTargetForIndex(int index) const
{
    if (!m_lyricsList || index < 0 || index >= m_lyricsList->count()) return -1;
    QListWidgetItem *item = m_lyricsList->item(index);
    if (!item) return -1;
    auto *bar = m_lyricsList->verticalScrollBar();
    int currentBarVal = bar ? bar->value() : 0;
    QRect rect = m_lyricsList->visualItemRect(item);
    if (!rect.isValid() && bar) {
        const bool wasBlocked = bar->blockSignals(true);
        m_lyricsList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        rect = m_lyricsList->visualItemRect(item);
        bar->setValue(currentBarVal);
        bar->blockSignals(wasBlocked);
    }
    if (!rect.isValid()) return -1;
    int extraTop    = (index == 0)                         ? qMax(0, m_lyricsList->viewport()->height()/2 - 20) : 0;
    int extraBottom = (index == m_lyricsList->count() - 1) ? qMax(0, m_lyricsList->viewport()->height()/2 - 20) : 0;
    QRect contentRect = rect;
    contentRect.setTop(contentRect.top() + extraTop);
    contentRect.setBottom(contentRect.bottom() - extraBottom);
    int absoluteCenterY = (bar ? bar->value() : 0) + contentRect.center().y();
    int target = absoluteCenterY - m_lyricsList->viewport()->height()/2;
    return bar ? qBound(bar->minimum(), target, bar->maximum()) : target;
}

void FullscreenPlayer::suspendLyricsAutoScroll()
{
    m_lyricsHoldUntilMs        = QDateTime::currentMSecsSinceEpoch() + 1500;
    m_lyricsAutoScrollSuppressed = true;
}
bool FullscreenPlayer::lyricsAutoScrollSuspended() const
{
    return QDateTime::currentMSecsSinceEpoch() < m_lyricsHoldUntilMs;
}
void FullscreenPlayer::maybeResumeLyricsAutoScroll()
{
    if (!m_lyricsAutoScrollSuppressed || !m_lyricsVisible || !m_lyricsSyncedAvailable
        || lyricsAutoScrollSuspended() || m_lyricsCurrentIndex < 0) return;
    m_lyricsAutoScrollSuppressed = false;
    animateLyricsScrollTo(m_lyricsCurrentIndex);
}

void FullscreenPlayer::extractPalette(const QPixmap &albumArt)
{
    m_palette.clear();
    if (albumArt.isNull()) { m_palette = {{180,60,80},{60,80,180},{80,160,120}}; return; }
    QImage img = albumArt.scaled(48,48,Qt::IgnoreAspectRatio,Qt::SmoothTransformation)
                          .toImage().convertToFormat(QImage::Format_ARGB32);
    struct Bin { int count=0, rSum=0, gSum=0, bSum=0; };
    QVector<Bin> bins(1<<15);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]);
            if (c.alpha() < 16) continue;
            const int key = ((c.red()>>3)<<10)|((c.green()>>3)<<5)|(c.blue()>>3);
            Bin &b = bins[key]; b.count++; b.rSum+=c.red(); b.gSum+=c.green(); b.bSum+=c.blue();
        }
    }
    struct BinColor { QColor color; int count=0; };
    QVector<BinColor> colors;
    for (const Bin &b : bins) {
        if (b.count<=0) continue;
        colors.append({QColor(b.rSum/b.count, b.gSum/b.count, b.bSum/b.count), b.count});
    }
    if (colors.isEmpty()) { m_palette = {{180,60,80},{60,80,180},{80,160,120}}; return; }
    std::sort(colors.begin(), colors.end(), [](const BinColor &a, const BinColor &b){ return a.count > b.count; });
    QVector<QColor> fp;
    for (const auto &bc : colors) {
        bool tooSimilar = false;
        for (const QColor &ex : fp) {
            int dr=bc.color.red()-ex.red(), dg=bc.color.green()-ex.green(), db=bc.color.blue()-ex.blue();
            if ((dr*dr+dg*dg+db*db) < 2025) { tooSimilar=true; break; }
        }
        if (!tooSimilar) fp.append(bc.color);
        if (fp.size()>=3) break;
    }
    while (fp.size()<3) fp.append(fp.isEmpty() ? QColor(30,30,30) : fp.last());
    m_palette = fp;
}

void FullscreenPlayer::paintEvent(QPaintEvent *e)
{
    QWidget::paintEvent(e);
}

void FullscreenPlayer::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (m_bgWidget) {
        m_bgWidget->move(0, 0);
        m_bgWidget->resize(size());
    }
    if (m_isOpen) updateLayout();
}

void FullscreenPlayer::updateBassLevel(float level)
{
    if (level > m_lastLevel) m_lastLevel = m_lastLevel * 0.2f + level * 0.8f;
    else                     m_lastLevel = m_lastLevel * 0.92f + level * 0.08f;
}

void FullscreenPlayer::updateLayout()
{
    m_rootLayout->setGeometry(rect());
    const int w = width(), h = height();
    if (m_dimOverlay) m_dimOverlay->setGeometry(rect());
    m_titleBar->setGeometry(0, 0, w, 60);

    m_centerArea->adjustSize();
    QPoint centerPos((w - m_centerArea->width())/2, (h - m_centerArea->height())/2);
    centerPos += QPoint((int)m_centerOffset.x(), (int)m_centerOffset.y());
    m_centerArea->move(centerPos);

    m_lyricsHint->move((int)m_hintX, (h - m_lyricsHint->height())/2);
    m_lyricsHintOpacityEffect->setOpacity((double)m_hintAlpha);

    const int newLyricsX = (int)m_lyricsPanelX;
    const int newLyricsW = w - newLyricsX;
    if (m_lyricsPanel->x() != newLyricsX || m_lyricsPanel->width() != newLyricsW) {
        m_lyricsPanel->setGeometry(newLyricsX, (int)m_centerOffset.y(), newLyricsW, h);
    }

    const int pcH = m_playbackControls->sizeHint().height();
    const int sbH = m_seekBarArea->sizeHint().height();
    m_seekBarArea->setGeometry(0, h - sbH, w, sbH);
    m_playbackControls->setGeometry(0, (int)m_controlsY, w, pcH);
    m_playbackControlsOpacityEffect->setOpacity((double)m_controlsAlpha);
    m_playbackControls->setEnabled(m_controlsAlpha > 0.1f);

    if (m_rootOpacityEffect) m_rootOpacityEffect->setOpacity((double)m_openAlpha);
    setWindowOpacity((double)m_openAlpha);
}

void FullscreenPlayer::showControls()
{
    m_hideControlsTimer->stop();
    if (!m_stateLifted) {
        m_stateLifted = true;
        updateState();
    }
}

void FullscreenPlayer::hideControls()
{
    if (m_stateLifted) {
        m_stateLifted = false;
        updateState();
    }
}

void FullscreenPlayer::updateState()
{
    double tx = m_lyricsVisible ? -260.0 : (m_stateHinted ? -50.0 : 0.0);
    double ty = m_stateLifted  ? -28.0  : 0.0;
    m_centerOffsetTarget = QPointF(tx, ty);

    const int pcH   = m_playbackControls->sizeHint().height();
    const int sbH   = m_seekBarArea->sizeHint().height();
    const int baseY = height() - sbH - pcH;
    m_controlsYTarget     = m_stateLifted ? (float)baseY : (float)(baseY + 40);
    m_controlsAlphaTarget = m_stateLifted ? 1.f : 0.f;
}

void FullscreenPlayer::mouseMoveEvent(QMouseEvent *e) { QWidget::mouseMoveEvent(e); }
void FullscreenPlayer::leaveEvent(QEvent *e)          { QWidget::leaveEvent(e); }

void FullscreenPlayer::updateCoverWidget()
{
    if (m_rawCover.isNull()) {
        m_coverLabel->clear();
        m_coverLabel->setFixedSize(kCardWidth, kCardWidth);
        return;
    }
    QPixmap px = m_rawCover.scaled(kCardWidth, kCardWidth, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_coverLabel->setPixmap(rounded(px, 10));
    m_coverLabel->setFixedSize(px.size());
}

void FullscreenPlayer::animateTick()
{
    const qint64 now   = m_frameTimer.elapsed();
    const float  dtRaw = (float)(now - m_lastFrameMs) / 1000.f;
    m_lastFrameMs      = now;
    const float dt     = qBound(0.001f, dtRaw, 0.05f);

    m_phase += 0.015f * m_animationSpeed * (dt / 0.016f);
    if (m_bgWidget) m_bgWidget->setTime(m_phase);

    static constexpr float kStiff  = 280.f;
    static constexpr float kDamp   = 26.f;
    static constexpr float kStiffF = 200.f;
    static constexpr float kDampF  = 22.f;

    m_centerOffset = springStep(m_centerOffset, m_centerOffsetTarget, m_centerOffsetVelocity, dt, kStiff, kDamp);

    m_lyricsPanelX = springStep1D(m_lyricsPanelX, m_lyricsPanelXTarget, m_lyricsPanelXVelocity, dt, kStiffF, kDampF);

    m_controlsY     = springStep1D(m_controlsY,     m_controlsYTarget,     m_controlsYVelocity,     dt, kStiffF, kDampF);
    m_controlsAlpha = springStep1D(m_controlsAlpha, m_controlsAlphaTarget, m_controlsAlphaVelocity, dt, kStiff,  kDamp);
    m_controlsAlpha = qBound(0.f, m_controlsAlpha, 1.f);

    m_hintAlpha = springStep1D(m_hintAlpha, m_hintAlphaTarget, m_hintAlphaVelocity, dt, kStiff, kDamp);
    m_hintAlpha = qBound(0.f, m_hintAlpha, 1.f);
    m_hintX     = springStep1D(m_hintX, m_hintXTarget, m_hintXVelocity, dt, kStiffF, kDampF);

    m_openAlpha = springStep1D(m_openAlpha, m_openAlphaTarget, m_openAlphaVelocity, dt, kStiff, kDamp);
    m_openAlpha = qBound(0.f, m_openAlpha, 1.f);

    updateLayout();
    maybeResumeLyricsAutoScroll();

    if (m_positionAnchorPlaying && m_lyricsSyncedAvailable && m_lyricsVisible
        && QDateTime::currentMSecsSinceEpoch() >= m_seekIgnoreUntilMs && !m_userSeeking) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_positionAnchorWallMs;
        const qint64 capped  = qBound(qint64(0), elapsed, qint64(500));
        updateLyricsHighlight(m_positionAnchorAudioMs + (int)capped);
    }

    if (!isVisible()) return;

    QPoint pos = mapFromGlobal(QCursor::pos());
    const int w = width(), h = height();
    bool isMouseInside = (pos.x() >= -2 && pos.x() <= w+2 && pos.y() >= -2 && pos.y() <= h+2);

    if (isMouseInside && !m_userSeeking) {
        bool inBottom = (h - pos.y()) <= h * 0.20;
        bool inRight  = (w - pos.x()) <= w * 0.20;

        if (inBottom) {
            showControls();
        } else if (m_stateLifted && !m_hideControlsTimer->isActive()) {
            m_hideControlsTimer->start(400);
        }

        if (!m_lyricsVisible && inRight != m_stateHinted) {
            m_stateHinted    = inRight;
            m_hintAlphaTarget = m_stateHinted ? 1.f : 0.f;
            m_hintXTarget     = (float)(w - m_lyricsHint->width() - 24 + (m_stateHinted ? 0 : 20));
            updateState();
        }
    } else if (!isMouseInside && !m_userSeeking) {
        if (m_stateLifted && !m_hideControlsTimer->isActive()) m_hideControlsTimer->start(200);
        if (m_stateHinted && !m_lyricsVisible) {
            m_stateHinted    = false;
            m_hintAlphaTarget = 0.f;
            m_hintXTarget     = (float)(width() - m_lyricsHint->width() - 24 + 20);
            updateState();
        }
    }
}