#include "globecamera.h"
#include "crs.h"
#include "geo.h"

#include <QtMath>
#include <cmath>

namespace {

double mercToLat(double m)
{
    return 2.0 * std::atan(std::exp(m)) - M_PI / 2.0;
}

} // namespace

const double GlobeCamera::kMercMaxLat = 1.4844222297453324;  // 85.0511°，Web 墨卡托纬度上限
const float GlobeCamera::kFovDeg = 45.0f;
// 相机离地最低高度（地球半径单位）。原来是 0.0008（≈5km），那个高度下瓦片最多细分到
// z15、在屏幕上只有 153px（原生 256px 的 0.6 倍），地名标签被缩到看不清。
// 降到 ≈320m 后：天地图（原生 z18）能以 1:1 显示，高德（原生 z17）约 2.5 倍放大，
// 文字才真正可读。代价是视野只有两百多米，需要放大缩小来回看。
const double GlobeCamera::kMinAlt = 0.00005;                 // ≈ 320 m
const double GlobeCamera::kMaxAlt = 8.0;
const float GlobeCamera::kRefinePx = 300.0f;                 // 瓦片投影尺寸超过此像素数就细分

GlobeCamera::GlobeCamera()
    : m_camLon(qDegreesToRadians(105.0)),
      m_camLat(qDegreesToRadians(35.0)),
      m_alt(2.5)
{
}

void GlobeCamera::toScene(double lonWgs, double latWgs, double *lonScene, double *latScene) const
{
    if (!m_sceneCrs) {
        *lonScene = lonWgs;
        *latScene = latWgs;
        return;
    }
    crs::wgs84ToGcj02(lonWgs, latWgs, lonScene, latScene);
}

void GlobeCamera::fromScene(double lonScene, double latScene, double *lonWgs, double *latWgs) const
{
    if (!m_sceneCrs) {
        *lonWgs = lonScene;
        *latWgs = latScene;
        return;
    }
    crs::gcj02ToWgs84(lonScene, latScene, lonWgs, latWgs);
}

bool GlobeCamera::setSceneGcj(bool gcj)
{
    if (gcj == m_sceneCrs)
        return false;

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
    m_dirty = true;
    return true;
}

void GlobeCamera::reset()
{
    m_camLon = qDegreesToRadians(105.0);
    m_camLat = qDegreesToRadians(35.0);
    m_alt = 2.5;
    m_dirty = true;
}

void GlobeCamera::lookAt(double lonDeg, double latDeg, double altKm)
{
    double slon = lonDeg, slat = latDeg;
    toScene(lonDeg, latDeg, &slon, &slat);       // 入参是 WGS84（航迹/命令行坐标）
    m_camLon = qDegreesToRadians(slon);
    m_camLat = qBound(-qDegreesToRadians(89.9), qDegreesToRadians(slat), qDegreesToRadians(89.9));
    m_alt = qBound(kMinAlt, altKm / geo::kEarthRadiusKm, kMaxAlt);
    m_dirty = true;
}

void GlobeCamera::view(double *lonDeg, double *latDeg, double *altKm) const
{
    double lonW = 0.0, latW = 0.0;
    fromScene(qRadiansToDegrees(m_camLon), qRadiansToDegrees(m_camLat), &lonW, &latW);
    if (lonDeg)
        *lonDeg = lonW;
    if (latDeg)
        *latDeg = latW;
    if (altKm)
        *altKm = m_alt * geo::kEarthRadiusKm;
}

void GlobeCamera::zoomBy(double steps)
{
    m_alt = qBound(kMinAlt, m_alt * std::pow(0.8, steps), kMaxAlt);
    m_dirty = true;
}

void GlobeCamera::panPixels(const QPoint &from, const QPoint &to, const QSize &viewport)
{
    updateMatrices(viewport);
    double lon0 = 0.0, lat0 = 0.0, lon1 = 0.0, lat1 = 0.0;
    if (pick(from, viewport, &lon0, &lat0) && pick(to, viewport, &lon1, &lat1)) {
        // 抓取式拖动：让鼠标按下处的地面跟着鼠标走
        double dlon = lon0 - lon1;
        if (dlon > M_PI) dlon -= 2.0 * M_PI;
        if (dlon < -M_PI) dlon += 2.0 * M_PI;
        m_camLon += dlon;
        m_camLat += lat0 - lat1;
    } else {
        // 鼠标在地球外：按像素换算角度
        const double k = qMin(m_alt, 2.0) * qDegreesToRadians(double(kFovDeg)) / qMax(1, viewport.height());
        m_camLon -= (to.x() - from.x()) * k / qMax(0.2, std::cos(m_camLat));
        m_camLat += (to.y() - from.y()) * k;
    }
    const double latLimit = qDegreesToRadians(89.9);
    m_camLat = qBound(-latLimit, m_camLat, latLimit);
    m_camLon = std::remainder(m_camLon, 2.0 * M_PI);
    m_dirty = true;
}

double GlobeCamera::altitudeKm() const
{
    return m_alt * geo::kEarthRadiusKm;
}

void GlobeCamera::updateMatrices(const QSize &viewport) const
{
    if (!m_dirty && m_viewport == viewport)
        return;

    const double sl = std::sin(m_camLat), cl = std::cos(m_camLat);
    const double so = std::sin(m_camLon), co = std::cos(m_camLon);
    m_eye = geo::ecef(m_camLon, m_camLat, m_alt);
    const QVector3D target = geo::ecef(m_camLon, m_camLat, 0.0);
    const QVector3D north(float(-sl * co), float(-sl * so), float(cl));

    m_view.setToIdentity();
    m_view.lookAt(m_eye, target, north);

    const float aspect = float(qMax(1, viewport.width())) / float(qMax(1, viewport.height()));
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

    m_focalPx = float(qMax(1, viewport.height()) / (2.0 * std::tan(qDegreesToRadians(kFovDeg) / 2.0)));
    m_viewport = viewport;
    m_dirty = false;
}

// 鼠标射线与椭球求交（先把 z 轴按 b/a 缩放，椭球变成单位球）
bool GlobeCamera::pick(const QPointF &pos, const QSize &viewport, double *lonRad, double *latRad) const
{
    if (viewport.width() <= 0 || viewport.height() <= 0)
        return false;
    updateMatrices(viewport);
    bool ok = false;
    const QMatrix4x4 inv = m_mvp.inverted(&ok);
    if (!ok)
        return false;
    const float nx = float(2.0 * pos.x() / viewport.width() - 1.0);
    const float ny = float(1.0 - 2.0 * pos.y() / viewport.height());
    const QVector3D dir = (inv.map(QVector3D(nx, ny, 1.0f)) - inv.map(QVector3D(nx, ny, -1.0f))).normalized();

    const double bz = std::sqrt(1.0 - geo::kE2);
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
    *lonRad = std::atan2(y, x);
    *latRad = std::atan2(z, (1.0 - geo::kE2) * std::sqrt(x * x + y * y));
    return true;
}

bool GlobeCamera::geoAt(const QPointF &pos, const QSize &viewport, double *lonDeg, double *latDeg) const
{
    double lon = 0.0, lat = 0.0;                 // 场景坐标（弧度）
    if (!pick(pos, viewport, &lon, &lat))
        return false;
    fromScene(qRadiansToDegrees(lon), qRadiansToDegrees(lat), lonDeg, latDeg);
    return true;
}

// 场景坐标（= 当前底图的坐标系）-> 屏幕，不做任何转换
bool GlobeCamera::sceneScreenPos(double lonDeg, double latDeg, double altM,
                                 const QSize &viewport, QPointF *pos) const
{
    updateMatrices(viewport);
    const QVector4D c = m_mvp * QVector4D(geo::ecef(qDegreesToRadians(lonDeg), qDegreesToRadians(latDeg),
                                                    altM / (geo::kEarthRadiusKm * 1000.0)), 1.0f);
    if (c.w() < 1e-6f)
        return false;
    *pos = QPointF((c.x() / c.w() + 1.0) * 0.5 * viewport.width(),
                   (1.0 - c.y() / c.w()) * 0.5 * viewport.height());
    return true;
}

bool GlobeCamera::sceneVisiblePos(double lonDeg, double latDeg, double altM,
                                  const QSize &viewport, QPointF *pos) const
{
    updateMatrices(viewport);
    const QVector3D p = geo::ecef(qDegreesToRadians(lonDeg), qDegreesToRadians(latDeg),
                                  altM / (geo::kEarthRadiusKm * 1000.0));
    if (QVector3D::dotProduct(p, m_eye - p) < 0.0f)
        return false;   // 地球背面
    const QVector4D c = m_mvp * QVector4D(p, 1.0f);
    if (c.w() < 1e-6f)
        return false;
    *pos = QPointF((c.x() / c.w() + 1.0) * 0.5 * viewport.width(),
                   (1.0 - c.y() / c.w()) * 0.5 * viewport.height());
    return true;
}

bool GlobeCamera::screenPos(double lonDeg, double latDeg, double altM,
                            const QSize &viewport, QPointF *pos) const
{
    double slon = lonDeg, slat = latDeg;
    toScene(lonDeg, latDeg, &slon, &slat);      // 入参是 WGS84
    return sceneScreenPos(slon, slat, altM, viewport, pos);
}

bool GlobeCamera::visiblePos(double lonDeg, double latDeg, double altM,
                             const QSize &viewport, QPointF *pos) const
{
    double slon = lonDeg, slat = latDeg;
    toScene(lonDeg, latDeg, &slon, &slat);      // 入参是 WGS84（航迹/旗标/标尺）
    return sceneVisiblePos(slon, slat, altM, viewport, pos);
}

QVector<QPolygonF> GlobeCamera::geodesicLines(double lat1, double lon1, double lat2, double lon2,
                                              const QSize &viewport) const
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
        if (sceneVisiblePos(lon, lat, 0.0, viewport, &sp)) {
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

// 套索边界采样：两点间按经纬度平面线性插值再投影。
// 必须和 lassogeo::pointInPolygon 的多边形口径一致，否则「看到的边界」和「查询用的边界」
// 会差出十几公里，出现「明明在框里却没被选中」的假象。
void GlobeCamera::sampleLonLatLine(double lat1, double lon1, double lat2, double lon2,
                                   const QSize &viewport, QVector<QPointF> *pts, QVector<bool> *vis) const
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
        const bool ok = visiblePos(lon1 + dLon * f, lat1 + dLat * f, 0.0, viewport, &sp);
        pts->append(sp);
        vis->append(ok);
    }
}

quint64 GlobeCamera::tileKey(int z, int x, int y)
{
    return (quint64(z) << 48) | (quint64(x) << 24) | quint64(y);
}

// XYZ 瓦片范围：经度起止，墨卡托 y 起止（北 -> 南，对应纹理从上到下）
QVector4D GlobeCamera::tileRange(int z, int x, int y)
{
    const double n = double(1 << z);
    return QVector4D(float(x / n * 2.0 * M_PI - M_PI),
                     float((x + 1) / n * 2.0 * M_PI - M_PI),
                     float(M_PI * (1.0 - 2.0 * y / n)),
                     float(M_PI * (1.0 - 2.0 * (y + 1) / n)));
}

void GlobeCamera::tileBounds(int z, int x, int y, QVector3D *center, float *radius)
{
    const QVector4D r = tileRange(z, x, y);
    QVector3D pts[25];
    QVector3D sum;
    int k = 0;
    for (int j = 0; j < 5; ++j) {
        const double lat = mercToLat(r.z() + (r.w() - r.z()) * j / 4.0);
        for (int i = 0; i < 5; ++i) {
            pts[k] = geo::ecef(r.x() + (r.y() - r.x()) * i / 4.0, lat, 0.0);
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

// 四叉树遍历：背面（地平线以下）和视锥外的瓦片剔除，近处瓦片细分到更高层级
void GlobeCamera::collectTiles(int z, int x, int y, int mapMaxZoom, QVector<TileId> &out) const
{
    const int maxZ = qMax(2, mapMaxZoom);
    bool refine = z < 2;    // 至少细分到 2 级，保证椭球形状足够圆
    if (z >= 2) {
        QVector3D c;
        float r = 0.0f;
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
                collectTiles(z + 1, 2 * x + dx, 2 * y + dy, mapMaxZoom, out);
    } else {
        out.append(TileId{z, x, y});
    }
}
