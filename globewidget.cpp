#include "globewidget.h"
#include "track.h"
#include "maptools.h"
#include "geo.h"
#include "theme.h"

#include <QApplication>
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QOpenGLShaderProgram>
#include <QOpenGLContext>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QToolTip>
#include <QImage>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {

const double kEarthRadiusKm = 6378.137;
const double kE2 = 6.69437999014e-3;               // WGS84 第一偏心率平方
const double kMercMaxLat = 1.4844222297453324;     // 85.0511°，Web 墨卡托纬度上限
const int kGridN = 16;                             // 每块瓦片的网格细分数
const float kFovDeg = 45.0f;
const double kMinAlt = 0.0008;                     // ≈ 5 km
const double kMaxAlt = 8.0;
const float kRefinePx = 300.0f;                    // 瓦片投影尺寸超过此像素数就细分
const int kLoadPerFrame = 8;                       // 每帧最多解码的瓦片数，避免卡顿
const int kMaxTextures = 256;
const float kPickTolPx = 6.0f;                     // 鼠标离航迹多近算命中
const int kClickSlopPx = 4;                        // 按下到松开移动不超过它算单击
const double kFlagScale = 1.2;                     // RadarView 旗标缩放默认值
const QColor kRulerColor(0xf5, 0x9e, 0x0b);        // #f59e0b

// RadarView flagRenderer.ts「flag-pin」：32×32 红色图钉，白色描边，中间白点
QPixmap makeFlagPin(qreal dpr)
{
    const int size = 32;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor red(0xff, 0x44, 0x44);
    const QPointF c(size / 2.0, size / 2.0 - 4);
    p.setBrush(red);
    p.setPen(QPen(Qt::white, 2));
    p.drawEllipse(c, 10, 10);
    QPainterPath tail;
    tail.moveTo(size / 2.0 - 5, size / 2.0 + 2);
    tail.lineTo(size / 2.0, size - 4);
    tail.lineTo(size / 2.0 + 5, size / 2.0 + 2);
    p.setPen(Qt::NoPen);
    p.fillPath(tail, red);
    p.strokePath(tail, QPen(Qt::white, 1.5));
    p.setBrush(Qt::white);
    p.drawEllipse(c, 4, 4);
    return pm;
}

// Cesium FILL_AND_OUTLINE 文字：先描边再填充
void drawOutlinedText(QPainter &p, const QPointF &baseline, const QString &text, const QFont &font,
                      const QColor &fill, const QColor &outline, qreal outlineWidth)
{
    QPainterPath path;
    path.addText(baseline, font, text);
    p.strokePath(path, QPen(outline, outlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, fill);
}

QFont pxFont(int px, bool bold = false)
{
    QFont f(QStringLiteral("Microsoft YaHei"));
    f.setFamilies(QStringList() << QStringLiteral("Segoe UI") << QStringLiteral("Microsoft YaHei")
                                << QStringLiteral("WenQuanYi Micro Hei") << QStringLiteral("Noto Sans CJK SC"));
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

// 逻辑 rem 字号 -> 随全局根字号缩放的像素字号（地图内文字同样参与「字号大小」设置）
QFont remFont(double rem, bool bold = false)
{
    return pxFont(ui::px(rem), bold);
}

// 椭球长半轴归一化为 1；顶点位置在着色器里由经纬度算出，所有瓦片共用一套网格
const char *const kVertexSrc = R"(
attribute vec3 a_uv;            // xy: 瓦片内坐标 0..1，z: 1 表示裙边顶点
uniform mat4 u_mvp;
uniform vec3 u_eye;
uniform vec4 u_range;           // 经度起止，纬度起止（墨卡托 y 或纬度）
uniform float u_linearLat;      // 1: u_range.zw 是纬度；0: 是墨卡托 y
uniform float u_skirt;
uniform vec4 u_texXform;        // 纹理坐标偏移与缩放（使用父级瓦片时）
varying vec2 v_tex;
varying float v_light;
void main()
{
    float lon = mix(u_range.x, u_range.y, a_uv.x);
    float t = mix(u_range.z, u_range.w, a_uv.y);
    float lat = u_linearLat > 0.5 ? t : 2.0 * atan(exp(t)) - 1.5707963;
    float sl = sin(lat);
    float cl = cos(lat);
    float n = 1.0 / sqrt(1.0 - 0.00669438 * sl * sl);
    vec3 normal = vec3(cl * cos(lon), cl * sin(lon), sl);
    vec3 pos = vec3(n * cl * cos(lon), n * cl * sin(lon), n * 0.99330562 * sl);
    pos *= 1.0 - a_uv.z * u_skirt;
    v_tex = u_texXform.xy + a_uv.xy * u_texXform.zw;
    v_light = 0.55 + 0.45 * max(dot(normal, normalize(u_eye - pos)), 0.0);
    gl_Position = u_mvp * vec4(pos, 1.0);
}
)";

const char *const kFragmentSrc = R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform float u_useTex;
uniform vec4 u_color;
varying vec2 v_tex;
varying float v_light;
void main()
{
    vec4 c = u_useTex > 0.5 ? texture2D(u_tex, v_tex) : u_color;
    gl_FragColor = vec4(c.rgb * v_light, 1.0);
}
)";

quint64 tileKey(int z, int x, int y)
{
    return (quint64(z) << 48) | (quint64(x) << 24) | quint64(y);
}

double mercToLat(double m)
{
    return 2.0 * std::atan(std::exp(m)) - M_PI / 2.0;
}

// 大地坐标 -> 地心直角坐标（Z 轴指向北极）
QVector3D ecef(double lon, double lat, double h)
{
    const double sl = std::sin(lat), cl = std::cos(lat);
    const double n = 1.0 / std::sqrt(1.0 - kE2 * sl * sl);
    return QVector3D(float((n + h) * cl * std::cos(lon)),
                     float((n + h) * cl * std::sin(lon)),
                     float((n * (1.0 - kE2) + h) * sl));
}

// XYZ 瓦片范围：经度起止，墨卡托 y 起止（北 -> 南，对应纹理从上到下）
QVector4D tileRange(int z, int x, int y)
{
    const double n = double(1 << z);
    return QVector4D(float(x / n * 2.0 * M_PI - M_PI),
                     float((x + 1) / n * 2.0 * M_PI - M_PI),
                     float(M_PI * (1.0 - 2.0 * y / n)),
                     float(M_PI * (1.0 - 2.0 * (y + 1) / n)));
}

void tileBounds(int z, int x, int y, QVector3D *center, float *radius)
{
    const QVector4D r = tileRange(z, x, y);
    QVector3D pts[25];
    QVector3D sum;
    int k = 0;
    for (int j = 0; j < 5; ++j) {
        const double lat = mercToLat(r.z() + (r.w() - r.z()) * j / 4.0);
        for (int i = 0; i < 5; ++i) {
            pts[k] = ecef(r.x() + (r.y() - r.x()) * i / 4.0, lat, 0.0);
            sum += pts[k++];
        }
    }
    const QVector3D c = sum / 25.0f;
    float rad = 0.0f;
    for (const QVector3D &p : pts)
        rad = qMax(rad, (p - c).length());
    *center = c;
    *radius = rad * 1.05f + 1e-4f;
}

} // namespace

// 叠加层画在一个透明子控件上（光栅绘制，不碰 GL）。QPainter 直接画在 QOpenGLWidget 上会走 GL 绘制引擎，
// ANGLE（D3D11/D3D9）下会产生 GL 错误；子控件由 backing store 和 GL 画面合成，对鼠标事件透明
class GlobeOverlay : public QWidget
{
public:
    explicit GlobeOverlay(GlobeWidget *globe) : QWidget(globe), m_globe(globe)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        m_globe->paintOverlay(p);
    }

private:
    GlobeWidget *m_globe;
};

GlobeWidget::GlobeWidget(QWidget *parent)
    : QOpenGLWidget(parent),
      m_northCap(0.80f, 0.85f, 0.90f, 1.0f),
      m_southCap(0.92f, 0.94f, 0.96f, 1.0f),
      m_baseColor(0.16f, 0.26f, 0.38f, 1.0f),
      m_flagScale(kFlagScale)
{
    setMouseTracking(true);
    setMinimumSize(200, 200);
    m_fpsClock.start();
    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(0);
    connect(&m_statusTimer, &QTimer::timeout, this, [this]() {
        emit viewStatusChanged(m_alt * kEarthRadiusKm,
                               m_cursorValid ? qRadiansToDegrees(m_cursorLon) : 0.0,
                               m_cursorValid ? qRadiansToDegrees(m_cursorLat) : 0.0, fps());
    });
    m_camLon = qDegreesToRadians(105.0);
    m_camLat = qDegreesToRadians(35.0);
    m_overlay = new GlobeOverlay(this);
    m_overlay->setGeometry(rect());
    m_overlay->show();
}

GlobeWidget::~GlobeWidget()
{
    cleanupGL();
}

bool GlobeWidget::openTiles(const QString &path, QString *error)
{
    std::unique_ptr<TileSource> src(new TileSource);
    if (!src->open(path, error))
        return false;

    if (m_glReady) {
        makeCurrent();
        clearTextures();
        doneCurrent();
    }
    m_missing.clear();
    m_tiles = std::move(src);
    updateCapColors();
    update();
    emitStatus();
    return true;
}

QString GlobeWidget::tileName() const
{
    return m_tiles ? m_tiles->name() : QString();
}

void GlobeWidget::resetView()
{
    m_camLon = qDegreesToRadians(105.0);
    m_camLat = qDegreesToRadians(35.0);
    m_alt = 2.5;
    update();
}

void GlobeWidget::zoomBy(double steps)
{
    m_alt = qBound(kMinAlt, m_alt * std::pow(0.8, steps), kMaxAlt);
    update();
}

void GlobeWidget::panPixels(const QPoint &from, const QPoint &to)
{
    updateMatrices();
    double lon0, lat0, lon1, lat1;
    if (pick(from, &lon0, &lat0) && pick(to, &lon1, &lat1)) {
        // 抓取式拖动：让鼠标按下处的地面跟着鼠标走
        double dlon = lon0 - lon1;
        if (dlon > M_PI) dlon -= 2.0 * M_PI;
        if (dlon < -M_PI) dlon += 2.0 * M_PI;
        m_camLon += dlon;
        m_camLat += lat0 - lat1;
    } else {
        // 鼠标在地球外：按像素换算角度
        const double k = qMin(m_alt, 2.0) * qDegreesToRadians(double(kFovDeg)) / qMax(1, height());
        m_camLon -= (to.x() - from.x()) * k / qMax(0.2, std::cos(m_camLat));
        m_camLat += (to.y() - from.y()) * k;
    }
    const double latLimit = qDegreesToRadians(89.9);
    m_camLat = qBound(-latLimit, m_camLat, latLimit);
    m_camLon = std::remainder(m_camLon, 2.0 * M_PI);
    update();
}

bool GlobeWidget::geoAt(const QPointF &pos, double *lonDeg, double *latDeg)
{
    updateMatrices();
    double lon, lat;
    if (!pick(pos, &lon, &lat))
        return false;
    *lonDeg = qRadiansToDegrees(lon);
    *latDeg = qRadiansToDegrees(lat);
    return true;
}

bool GlobeWidget::screenPos(double lonDeg, double latDeg, double altM, QPointF *pos)
{
    updateMatrices();
    const QVector4D c = m_mvp * QVector4D(ecef(qDegreesToRadians(lonDeg), qDegreesToRadians(latDeg),
                                               altM / (kEarthRadiusKm * 1000.0)), 1.0f);
    if (c.w() < 1e-6f)
        return false;
    *pos = QPointF((c.x() / c.w() + 1.0) * 0.5 * width(), (1.0 - c.y() / c.w()) * 0.5 * height());
    return true;
}

int GlobeWidget::trackAt(const QPointF &pos)
{
    updateMatrices();
    return m_trackLayer.pick(m_mvp, m_eye, QSizeF(width(), height()), pos, kPickTolPx);
}

void GlobeWidget::lookAt(double lonDeg, double latDeg, double altKm)
{
    m_camLon = qDegreesToRadians(lonDeg);
    m_camLat = qBound(-qDegreesToRadians(89.9), qDegreesToRadians(latDeg), qDegreesToRadians(89.9));
    m_alt = qBound(kMinAlt, altKm / kEarthRadiusKm, kMaxAlt);
    update();
}

void GlobeWidget::setTrackStore(const TrackStore *store)
{
    m_trackStore = store;
    m_trackLayer.setStore(store);
    update();
}

void GlobeWidget::setMapTools(FlagStore *flags, RulerState *ruler)
{
    m_flags = flags;
    m_ruler = ruler;
    connect(flags, &FlagStore::changed, this, [this]() { update(); });
    connect(ruler, &RulerState::changed, this, [this]() { update(); });
    connect(ruler, &RulerState::mouseMoved, this, [this]() { update(); });
}

void GlobeWidget::setShowLabels(bool on)
{
    m_showLabels = on;
    update();
}

bool GlobeWidget::visiblePos(double lonDeg, double latDeg, double altM, QPointF *pos)
{
    updateMatrices();
    const QVector3D p = ecef(qDegreesToRadians(lonDeg), qDegreesToRadians(latDeg), altM / (kEarthRadiusKm * 1000.0));
    if (QVector3D::dotProduct(p, m_eye - p) < 0.0f)
        return false;   // 地球背面
    const QVector4D c = m_mvp * QVector4D(p, 1.0f);
    if (c.w() < 1e-6f)
        return false;
    *pos = QPointF((c.x() / c.w() + 1.0) * 0.5 * width(), (1.0 - c.y() / c.w()) * 0.5 * height());
    return true;
}

// 旗标图标：底边中点对准旗标位置，32px × 0.8 × 缩放
QRectF GlobeWidget::flagIconRect(const QPointF &anchor) const
{
    const double s = 32.0 * 0.8 * m_flagScale;
    return QRectF(anchor.x() - s / 2.0, anchor.y() - s, s, s);
}

QString GlobeWidget::flagAt(const QPointF &pos)
{
    if (!m_flags)
        return QString();
    const QVector<MapFlag> &flags = m_flags->flags();
    for (int i = flags.size() - 1; i >= 0; --i) {   // 后放的在上面
        QPointF sp;
        if (visiblePos(flags[i].lon, flags[i].lat, 0.0, &sp) && flagIconRect(sp).contains(pos))
            return flags[i].id;
    }
    return QString();
}

void GlobeWidget::resetInteraction()
{
    m_tipTrack = m_tipPoint = -1;
    m_trackLayer.setHoveredTrack(-1);
    QToolTip::hideText();
    unsetCursor();
    update();
}

bool GlobeWidget::pointHover(int *track, int *point) const
{
    *track = m_tipTrack;
    *point = m_tipPoint;
    return m_tipTrack >= 0;
}

QString GlobeWidget::trackLabel(const Track &t)
{
    QStringList parts;
    if (!t.flightNo.isEmpty())
        parts << t.flightNo;
    if (!t.aircraftType.isEmpty())
        parts << t.aircraftType;
    return parts.isEmpty() ? t.id : parts.join(QStringLiteral(" | "));
}

// ---------------------------------------------------------------
//  叠加层：标尺、旗标、航迹标签、点迹悬停标签（QPainter 画在 GL 画面之上）
// ---------------------------------------------------------------
QVector<QPolygonF> GlobeWidget::geodesicLines(double lat1, double lon1, double lat2, double lon2)
{
    const QVector3D a = geo::ecefDeg(lon1, lat1, 0.0).normalized();
    const QVector3D b = geo::ecefDeg(lon2, lat2, 0.0).normalized();
    const double ang = std::acos(qBound(-1.0, double(QVector3D::dotProduct(a, b)), 1.0));
    const int n = 64;
    QVector<QPolygonF> out;
    QPolygonF cur;
    for (int i = 0; i <= n; ++i) {
        const double f = double(i) / n;
        QVector3D v;
        if (ang < 1e-9) {
            v = a;
        } else {
            const double s = std::sin(ang);
            v = a * float(std::sin((1 - f) * ang) / s) + b * float(std::sin(f * ang) / s);
        }
        const double lat = qRadiansToDegrees(std::asin(qBound(-1.0, double(v.z()), 1.0)));
        const double lon = qRadiansToDegrees(std::atan2(double(v.y()), double(v.x())));
        QPointF sp;
        if (visiblePos(lon, lat, 0.0, &sp)) {
            cur << sp;
        } else if (!cur.isEmpty()) {
            out << cur;
            cur.clear();
        }
    }
    if (cur.size() > 1)
        out << cur;
    return out;
}

void GlobeWidget::drawRuler(QPainter &p)
{
    m_rulerMarkersDrawn = 0;
    if (!m_ruler || !m_ruler->isActive() || m_ruler->waypoints().isEmpty())
        return;
    const QVector<RulerState::Waypoint> &w = m_ruler->waypoints();
    QPen seg(kRulerColor, 2.0, Qt::CustomDashLine, Qt::FlatCap);
    seg.setDashPattern(QVector<qreal>() << 6.0 << 6.0);   // Cesium dashLength 12：一半实一半空
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i + 1 < w.size(); ++i) {
        p.setPen(seg);
        for (const QPolygonF &line : geodesicLines(w[i].lat, w[i].lon, w[i + 1].lat, w[i + 1].lon))
            p.drawPolyline(line);
    }
    if (m_ruler->hasMouse()) {
        QColor c = kRulerColor;
        c.setAlphaF(0.45);
        QPen pre(c, 1.5, Qt::CustomDashLine, Qt::FlatCap);
        pre.setDashPattern(QVector<qreal>() << 8.0 / 1.5 / 2.0 << 8.0 / 1.5 / 2.0);
        p.setPen(pre);
        for (const QPolygonF &line : geodesicLines(w.last().lat, w.last().lon, m_ruler->mouseLat(), m_ruler->mouseLon()))
            p.drawPolyline(line);
    }
    // 航点：r=11 琥珀色圆、黑色描边、白色粗体序号
    const QFont f = remFont(0.857, true);
    p.setFont(f);
    for (int i = 0; i < w.size(); ++i) {
        QPointF sp;
        if (!visiblePos(w[i].lon, w[i].lat, 0.0, &sp))
            continue;
        p.setPen(QPen(Qt::black, 1.5));
        p.setBrush(kRulerColor);
        const double r = ui::sz(11);
        p.drawEllipse(sp, r, r);
        p.setPen(Qt::white);
        p.drawText(QRectF(sp.x() - r, sp.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QString::number(i + 1));
        ++m_rulerMarkersDrawn;
    }
    p.setBrush(Qt::NoBrush);
}

void GlobeWidget::drawFlags(QPainter &p)
{
    m_flagsDrawn = 0;
    if (!m_flags || m_flags->flags().isEmpty())
        return;
    MapFlag a, b;
    if (m_flags->selectedPair(&a, &b)) {
        QColor c(Qt::yellow);
        c.setAlphaF(0.8);
        p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        for (const QPolygonF &line : geodesicLines(a.lat, a.lon, b.lat, b.lon))
            p.drawPolyline(line);
    }
    const qreal iconDpr = devicePixelRatioF() * 0.8 * m_flagScale;   // 按显示尺寸生成，缩放后不发虚
    if (m_flagIcon.isNull() || !qFuzzyCompare(m_flagIcon.devicePixelRatio(), iconDpr))
        m_flagIcon = makeFlagPin(iconDpr);
    const QFont f = remFont(0.857 * m_flagScale);
    const QFontMetricsF fm(f);
    for (const MapFlag &fl : m_flags->flags()) {
        QPointF sp;
        if (!visiblePos(fl.lon, fl.lat, 0.0, &sp))
            continue;
        p.drawPixmap(flagIconRect(sp), m_flagIcon, QRectF(QPointF(0, 0), QSizeF(m_flagIcon.size())));
        // 标签：黄色，黑色描边 2，顶边在旗标位置下方 round(8 × 缩放) px
        const double w = fm.horizontalAdvance(fl.label);
        const QPointF base(sp.x() - w / 2.0, sp.y() + std::lround(8 * m_flagScale) + fm.ascent());
        drawOutlinedText(p, base, fl.label, f, Qt::yellow, Qt::black, 2.0);
        ++m_flagsDrawn;
    }
}

// RadarView trackRenderer.ts：18px，填充为航迹线颜色、黑色描边 2，底边在端点上方 20px，水平居中
void GlobeWidget::drawLabels(QPainter &p)
{
    m_labelsDrawn = 0;
    if (!m_showLabels || !m_trackStore)
        return;
    const QVector<TrackLayer::EndPoint> &eps = m_trackLayer.endpoints();
    const QVector<Track> &tracks = m_trackStore->tracks();
    const QFont f = remFont(1.286);
    const QFontMetricsF fm(f);
    const qreal dpr = devicePixelRatioF();
    if (m_labelCache.size() > 20000)
        m_labelCache.clear();
    for (const TrackLayer::EndPoint &e : eps) {
        if (e.track < 0 || e.track >= tracks.size())
            continue;
        QPointF sp;
        if (!visiblePos(e.lon, e.lat, TrackLayer::kAltitudeM, &sp))
            continue;
        if (sp.x() < -200 || sp.y() < -50 || sp.x() > width() + 200 || sp.y() > height() + 50)
            continue;
        const QString text = trackLabel(tracks[e.track]);
        const QColor color = m_trackLayer.trackColor(e.track);
        const QString key = text + QLatin1Char('\x1f') + color.name();
        auto it = m_labelCache.find(key);
        if (it == m_labelCache.end()) {
            const QSizeF sz(fm.horizontalAdvance(text) + 6.0, fm.height() + 4.0);
            QPixmap pm(int(std::ceil(sz.width() * dpr)), int(std::ceil(sz.height() * dpr)));
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter lp(&pm);
            lp.setRenderHint(QPainter::Antialiasing);
            drawOutlinedText(lp, QPointF(3.0, 2.0 + fm.ascent()), text, f, color, Qt::black, 2.0);
            lp.end();
            it = m_labelCache.insert(key, pm);
        }
        const QSizeF sz = QSizeF(it->size()) / dpr;
        p.drawPixmap(QPointF(sp.x() - sz.width() / 2.0, sp.y() - ui::sz(20) - sz.height() + 2.0), *it);
        ++m_labelsDrawn;
    }
}

// RadarView pointDotRenderer.ts showPointDotHover：两行，14px 白字黑底，内边距 (8, 6)，在点右侧 点径+10 px
void GlobeWidget::drawPointTip(QPainter &p)
{
    if (m_tipTrack < 0 || !m_trackStore || m_tipTrack >= m_trackStore->size())
        return;
    const Track &t = m_trackStore->tracks()[m_tipTrack];
    if (m_tipPoint < 0 || m_tipPoint >= t.points.size())
        return;
    const TrackPoint &pt = t.points[m_tipPoint];
    QPointF sp;
    if (!visiblePos(pt.lon, pt.lat, TrackLayer::kAltitudeM, &sp))
        return;
    const QString label = !t.flightNo.isEmpty() ? t.flightNo : !t.registration.isEmpty() ? t.registration : t.id;
    const QString l1 = QStringLiteral("%1  ·  %2m  ·  %3kt  ·  %4°")
                           .arg(label).arg(double(pt.alt), 0, 'f', 0).arg(double(pt.speed), 0, 'f', 0)
                           .arg(double(pt.heading), 0, 'f', 0);
    const QString l2 = QDateTime::fromMSecsSinceEpoch(pt.t, Qt::OffsetFromUTC, 8 * 3600)
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QFont f = remFont(1.0);
    const QFontMetricsF fm(f);
    const double w = qMax(fm.horizontalAdvance(l1), fm.horizontalAdvance(l2));
    const double lineH = fm.height();
    const double pad = ui::sz(8), padY = ui::sz(6);
    const QRectF box(sp.x() + m_trackLayer.pointDotPx() + ui::sz(10), sp.y() - lineH - padY,
                     w + 2 * pad, lineH * 2 + 2 * padY);
    p.fillRect(box, Qt::black);
    QColor outline(Qt::black);
    outline.setAlphaF(0.9);
    drawOutlinedText(p, QPointF(box.left() + pad, box.top() + padY + fm.ascent()), l1, f, Qt::white, outline, 2.0);
    drawOutlinedText(p, QPointF(box.left() + pad, box.top() + padY + lineH + fm.ascent()), l2, f, Qt::white, outline, 2.0);
}

void GlobeWidget::paintOverlay(QPainter &p)
{
    m_labelsDrawn = m_flagsDrawn = m_rulerMarkersDrawn = 0;
    if (!m_overlayWanted)
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    drawLabels(p);
    drawRuler(p);
    drawFlags(p);
    drawPointTip(p);
}

void GlobeWidget::resizeEvent(QResizeEvent *e)
{
    QOpenGLWidget::resizeEvent(e);
    if (m_overlay)
        m_overlay->setGeometry(rect());
}

double GlobeWidget::altitudeKm() const
{
    return m_alt * kEarthRadiusKm;
}

QString GlobeWidget::debugInfo() const
{
    return QStringLiteral("gl=[%1] tiles=[%2 z%3-%4] drawn=%5 drawnMaxZ=%6 textures=%7 missing=%8 pending=%9")
               .arg(m_glInfo)
               .arg(m_tiles ? m_tiles->name() : QStringLiteral("none"))
               .arg(m_tiles ? m_tiles->minZoom() : -1)
               .arg(m_tiles ? m_tiles->maxZoom() : -1)
               .arg(m_lastDrawn).arg(m_lastMaxZ)
               .arg(m_textures.size()).arg(m_missing.size()).arg(m_pending)
         + QStringLiteral(" glErrors=%1 alt=%2km lon=%3 lat=%4")
               .arg(m_glErrors)
               .arg(m_alt * kEarthRadiusKm, 0, 'f', 1)
               .arg(qRadiansToDegrees(m_camLon), 0, 'f', 3)
               .arg(qRadiansToDegrees(m_camLat), 0, 'f', 3);
}

// ---------------------------------------------------------------
//  OpenGL 资源
// ---------------------------------------------------------------
void GlobeWidget::initializeGL()
{
    initializeOpenGLFunctions();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed,
            this, &GlobeWidget::cleanupGL, Qt::UniqueConnection);

    m_glInfo = QStringLiteral("%1 | %2%3")
                   .arg(QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_RENDERER))))
                   .arg(QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_VERSION))))
                   .arg(context()->isOpenGLES() ? QStringLiteral(" (ES)") : QString());

    m_prog = new QOpenGLShaderProgram;
    bool ok = m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc)
           && m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentSrc);
    if (ok) {
        m_prog->bindAttributeLocation("a_uv", 0);
        ok = m_prog->link();
    }
    if (!ok) {
        m_glInfo += QStringLiteral(" | shader error: ") + m_prog->log();
        qWarning("GlobeWidget shader error: %s", qPrintable(m_prog->log()));
        delete m_prog;
        m_prog = nullptr;
        return;
    }
    m_uMvp = m_prog->uniformLocation("u_mvp");
    m_uEye = m_prog->uniformLocation("u_eye");
    m_uRange = m_prog->uniformLocation("u_range");
    m_uLinearLat = m_prog->uniformLocation("u_linearLat");
    m_uSkirt = m_prog->uniformLocation("u_skirt");
    m_uTexXform = m_prog->uniformLocation("u_texXform");
    m_uUseTex = m_prog->uniformLocation("u_useTex");
    m_uColor = m_prog->uniformLocation("u_color");
    m_uTex = m_prog->uniformLocation("u_tex");

    // 共用网格：(N+1)^2 个面顶点 + 四条边各 N+1 个裙边顶点（遮住相邻层级之间的缝）
    const int n = kGridN;
    auto gridIndex = [n](int i, int j) { return GLushort(j * (n + 1) + i); };
    auto edgePoint = [n](int e, int k, int *i, int *j) {
        switch (e) {
        case 0: *i = k; *j = 0; break;
        case 1: *i = k; *j = n; break;
        case 2: *i = 0; *j = k; break;
        default: *i = n; *j = k; break;
        }
    };

    QVector<GLfloat> verts;
    QVector<GLushort> idx;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            verts << GLfloat(i) / n << GLfloat(j) / n << 0.0f;
    const int skirtBase = (n + 1) * (n + 1);
    for (int e = 0; e < 4; ++e) {
        for (int k = 0; k <= n; ++k) {
            int i, j;
            edgePoint(e, k, &i, &j);
            verts << GLfloat(i) / n << GLfloat(j) / n << 1.0f;
        }
    }
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const GLushort a = gridIndex(i, j), b = gridIndex(i + 1, j);
            const GLushort c = gridIndex(i, j + 1), d = gridIndex(i + 1, j + 1);
            idx << a << c << b << b << c << d;
        }
    }
    for (int e = 0; e < 4; ++e) {
        for (int k = 0; k < n; ++k) {
            int i0, j0, i1, j1;
            edgePoint(e, k, &i0, &j0);
            edgePoint(e, k + 1, &i1, &j1);
            const GLushort a = gridIndex(i0, j0), b = gridIndex(i1, j1);
            const GLushort sa = GLushort(skirtBase + e * (n + 1) + k), sb = GLushort(sa + 1);
            idx << a << b << sb << a << sb << sa;
        }
    }

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(verts.constData(), int(verts.size() * sizeof(GLfloat)));
    m_vbo.release();
    m_ibo.create();
    m_ibo.bind();
    m_ibo.allocate(idx.constData(), int(idx.size() * sizeof(GLushort)));
    m_ibo.release();
    m_indexCount = idx.size();

    if (!m_trackLayer.initialize(this))
        m_glInfo += QStringLiteral(" | track shader error: ") + m_trackLayer.shaderLog();
    m_trackLayer.setStore(m_trackStore);

    m_glReady = true;
}

void GlobeWidget::cleanupGL()
{
    if (!m_glReady)
        return;
    makeCurrent();
    clearTextures();
    m_trackLayer.cleanup();
    m_vbo.destroy();
    m_ibo.destroy();
    delete m_prog;
    m_prog = nullptr;
    m_glReady = false;
    doneCurrent();
}

void GlobeWidget::clearTextures()
{
    for (const CachedTexture &c : qAsConst(m_textures))
        glDeleteTextures(1, &c.tex);
    m_textures.clear();
}

// ---------------------------------------------------------------
//  相机与瓦片选择
// ---------------------------------------------------------------
void GlobeWidget::updateMatrices()
{
    const double sl = std::sin(m_camLat), cl = std::cos(m_camLat);
    const double so = std::sin(m_camLon), co = std::cos(m_camLon);
    m_eye = ecef(m_camLon, m_camLat, m_alt);
    const QVector3D target = ecef(m_camLon, m_camLat, 0.0);
    const QVector3D north(float(-sl * co), float(-sl * so), float(cl));

    m_view.setToIdentity();
    m_view.lookAt(m_eye, target, north);

    const float aspect = float(qMax(1, width())) / float(qMax(1, height()));
    const float nearP = float(qMax(m_alt * 0.05, 1e-5));
    const float farP = float(std::sqrt(m_alt * (2.0 + m_alt)) + 0.2);   // 到地平线的距离 + 余量
    m_proj.setToIdentity();
    m_proj.perspective(kFovDeg, aspect, nearP, farP);
    m_mvp = m_proj * m_view;

    // 从 MVP 提取视锥 6 个平面，用于剔除
    const QVector4D r0 = m_mvp.row(0), r1 = m_mvp.row(1), r2 = m_mvp.row(2), r3 = m_mvp.row(3);
    const QVector4D planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2 };
    for (int i = 0; i < 6; ++i)
        m_planes[i] = planes[i] / planes[i].toVector3D().length();

    m_focalPx = float(qMax(1, height()) / (2.0 * std::tan(qDegreesToRadians(kFovDeg) / 2.0)));
}

// 四叉树遍历：背面（地平线以下）和视锥外的瓦片剔除，近处瓦片细分到更高层级
void GlobeWidget::collectTiles(int z, int x, int y, QVector<TileId> &out) const
{
    const int maxZ = m_tiles ? qMax(2, m_tiles->maxZoom()) : 3;
    bool refine = z < 2;    // 至少细分到 2 级，保证椭球形状足够圆
    if (z >= 2) {
        QVector3D c;
        float r;
        tileBounds(z, x, y, &c, &r);
        const float eyeLen = m_eye.length();
        if (QVector3D::dotProduct(c, m_eye / eyeLen) + r < 1.0f / eyeLen - 0.005f)
            return;
        for (const QVector4D &p : m_planes) {
            if (QVector3D::dotProduct(p.toVector3D(), c) + p.w() < -r)
                return;
        }
        if (z < maxZ) {
            const float d = qMax((m_eye - c).length() - r, 1e-6f);
            refine = 2.0f * r / d * m_focalPx > kRefinePx;
        }
    }
    if (refine) {
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                collectTiles(z + 1, 2 * x + dx, 2 * y + dy, out);
    } else {
        out.append(TileId{z, x, y});
    }
}

// 自身纹理未加载时向上找最近的父级纹理，并算出对应的子区域纹理坐标
GLuint GlobeWidget::textureFor(const TileId &t, QVector4D *xform)
{
    if (!m_tiles)
        return 0;
    for (int az = qMin(t.z, m_tiles->maxZoom()); az >= m_tiles->minZoom(); --az) {
        const int dz = t.z - az;
        const int ax = t.x >> dz, ay = t.y >> dz;
        auto it = m_textures.find(tileKey(az, ax, ay));
        if (it == m_textures.end())
            continue;
        it->lastUsed = m_frame;
        const float s = 1.0f / float(1 << dz);
        *xform = QVector4D((t.x - (ax << dz)) * s, (t.y - (ay << dz)) * s, s, s);
        return it->tex;
    }
    return 0;
}

void GlobeWidget::loadTexture(int z, int x, int y)
{
    const quint64 key = tileKey(z, x, y);
    const QImage img = m_tiles->tile(z, x, y);
    if (img.isNull()) {
        m_missing.insert(key);
        return;
    }
    // 只用 OpenGL ES 2.0 子集（GL_RGBA + glGenerateMipmap），ANGLE/D3D 下不报错
    // QOpenGLTexture 会设置 ES2 没有的 GL_TEXTURE_MAX_LEVEL 等参数，Win7 走 ANGLE 时会产生 GL_INVALID_ENUM
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    const bool pot = (rgba.width() & (rgba.width() - 1)) == 0 && (rgba.height() & (rgba.height() - 1)) == 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    // ES2 只允许 2 的幂尺寸纹理生成 mipmap
    if (pot)
        glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pot ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_textures.insert(key, CachedTexture{tex, m_frame});
}

// 超出上限时释放最久未用的纹理；0、1 级常驻，作为兜底
void GlobeWidget::evictTextures()
{
    int excess = m_textures.size() - kMaxTextures;
    if (excess <= 0)
        return;
    QVector<QPair<quint64, quint64>> candidates;   // (lastUsed, key)
    for (auto it = m_textures.constBegin(); it != m_textures.constEnd(); ++it) {
        if ((it.key() >> 48) > 1 && it->lastUsed + 1 < m_frame)
            candidates.append(qMakePair(it->lastUsed, it.key()));
    }
    std::sort(candidates.begin(), candidates.end());
    for (int i = 0; i < candidates.size() && excess > 0; ++i, --excess) {
        const GLuint tex = m_textures.value(candidates[i].second).tex;
        glDeleteTextures(1, &tex);
        m_textures.remove(candidates[i].second);
    }
}

// 极地补洞的颜色取 0 级瓦片最上/最下一行的平均色，和瓦片边缘自然衔接
void GlobeWidget::updateCapColors()
{
    m_northCap = QVector4D(0.80f, 0.85f, 0.90f, 1.0f);
    m_southCap = QVector4D(0.92f, 0.94f, 0.96f, 1.0f);
    if (!m_tiles || m_tiles->minZoom() > 0)
        return;
    const QImage img = m_tiles->tile(0, 0, 0).convertToFormat(QImage::Format_RGB32);
    if (img.isNull())
        return;
    auto rowAverage = [&img](int row) {
        double r = 0, g = 0, b = 0;
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, row);
            r += qRed(p);
            g += qGreen(p);
            b += qBlue(p);
        }
        const double s = 255.0 * img.width();
        return QVector4D(float(r / s), float(g / s), float(b / s), 1.0f);
    };
    m_northCap = rowAverage(0);
    m_southCap = rowAverage(img.height() - 1);
}

// ---------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------
void GlobeWidget::drawPatch(const QVector4D &range)
{
    m_prog->setUniformValue(m_uRange, range);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_SHORT, nullptr);
}

void GlobeWidget::paintGL()
{
    ++m_frame;
    // 上一帧 QPainter 叠加层可能留下的状态
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, int(width() * devicePixelRatioF()), int(height() * devicePixelRatioF()));
    glClearColor(0.118f, 0.118f, 0.118f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_prog || width() <= 0 || height() <= 0)
        return;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    updateMatrices();

    // 预载最低两级，保证任何位置都有父级纹理可用
    if (m_tiles && m_tiles->minZoom() <= 1) {
        for (int z = m_tiles->minZoom(); z <= qMin(1, m_tiles->maxZoom()); ++z) {
            for (int y = 0; y < (1 << z); ++y) {
                for (int x = 0; x < (1 << z); ++x) {
                    const quint64 key = tileKey(z, x, y);
                    if (!m_textures.contains(key) && !m_missing.contains(key))
                        loadTexture(z, x, y);
                }
            }
        }
    }

    QVector<TileId> tiles;
    tiles.reserve(256);
    collectTiles(0, 0, 0, tiles);

    // 加载缺失纹理：低层级优先，每帧有上限，剩下的下一帧继续
    QVector<TileId> wanted;
    if (m_tiles) {
        for (const TileId &t : qAsConst(tiles)) {
            const quint64 key = tileKey(t.z, t.x, t.y);
            if (t.z >= m_tiles->minZoom() && t.z <= m_tiles->maxZoom()
                    && !m_textures.contains(key) && !m_missing.contains(key))
                wanted.append(t);
        }
        std::sort(wanted.begin(), wanted.end(),
                  [](const TileId &a, const TileId &b) { return a.z < b.z; });
    }
    const int loadCount = qMin(wanted.size(), kLoadPerFrame);
    for (int i = 0; i < loadCount; ++i)
        loadTexture(wanted[i].z, wanted[i].x, wanted[i].y);
    m_pending = wanted.size() - loadCount;

    m_prog->bind();
    m_vbo.bind();
    m_prog->enableAttributeArray(0);
    m_prog->setAttributeBuffer(0, GL_FLOAT, 0, 3, 3 * sizeof(GLfloat));
    m_ibo.bind();
    m_prog->setUniformValue(m_uMvp, m_mvp);
    m_prog->setUniformValue(m_uEye, m_eye);
    m_prog->setUniformValue(m_uTex, 0);
    glActiveTexture(GL_TEXTURE0);

    // 极地：墨卡托瓦片覆盖不到 ±85.05° 以外，用纯色补齐
    m_prog->setUniformValue(m_uUseTex, 0.0f);
    m_prog->setUniformValue(m_uLinearLat, 1.0f);
    m_prog->setUniformValue(m_uSkirt, 0.01f);
    for (int q = 0; q < 4; ++q) {
        const float lon0 = float(-M_PI + q * M_PI / 2.0), lon1 = float(lon0 + M_PI / 2.0);
        m_prog->setUniformValue(m_uColor, m_northCap);
        drawPatch(QVector4D(lon0, lon1, float(M_PI / 2.0), float(kMercMaxLat)));
        m_prog->setUniformValue(m_uColor, m_southCap);
        drawPatch(QVector4D(lon0, lon1, float(-kMercMaxLat), float(-M_PI / 2.0)));
    }

    m_prog->setUniformValue(m_uLinearLat, 0.0f);
    int maxZ = 0;
    for (const TileId &t : qAsConst(tiles)) {
        QVector4D xform;
        if (const GLuint tex = textureFor(t, &xform)) {
            glBindTexture(GL_TEXTURE_2D, tex);
            m_prog->setUniformValue(m_uUseTex, 1.0f);
            m_prog->setUniformValue(m_uTexXform, xform);
        } else {
            m_prog->setUniformValue(m_uUseTex, 0.0f);
            m_prog->setUniformValue(m_uColor, m_baseColor);
        }
        // 裙边深度取粗两级网格的弦高，足以盖住相邻层级之间的裂缝
        const double angle = 2.0 * M_PI / double(1 << t.z) / kGridN;
        m_prog->setUniformValue(m_uSkirt, float(2.0 * angle * angle + 1e-5));
        drawPatch(tileRange(t.z, t.x, t.y));
        maxZ = qMax(maxZ, t.z);
    }

    m_ibo.release();
    m_prog->disableAttributeArray(0);
    m_vbo.release();
    m_prog->release();

    // 航迹画在瓦片之上；深度测试保留，地球背面的航迹被遮挡
    m_trackLayer.draw(m_mvp, QSize(int(width() * devicePixelRatioF()), int(height() * devicePixelRatioF())),
                      float(devicePixelRatioF()));

    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        ++m_glErrors;

    // 文字和图标由叠加层控件画（有内容时，或刚从有内容变为没有、需要擦掉时才重画）
    const bool overlay = (m_showLabels && m_trackLayer.visibleTrackCount() > 0)
                         || (m_flags && !m_flags->flags().isEmpty())
                         || (m_ruler && m_ruler->isActive() && !m_ruler->waypoints().isEmpty())
                         || m_tipTrack >= 0;
    if (overlay || m_overlayWanted) {
        m_overlayWanted = overlay;
        m_overlay->update();
    }

    m_lastDrawn = tiles.size();
    m_lastMaxZ = maxZ;
    evictTextures();
    trackFps();
    emitStatus();
    if (m_pending > 0)
        update();
}

// ---------------------------------------------------------------
//  交互
// ---------------------------------------------------------------
// 鼠标射线与椭球求交（先把 z 轴按 b/a 缩放，椭球变成单位球）
bool GlobeWidget::pick(const QPointF &pos, double *lon, double *lat)
{
    if (width() <= 0 || height() <= 0)
        return false;
    bool ok = false;
    const QMatrix4x4 inv = m_mvp.inverted(&ok);
    if (!ok)
        return false;
    const float nx = float(2.0 * pos.x() / width() - 1.0);
    const float ny = float(1.0 - 2.0 * pos.y() / height());
    const QVector3D dir = (inv.map(QVector3D(nx, ny, 1.0f)) - inv.map(QVector3D(nx, ny, -1.0f))).normalized();

    const double bz = std::sqrt(1.0 - kE2);
    const double ox = m_eye.x(), oy = m_eye.y(), oz = m_eye.z() / bz;
    const double dx = dir.x(), dy = dir.y(), dz = dir.z() / bz;
    const double a = dx * dx + dy * dy + dz * dz;
    const double b = 2.0 * (ox * dx + oy * dy + oz * dz);
    const double c = ox * ox + oy * oy + oz * oz - 1.0;
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0)
        return false;
    const double t = (-b - std::sqrt(disc)) / (2.0 * a);
    if (t < 0.0)
        return false;
    const double x = ox + t * dx, y = oy + t * dy, z = (oz + t * dz) * bz;
    *lon = std::atan2(y, x);
    *lat = std::atan2(z, (1.0 - kE2) * std::sqrt(x * x + y * y));
    return true;
}

void GlobeWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_clickCandidate = true;
        m_lastPos = m_pressPos = e->pos();
    }
}

void GlobeWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging) {
        if (m_clickCandidate && (e->pos() - m_pressPos).manhattanLength() > kClickSlopPx) {
            m_clickCandidate = false;
            setCursor(Qt::ClosedHandCursor);
            QToolTip::hideText();
            m_trackLayer.setHoveredTrack(-1);
        }
        if (!m_clickCandidate) {
            panPixels(m_lastPos, e->pos());
            m_lastPos = e->pos();
        }
    } else {
        updateHover(e->pos(), e->globalPos());
    }
    updateMatrices();
    m_cursorValid = pick(e->pos(), &m_cursorLon, &m_cursorLat);
    if (m_ruler && m_ruler->isActive())
        m_ruler->setMouse(m_cursorValid, qRadiansToDegrees(m_cursorLat), qRadiansToDegrees(m_cursorLon));
    emitStatus();
}

void GlobeWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const bool click = m_clickCandidate;
    m_dragging = false;
    m_clickCandidate = false;
    unsetCursor();
    if (click) {
        if (m_ruler && m_ruler->isActive()) {
            // 标尺模式：单击只放航点，不选航迹（RadarView interactionHandler LEFT_CLICK）
            double lon, lat;
            if (geoAt(e->pos(), &lon, &lat))
                m_ruler->addWaypoint(lat, lon);
        } else {
            emit trackClicked(trackAt(e->pos()));
        }
    }
    updateHover(e->pos(), e->globalPos());
}

// 双击：在旗标上删除，否则在该处放旗标（RadarView LEFT_DOUBLE_CLICK）
void GlobeWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !m_flags)
        return;
    const QString id = flagAt(e->pos());
    if (!id.isEmpty()) {
        m_flags->removeFlag(id);
        return;
    }
    double lon, lat;
    if (geoAt(e->pos(), &lon, &lat))
        m_flags->addFlag(lat, lon);
}

void GlobeWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QToolTip::hideText();
    e->accept();
    if (m_ruler && m_ruler->isActive())
        return;   // 标尺模式下右键不弹菜单
    const QString flag = flagAt(e->pos());
    if (!flag.isEmpty()) {
        emit flagContextMenuRequested(flag, e->globalPos());
        return;
    }
    emit trackContextMenuRequested(trackAt(e->pos()), e->globalPos());
}

void GlobeWidget::leaveEvent(QEvent *e)
{
    QOpenGLWidget::leaveEvent(e);
    m_cursorValid = false;     // 与 RadarView 一致：鼠标离开地图后经纬度归零
    emitStatus();
    if (m_ruler && m_ruler->isActive())
        m_ruler->setMouse(false, 0.0, 0.0);
    if (m_tipTrack >= 0) {
        m_tipTrack = m_tipPoint = -1;
        update();
    }
    if (m_trackLayer.hoveredTrack() >= 0) {
        m_trackLayer.setHoveredTrack(-1);
        QToolTip::hideText();
        unsetCursor();
        update();
    }
}

// 悬停：红色高亮 + 提示框（航班号、来源、点数、时间范围）
void GlobeWidget::updateHover(const QPoint &pos, const QPoint &globalPos)
{
    const int idx = trackAt(pos);
    // 点迹悬停：拾取到的航迹若显示了点迹，找离鼠标最近的点，在点旁边显示该点数据，不再弹航迹提示框
    int tipTrack = -1, tipPoint = -1;
    if (idx >= 0 && m_trackLayer.hasPointDots(idx)) {
        updateMatrices();
        if (m_trackLayer.pickPointDot(m_mvp, m_eye, QSizeF(width(), height()), pos, idx, &tipPoint))
            tipTrack = idx;
    }
    if (tipTrack != m_tipTrack || tipPoint != m_tipPoint) {
        m_tipTrack = tipTrack;
        m_tipPoint = tipPoint;
        update();
    }
    if (idx == m_trackLayer.hoveredTrack()) {
        if (idx >= 0 && tipTrack < 0)
            QToolTip::showText(globalPos, trackTooltip(idx), this);   // 跟随鼠标
        else if (tipTrack >= 0)
            QToolTip::hideText();
        return;
    }
    m_trackLayer.setHoveredTrack(idx);
    if (idx >= 0) {
        setCursor(Qt::PointingHandCursor);
        if (tipTrack < 0)
            QToolTip::showText(globalPos, trackTooltip(idx), this);
        else
            QToolTip::hideText();
    } else {
        unsetCursor();
        QToolTip::hideText();
    }
    update();
}

QString GlobeWidget::trackTooltip(int index) const
{
    if (!m_trackStore || index < 0 || index >= m_trackStore->size())
        return QString();
    const Track &t = m_trackStore->tracks().at(index);
    QString title = t.flightNo.isEmpty() ? t.id : t.flightNo;
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped());
    if (!t.flightNo.isEmpty() && t.flightNo != t.id)
        lines << tr("ICAO：%1").arg(t.id.toHtmlEscaped());
    lines << tr("来源：%1（%2）").arg(trackSourceName(t.source), t.fileName.toHtmlEscaped());
    if (!t.aircraftType.isEmpty())
        lines << tr("机型：%1").arg(t.aircraftType.toHtmlEscaped());
    if (!t.origin.isEmpty() || !t.destination.isEmpty())
        lines << tr("航线：%1 → %2").arg(t.origin.toHtmlEscaped(), t.destination.toHtmlEscaped());
    lines << tr("点数：%1").arg(t.points.size());
    lines << tr("时间：%1 ~ %2").arg(formatBeijingTime(t.minTime()), formatBeijingTime(t.maxTime()));
    return QStringLiteral("<qt style='white-space:pre'>") + lines.join(QStringLiteral("<br>"))
         + QStringLiteral("</qt>");
}

void GlobeWidget::wheelEvent(QWheelEvent *e)
{
    zoomBy(e->angleDelta().y() / 120.0);
    e->accept();
}

// 只排队：真正的信号在事件循环下一轮发出。paintGL 里会调到这里，绘制过程中直接改状态栏文字
// 会触发其他控件的重绘和重新布局，可能让 QOpenGLWidget 的合成拖慢甚至错过更新
void GlobeWidget::emitStatus()
{
    if (!m_statusTimer.isActive())
        m_statusTimer.start();
}

int GlobeWidget::fps() const
{
    return int(std::lround(m_fpsSmoothed));
}

// 与 RadarView viewerCore.ts setupFpsTracking 相同：每 500 ms（且至少 5 帧）采样一次，
// 与上次结果对半平滑；超过 1.5 s 没有新帧（画面静止）则归零，状态栏显示 "--"
void GlobeWidget::trackFps()
{
    const qint64 now = m_fpsClock.elapsed();
    if (m_fpsLastSample < 0) {
        m_fpsLastSample = now;
        m_fpsFrames = 1;
        return;
    }
    ++m_fpsFrames;
    const qint64 elapsed = now - m_fpsLastSample;
    if (elapsed >= 500 && m_fpsFrames >= 5) {
        const double instant = m_fpsFrames / (elapsed / 1000.0);
        m_fpsSmoothed = m_fpsSmoothed == 0.0 ? instant : m_fpsSmoothed * 0.5 + instant * 0.5;
        m_fpsFrames = 0;
        m_fpsLastSample = now;
    }
    if (elapsed > 1500) {
        m_fpsSmoothed = 0.0;
        m_fpsFrames = 0;
        m_fpsLastSample = now;
    }
}
