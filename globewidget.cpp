#include "globewidget.h"
#include "crs.h"
#include "netmap.h"
#include "track.h"
#include "maptools.h"
#include "spatiallasso.h"
#include "drawingmanager.h"
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
#include <QLineF>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {

const double kEarthRadiusKm = 6378.137;
const double kE2 = 6.69437999014e-3;               // WGS84 第一偏心率平方
const double kMercMaxLat = 1.4844222297453324;     // 85.0511°，Web 墨卡托纬度上限
const int kGridN = 16;                             // 每块瓦片的网格细分数
const float kFovDeg = 45.0f;
// 相机离地最低高度（地球半径单位）。原来是 0.0008（≈5km），那个高度下瓦片最多细分到
// z15、在屏幕上只有 153px（原生 256px 的 0.6 倍），地名标签被缩到看不清。
// 降到 ≈320m 后：天地图（原生 z18）能以 1:1 显示，高德（原生 z17）约 2.5 倍放大，
// 文字才真正可读。代价是视野只有两百多米，需要放大缩小来回看。
const double kMinAlt = 0.00005;                    // ≈ 320 m
const double kMaxAlt = 8.0;
const float kRefinePx = 300.0f;                    // 瓦片投影尺寸超过此像素数就细分
const int kLoadPerFrame = 32;                      // 每帧最多解码的瓦片数，避免卡顿
const int kMaxTextures = 512;                      // 纹理 LRU 上限；一屏约 300 张，留一倍余量让回看不重解码
const float kPickTolPx = 6.0f;                     // 鼠标离航迹多近算命中
const int kClickSlopPx = 4;                        // 按下到松开移动不超过它算单击
const double kFlagScale = 1.2;                     // RadarView 旗标缩放默认值
const QColor kRulerColor(0xf5, 0x9e, 0x0b);        // #f59e0b
const QColor kLassoColor(0x10, 0xb9, 0x81);        // #10b981（RadarView 套索绿）
const QColor kSelectColor(0x3b, 0x82, 0xf6);       // #3b82f6（选中高亮蓝，和绘图橙区分开）
const double kSiteDotPx = 6.0;                      // 站点圆点半径：与 TrackLayer 端点圆球基准 6px 一致
const double kSnapPx = 10.0;                       // 吸附半径（屏幕像素）

// 点到线段的屏幕距离（橡皮擦命中测试）
double distToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 < 1e-12)
        return QLineF(p, a).length();
    double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2;
    t = qBound(0.0, t, 1.0);
    return QLineF(p, QPointF(a.x() + ab.x() * t, a.y() + ab.y() * t)).length();
}

// 屏幕线段与矩形是否相交（框选删除用；任一顶点在框内也算）
bool segmentHitsRect(const QPointF &a, const QPointF &b, const QRectF &r)
{
    if (r.contains(a) || r.contains(b))
        return true;
    const QPointF corner[4] = { r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft() };
    auto cross = [](const QPointF &o, const QPointF &p, const QPointF &q) {
        return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x());
    };
    for (int i = 0; i < 4; ++i) {
        const QPointF &c = corner[i], &d = corner[(i + 1) % 4];
        const double d1 = cross(c, d, a), d2 = cross(c, d, b);
        const double d3 = cross(a, b, c), d4 = cross(a, b, d);
        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0))
            && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
            return true;
    }
    return false;
}

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
                               m_cursorValid ? qRadiansToDegrees(m_cursorLat) : 0.0, fps(),
                               m_cursorValid);
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

// 场景坐标系由底图决定：高德瓦片网格是 GCJ-02；天地图和离线 mbtiles 是 WGS84。
// 离线包可以在 metadata 里写 crs=GCJ-02 声明（有些用高德瓦片抓的包就是加密坐标）。
bool GlobeWidget::sceneIsGcj() const
{
    if (m_online)
        return m_online->source() == NetTileSource::Source::Amap;
    return m_tiles && m_tiles->isGcj();
}

void GlobeWidget::toScene(double lonWgs, double latWgs, double *lonScene, double *latScene) const
{
    if (!m_sceneCrs) {
        *lonScene = lonWgs;
        *latScene = latWgs;
        return;
    }
    crs::wgs84ToGcj02(lonWgs, latWgs, lonScene, latScene);
}

void GlobeWidget::fromScene(double lonScene, double latScene, double *lonWgs, double *latWgs) const
{
    if (!m_sceneCrs) {
        *lonWgs = lonScene;
        *latWgs = latScene;
        return;
    }
    crs::gcj02ToWgs84(lonScene, latScene, lonWgs, latWgs);
}

void GlobeWidget::applySceneCrs()
{
    const bool gcj = sceneIsGcj();
    if (gcj == m_sceneCrs)
        return;

    // 相机记的也是场景坐标：先按旧基准还原成 WGS84，再按新基准换算，
    // 这样切换底图时视野仍停在同一个真实地点（否则街道级下会跳 500m）
    const double camLon = qRadiansToDegrees(m_camLon);
    const double camLat = qRadiansToDegrees(m_camLat);
    double wlon = camLon, wlat = camLat;
    if (m_sceneCrs)
        crs::gcj02ToWgs84(camLon, camLat, &wlon, &wlat);
    m_sceneCrs = gcj;
    double slon = wlon, slat = wlat;
    if (gcj)
        crs::wgs84ToGcj02(wlon, wlat, &slon, &slat);
    m_camLon = qDegreesToRadians(slon);
    m_camLat = qDegreesToRadians(slat);
    m_trackLayer.setSceneGcj(gcj);      // 重建航迹几何：坐标系变了，旧顶点全部失效
    update();
    emitStatus();
}

bool GlobeWidget::openTiles(const QString &path, QString *error)
{
    std::unique_ptr<TileSource> src(new TileSource);
    if (!src->open(path, error))
        return false;

    // 打开本地包 = 只要这份离线数据：把在线源整个关掉。
    // 否则 m_online 还活着，loadTexture() 会走网络分支，屏幕上看到的是
    // 在线瓦片或它的磁盘缓存，本地包反而没被用上。
    if (m_online) {
        delete m_online;
        m_online = nullptr;
    }

    if (m_glReady) {
        makeCurrent();
        clearTextures();
        doneCurrent();
    }
    m_missing.clear();
    m_pendingTiles.clear();     // 上一个源还在路上的瓦片不能漏进新包
    m_tiles = std::move(src);
    applySceneCrs();
    updateCapColors();
    update();
    emitStatus();
    return true;
}

QString GlobeWidget::tileName() const
{
    if (m_online)
        return m_online->name();
    return m_tiles ? m_tiles->name() : QString();
}

QString GlobeWidget::mapSourceName() const
{
    return tileName();
}

bool GlobeWidget::onlineBusy() const
{
    return m_online && m_online->isBusy();
}

// 打开在线底图：本地 mbtiles 不释放，网络瓦片缺失时还能拿它兜底
bool GlobeWidget::openOnlineMap(const QString &token, bool useAmap, QString *error)
{
    if (token.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("没有填写天地图 key");
        return false;
    }
    if (!m_online) {
        m_online = new NetTileSource(this);
        connect(m_online, &NetTileSource::tileReady, this,
                [this](int z, int x, int y, const QImage &img) {
                    // 网络回调在 GUI 线程，纹理上传留到 paintGL 里做
                    m_pendingTiles.insert(tileKey(z, x, y), img);
                    update();
                });
        connect(m_online, &NetTileSource::tileFailed, this, [this](int z, int x, int y) {
            m_missing.insert(tileKey(z, x, y));
            update();
        });
    }
    m_online->setSource(useAmap ? NetTileSource::Source::Amap : NetTileSource::Source::Tianditu);
    m_online->setKey(token);

    if (m_glReady) {
        makeCurrent();
        clearTextures();
        doneCurrent();
    }
    m_missing.clear();
    m_pendingTiles.clear();
    applySceneCrs();
    updateCapColors();
    update();
    emitStatus();
    return true;
}

void GlobeWidget::closeOnlineMap()
{
    if (!m_online)
        return;
    delete m_online;
    m_online = nullptr;
    m_pendingTiles.clear();
    m_missing.clear();
    if (m_glReady) {
        makeCurrent();
        clearTextures();
        doneCurrent();
    }
    applySceneCrs();
    updateCapColors();
    update();
    emitStatus();
}

int GlobeWidget::mapMinZoom() const
{
    if (m_online)
        return m_online->minZoom();
    return m_tiles ? m_tiles->minZoom() : 0;
}

int GlobeWidget::mapMaxZoom() const
{
    if (m_online)
        return m_online->maxZoom();
    return m_tiles ? m_tiles->maxZoom() : 3;
}

void GlobeWidget::resetView()
{
    m_camLon = qDegreesToRadians(105.0);
    m_camLat = qDegreesToRadians(35.0);
    m_alt = 2.5;
    update();
    emit cameraChanged();
}

void GlobeWidget::cameraView(double *lonDeg, double *latDeg, double *altKm) const
{
    double lonW = 0.0, latW = 0.0;
    fromScene(qRadiansToDegrees(m_camLon), qRadiansToDegrees(m_camLat), &lonW, &latW);
    if (lonDeg)
        *lonDeg = lonW;
    if (latDeg)
        *latDeg = latW;
    if (altKm)
        *altKm = m_alt * kEarthRadiusKm;
}

void GlobeWidget::setCameraView(double lonDeg, double latDeg, double altKm)
{
    lookAt(lonDeg, latDeg, altKm);
}

void GlobeWidget::zoomBy(double steps)
{
    m_alt = qBound(kMinAlt, m_alt * std::pow(0.8, steps), kMaxAlt);
    update();
    emit cameraChanged();
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
    double lon, lat;                     // 场景坐标（弧度）
    if (!pick(pos, &lon, &lat))
        return false;
    fromScene(qRadiansToDegrees(lon), qRadiansToDegrees(lat), lonDeg, latDeg);
    return true;
}

// 场景坐标（= 当前底图的坐标系）→ 屏幕，不做任何转换
bool GlobeWidget::sceneScreenPos(double lonDeg, double latDeg, double altM, QPointF *pos)
{
    updateMatrices();
    const QVector4D c = m_mvp * QVector4D(ecef(qDegreesToRadians(lonDeg), qDegreesToRadians(latDeg),
                                               altM / (kEarthRadiusKm * 1000.0)), 1.0f);
    if (c.w() < 1e-6f)
        return false;
    *pos = QPointF((c.x() / c.w() + 1.0) * 0.5 * width(), (1.0 - c.y() / c.w()) * 0.5 * height());
    return true;
}

bool GlobeWidget::screenPos(double lonDeg, double latDeg, double altM, QPointF *pos)
{
    double slon = lonDeg, slat = latDeg;
    toScene(lonDeg, latDeg, &slon, &slat);      // 入参是 WGS84
    return sceneScreenPos(slon, slat, altM, pos);
}

int GlobeWidget::trackAt(const QPointF &pos)
{
    updateMatrices();
    return m_trackLayer.pick(m_mvp, m_eye, QSizeF(width(), height()), pos, kPickTolPx);
}

void GlobeWidget::lookAt(double lonDeg, double latDeg, double altKm)
{
    double slon = lonDeg, slat = latDeg;
    toScene(lonDeg, latDeg, &slon, &slat);       // 入参是 WGS84（航迹/命令行坐标）
    m_camLon = qDegreesToRadians(slon);
    m_camLat = qBound(-qDegreesToRadians(89.9), qDegreesToRadians(slat), qDegreesToRadians(89.9));
    m_alt = qBound(kMinAlt, altKm / kEarthRadiusKm, kMaxAlt);
    update();
    emit cameraChanged();
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

void GlobeWidget::setSpatialLasso(SpatialLasso *lasso)
{
    m_lasso = lasso;
    if (m_lasso) {
        connect(m_lasso, &SpatialLasso::changed, this, [this]() { m_overlay->update(); update(); });
        connect(m_lasso, &SpatialLasso::mouseMoved, this, [this]() { m_overlay->update(); });
    }
}

void GlobeWidget::setDrawingManager(DrawingManager *drawing)
{
    if (m_draw)
        disconnect(m_draw, nullptr, this, nullptr);
    m_draw = drawing;
    if (m_draw) {
        // 撤销/删除后选中的要素可能已经不存在了，顺手把选中状态清掉
        connect(m_draw, &DrawingManager::changed, this, [this]() {
            if (m_selectedFeature > 0 && m_draw->indexOf(m_selectedFeature) < 0)
                setSelectedFeature(0);
            update();
        });
    }
    update();
}

void GlobeWidget::setSelectedFeature(int id)
{
    if (m_selectedFeature == id)
        return;
    m_selectedFeature = id;
    emit drawSelectionChanged(id);
    update();
}

bool GlobeWidget::deleteSelected()
{
    if (!m_draw || m_selectedFeature <= 0)
        return false;
    const int id = m_selectedFeature;
    setSelectedFeature(0);
    return m_draw->removeFeature(id);
}

void GlobeWidget::setShowLabels(bool on)
{
    m_showLabels = on;
    update();
}

bool GlobeWidget::sceneVisiblePos(double lonDeg, double latDeg, double altM, QPointF *pos)
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

bool GlobeWidget::visiblePos(double lonDeg, double latDeg, double altM, QPointF *pos)
{
    double slon = lonDeg, slat = latDeg;
    toScene(lonDeg, latDeg, &slon, &slat);      // 入参是 WGS84（航迹/旗标/标尺）
    return sceneVisiblePos(slon, slat, altM, pos);
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
    // 端点先转成场景坐标，中间的插值点也都在场景坐标系里，最后用 sceneVisiblePos 投影，
    // 不能混着用（否则会被转换两次）
    double s1lon = lon1, s1lat = lat1, s2lon = lon2, s2lat = lat2;
    toScene(lon1, lat1, &s1lon, &s1lat);
    toScene(lon2, lat2, &s2lon, &s2lat);
    const QVector3D a = geo::ecefDeg(s1lon, s1lat, 0.0).normalized();
    const QVector3D b = geo::ecefDeg(s2lon, s2lat, 0.0).normalized();
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
        // v 是单位方向向量：直接 asin(z) 得到的是「地心纬度」，与大地纬度最多差 0.19°（约 21 km），
        // 喂给按大地纬度定义的 ecef 会让整条线偏离真实位置，必须先换算
        const QVector3D dir = v.normalized();
        const double lat = qRadiansToDegrees(
            geo::geodeticLatFromDir(double(dir.z()), std::sqrt(double(dir.x()) * dir.x() + double(dir.y()) * dir.y())));
        const double lon = qRadiansToDegrees(std::atan2(double(dir.y()), double(dir.x())));
        QPointF sp;
        if (sceneVisiblePos(lon, lat, 0.0, &sp)) {
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

// 套索边界采样：两点间按经纬度平面线性插值再投影。
// 必须和 lassogeo::pointInPolygon 的多边形口径一致，否则「看到的边界」和「查询用的边界」
// 会差出十几公里，出现「明明在框里却没被选中」的假象。
void GlobeWidget::sampleLonLatLine(double lat1, double lon1, double lat2, double lon2,
                                   QVector<QPointF> *pts, QVector<bool> *vis)
{
    double dLon = lon2 - lon1;              // 跨日界线时走最短方向，不绕地球一圈
    while (dLon > 180.0)
        dLon -= 360.0;
    while (dLon < -180.0)
        dLon += 360.0;
    const double dLat = lat2 - lat1;
    // 每 ~0.25° 一个采样点：投影曲率贴合，自由手绘也不会爆点数。不含终点（下一条边接着补）
    int steps = int(qMax(std::fabs(dLat), std::fabs(dLon)) / 0.25) + 1;
    steps = qBound(1, steps, 64);
    pts->reserve(pts->size() + steps);
    vis->reserve(vis->size() + steps);
    for (int i = 0; i < steps; ++i) {
        const double f = double(i) / steps;
        QPointF sp;
        const bool ok = visiblePos(lon1 + dLon * f, lat1 + dLat * f, 0.0, &sp);
        pts->append(sp);
        vis->append(ok);
    }
}

// 空间套索：填充多边形 + 边界 + 顶点序号；绘制中额外画「最后顶点→鼠标→首顶点」虚线预览。
// 填充与边界共用同一批采样点，保证两者严丝合缝。
void GlobeWidget::drawLasso(QPainter &p)
{
    m_lassoVerticesDrawn = 0;
    if (!m_lasso)
        return;
    const QVector<LassoVertex> &v = m_lasso->vertices();
    const int n = v.size();
    if (n == 0)
        return;
    const bool closed = m_lasso->isClosed() && n >= 3;

    // 顶点投影（画序号用）
    QVector<QPointF> vp(n);
    QVector<bool> vvis(n, false);
    for (int i = 0; i < n; ++i)
        vvis[i] = visiblePos(v[i].lon, v[i].lat, 0.0, &vp[i]);

    // 边界采样：闭合时含收尾边，背面点自动断开
    QVector<QPointF> ring;
    QVector<bool> ringVis;
    for (int i = 0; i < n; ++i) {
        if (i + 1 >= n && !closed)
            break;
        const int j = (i + 1) % n;
        sampleLonLatLine(v[i].lat, v[i].lon, v[j].lat, v[j].lon, &ring, &ringVis);
    }
    if (closed) {              // 采样不含终点：补上首顶点，边界才是真正闭合的一圈
        ring << vp[0];
        ringVis << vvis[0];
    }

    // 填充：闭合且整圈都在可见半球时才填，否则会出现穿过地球的怪异多边形
    bool fillOk = closed && ring.size() >= 3;
    for (int i = 0; i < ringVis.size() && fillOk; ++i)
        if (!ringVis[i])
            fillOk = false;
    if (fillOk) {
        QPainterPath path;
        path.moveTo(ring[0]);
        for (int i = 1; i < ring.size(); ++i)
            path.lineTo(ring[i]);
        path.closeSubpath();
        QColor fill = kLassoColor;
        fill.setAlphaF(0.15f);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(path);
    }

    // 边界
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kLassoColor, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF cur;
    for (int i = 0; i < ring.size(); ++i) {
        if (ringVis[i]) {
            cur << ring[i];
        } else if (!cur.isEmpty()) {
            if (cur.size() > 1)
                p.drawPolyline(cur);
            cur.clear();
        }
    }
    if (cur.size() > 1)
        p.drawPolyline(cur);

    // 绘制中的预览虚线：最后顶点 → 鼠标，鼠标 → 首顶点（与最终边界同一套采样）
    if (m_lasso->isActive() && !m_lasso->isClosed() && m_lasso->hasMouse()) {
        QColor pc = kLassoColor;
        pc.setAlphaF(0.45f);
        QPen pre(pc, 1.5, Qt::CustomDashLine, Qt::FlatCap);
        pre.setDashPattern(QVector<qreal>() << 6.0 << 6.0);
        p.setPen(pre);
        auto dashLine = [&](double lat1, double lon1, double lat2, double lon2) {
            QVector<QPointF> pts;
            QVector<bool> vis;
            sampleLonLatLine(lat1, lon1, lat2, lon2, &pts, &vis);
            QPointF endPt;
            const bool endOk = visiblePos(lon2, lat2, 0.0, &endPt);
            pts.append(endPt);
            vis.append(endOk);
            QPolygonF line;
            for (int i = 0; i < pts.size(); ++i) {
                if (vis[i]) {
                    line << pts[i];
                } else if (!line.isEmpty()) {
                    if (line.size() > 1)
                        p.drawPolyline(line);
                    line.clear();
                }
            }
            if (line.size() > 1)
                p.drawPolyline(line);
        };
        dashLine(v.last().lat, v.last().lon, m_lasso->mouseLat(), m_lasso->mouseLon());
        if (n >= 2)
            dashLine(m_lasso->mouseLat(), m_lasso->mouseLon(), v[0].lat, v[0].lon);
    }

    // 顶点：绿色圆、黑色描边、白色序号
    const QFont f = remFont(0.857, true);
    p.setFont(f);
    for (int i = 0; i < n; ++i) {
        if (!vvis[i])
            continue;
        p.setPen(QPen(Qt::black, 1.5));
        p.setBrush(kLassoColor);
        const double r = ui::sz(9);
        p.drawEllipse(vp[i], r, r);
        p.setPen(Qt::white);
        p.drawText(QRectF(vp[i].x() - r, vp[i].y() - r, 2 * r, 2 * r),
                   Qt::AlignCenter, QString::number(i + 1));
        ++m_lassoVerticesDrawn;
    }
    p.setBrush(Qt::NoBrush);
}

// 贴地绘图：DrawingManager 里的顶点都已经按大圆加密（圆是 Vincenty 正算点环，
// 折线每段用 slerp 插值），这里只做「WGS84 -> 屏幕」投影 + 折线，和标尺/套索同一套口径。
// 背面顶点投影失败时断开折线，否则会画出一条横穿地球的假线。
void GlobeWidget::drawFeatures(QPainter &p)
{
    m_featuresDrawn = 0;
    if (!m_draw || m_draw->features().isEmpty())
        return;
    for (const DrawingFeature &f : m_draw->features()) {
        const bool sel = (f.id == m_selectedFeature);
        // 文本框：锚点投影后画描边文字（和航迹标签同一套描边参数）
        if (f.isText()) {
            if (f.pts.isEmpty())
                continue;
            QPointF sp;
            if (!visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, &sp))
                continue;
            QFont font = pxFont(ui::sz(int(f.textSizePx)), true);
            QColor outline(0, 0, 0, 210);
            drawOutlinedText(p, sp, f.text, font, sel ? kSelectColor : f.color, outline, 2.5);
            if (sel) {                       // 选中的文本：套一个虚线框，一眼看出选中的是谁
                const QRectF box = QFontMetricsF(font).boundingRect(f.text).translated(sp);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kSelectColor, 1.0, Qt::DashLine));
                p.drawRect(box.adjusted(-4, -3, 4, 3));
            }
            ++m_featuresDrawn;
            continue;
        }
        // 站点圆点：屏幕尺寸固定的实心圆，不随缩放变化（大小同航迹端点圆球）
        if (f.kind == DrawKind::Dot) {
            if (f.pts.isEmpty())
                continue;
            QPointF sp;
            if (!visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, &sp))
                continue;
            p.setPen(QPen(QColor(255, 255, 255, 210), qMax(1.0, f.widthPx * 0.5)));
            p.setBrush(sel ? kSelectColor : (f.fill.isValid() ? f.fill : f.color));
            p.drawEllipse(sp, kSiteDotPx, kSiteDotPx);
            if (sel) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kSelectColor, 1.0, Qt::DashLine));
                p.drawEllipse(sp, kSiteDotPx + 4.0, kSiteDotPx + 4.0);
            }
            ++m_featuresDrawn;
            continue;
        }
        if (f.pts.size() < 2)
            continue;

        // 填充：只有整圈都在可见半球时才填，否则会出现穿过地球的怪异多边形
        if (f.fill.isValid() && f.isClosed()) {
            bool complete = false;
            const QPolygonF ring = projectedRing(f, &complete);
            if (complete && ring.size() >= 3) {
                QPainterPath path;
                path.addPolygon(ring);
                path.closeSubpath();
                p.setPen(Qt::NoPen);
                p.setBrush(f.fill);
                p.drawPath(path);
            }
        }

        p.setBrush(Qt::NoBrush);
        // 纯填充要素不描边；没设填充色的填充要素按线画，避免完全看不见
        if (f.kind == DrawKind::Fill && f.fill.isValid()) {
            ++m_featuresDrawn;
            continue;
        }
        // 先把顶点投影成屏幕折线（背面断开），再统一描边：选中的先画一圈光晕加粗
        QVector<QPolygonF> chunks;
        QPolygonF cur;
        bool any = false;
        for (const QPointF &pt : f.pts) {        // x = 经度, y = 纬度
            QPointF sp;
            if (visiblePos(pt.x(), pt.y(), 0.0, &sp)) {
                cur << sp;
                any = true;
            } else if (!cur.isEmpty()) {
                if (cur.size() > 1)
                    chunks.append(cur);
                cur.clear();
            }
        }
        if (cur.size() > 1)
            chunks.append(cur);
        if (sel) {
            QColor halo = kSelectColor;
            halo.setAlphaF(0.45f);
            p.setPen(QPen(halo, qMax(1.0, f.widthPx) + 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (const QPolygonF &c : chunks)
                p.drawPolyline(c);
        }
        p.setPen(QPen(sel ? kSelectColor : f.color, qMax(1.0, f.widthPx) + (sel ? 1.0 : 0.0),
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QPolygonF &c : chunks)
            p.drawPolyline(c);

        // 打断/裁剪点：白心黑边的小圆点。标记挂在要素上，要素删了自然就没了
        for (const QPointF &mk : f.marks) {
            QPointF sp;
            if (!visiblePos(mk.x(), mk.y(), 0.0, &sp))
                continue;
            p.setPen(QPen(QColor(0, 0, 0, 200), 1.0));
            p.setBrush(QColor(255, 255, 255));
            p.drawEllipse(sp, 3.5, 3.5);
            p.setBrush(Qt::NoBrush);
        }
        if (any)
            ++m_featuresDrawn;
    }
}

// 正在画的图形（预览）：直线/矩形从按下点连到鼠标，多边形画已有边 + 虚线预览，
// 自由曲线把已采到的点直接连起来。预览统一用半透明虚线，和已完成的图形区分开。
void GlobeWidget::drawInProgress(QPainter &p)
{
    m_drawPreviewSegments = 0;
    if (m_tool == DrawTool::None)
        return;
    const QColor c = m_drawColor;
    QColor pc = c;
    pc.setAlphaF(0.55f);
    QPen solid(c, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen preview(pc, 2.0, Qt::CustomDashLine, Qt::FlatCap);
    preview.setDashPattern(QVector<qreal>() << 5.0 << 5.0);
    p.setBrush(Qt::NoBrush);

    if (m_strokeActive && m_tool == DrawTool::Polyline) {
        p.setPen(solid);
        QPolygonF poly;
        for (const QPointF &pt : m_stroke) {
            QPointF sp;
            if (visiblePos(pt.x(), pt.y(), 0.0, &sp))
                poly << sp;
        }
        if (poly.size() > 1) {
            p.drawPolyline(poly);
            ++m_drawPreviewSegments;
        }
        return;
    }

    const double curLon = qRadiansToDegrees(m_cursorLon);
    const double curLat = qRadiansToDegrees(m_cursorLat);
    if (m_strokeActive && (m_tool == DrawTool::Line || m_tool == DrawTool::Rect) && m_cursorValid) {
        p.setPen(preview);
        if (m_tool == DrawTool::Line) {
            for (const QPolygonF &line : geodesicLines(m_pressLat, m_pressLon, curLat, curLon)) {
                p.drawPolyline(line);
                ++m_drawPreviewSegments;
            }
        } else {
            const double s = qMin(m_pressLat, curLat), n = qMax(m_pressLat, curLat);
            const double w = qMin(m_pressLon, curLon), e = qMax(m_pressLon, curLon);
            const double lats[5] = { s, s, n, n, s };
            const double lons[5] = { w, e, e, w, w };
            for (int i = 0; i < 4; ++i) {
                for (const QPolygonF &line : geodesicLines(lats[i], lons[i], lats[i + 1], lons[i + 1])) {
                    p.drawPolyline(line);
                    ++m_drawPreviewSegments;
                }
            }
        }
    }

    // 裁剪/打断：拖动时显示一条红色虚线，就是这把「刀」走的路径
    if (m_tool == DrawTool::Break && m_strokeActive && m_cursorValid) {
        QPen knife(QColor(0xef, 0x44, 0x44), 2.0, Qt::CustomDashLine);
        knife.setDashPattern(QVector<qreal>() << 4.0 << 4.0);
        p.setPen(knife);
        for (const QPolygonF &line : geodesicLines(m_pressLat, m_pressLon, curLat, curLon)) {
            p.drawPolyline(line);
            ++m_drawPreviewSegments;
        }
    }

    if (m_tool == DrawTool::Polygon && !m_stroke.isEmpty()) {
        p.setPen(solid);
        for (int i = 0; i + 1 < m_stroke.size(); ++i) {
            for (const QPolygonF &line : geodesicLines(m_stroke[i].y(), m_stroke[i].x(),
                                                       m_stroke[i + 1].y(), m_stroke[i + 1].x())) {
                p.drawPolyline(line);
                ++m_drawPreviewSegments;
            }
        }
        if (m_cursorValid) {
            p.setPen(preview);
            const QPointF &last = m_stroke.last();
            for (const QPolygonF &line : geodesicLines(last.y(), last.x(), curLat, curLon))
                p.drawPolyline(line);
            if (m_stroke.size() >= 2) {
                const QPointF &first = m_stroke.first();
                for (const QPolygonF &line : geodesicLines(curLat, curLon, first.y(), first.x()))
                    p.drawPolyline(line);
            }
        }
        p.setPen(QPen(Qt::black, 1.0));
        p.setBrush(c);
        for (const QPointF &pt : m_stroke) {
            QPointF sp;
            if (visiblePos(pt.x(), pt.y(), 0.0, &sp))
                p.drawEllipse(sp, 3.0, 3.0);
        }
        p.setBrush(Qt::NoBrush);
    }

    // 吸附提示：绿色小方块画在吸到的那个顶点上，告诉用户「这一下会落在这里」
    if (m_snapActive) {
        p.setPen(QPen(QColor(0, 0, 0, 170), 1.0));
        p.setBrush(m_snapColor);
        p.drawRect(QRectF(m_snapPos - QPointF(4.0, 4.0), QSizeF(8.0, 8.0)));
        p.setBrush(Qt::NoBrush);
    }
}

// 框选删除的橡皮筋：半透明填充 + 虚线边框
void GlobeWidget::drawBand(QPainter &p)
{
    if (!m_bandActive || m_bandRect.isNull())
        return;
    QColor fill = kLassoColor;
    fill.setAlphaF(0.15f);
    p.setBrush(fill);
    p.setPen(QPen(kLassoColor, 1.0, Qt::DashLine));
    p.drawRect(m_bandRect.normalized());
    p.setBrush(Qt::NoBrush);
}

// ---------------------------------------------------------------
//  绘图交互
// ---------------------------------------------------------------
void GlobeWidget::setDrawTool(DrawTool t)
{
    if (m_tool == t)
        return;
    cancelDrawing();
    m_tool = t;
    m_hoverFeature = 0;
    if (t == DrawTool::None)
        unsetCursor();
    else if (t == DrawTool::Select)
        setCursor(Qt::ArrowCursor);        // 选择工具用箭头，跟「画」区分开
    else
        setCursor(Qt::CrossCursor);
    update();
}

bool GlobeWidget::cancelDrawing()
{
    const bool had = m_strokeActive || !m_stroke.isEmpty() || m_bandActive || m_hoverFeature != 0
                     || m_snapActive;
    m_stroke.clear();
    m_strokeActive = false;
    m_bandActive = false;
    m_bandRect = QRect();
    m_hoverFeature = 0;
    m_snapActive = false;
    m_dragging = false;
    m_clickCandidate = false;
    if (had)
        update();
    return had;
}

// 只有需要「落点精确」的工具才吸附：直线 / 矩形 / 多边形。
// 自由手绘是逐点采样，吸了反而会把线扯到别的图形上。
bool GlobeWidget::snappingTool() const
{
    return m_tool == DrawTool::Line || m_tool == DrawTool::Rect || m_tool == DrawTool::Polygon;
}

// 吸附：找屏幕上离鼠标最近、且在 kSnapPx（10px）以内的已有顶点。
// 命中时返回该顶点的 WGS84 经纬度与屏幕坐标，调用方用它替换当前落点。
bool GlobeWidget::snapToFeature(const QPointF &pos, double *lonDeg, double *latDeg, QPointF *screen)
{
    if (!m_draw || m_draw->features().isEmpty())
        return false;
    double best = kSnapPx;
    bool found = false;
    QPointF bestPos;
    for (const DrawingFeature &f : m_draw->features()) {
        for (const QPointF &pt : f.pts) {
            QPointF sp;
            if (!visiblePos(pt.x(), pt.y(), 0.0, &sp))
                continue;
            const double d = std::hypot(sp.x() - pos.x(), sp.y() - pos.y());
            if (d <= best) {
                best = d;
                bestPos = sp;
                *lonDeg = pt.x();
                *latDeg = pt.y();
                found = true;
            }
        }
    }
    if (found && screen)
        *screen = bestPos;
    return found;
}

void GlobeWidget::finishDrawStroke(double lat, double lon)
{
    if (!m_draw)
        return;
    int id = 0;
    if (m_tool == DrawTool::Line)
        id = m_draw->addLine(m_pressLat, m_pressLon, lat, lon, m_drawColor);
    else if (m_tool == DrawTool::Rect)
        id = m_draw->addRect(m_pressLat, m_pressLon, lat, lon, m_drawColor);
    if (id > 0)
        emit drawFeatureAdded(id);
}

void GlobeWidget::closeDrawPolygon()
{
    if (m_draw && m_stroke.size() >= 3) {
        const int id = m_draw->addPolygon(m_stroke, m_drawColor);
        if (id > 0)
            emit drawFeatureAdded(id);
    }
    m_stroke.clear();
    m_strokeActive = false;
    update();
}

void GlobeWidget::applyBoxSelect()
{
    if (!m_draw)
        return;
    const QRectF r = QRectF(m_bandRect).normalized();
    if (r.width() < 3.0 && r.height() < 3.0)
        return;                       // 误点：没拉出框就不删
    QVector<int> ids;
    for (const DrawingFeature &f : m_draw->features())
        if (featureHitsRect(f, r))
            ids.append(f.id);
    m_draw->removeFeatures(ids);
}

// 闭合图形整圈都可见时返回投影后的多边形；有任何一点在背面就返回空并置 complete=false
QPolygonF GlobeWidget::projectedRing(const DrawingFeature &f, bool *complete)
{
    QPolygonF ring;
    ring.reserve(f.pts.size());
    for (const QPointF &pt : f.pts) {
        QPointF sp;
        if (!visiblePos(pt.x(), pt.y(), 0.0, &sp)) {
            if (complete)
                *complete = false;
            return QPolygonF();
        }
        ring << sp;
    }
    if (complete)
        *complete = true;
    return ring;
}

// 橡皮擦：屏幕空间点到折线的距离，取最近的一个（后画的优先）
int GlobeWidget::featureAtScreen(const QPointF &pos, double tolPx)
{
    if (!m_draw)
        return 0;
    const QVector<DrawingFeature> &fs = m_draw->features();
    int best = 0;
    double bestD = tolPx;
    for (int i = fs.size() - 1; i >= 0; --i) {
        const DrawingFeature &f = fs.at(i);
        if (f.isText()) {
            QPointF sp;
            if (!f.pts.isEmpty() && visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, &sp)) {
                const double d = QLineF(sp, pos).length();
                if (d <= 14.0 && d <= bestD + 8.0) {   // 文字锚点允许宽松一点
                    best = f.id;
                    bestD = qMin(bestD, d);
                }
            }
            continue;
        }
        if (f.kind == DrawKind::Dot) {
            QPointF sp;
            if (!f.pts.isEmpty() && visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, &sp)) {
                const double d = QLineF(sp, pos).length();
                if (d <= kSiteDotPx + 3.0 && d <= bestD + 8.0) {
                    best = f.id;
                    bestD = qMin(bestD, d);
                }
            }
            continue;
        }
        QPointF prev;
        bool have = false;
        for (const QPointF &pt : f.pts) {
            QPointF sp;
            if (!visiblePos(pt.x(), pt.y(), 0.0, &sp)) {
                have = false;
                continue;
            }
            if (have) {
                const double d = distToSegment(pos, prev, sp);
                if (d < bestD) {
                    bestD = d;
                    best = f.id;
                }
            }
            prev = sp;
            have = true;
        }
    }
    return best;
}

// 油漆桶：点是否落在某个闭合图形里（屏幕空间射线法，整圈可见才判）
int GlobeWidget::closedFeatureAt(const QPointF &pos)
{
    if (!m_draw)
        return 0;
    const QVector<DrawingFeature> &fs = m_draw->features();
    for (int i = fs.size() - 1; i >= 0; --i) {
        const DrawingFeature &f = fs.at(i);
        if (!f.isClosed())
            continue;
        bool complete = false;
        const QPolygonF ring = projectedRing(f, &complete);
        if (complete && ring.size() >= 3 && ring.containsPoint(pos, Qt::OddEvenFill))
            return f.id;
    }
    return 0;
}

// 框选（交叉选择）：任一顶点在框内，或任一线段与框边相交
bool GlobeWidget::featureHitsRect(const DrawingFeature &f, const QRectF &rect)
{
    if (f.pts.isEmpty())
        return false;
    if (f.isText()) {
        QPointF sp;
        return visiblePos(f.pts.first().x(), f.pts.first().y(), 0.0, &sp) && rect.contains(sp);
    }
    QPointF prev;
    bool have = false;
    for (const QPointF &pt : f.pts) {
        QPointF sp;
        if (!visiblePos(pt.x(), pt.y(), 0.0, &sp)) {
            have = false;
            continue;
        }
        if (rect.contains(sp))
            return true;
        if (have && segmentHitsRect(prev, sp, rect))
            return true;
        prev = sp;
        have = true;
    }
    return false;
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
    const QString l2 = formatBeijingTime(pt.t);
    QFont f = remFont(1.0);
    f.setPixelSize(14);   // 悬停信息独立于主界面字号，避免大面板遮挡地图。
    const QFontMetricsF fm(f);
    const double w = qMax(fm.horizontalAdvance(l1), fm.horizontalAdvance(l2));
    const double lineH = fm.height();
    const double pad = 8, padY = 6;
    const QRectF box(sp.x() + m_trackLayer.pointDotPx() + 10, sp.y() - lineH - padY,
                     w + 2 * pad, lineH * 2 + 2 * padY);
    p.fillRect(box, Qt::black);
    QColor outline(Qt::black);
    outline.setAlphaF(0.9);
    drawOutlinedText(p, QPointF(box.left() + pad, box.top() + padY + fm.ascent()), l1, f, Qt::white, outline, 2.0);
    drawOutlinedText(p, QPointF(box.left() + pad, box.top() + padY + lineH + fm.ascent()), l2, f, Qt::white, outline, 2.0);
}

void GlobeWidget::paintOverlay(QPainter &p)
{
    m_labelsDrawn = m_flagsDrawn = m_rulerMarkersDrawn = m_lassoVerticesDrawn = 0;
    m_featuresDrawn = 0;
    if (!m_overlayWanted)
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    drawLabels(p);
    drawFeatures(p);
    drawInProgress(p);
    drawBand(p);
    drawRuler(p);
    drawLasso(p);
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
         + QStringLiteral(" glErrors=%1 alt=%2km lon=%3 lat=%4 crs=%5")
               .arg(m_glErrors)
               .arg(m_alt * kEarthRadiusKm, 0, 'f', 1)
               .arg(qRadiansToDegrees(m_camLon), 0, 'f', 3)
               .arg(qRadiansToDegrees(m_camLat), 0, 'f', 3)
               .arg(coordSystemName());
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

// 网络瓦片到货后在这里统一上传：paintGL 里 GL 上下文才是当前的
void GlobeWidget::uploadPendingTiles()
{
    if (m_pendingTiles.isEmpty())
        return;
    int n = 0;
    for (auto it = m_pendingTiles.begin(); it != m_pendingTiles.end() && n < 32;) {
        uploadTexture(it.key(), it.value());
        it = m_pendingTiles.erase(it);
        ++n;
    }
}

int GlobeWidget::tileMaxZoom() const
{
    if (m_online)
        return m_online->maxZoom();
    return m_tiles ? m_tiles->maxZoom() : -1;
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
    const int maxZ = qMax(2, mapMaxZoom());
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
    const int maxZ = mapMaxZoom();
    const int minZ = mapMinZoom();
    // 缺本层瓦片时向上找父级纹理，用子区域贴过去——低清但连续，
    // 比露底色好；这也是缩放开合时不闪白的关键。
    for (int az = qMin(t.z, maxZ); az >= minZ; --az) {
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

// 在线模式优先网络瓦片，没到货时用本地 mbtiles 顶一下，避免整片空白
void GlobeWidget::loadTexture(int z, int x, int y)
{
    const quint64 key = tileKey(z, x, y);
    if (m_online) {
        // 在线模式：瓦片异步到手（线程池解码 / 网络下载），到了走 tileReady。
        // 这里不用本地 mbtiles 兜底——wanted 里的层级都在在线源范围内，
        // 拿低清离线瓦片顶上去，这个 key 就永久停在模糊图上，在线瓦片再也加载不进来。
        m_online->startLoad(z, x, y);
        return;
    }
    const QImage img = m_tiles ? m_tiles->tile(z, x, y) : QImage();
    if (img.isNull()) {
        m_missing.insert(key);
        return;
    }
    uploadTexture(key, img);
}

void GlobeWidget::uploadTexture(quint64 key, const QImage &img)
{
    if (img.isNull() || m_textures.contains(key))
        return;
    // 只用 OpenGL ES 2.0 子集（GL_RGBA + glGenerateMipmap），ANGLE/D3D 下不报错
    // QOpenGLTexture 会设置 ES2 没有的 GL_TEXTURE_MAX_LEVEL 等参数，Win7 走 ANGLE 时会产生 GL_INVALID_ENUM
    // 在线瓦片合成后是 Format_RGB32（0xffRRGGBB，小端内存序即 B,G,R,A），正好能直接喂 GL_BGRA，
    // 省掉一次整图 convertToFormat 的 256KB 内存拷贝。其余格式仍走 RGBA8888。
    QImage rgba;
    bool bgra = false;
    if (img.format() == QImage::Format_RGB32 && !context()->isOpenGLES()) {
        rgba = img;
        bgra = true;
    } else {
        rgba = img.convertToFormat(QImage::Format_RGBA8888);
    }
    const bool pot = (rgba.width() & (rgba.width() - 1)) == 0 && (rgba.height() & (rgba.height() - 1)) == 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (bgra) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgba.width(), rgba.height(), 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, rgba.constBits());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    }
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
    uploadPendingTiles();

    // 预载最低两级，保证任何位置都有父级纹理可用
    if (mapMinZoom() <= 1) {
        for (int z = mapMinZoom(); z <= qMin(1, mapMaxZoom()); ++z) {
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
            if (t.z >= mapMinZoom() && t.z <= mapMaxZoom()
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

    // 航迹贴地绘制：关掉深度测试（否则与瓦片同深度会 z-fighting），
    // 地球背面的遮挡由着色器按相机位置做地平线剔除
    m_trackLayer.setEye(m_eye);
    m_trackLayer.draw(m_mvp, QSize(int(width() * devicePixelRatioF()), int(height() * devicePixelRatioF())),
                      float(devicePixelRatioF()));

    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        ++m_glErrors;

    // 文字和图标由叠加层控件画（有内容时，或刚从有内容变为没有、需要擦掉时才重画）
    const bool overlay = (m_showLabels && m_trackLayer.visibleTrackCount() > 0)
                         || (m_flags && !m_flags->flags().isEmpty())
                         || (m_ruler && m_ruler->isActive() && !m_ruler->waypoints().isEmpty())
                         || (m_lasso && !m_lasso->vertices().isEmpty())
                         || (m_draw && !m_draw->features().isEmpty())
                         || (m_tool != DrawTool::None && (!m_stroke.isEmpty() || m_bandActive))
                         || m_snapActive
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
    // 绘图模式：左键归画笔，右键拖拽旋转（不弹右键菜单）；滚轮缩放照常
    if (m_tool != DrawTool::None) {
        if (e->button() == Qt::RightButton) {
            m_dragging = true;
            m_clickCandidate = true;
            m_lastPos = m_pressPos = e->pos();
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        double lon = 0.0, lat = 0.0;
        const bool hit = geoAt(e->pos(), &lon, &lat);
        // 吸附：落点离已有顶点不到 10px 就吸过去（直线/矩形/多边形）
        m_snapActive = false;
        if (hit && snappingTool()) {
            QPointF sp;
            double sl = 0.0, sa = 0.0;
            if (snapToFeature(e->pos(), &sl, &sa, &sp)) {
                lon = sl;
                lat = sa;
                m_snapActive = true;
                m_snapPos = sp;
            }
        }
        switch (m_tool) {
        case DrawTool::Text:
            if (hit)
                emit drawTextRequested(lat, lon, e->globalPos());
            return;
        case DrawTool::Fill:
            emit drawFillRequested(hit ? closedFeatureAt(e->pos()) : 0);
            return;
        case DrawTool::Eraser: {
            const int id = hit ? featureAtScreen(e->pos(), kPickTolPx) : 0;
            if (id > 0 && m_draw)
                m_draw->removeFeature(id);
            return;
        }
        case DrawTool::BoxSelect:
            m_bandActive = true;
            m_bandRect = QRect(e->pos(), QSize(0, 0));
            m_pressPos = e->pos();
            return;
        case DrawTool::Polygon:
            if (!hit)
                return;
            if (m_stroke.isEmpty())
                m_strokeActive = true;
            // 吸到起点就自动闭合：闭合边严丝合缝，不会差出一点点
            if (m_stroke.size() >= 3 && QPointF(lon, lat) == m_stroke.first()) {
                closeDrawPolygon();
                return;
            }
            m_stroke.append(QPointF(lon, lat));      // 多边形：单击放顶点
            update();
            return;
        case DrawTool::Select: {
            int id = hit ? featureAtScreen(e->pos(), kPickTolPx) : 0;
            if (id <= 0 && hit)
                id = closedFeatureAt(e->pos());      // 点在填充/多边形内部也能选中
            setSelectedFeature(id);
            return;
        }
        case DrawTool::Break:
            if (!hit)
                return;
            m_strokeActive = true;                   // 按下开始「下刀」，松开时判断是点还是拖
            m_pressLat = lat;
            m_pressLon = lon;
            m_pressPos = e->pos();
            update();
            return;
        case DrawTool::Line:
        case DrawTool::Rect:
        case DrawTool::Polyline:
        default:
            if (!hit)
                return;
            m_strokeActive = true;
            m_stroke.clear();
            m_stroke.append(QPointF(lon, lat));
            m_pressLat = lat;
            m_pressLon = lon;
            m_pressPos = e->pos();
            update();
            return;
        }
    }
    // 套索绘制中：左键不旋转地球。按下先只记录，松开时若是单击就放一个顶点；
    // 按住移动超过阈值则自动转为自由手绘（这样逐点框选和手绘都不需要切模式）。
    if (m_lasso && m_lasso->isActive() && e->button() == Qt::LeftButton) {
        double lon = 0.0, lat = 0.0;
        m_dragging = false;
        m_clickCandidate = false;
        m_lassoPressValid = geoAt(e->pos(), &lon, &lat);
        if (m_lassoPressValid) {
            m_lassoPressLat = lat;
            m_lassoPressLon = lon;
            m_clickCandidate = true;
            m_pressPos = e->pos();
        }
        return;
    }
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_clickCandidate = true;
        m_lastPos = m_pressPos = e->pos();
    }
}

void GlobeWidget::mouseMoveEvent(QMouseEvent *e)
{
    // 绘图模式：更新预览、自由曲线采点、框选橡皮筋、橡皮擦悬停
    if (m_tool != DrawTool::None) {
        double lon = 0.0, lat = 0.0;
        const bool hit = geoAt(e->pos(), &lon, &lat);
        // 吸附：预览点直接吸到已有顶点上，落笔位置和看到的一致
        m_snapActive = false;
        if (hit && snappingTool()) {
            QPointF sp;
            double sl = 0.0, sa = 0.0;
            if (snapToFeature(e->pos(), &sl, &sa, &sp)) {
                lon = sl;
                lat = sa;
                m_snapActive = true;
                m_snapPos = sp;
            }
        }
        m_cursorValid = hit;
        if (hit) {
            m_cursorLon = qDegreesToRadians(lon);
            m_cursorLat = qDegreesToRadians(lat);
        }
        if (m_dragging) {                       // 右键拖拽旋转
            if (m_clickCandidate && (e->pos() - m_pressPos).manhattanLength() > kClickSlopPx)
                m_clickCandidate = false;
            if (!m_clickCandidate) {
                panPixels(m_lastPos, e->pos());
                m_lastPos = e->pos();
            }
            updateMatrices();
            emitStatus();
            return;
        }
        if (m_bandActive) {
            m_bandRect = QRect(m_pressPos, e->pos()).normalized();
            update();
            emitStatus();
            return;
        }
        if (m_strokeActive && m_tool == DrawTool::Polyline && hit) {
            // 与上一点隔 ~200 m 才采点：手一停不会堆出上千个重复点
            const QPointF last = m_stroke.last();
            if (std::hypot(lon - last.x(), lat - last.y()) > 0.002)
                m_stroke.append(QPointF(lon, lat));
            update();
        } else if (m_strokeActive || (m_tool == DrawTool::Polygon && !m_stroke.isEmpty())) {
            update();                           // 直线/矩形/多边形：重画预览
        }
        if (m_tool == DrawTool::Eraser) {
            const int id = hit ? featureAtScreen(e->pos(), kPickTolPx) : 0;
            if (id != m_hoverFeature) {
                m_hoverFeature = id;
                update();
            }
        }
        emitStatus();
        return;
    }
    // 套索绘制中：只更新预览线和自由手绘轨迹，不拖拽地球
    if (m_lasso && m_lasso->isActive()) {
        double lon = 0.0, lat = 0.0;
        const bool hit = geoAt(e->pos(), &lon, &lat);
        m_lasso->setMouse(hit, lat, lon);
        if (m_lasso->isDrawingFreehand()) {
            if (hit)
                m_lasso->addFreehandPoint(lat, lon);
        } else if (m_clickCandidate && m_lassoPressValid &&
                   (e->pos() - m_pressPos).manhattanLength() > kClickSlopPx) {
            // 从空白开始拖动 → 自由手绘；已有顶点时拖动不改变已画内容
            m_clickCandidate = false;
            if (m_lasso->vertices().isEmpty()) {
                m_lasso->beginFreehand(m_lassoPressLat, m_lassoPressLon);
                if (hit)
                    m_lasso->addFreehandPoint(lat, lon);
            }
        }
        updateMatrices();
        m_cursorValid = hit;
        if (hit) {
            m_cursorLon = qDegreesToRadians(lon);
            m_cursorLat = qDegreesToRadians(lat);
        }
        emitStatus();
        return;
    }
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
    double clon = 0.0, clat = 0.0;
    m_cursorValid = pick(e->pos(), &clon, &clat);
    if (m_cursorValid) {              // 场景坐标 → WGS84，供状态栏/标尺使用
        double wlon = 0.0, wlat = 0.0;
        fromScene(qRadiansToDegrees(clon), qRadiansToDegrees(clat), &wlon, &wlat);
        m_cursorLon = qDegreesToRadians(wlon);
        m_cursorLat = qDegreesToRadians(wlat);
    }
    if (m_ruler && m_ruler->isActive())
        m_ruler->setMouse(m_cursorValid, qRadiansToDegrees(m_cursorLat), qRadiansToDegrees(m_cursorLon));
    emitStatus();
}

void GlobeWidget::mouseReleaseEvent(QMouseEvent *e)
{
    // 绘图模式：右键单击 = 闭合多边形 / 取消当前笔画；左键松开 = 落笔
    if (m_tool != DrawTool::None) {
        if (e->button() == Qt::RightButton) {
            const bool click = m_clickCandidate;
            m_dragging = false;
            m_clickCandidate = false;
            if (click) {
                if (m_tool == DrawTool::Polygon && m_stroke.size() >= 3)
                    closeDrawPolygon();
                else
                    cancelDrawing();
            }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        if (m_bandActive) {
            applyBoxSelect();
            m_bandActive = false;
            m_bandRect = QRect();
            update();
            return;
        }
        // 裁剪/打断：拖得短算「点一下」（打断），拖得长算「划一刀」（裁剪）
        if (m_tool == DrawTool::Break) {
            const bool click = (e->pos() - m_pressPos).manhattanLength() < 5;
            double lon = 0.0, lat = 0.0;
            const bool hit = geoAt(e->pos(), &lon, &lat);
            m_strokeActive = false;
            m_snapActive = false;
            if (m_draw && hit) {
                if (click) {
                    const int id = featureAtScreen(e->pos(), kPickTolPx);
                    if (id <= 0)
                        emit drawMessage(tr("这里没有可打断的线段"));
                    else if (!m_draw->breakFeature(id, m_draw->nearestVertex(id, lat, lon)))
                        emit drawMessage(tr("这里断不开（文本不能打断，端点处也断不出两段）"));
                    else
                        emit drawMessage(tr("已在该点打断成两段"));
                } else {
                    const int n = m_draw->trimCrossing(m_pressLat, m_pressLon, lat, lon);
                    emit drawMessage(n > 0 ? tr("已裁剪 %1 个图形").arg(n)
                                           : tr("剪切线没有穿过任何开放线段"));
                }
            }
            update();
            return;
        }
        if (!m_strokeActive)
            return;
        if (m_tool == DrawTool::Polygon)
            return;                              // 多边形等双击/右键闭合
        double lon = 0.0, lat = 0.0;
        const bool hit = geoAt(e->pos(), &lon, &lat);
        if (hit && snappingTool()) {             // 落笔点也吸附，终点才能精确落在已有顶点上
            double sl = 0.0, sa = 0.0;
            if (snapToFeature(e->pos(), &sl, &sa, nullptr)) {
                lon = sl;
                lat = sa;
            }
        }
        if (m_tool == DrawTool::Polyline) {
            if (m_stroke.size() >= 2 && m_draw) {
                const int id = m_draw->addPolyline(m_stroke, m_drawColor);
                if (id > 0)
                    emit drawFeatureAdded(id);
            }
        } else if (hit && (e->pos() - m_pressPos).manhattanLength() >= 3) {
            finishDrawStroke(lat, lon);           // 直线/矩形：太短的拖动当成误点
        }
        m_stroke.clear();
        m_strokeActive = false;
        update();
        return;
    }
    // 套索绘制中：自由手绘松手即闭合；顶点模式单击放一个顶点
    if (m_lasso && m_lasso->isActive() && e->button() == Qt::LeftButton) {
        if (m_lasso->isDrawingFreehand()) {
            m_lasso->endFreehand();
        } else if (m_clickCandidate) {
            double lon = 0.0, lat = 0.0;
            if (geoAt(e->pos(), &lon, &lat))
                m_lasso->addVertex(lat, lon);
        }
        m_clickCandidate = false;
        m_lassoPressValid = false;
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const bool click = m_clickCandidate;
    const bool wasDragging = m_dragging;
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
    if (wasDragging)
        emit cameraChanged();       // 拖动结束才存视角，拖动过程中不写设置
}

// 双击：在旗标上删除，否则在该处放旗标（RadarView LEFT_DOUBLE_CLICK）
void GlobeWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    // 绘图模式：多边形双击闭合；其他工具下双击也不能误放旗标
    if (m_tool != DrawTool::None) {
        if (m_tool == DrawTool::Polygon && e->button() == Qt::LeftButton) {
            closeDrawPolygon();
        } else if (m_tool == DrawTool::Select && e->button() == Qt::LeftButton) {
            // 双击 = 打开属性窗（颜色 / 线宽 / 填充透明度 / 圆的半径）
            int id = featureAtScreen(e->pos(), kPickTolPx);
            if (id <= 0)
                id = closedFeatureAt(e->pos());
            if (id > 0) {
                setSelectedFeature(id);
                emit drawFeatureEditRequested(id);
            }
        }
        return;
    }
    // 套索绘制中：双击闭合多边形
    if (m_lasso && m_lasso->isActive() && e->button() == Qt::LeftButton) {
        m_lasso->closePolygon();
        return;
    }
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
    // 绘图模式：右键用于拖拽旋转/闭合图形，不弹菜单
    if (m_tool != DrawTool::None)
        return;
    // 套索绘制中：右键闭合（顶点足够时），否则取消
    if (m_lasso && m_lasso->isActive()) {
        if (m_lasso->vertices().size() >= 3)
            m_lasso->closePolygon();
        else
            m_lasso->clearAll();
        return;
    }
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
    return QStringLiteral("<qt><div style='font-size:14px'>") + lines.join(QStringLiteral("<br>"))
         + QStringLiteral("</div></qt>");
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
