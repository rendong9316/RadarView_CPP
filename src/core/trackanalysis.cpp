#include "trackanalysis.h"
#include "geocalc.h"

#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace trackanalysis {

namespace {

double wrapLon(double d)
{
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

double interpFinite(double a, double b, double f)
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return std::numeric_limits<double>::quiet_NaN();
    return a + (b - a) * f;
}

double interpAngle(double a, double b, double f)
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return std::numeric_limits<double>::quiet_NaN();
    double v = a + wrapAngleDiffDeg(b - a) * f;
    while (v < 0.0) v += 360.0;
    while (v >= 360.0) v -= 360.0;
    return v;
}

// 在按时间升序的点列中找距 t 最近且差值 <= maxGapMs 的下标，找不到返回 -1
int findNearestByTime(const QVector<TrackPoint> &pts, qint64 t, qint64 maxGapMs)
{
    if (pts.isEmpty())
        return -1;
    auto it = std::lower_bound(pts.constBegin(), pts.constEnd(), t,
                               [](const TrackPoint &p, qint64 v) { return p.t < v; });
    int best = -1;
    qint64 bestDiff = maxGapMs + 1;
    if (it != pts.constEnd()) {
        const qint64 d = std::llabs(it->t - t);
        if (d < bestDiff) {
            bestDiff = d;
            best = int(it - pts.constBegin());
        }
    }
    if (it != pts.constBegin()) {
        const auto prev = it - 1;
        const qint64 d = std::llabs(prev->t - t);
        if (d < bestDiff) {
            bestDiff = d;
            best = int(prev - pts.constBegin());
        }
    }
    return bestDiff <= maxGapMs ? best : -1;
}

bool interpolateAt(const QVector<TrackPoint> &pts, qint64 t, qint64 maxGapMs, TrackPoint *out)
{
    if (pts.isEmpty() || t < pts.first().t || t > pts.last().t)
        return false;
    auto it = std::lower_bound(pts.constBegin(), pts.constEnd(), t,
                               [](const TrackPoint &p, qint64 v) { return p.t < v; });
    if (it != pts.constEnd() && it->t == t) {
        *out = *it;
        return true;
    }
    if (it == pts.constBegin() || it == pts.constEnd())
        return false;
    const TrackPoint &a = *(it - 1);
    const TrackPoint &b = *it;
    const qint64 span = b.t - a.t;
    if (span <= 0 || span > maxGapMs)
        return false;
    const double f = double(t - a.t) / double(span);
    TrackPoint p;
    p.t = t;
    p.lat = a.lat + (b.lat - a.lat) * f;
    p.lon = wrapLon(a.lon + wrapLon(b.lon - a.lon) * f);
    p.alt = float(interpFinite(a.alt, b.alt, f));
    p.heading = float(interpAngle(a.heading, b.heading, f));
    p.speed = float(interpFinite(a.speed, b.speed, f));
    p.vrate = float(interpFinite(a.vrate, b.vrate, f));
    p.prange = float(interpFinite(a.prange, b.prange, f));
    p.pvr = float(interpFinite(a.pvr, b.pvr, f));
    p.paz = float(interpAngle(a.paz, b.paz, f));
    p.snr = float(interpFinite(a.snr, b.snr, f));
    *out = p;
    return true;
}

ErrorMetric computeMetric(const QVector<double> &vals)
{
    ErrorMetric m;
    QVector<double> absVals;
    absVals.reserve(vals.size());
    double sumSq = 0.0, sumAbs = 0.0, sum = 0.0, maxA = 0.0;
    for (double v : vals) {
        if (!std::isfinite(v))
            continue;
        const double a = std::fabs(v);
        absVals.append(a);
        sum += v;
        sumAbs += a;
        sumSq += v * v;
        if (a > maxA)
            maxA = a;
    }
    if (absVals.isEmpty())
        return m;
    std::sort(absVals.begin(), absVals.end());
    m.count = absVals.size();
    m.rmse = std::sqrt(sumSq / double(m.count));
    m.mae = sumAbs / double(m.count);
    m.bias = sum / double(m.count);
    m.maxAbs = maxA;
    const int idx95 = qBound(0, int(std::ceil(0.95 * double(m.count))) - 1, m.count - 1);
    m.p95 = absVals.at(idx95);
    return m;
}

} // namespace

double wrapAngleDiffDeg(double deg)
{
    if (!std::isfinite(deg))
        return std::numeric_limits<double>::quiet_NaN();
    while (deg > 180.0) deg -= 360.0;
    while (deg <= -180.0) deg += 360.0;
    return deg;
}

QVector<FilterResidualPoint> computeFilterResiduals(const Track &track, qint64 maxMatchGapMs)
{
    QVector<FilterResidualPoint> out;
    const QVector<TrackPoint> &assc = track.hasRawPoints() ? track.rawPoints : QVector<TrackPoint>();
    if (assc.isEmpty())
        return out;
    out.reserve(assc.size());
    for (const TrackPoint &a : assc) {
        FilterResidualPoint r;
        r.timeMs = a.t;
        r.asscLat = a.lat;
        r.asscLon = a.lon;
        const int pi = findNearestByTime(track.predPoints, a.t, maxMatchGapMs);
        if (pi >= 0) {
            const TrackPoint &p = track.predPoints.at(pi);
            r.hasPred = true;
            r.predLat = p.lat;
            r.predLon = p.lon;
            r.predGeoErrKm = geocalc::vincentyKm(p.lat, p.lon, a.lat, a.lon);
            if (std::isfinite(a.prange) && std::isfinite(p.prange))
                r.dRange = double(a.prange) - double(p.prange);
            if (std::isfinite(a.paz) && std::isfinite(p.paz))
                r.dAz = wrapAngleDiffDeg(double(a.paz) - double(p.paz));
            if (std::isfinite(a.pvr) && std::isfinite(p.pvr))
                r.dVr = double(a.pvr) - double(p.pvr);
        }
        const int si = findNearestByTime(track.points, a.t, maxMatchGapMs);
        if (si >= 0) {
            const TrackPoint &s = track.points.at(si);
            r.hasSmooth = true;
            r.smoothLat = s.lat;
            r.smoothLon = s.lon;
            r.smoothGeoErrKm = geocalc::vincentyKm(s.lat, s.lon, a.lat, a.lon);
            if (std::isfinite(a.prange) && std::isfinite(s.prange))
                r.smoothDRange = double(a.prange) - double(s.prange);
            if (std::isfinite(a.paz) && std::isfinite(s.paz))
                r.smoothDAz = wrapAngleDiffDeg(double(a.paz) - double(s.paz));
            if (std::isfinite(a.pvr) && std::isfinite(s.pvr))
                r.smoothDVr = double(a.pvr) - double(s.pvr);
        }
        if (r.hasPred || r.hasSmooth)
            out.append(r);
    }
    return out;
}

TrackCompareResult compareTracks(const QVector<TrackPoint> &a,
                                 const QVector<TrackPoint> &b,
                                 qint64 maxInterpGapMs)
{
    TrackCompareResult res;
    if (a.isEmpty() || b.isEmpty())
        return res;
    const qint64 lo = qMax(a.first().t, b.first().t);
    const qint64 hi = qMin(a.last().t, b.last().t);
    if (hi < lo)
        return res;
    res.timeOverlapped = true;
    res.overlapStartMs = lo;
    res.overlapEndMs = hi;

    QVector<double> vGeo, vR, vAz, vVr, vAlt, vSpd;
    res.samples.reserve(b.size());
    double maxGeo = -1.0;

    for (const TrackPoint &pb : b) {
        if (pb.t < lo || pb.t > hi)
            continue;
        TrackPoint pa;
        if (!interpolateAt(a, pb.t, maxInterpGapMs, &pa))
            continue;
        TrackDiffSample s;
        s.timeMs = pb.t;
        s.latA = pa.lat;
        s.lonA = pa.lon;
        s.latB = pb.lat;
        s.lonB = pb.lon;
        s.geoDistKm = geocalc::vincentyKm(pa.lat, pa.lon, pb.lat, pb.lon);
        vGeo.append(s.geoDistKm);
        if (s.geoDistKm > maxGeo) {
            maxGeo = s.geoDistKm;
            res.maxGeoErrorIndex = res.samples.size();
        }
        if (std::isfinite(pa.prange) && std::isfinite(pb.prange)) {
            s.dRange = double(pb.prange) - double(pa.prange);
            vR.append(s.dRange);
        }
        if (std::isfinite(pa.paz) && std::isfinite(pb.paz)) {
            s.dAz = wrapAngleDiffDeg(double(pb.paz) - double(pa.paz));
            vAz.append(s.dAz);
        }
        if (std::isfinite(pa.pvr) && std::isfinite(pb.pvr)) {
            s.dVr = double(pb.pvr) - double(pa.pvr);
            vVr.append(s.dVr);
        }
        if (std::isfinite(pa.alt) && std::isfinite(pb.alt) && (pa.alt != 0.0f || pb.alt != 0.0f)) {
            s.dAltM = double(pb.alt) - double(pa.alt);
            vAlt.append(s.dAltM);
        }
        if (std::isfinite(pa.speed) && std::isfinite(pb.speed) && (pa.speed != 0.0f || pb.speed != 0.0f)) {
            s.dSpeedKt = double(pb.speed) - double(pa.speed);
            vSpd.append(s.dSpeedKt);
        }
        res.samples.append(s);
    }

    res.coverageB = b.isEmpty() ? 0.0 : double(res.samples.size()) / double(b.size());
    res.geoDistKm = computeMetric(vGeo);
    res.dRange = computeMetric(vR);
    res.dAz = computeMetric(vAz);
    res.dVr = computeMetric(vVr);
    res.dAltM = computeMetric(vAlt);
    res.dSpeedKt = computeMetric(vSpd);
    return res;
}

bool exportCompareCsv(const TrackCompareResult &res, const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QTextStream out(&f);
    out.setCodec("UTF-8");
    out.setGenerateByteOrderMark(true);
    out << "时间戳(UTC+8),A_纬度,A_经度,B_纬度,B_经度,水平距离误差(km),dRange,dAz(deg),dVr(m/s),dAlt(m),dSpeed(kt)\r\n";
    auto fmt = [](double v, int prec) {
        return std::isfinite(v) ? QString::number(v, 'f', prec) : QString();
    };
    for (const TrackDiffSample &s : res.samples) {
        out << formatBeijingTime(s.timeMs) << ','
            << QString::number(s.latA, 'f', 6) << ','
            << QString::number(s.lonA, 'f', 6) << ','
            << QString::number(s.latB, 'f', 6) << ','
            << QString::number(s.lonB, 'f', 6) << ','
            << QString::number(s.geoDistKm, 'f', 4) << ','
            << fmt(s.dRange, 3) << ','
            << fmt(s.dAz, 3) << ','
            << fmt(s.dVr, 3) << ','
            << fmt(s.dAltM, 2) << ','
            << fmt(s.dSpeedKt, 2) << "\r\n";
    }
    out.flush();
    if (f.error() != QFile::NoError) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace trackanalysis
