#include "spatiallasso.h"
#include "geocalc.h"

#include <algorithm>
#include <cmath>

namespace {

const double kPi = 3.14159265358979323846;
const double kEarthRadiusM = 6371000.0;

double toRad(double deg) { return deg * kPi / 180.0; }

// Douglas-Peucker：对下标区间 [lo, hi] 递归取最大偏差点
void dpRecurse(const QVector<LassoVertex> &pts, int lo, int hi, double epsSq, QVector<bool> *keep)
{
    if (hi <= lo + 1)
        return;
    const double x1 = pts[lo].lon, y1 = pts[lo].lat;
    const double dx = pts[hi].lon - x1, dy = pts[hi].lat - y1;
    const double lenSq = dx * dx + dy * dy;

    double maxDistSq = 0.0;
    int maxIdx = lo;
    for (int i = lo + 1; i < hi; ++i) {
        double distSq;
        if (lenSq == 0.0) {
            const double ddx = pts[i].lon - x1, ddy = pts[i].lat - y1;
            distSq = ddx * ddx + ddy * ddy;
        } else {
            const double cross = (pts[i].lon - x1) * dy - (pts[i].lat - y1) * dx;
            distSq = (cross * cross) / lenSq;
        }
        if (distSq > maxDistSq) {
            maxDistSq = distSq;
            maxIdx = i;
        }
    }

    if (maxDistSq > epsSq) {
        (*keep)[maxIdx] = true;
        dpRecurse(pts, lo, maxIdx, epsSq, keep);
        dpRecurse(pts, maxIdx, hi, epsSq, keep);
    }
}

} // namespace

namespace lassogeo {

bool pointInPolygon(double lat, double lon, const QVector<LassoVertex> &poly)
{
    const int n = poly.size();
    if (n < 3)
        return false;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const double yi = poly[i].lat, yj = poly[j].lat;
        const double xi = poly[i].lon, xj = poly[j].lon;
        if ((yi > lat) != (yj > lat)) {
            const double intersectX = xi + ((lat - yi) / (yj - yi)) * (xj - xi);
            if (lon < intersectX)
                inside = !inside;
        }
    }
    return inside;
}

bool segmentsIntersect(double ax, double ay, double bx, double by,
                       double cx, double cy, double dx, double dy)
{
    // 包围盒快速排除
    const double s1MinX = std::min(ax, bx), s1MaxX = std::max(ax, bx);
    const double s1MinY = std::min(ay, by), s1MaxY = std::max(ay, by);
    const double s2MinX = std::min(cx, dx), s2MaxX = std::max(cx, dx);
    const double s2MinY = std::min(cy, dy), s2MaxY = std::max(cy, dy);
    if (s1MaxX < s2MinX || s2MaxX < s1MinX || s1MaxY < s2MinY || s2MaxY < s1MinY)
        return false;

    auto cross = [](double ox, double oy, double ax2, double ay2, double bx2, double by2) {
        return (ax2 - ox) * (by2 - oy) - (ay2 - oy) * (bx2 - ox);
    };
    const double d1 = cross(cx, cy, dx, dy, ax, ay);
    const double d2 = cross(cx, cy, dx, dy, bx, by);
    const double d3 = cross(ax, ay, bx, by, cx, cy);
    const double d4 = cross(ax, ay, bx, by, dx, dy);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;

    // 共线且落在对方包围盒内
    if (d1 == 0 && cx >= std::min(ax, bx) && cx <= std::max(ax, bx) &&
        cy >= std::min(ay, by) && cy <= std::max(ay, by))
        return true;
    if (d2 == 0 && dx >= std::min(ax, bx) && dx <= std::max(ax, bx) &&
        dy >= std::min(ay, by) && dy <= std::max(ay, by))
        return true;
    if (d3 == 0 && ax >= std::min(cx, dx) && ax <= std::max(cx, dx) &&
        ay >= std::min(cy, dy) && ay <= std::max(cy, dy))
        return true;
    if (d4 == 0 && bx >= std::min(cx, dx) && bx <= std::max(cx, dx) &&
        by >= std::min(cy, dy) && by <= std::max(cy, dy))
        return true;
    return false;
}

bool trackIntersectsPolygon(const Track &t, const QVector<LassoVertex> &poly)
{
    if (t.points.isEmpty() || poly.size() < 3)
        return false;

    // 多边形包围盒（O(E) 只算一次）
    double polyMinLat = 1e300, polyMaxLat = -1e300;
    double polyMinLng = 1e300, polyMaxLng = -1e300;
    for (const LassoVertex &v : poly) {
        polyMinLat = std::min(polyMinLat, v.lat);
        polyMaxLat = std::max(polyMaxLat, v.lat);
        polyMinLng = std::min(polyMinLng, v.lon);
        polyMaxLng = std::max(polyMaxLng, v.lon);
    }

    // 第一遍：逐点判定，同时累计航迹包围盒
    double trkMinLat = 1e300, trkMaxLat = -1e300;
    double trkMinLng = 1e300, trkMaxLng = -1e300;
    for (const TrackPoint &p : t.points) {
        trkMinLat = std::min(trkMinLat, p.lat);
        trkMaxLat = std::max(trkMaxLat, p.lat);
        trkMinLng = std::min(trkMinLng, p.lon);
        trkMaxLng = std::max(trkMaxLng, p.lon);

        if (p.lat < polyMinLat || p.lat > polyMaxLat || p.lon < polyMinLng || p.lon > polyMaxLng)
            continue;
        if (pointInPolygon(p.lat, p.lon, poly))
            return true;
    }

    // 航迹包围盒与多边形包围盒不相交 → 不可能有线段穿越
    if (trkMaxLat < polyMinLat || trkMinLat > polyMaxLat ||
        trkMaxLng < polyMinLng || trkMinLng > polyMaxLng)
        return false;

    // 第二遍：线段穿越判定（两端都在框外但中间穿过）
    const int n = poly.size();
    for (int i = 0; i + 1 < t.points.size(); ++i) {
        const TrackPoint &a = t.points[i];
        const TrackPoint &b = t.points[i + 1];
        const double segMinLat = std::min(a.lat, b.lat), segMaxLat = std::max(a.lat, b.lat);
        const double segMinLng = std::min(a.lon, b.lon), segMaxLng = std::max(a.lon, b.lon);
        if (segMaxLat < polyMinLat || segMinLat > polyMaxLat ||
            segMaxLng < polyMinLng || segMinLng > polyMaxLng)
            continue;
        for (int j = 0; j < n; ++j) {
            const LassoVertex &p1 = poly[j];
            const LassoVertex &p2 = poly[(j + 1) % n];
            if (segmentsIntersect(a.lon, a.lat, b.lon, b.lat,
                                  p1.lon, p1.lat, p2.lon, p2.lat))
                return true;
        }
    }
    return false;
}

QVector<LassoVertex> simplifyPolygon(const QVector<LassoVertex> &poly, double epsilonMeters)
{
    const int n = poly.size();
    if (n <= 3)
        return poly;

    // 米 → 度：按平均纬度换算经向尺度
    double meanLat = 0.0;
    for (const LassoVertex &v : poly)
        meanLat += v.lat;
    meanLat /= n;
    const double cosLat = std::max(1e-6, std::fabs(std::cos(toRad(meanLat))));
    const double degPerMeter = 1.0 / (111320.0 * cosLat);
    const double eps = epsilonMeters * degPerMeter;

    QVector<bool> keep(n, false);
    keep[0] = true;
    keep[n - 1] = true;
    dpRecurse(poly, 0, n - 1, eps * eps, &keep);

    QVector<LassoVertex> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i)
        if (keep[i])
            out.append(poly[i]);
    return out.size() >= 3 ? out : poly;
}

double polygonAreaSqKm(const QVector<LassoVertex> &poly)
{
    const int n = poly.size();
    if (n < 3)
        return 0.0;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const LassoVertex &prev = poly[(i - 1 + n) % n];
        const LassoVertex &next = poly[(i + 1) % n];
        const double lng = toRad(poly[i].lon);
        sum += lng * (std::sin(toRad(next.lat)) - std::sin(toRad(prev.lat)));
    }
    return std::fabs(sum) * kEarthRadiusM * kEarthRadiusM / 2.0 / 1.0e6;
}

double polygonPerimeterKm(const QVector<LassoVertex> &poly)
{
    const int n = poly.size();
    if (n < 2)
        return 0.0;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const LassoVertex &a = poly[i];
        const LassoVertex &b = poly[(i + 1) % n];
        sum += geocalc::haversineKm(a.lat, a.lon, b.lat, b.lon);
    }
    return sum;
}

} // namespace lassogeo

// ---------------------------------------------------------------
//  SpatialLasso
// ---------------------------------------------------------------
SpatialLasso::SpatialLasso(QObject *parent) : QObject(parent)
{
}

void SpatialLasso::emitChanged()
{
    emit changed();
}

double SpatialLasso::areaSqKm() const
{
    return m_closed ? lassogeo::polygonAreaSqKm(m_vertices) : 0.0;
}

double SpatialLasso::perimeterKm() const
{
    return m_closed ? lassogeo::polygonPerimeterKm(m_vertices) : 0.0;
}

void SpatialLasso::setMode(Mode m)
{
    if (m_mode == m)
        return;
    m_mode = m;
    if (!m_closed)
        m_vertices.clear();   // 换模式清掉半成品，避免两种输入混在一起
    emitChanged();
}

void SpatialLasso::activate()
{
    clearAll();
    m_active = true;
    emitChanged();
}

void SpatialLasso::deactivate()
{
    m_active = false;
    m_freehand = false;
    m_hasMouse = false;
    emitChanged();
}

void SpatialLasso::toggle()
{
    if (m_active)
        deactivate();
    else
        activate();
}

void SpatialLasso::clearAll()
{
    m_active = false;
    m_closed = false;
    m_freehand = false;
    m_hasMouse = false;
    m_vertices.clear();
    m_mouseLat = m_mouseLon = 0.0;
    emitChanged();
}

void SpatialLasso::addVertex(double lat, double lon)
{
    if (m_closed)
        return;
    m_vertices.append(LassoVertex(m_nextId++, lat, lon));
    emitChanged();
}

void SpatialLasso::removeVertex(int id)
{
    if (m_closed)
        return;
    for (int i = 0; i < m_vertices.size(); ++i)
        if (m_vertices[i].id == id) {
            m_vertices.remove(i);
            emitChanged();
            return;
        }
}

void SpatialLasso::moveVertex(int id, double lat, double lon)
{
    for (int i = 0; i < m_vertices.size(); ++i)
        if (m_vertices[i].id == id) {
            m_vertices[i].lat = lat;
            m_vertices[i].lon = lon;
            emitChanged();
            return;
        }
}

void SpatialLasso::undoLastVertex()
{
    if (m_closed || m_vertices.isEmpty())
        return;
    m_vertices.removeLast();
    emitChanged();
}

void SpatialLasso::closePolygon()
{
    if (m_closed || m_vertices.size() < 3)
        return;

    // 双击/松手常留下与上一点重合的顶点，去掉
    if (m_vertices.size() >= 2) {
        const LassoVertex &last = m_vertices.last();
        const LassoVertex &prev = m_vertices[m_vertices.size() - 2];
        if (std::fabs(last.lat - prev.lat) < 1e-7 && std::fabs(last.lon - prev.lon) < 1e-7)
            m_vertices.removeLast();
    }
    if (m_vertices.size() < 3)
        return;

    // 自由手绘顶点太多会让逐航迹判定变慢，闭合成多边形前先简化
    if (m_vertices.size() > 30)
        m_vertices = lassogeo::simplifyPolygon(m_vertices, 5.0);

    m_closed = true;
    m_active = false;
    m_freehand = false;
    m_hasMouse = false;
    emitChanged();
    emit closed();
}

void SpatialLasso::beginFreehand(double lat, double lon)
{
    m_vertices.clear();
    m_closed = false;
    m_freehand = true;
    m_active = true;
    m_vertices.append(LassoVertex(m_nextId++, lat, lon));
    emitChanged();
}

void SpatialLasso::addFreehandPoint(double lat, double lon)
{
    if (!m_freehand)
        return;
    if (!m_vertices.isEmpty()) {
        const LassoVertex &last = m_vertices.last();
        if (geocalc::haversineKm(last.lat, last.lon, lat, lon) < 0.010)   // 10 m 内不重复记录
            return;
    }
    m_vertices.append(LassoVertex(m_nextId++, lat, lon));
    emitChanged();
}

void SpatialLasso::endFreehand()
{
    m_freehand = false;
    if (m_vertices.size() >= 3)
        closePolygon();
    else
        clearAll();
}

void SpatialLasso::setMouse(bool valid, double lat, double lon)
{
    m_hasMouse = valid;
    m_mouseLat = lat;
    m_mouseLon = lon;
    emit mouseMoved();
}
