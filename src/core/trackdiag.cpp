#include "trackdiag.h"

#include "geocalc.h"
#include "matfile.h"
#include "point3dparsers.h"
#include "trackanalysis.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace trackdiag {

namespace {

const double kDegToRad = 3.14159265358979323846 / 180.0;
const double kNaN = std::numeric_limits<double>::quiet_NaN();
const float kNaNf = std::numeric_limits<float>::quiet_NaN();

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

bool validLatLon(double lat, double lon)
{
    return std::isfinite(lat) && std::isfinite(lon)
        && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

double wrapDeg180(double d)
{
    if (!std::isfinite(d))
        return kNaN;
    while (d > 180.0) d -= 360.0;
    while (d <= -180.0) d += 360.0;
    return d;
}

double wrapDeg360(double d)
{
    if (!std::isfinite(d))
        return kNaN;
    double r = std::fmod(d, 360.0);
    if (r < 0.0)
        r += 360.0;
    return r;
}

// 距离统一转 km（天波雷达 rIndex/prange 常直接为 km，若 > 20000 视为米）
double rangeToKm(double r)
{
    if (!std::isfinite(r))
        return kNaN;
    return r > 20000.0 ? (r / 1000.0) : r;
}

// 方位角是否落在 [startAz, endAz] 扇区内（支持跨 0°/360°）
bool azInSector(double azDeg, double startAzDeg, double endAzDeg)
{
    if (!std::isfinite(azDeg))
        return true;
    const double span = endAzDeg - startAzDeg;
    if (std::fabs(span) >= 359.5)
        return true;
    const double a = wrapDeg360(azDeg);
    const double s = wrapDeg360(startAzDeg);
    const double e = wrapDeg360(endAzDeg);
    if (s <= e)
        return a >= s && a <= e;
    return a >= s || a <= e;
}

double geoPathLengthKm(const QVector<TrackPoint> &pts)
{
    double total = 0.0;
    for (int i = 1; i < pts.size(); ++i) {
        const double d = geocalc::vincentyKm(pts.at(i - 1).lat, pts.at(i - 1).lon,
                                             pts.at(i).lat, pts.at(i).lon);
        if (std::isfinite(d) && d > 0.0)
            total += d;
    }
    return total;
}

// 在按时间升序的 TrackPoint 中二分找距 t 最近且差值 <= maxGapMs 的索引
int findNearestTrackPoint(const QVector<TrackPoint> &pts, qint64 t, qint64 maxGapMs)
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

// 在按时间升序的 DetPointFrame 中二分找距 t 最近且差值 <= maxGapMs 的帧下标
int findNearestDetFrame(const QVector<DetPointFrame> &frames, qint64 t, qint64 maxGapMs)
{
    if (frames.isEmpty())
        return -1;
    auto it = std::lower_bound(frames.constBegin(), frames.constEnd(), t,
                               [](const DetPointFrame &f, qint64 v) { return f.timeMs < v; });
    int best = -1;
    qint64 bestDiff = maxGapMs + 1;
    if (it != frames.constEnd()) {
        const qint64 d = std::llabs(it->timeMs - t);
        if (d < bestDiff) {
            bestDiff = d;
            best = int(it - frames.constBegin());
        }
    }
    if (it != frames.constBegin()) {
        const auto prev = it - 1;
        const qint64 d = std::llabs(prev->timeMs - t);
        if (d < bestDiff) {
            bestDiff = d;
            best = int(prev - frames.constBegin());
        }
    }
    return bestDiff <= maxGapMs ? best : -1;
}

double interpFinite(double a, double b, double f)
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return kNaN;
    return a + (b - a) * f;
}

double interpAngleDeg(double a, double b, double f)
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return kNaN;
    return wrapDeg360(a + wrapDeg180(b - a) * f);
}

bool sampleTrackAtTime(const QVector<TrackPoint> &pts, qint64 t, qint64 maxGapMs, TrackPoint *out)
{
    if (pts.isEmpty() || !out)
        return false;
    if (t <= pts.first().t) {
        if (pts.first().t - t > maxGapMs)
            return false;
        *out = pts.first();
        return true;
    }
    if (t >= pts.last().t) {
        if (t - pts.last().t > maxGapMs)
            return false;
        *out = pts.last();
        return true;
    }
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
    if (span <= 0 || span > maxGapMs) {
        const qint64 da = std::llabs(t - a.t);
        const qint64 db = std::llabs(b.t - t);
        if (qMin(da, db) > maxGapMs)
            return false;
        *out = (da <= db) ? a : b;
        return true;
    }
    const double f = double(t - a.t) / double(span);
    TrackPoint p;
    p.t = t;
    p.lat = a.lat + (b.lat - a.lat) * f;
    double dLon = b.lon - a.lon;
    while (dLon > 180.0) dLon -= 360.0;
    while (dLon < -180.0) dLon += 360.0;
    p.lon = a.lon + dLon * f;
    while (p.lon > 180.0) p.lon -= 360.0;
    while (p.lon < -180.0) p.lon += 360.0;
    p.prange = float(interpFinite(a.prange, b.prange, f));
    p.paz = float(interpAngleDeg(a.paz, b.paz, f));
    p.pvr = float(interpFinite(a.pvr, b.pvr, f));
    p.snr = float(interpFinite(a.snr, b.snr, f));
    *out = p;
    return true;
}

double computeMeanSnr(const Track &tr)
{
    double sum = 0.0;
    int cnt = 0;
    const QVector<TrackPoint> &src = tr.hasRawPoints() ? tr.rawPoints : tr.points;
    for (const TrackPoint &p : src) {
        if (std::isfinite(p.snr)) {
            sum += double(p.snr);
            ++cnt;
        }
    }
    if (cnt == 0 && tr.hasRawPoints()) {
        for (const TrackPoint &p : tr.points) {
            if (std::isfinite(p.snr)) {
                sum += double(p.snr);
                ++cnt;
            }
        }
    }
    return cnt > 0 ? (sum / double(cnt)) : kNaN;
}

double computeTrailingSnr(const Track &tr, int tailCount)
{
    const QVector<TrackPoint> &src = tr.hasRawPoints() ? tr.rawPoints : tr.points;
    double sum = 0.0;
    int cnt = 0;
    for (int i = src.size() - 1; i >= 0 && cnt < tailCount; --i) {
        if (std::isfinite(src.at(i).snr)) {
            sum += double(src.at(i).snr);
            ++cnt;
        }
    }
    return cnt > 0 ? (sum / double(cnt)) : kNaN;
}

int estimateSuccLoss(const Track &tr)
{
    if (tr.points.isEmpty() || tr.rawPoints.isEmpty())
        return 0;
    int streak = 0;
    for (int i = tr.points.size() - 1; i >= 0; --i) {
        if (findNearestTrackPoint(tr.rawPoints, tr.points.at(i).t, 2000) < 0)
            ++streak;
        else
            break;
    }
    return streak;
}

// 极坐标两点间近似平面距离（km）
double polarDistKm(double r1, double az1Deg, double r2, double az2Deg)
{
    const double rk1 = rangeToKm(r1);
    const double rk2 = rangeToKm(r2);
    if (!std::isfinite(rk1) || !std::isfinite(rk2))
        return std::numeric_limits<double>::infinity();
    const double dr = rk1 - rk2;
    const double dazRad = wrapDeg180(az1Deg - az2Deg) * kDegToRad;
    const double rMean = 0.5 * (rk1 + rk2);
    const double dCross = rMean * std::sin(dazRad);
    return std::hypot(dr, dCross);
}

// 由 TrackDiagMeta 的 filterPara 或默认值计算当前连丢次数下的有效波门
void effectiveGate(const TrackDiagMeta *meta, int lostStreak,
                   float *gateR, float *gateAz, float *gateVr)
{
    const int k = qBound(0, lostStreak, 10);
    if (meta && meta->valid && meta->gate.valid) {
        const double fr = meta->gate.fixedR > 0.0 ? meta->gate.fixedR : 10.0;
        const double fa = meta->gate.fixedA > 0.0 ? meta->gate.fixedA : 1.0;
        const double fv = meta->gate.fixedV > 0.0 ? meta->gate.fixedV : 5.0;
        const double flR = qMax(0.0, meta->gate.floatR);
        const double flA = qMax(0.0, meta->gate.floatA);
        const double flV = qMax(0.0, meta->gate.floatV);
        *gateR = float(fr * (1.0 + flR * double(k)));
        *gateAz = float(fa * (1.0 + flA * double(k)));
        *gateVr = float(fv * (1.0 + flV * double(k)));
    } else {
        *gateR = float(10.0 * (1.0 + 0.1 * double(k)));
        *gateAz = float(1.0 * (1.0 + 0.05 * double(k)));
        *gateVr = float(5.0 * (1.0 + 0.08 * double(k)));
    }
}

// 估计航迹尾段地理运动速度（km/s）与航向（deg）以及极坐标变化率
struct TailKinematics {
    double speedKmS;
    double bearingDeg;
    double drPerSec;
    double dazPerSec;
    double dvrPerSec;
    bool validGeo;
    bool validPolar;

    TailKinematics()
        : speedKmS(0.0), bearingDeg(0.0),
          drPerSec(0.0), dazPerSec(0.0), dvrPerSec(0.0),
          validGeo(false), validPolar(false)
    {}
};

TailKinematics estimateTailKinematics(const QVector<TrackPoint> &pts)
{
    TailKinematics k;
    if (pts.size() < 2)
        return k;
    const TrackPoint &p1 = pts.last();
    const int i0 = qMax(0, pts.size() - 4);
    const TrackPoint &p0 = pts.at(i0);
    const double dtSec = double(p1.t - p0.t) / 1000.0;
    if (dtSec <= 0.5)
        return k;

    const double distKm = geocalc::vincentyKm(p0.lat, p0.lon, p1.lat, p1.lon);
    if (std::isfinite(distKm)) {
        k.speedKmS = qBound(0.0, distKm / dtSec, 1.5);
        k.bearingDeg = geocalc::initialBearing(p0.lat, p0.lon, p1.lat, p1.lon);
        k.validGeo = true;
    }
    if (std::isfinite(p0.prange) && std::isfinite(p1.prange)
            && std::isfinite(p0.paz) && std::isfinite(p1.paz)
            && std::isfinite(p0.pvr) && std::isfinite(p1.pvr)) {
        k.drPerSec = (double(p1.prange) - double(p0.prange)) / dtSec;
        k.dazPerSec = qBound(-1.0, wrapDeg180(double(p1.paz) - double(p0.paz)) / dtSec, 1.0);
        k.dvrPerSec = qBound(-2.0, (double(p1.pvr) - double(p0.pvr)) / dtSec, 2.0);
        k.validPolar = true;
    }
    return k;
}

// 打包 (frameId, pointId) 为 64 位复合键，便于快速哈希查归属
inline quint64 packFramePointKey(int frameId, int pointId)
{
    return (quint64(quint32(frameId)) << 32) | quint64(quint32(pointId));
}

// 按时间稳定排序并去重（与 Track::normalize() 保持 1:1 对齐）
struct TimedRow {
    qint64 t;
    int frameId;
    int val;
};

void sortAndDedupTimedRows(QVector<TimedRow> *rows)
{
    std::stable_sort(rows->begin(), rows->end(),
                     [](const TimedRow &a, const TimedRow &b) { return a.t < b.t; });
    auto last = std::unique(rows->begin(), rows->end(),
                            [](const TimedRow &a, const TimedRow &b) { return a.t == b.t; });
    rows->erase(last, rows->end());
}

} // namespace

// ---------------------------------------------------------------------------
//  TrackDiagBundle 成员函数与工具函数
// ---------------------------------------------------------------------------

void TrackDiagBundle::clear()
{
    sourceFileName.clear();
    metas.clear();
    tempPoints.clear();
}

void TrackDiagBundle::merge(const TrackDiagBundle &other)
{
    if (other.isEmpty())
        return;

    const bool exactSameSource = !sourceFileName.isEmpty()
                              && sourceFileName == other.sourceFileName;
    const bool alreadyMergedSource = !sourceFileName.isEmpty()
                                  && !other.sourceFileName.isEmpty()
                                  && sourceFileName.split(QStringLiteral(" + ")).contains(other.sourceFileName);

    if (sourceFileName.isEmpty())
        sourceFileName = other.sourceFileName;
    else if (!other.sourceFileName.isEmpty() && !alreadyMergedSource)
        sourceFileName += QStringLiteral(" + ") + other.sourceFileName;

    for (auto it = other.metas.constBegin(); it != other.metas.constEnd(); ++it)
        metas.insert(it.key(), it.value());

    if (other.tempPoints.isEmpty())
        return;

    // 首次合并或同单文件重导：直接完整保留 other.tempPoints，避免单文件内同 (frameId, pointId) 的多条临时池点互相吞掉
    if (tempPoints.isEmpty() || exactSameSource) {
        tempPoints = other.tempPoints;
        return;
    }
    if (alreadyMergedSource)
        return;

    // 跨文件合并：仅对照已有 tempPoints 去重（不把 other 内部条目插入 existing，保留 other 完整条目）
    auto tempKey = [](const TempTrackPoint &tp) -> quint64 {
        quint64 h = packFramePointKey(tp.frameId, tp.pointId);
        h ^= quint64(tp.timeMs) * 0x9e3779b97f4a7c15ULL;
        const qint64 qr = std::isfinite(tp.prange) ? qint64(std::llround(double(tp.prange) * 100.0)) : 0;
        const qint64 qa = std::isfinite(tp.paz) ? qint64(std::llround(double(tp.paz) * 100.0)) : 0;
        const qint64 qv = std::isfinite(tp.pvr) ? qint64(std::llround(double(tp.pvr) * 100.0)) : 0;
        h ^= quint64(qr) * 0xbf58476d1ce4e5b9ULL;
        h ^= quint64(qa) * 0x94d049bb133111ebULL;
        h ^= quint64(qv) * 0x517cc1b727220a95ULL;
        return h;
    };
    QSet<quint64> existing;
    existing.reserve(tempPoints.size());
    for (const TempTrackPoint &tp : qAsConst(tempPoints))
        existing.insert(tempKey(tp));

    tempPoints.reserve(tempPoints.size() + other.tempPoints.size());
    for (const TempTrackPoint &tp : other.tempPoints) {
        if (!existing.contains(tempKey(tp)))
            tempPoints.append(tp);
    }
}

const TrackDiagMeta *TrackDiagBundle::findMeta(const Track &track) const
{
    const QString k = track.key();
    auto it = metas.constFind(k);
    if (it != metas.constEnd() && it->valid)
        return &it.value();

    const int bno = point3dBatchNo(track);
    if (bno <= 0)
        return nullptr;

    // 尝试按同文件名 + 批号匹配（兼容 Radar / RadarRaw 前缀差异）
    const TrackDiagMeta *fallback = nullptr;
    int matchCount = 0;
    const QString fileSuffix = QStringLiteral("::") + track.fileName;
    for (auto mit = metas.constBegin(); mit != metas.constEnd(); ++mit) {
        if (!mit->valid || mit->batchNo != bno)
            continue;
        if (mit.key().endsWith(fileSuffix))
            return &mit.value();
        fallback = &mit.value();
        ++matchCount;
    }
    return matchCount == 1 ? fallback : nullptr;
}

QString trackTypeName(int trackType)
{
    switch (trackType) {
    case 1: return QStringLiteral("稳定(1)");
    case 2: return QStringLiteral("维持(2)");
    case 6: return QStringLiteral("待定(6)");
    case 7: return QStringLiteral("消亡(7)");
    default: return trackType > 0 ? QStringLiteral("类型%1").arg(trackType) : QStringLiteral("未知");
    }
}

// ---------------------------------------------------------------------------
//  MAT 诊断字段解析
// ---------------------------------------------------------------------------

bool parseTrackDiagFromMat(const QString &matPath, const QString &displayName,
                           TrackSource src, TrackDiagBundle *out, QString *error)
{
    if (!out)
        return fail(error, QStringLiteral("输出诊断包指针为空"));
    out->clear();

    MatFile mat;
    if (!mat.open(matPath, error))
        return false;

    const MatArray list = mat.variable(QStringLiteral("trackList"), error);
    if (!list.isValid())
        return false;
    if (!list.isStruct())
        return fail(error, QStringLiteral("trackList 不是结构体数组"));

    const bool raw = (src == TrackSource::RadarRaw);
    const QString prefix = raw ? QStringLiteral("RAW") : QStringLiteral("RADAR");
    const QString fileName = displayName.isEmpty() ? QFileInfo(matPath).fileName() : displayName;
    out->sourceFileName = fileName;

    const QStringList smoothCandidates = raw
        ? QStringList{ QStringLiteral("asscPointList") }
        : QStringList{ QStringLiteral("smoothPointList"), QStringLiteral("outputPointList") };
    const QStringList predCandidates = {
        QStringLiteral("predictRes"),
        QStringLiteral("predPointList"),
        QStringLiteral("predictPointList"),
        QStringLiteral("predictedPointList"),
        QStringLiteral("prePointList"),
    };

    const int nTracks = list.numel();
    for (int i = 0; i < nTracks; ++i) {
        MatArray smoothPts;
        for (const QString &fn : smoothCandidates) {
            const MatArray cand = list.field(fn, i);
            if (cand.isStruct() && cand.numel() > 0) {
                smoothPts = cand;
                break;
            }
        }
        MatArray asscPts = list.field(QStringLiteral("asscPointList"), i);
        if (!asscPts.isStruct() || asscPts.numel() <= 0)
            asscPts = MatArray();

        MatArray predPts;
        for (const QString &fn : predCandidates) {
            const MatArray cand = list.field(fn, i);
            if (cand.isStruct() && cand.numel() > 0) {
                predPts = cand;
                break;
            }
        }
        if (!smoothPts.isValid() && !asscPts.isValid() && !predPts.isValid())
            continue;

        bool okBatch = false;
        const int batchNo = int(std::lround(list.field(QStringLiteral("BatchNo"), i).scalar(0, &okBatch)));
        if (!okBatch)
            continue;

        TrackDiagMeta meta;
        meta.batchNo = batchNo;
        const QString id = QStringLiteral("%1-%2").arg(prefix).arg(batchNo, 4, 10, QLatin1Char('0'));
        meta.trackKey = id + QStringLiteral("::") + trackSourceName(src) + QStringLiteral("::") + fileName;

        bool ok = false;
        double v = list.field(QStringLiteral("Type"), i).scalar(0, &ok);
        if (ok && std::isfinite(v))
            meta.trackType = int(std::lround(v));

        v = list.field(QStringLiteral("Quality"), i).scalar(0, &ok);
        if (ok && std::isfinite(v))
            meta.quality = qBound(0.0, v, 1.0);

        v = list.field(QStringLiteral("TotalPointCnt"), i).scalar(0, &ok);
        if (ok && std::isfinite(v))
            meta.totalPointCnt = int(std::lround(v));

        v = list.field(QStringLiteral("AsscPointCnt"), i).scalar(0, &ok);
        if (ok && std::isfinite(v))
            meta.asscPointCnt = int(std::lround(v));

        v = list.field(QStringLiteral("TotalLostPointCnt"), i).scalar(0, &ok);
        if (ok && std::isfinite(v))
            meta.totalLostCnt = int(std::lround(v));

        v = list.field(QStringLiteral("SuccLossPointCnt"), i).scalar(0, &ok);
        if (ok && std::isfinite(v))
            meta.succLossCnt = int(std::lround(v));

        v = list.field(QStringLiteral("travelLen"), i).scalar(0, &ok);
        if (ok && std::isfinite(v) && v >= 0.0)
            meta.travelLenKm = v > 50000.0 ? (v / 1000.0) : v;

        // startInfo: [startFrame, confirmFrame, asscCnt, initQ, winLen]
        const MatArray si = list.field(QStringLiteral("startInfo"), i);
        if (si.isNumeric() && si.numel() > 0) {
            if (si.numel() >= 1) {
                const double s0 = si.scalar(0, &ok);
                if (ok && std::isfinite(s0))
                    meta.startFrame = int(std::lround(s0));
            }
            if (si.numel() >= 2) {
                const double s1 = si.scalar(1, &ok);
                if (ok && std::isfinite(s1))
                    meta.confirmFrame = int(std::lround(s1));
            }
            if (si.numel() >= 3) {
                const double s2 = si.scalar(2, &ok);
                if (ok && std::isfinite(s2))
                    meta.startAsscCnt = int(std::lround(s2));
            }
            if (si.numel() >= 4) {
                const double s3 = si.scalar(3, &ok);
                if (ok && std::isfinite(s3))
                    meta.startQuality = s3;
            }
        }

        // filterPara: fixedR, fixedA, fixedV, floatR, floatA, floatV
        const MatArray fp = list.field(QStringLiteral("filterPara"), i);
        if (fp.isStruct() && fp.numel() > 0) {
            bool okR = false, okA = false, okV = false;
            const double fr = fp.field(QStringLiteral("fixedR"), 0).scalar(0, &okR);
            const double fa = fp.field(QStringLiteral("fixedA"), 0).scalar(0, &okA);
            const double fv = fp.field(QStringLiteral("fixedV"), 0).scalar(0, &okV);
            if (okR && okA && okV && std::isfinite(fr) && std::isfinite(fa) && std::isfinite(fv)
                    && fr > 0.0 && fa > 0.0 && fv > 0.0) {
                meta.gate.fixedR = fr;
                meta.gate.fixedA = fa;
                meta.gate.fixedV = fv;
                bool okFl = false;
                const double flR = fp.field(QStringLiteral("floatR"), 0).scalar(0, &okFl);
                if (okFl && std::isfinite(flR)) meta.gate.floatR = flR;
                const double flA = fp.field(QStringLiteral("floatA"), 0).scalar(0, &okFl);
                if (okFl && std::isfinite(flA)) meta.gate.floatA = flA;
                const double flV = fp.field(QStringLiteral("floatV"), 0).scalar(0, &okFl);
                if (okFl && std::isfinite(flV)) meta.gate.floatV = flV;
                meta.gate.valid = true;
            }
        }

        // lifeList: N×8 矩阵（MATLAB 列主序存储：元素 (r, c) 在索引 c * rows + r）
        const MatArray ll = list.field(QStringLiteral("lifeList"), i);
        if (ll.isNumeric() && ll.dims().size() >= 2) {
            const int rows = ll.dims().at(0);
            const int cols = ll.dims().at(1);
            if (rows > 0 && cols >= 6) {
                meta.life.reserve(rows);
                for (int r = 0; r < rows; ++r) {
                    bool ok0 = false, ok1 = false, ok2 = false, ok3 = false, ok4 = false, ok5 = false;
                    const double c0 = ll.scalar(0 * rows + r, &ok0);
                    const double c1 = ll.scalar(1 * rows + r, &ok1);
                    const double c2 = ll.scalar(2 * rows + r, &ok2);
                    const double c3 = ll.scalar(3 * rows + r, &ok3);
                    const double c4 = ll.scalar(4 * rows + r, &ok4);
                    const double c5 = ll.scalar(5 * rows + r, &ok5);
                    if (!ok0 || !std::isfinite(c0))
                        continue;
                    TrackLifeRow row;
                    row.frameId = int(std::lround(c0));
                    row.asscFlag = (ok1 && std::isfinite(c1)) ? int(std::lround(c1)) : 0;
                    row.lostCnt = (ok2 && std::isfinite(c2)) ? int(std::lround(c2)) : 0;
                    row.succLostCnt = (ok3 && std::isfinite(c3)) ? int(std::lround(c3)) : 0;
                    row.prevQuality = (ok4 && std::isfinite(c4)) ? c4 : 0.0;
                    row.curQuality = (ok5 && std::isfinite(c5)) ? c5 : 0.0;
                    meta.life.append(row);
                }
            }
        }

        // asscPointList -> asscPointIds & asscFrameIds（对齐 Track::rawPoints）
        QSet<int> asscFrameSet;
        if (asscPts.isValid()) {
            const int n = asscPts.numel();
            QVector<TimedRow> rows;
            rows.reserve(n);
            for (int j = 0; j < n; ++j) {
                bool okT = false, okLat = false, okLon = false;
                const double dn = asscPts.field(QStringLiteral("time"), j).scalar(0, &okT);
                const double lat = asscPts.field(QStringLiteral("lat"), j).scalar(0, &okLat);
                const double lon = asscPts.field(QStringLiteral("lon"), j).scalar(0, &okLon);
                if (!okT || !okLat || !okLon || dn <= 0.0 || !validLatLon(lat, lon))
                    continue;
                TimedRow trRow;
                trRow.t = datenumToMs(dn);
                bool okF = false, okP = false;
                const double fid = asscPts.field(QStringLiteral("frameID"), j).scalar(0, &okF);
                const double pid = asscPts.field(QStringLiteral("pointID"), j).scalar(0, &okP);
                trRow.frameId = (okF && std::isfinite(fid)) ? int(std::lround(fid)) : 0;
                trRow.val = (okP && std::isfinite(pid)) ? int(std::lround(pid)) : 0;
                rows.append(trRow);
            }
            sortAndDedupTimedRows(&rows);
            meta.asscFrameIds.reserve(rows.size());
            meta.asscPointIds.reserve(rows.size());
            for (const TimedRow &r : qAsConst(rows)) {
                meta.asscFrameIds.append(r.frameId);
                meta.asscPointIds.append(r.val);
                if (r.frameId > 0)
                    asscFrameSet.insert(r.frameId);
            }
        }

        // smoothPointList -> asscFlags & smoothFrameIds（按时间排序去重以对齐 Track::points）
        // 实测部分雷达 MAT 的 smoothPointList.asscFlag 在外推帧仍保持 1，
        // 若存在 asscPointList.frameID 集合，则以「帧号确实命中 asscPointList」为准还原真实实测/外推色带
        if (smoothPts.isValid()) {
            const int n = smoothPts.numel();
            QVector<TimedRow> rows;
            rows.reserve(n);
            const bool hasAsscFlag = smoothPts.hasField(QStringLiteral("asscFlag"));
            QHash<int, int> lifeAsscByFrame;
            for (const TrackLifeRow &lr : qAsConst(meta.life))
                lifeAsscByFrame.insert(lr.frameId, lr.asscFlag != 0 ? 1 : 0);

            for (int j = 0; j < n; ++j) {
                bool okT = false, okLat = false, okLon = false;
                const double dn = smoothPts.field(QStringLiteral("time"), j).scalar(0, &okT);
                const double lat = smoothPts.field(QStringLiteral("lat"), j).scalar(0, &okLat);
                const double lon = smoothPts.field(QStringLiteral("lon"), j).scalar(0, &okLon);
                if (!okT || !okLat || !okLon || dn <= 0.0 || !validLatLon(lat, lon))
                    continue;
                TimedRow trRow;
                trRow.t = datenumToMs(dn);
                bool okF = false;
                const double fid = smoothPts.field(QStringLiteral("frameID"), j).scalar(0, &okF);
                trRow.frameId = (okF && std::isfinite(fid)) ? int(std::lround(fid)) : 0;
                if (!asscFrameSet.isEmpty() && trRow.frameId > 0) {
                    trRow.val = asscFrameSet.contains(trRow.frameId) ? 1 : 0;
                } else if (trRow.frameId > 0 && lifeAsscByFrame.contains(trRow.frameId)) {
                    trRow.val = lifeAsscByFrame.value(trRow.frameId);
                } else if (hasAsscFlag) {
                    bool okA = false;
                    const double af = smoothPts.field(QStringLiteral("asscFlag"), j).scalar(0, &okA);
                    trRow.val = (okA && std::isfinite(af) && af != 0.0) ? 1 : 0;
                } else {
                    trRow.val = raw ? 1 : 0;
                }
                rows.append(trRow);
            }
            sortAndDedupTimedRows(&rows);
            meta.asscFlags.reserve(rows.size());
            meta.smoothFrameIds.reserve(rows.size());
            for (const TimedRow &r : qAsConst(rows)) {
                meta.asscFlags.append(r.val);
                meta.smoothFrameIds.append(r.frameId);
            }
        }

        // predictRes -> predFrameIds（对齐 Track::predPoints）
        if (predPts.isValid()) {
            const int n = predPts.numel();
            QVector<TimedRow> rows;
            rows.reserve(n);
            for (int j = 0; j < n; ++j) {
                bool okT = false, okLat = false, okLon = false;
                const double dn = predPts.field(QStringLiteral("time"), j).scalar(0, &okT);
                const double lat = predPts.field(QStringLiteral("lat"), j).scalar(0, &okLat);
                const double lon = predPts.field(QStringLiteral("lon"), j).scalar(0, &okLon);
                if (!okT || !okLat || !okLon || dn <= 0.0 || !validLatLon(lat, lon))
                    continue;
                TimedRow trRow;
                trRow.t = datenumToMs(dn);
                bool okF = false;
                const double fid = predPts.field(QStringLiteral("frameID"), j).scalar(0, &okF);
                trRow.frameId = (okF && std::isfinite(fid)) ? int(std::lround(fid)) : 0;
                trRow.val = 0;
                rows.append(trRow);
            }
            sortAndDedupTimedRows(&rows);
            meta.predFrameIds.reserve(rows.size());
            for (const TimedRow &r : qAsConst(rows))
                meta.predFrameIds.append(r.frameId);
        }

        meta.valid = true;
        out->metas.insert(meta.trackKey, meta);
    }

    // 解析 tempTrackList（可选，缺失不报错）
    const MatArray tempList = mat.variable(QStringLiteral("tempTrackList"));
    if (tempList.isValid() && tempList.isStruct() && tempList.numel() > 0) {
        const int nTemp = tempList.numel();
        out->tempPoints.reserve(nTemp);
        for (int j = 0; j < nTemp; ++j) {
            bool okT = false;
            const double dn = tempList.field(QStringLiteral("time"), j).scalar(0, &okT);
            if (!okT || !std::isfinite(dn) || dn <= 0.0)
                continue;
            TempTrackPoint tp;
            tp.timeMs = datenumToMs(dn);

            bool ok = false;
            double v = tempList.field(QStringLiteral("frameID"), j).scalar(0, &ok);
            if (ok && std::isfinite(v)) tp.frameId = int(std::lround(v));

            v = tempList.field(QStringLiteral("pointID"), j).scalar(0, &ok);
            if (ok && std::isfinite(v)) tp.pointId = int(std::lround(v));

            bool okLat = false, okLon = false;
            const double lat = tempList.field(QStringLiteral("lat"), j).scalar(0, &okLat);
            const double lon = tempList.field(QStringLiteral("lon"), j).scalar(0, &okLon);
            if (okLat && okLon && validLatLon(lat, lon)) {
                tp.lat = lat;
                tp.lon = lon;
            }

            v = tempList.field(QStringLiteral("prange"), j).scalar(0, &ok);
            if (ok && std::isfinite(v)) tp.prange = float(v);

            v = tempList.field(QStringLiteral("paz"), j).scalar(0, &ok);
            if (ok && std::isfinite(v)) tp.paz = float(v);

            v = tempList.field(QStringLiteral("pvr"), j).scalar(0, &ok);
            if (ok && std::isfinite(v)) tp.pvr = float(v);

            v = tempList.field(QStringLiteral("snr"), j).scalar(0, &ok);
            if (!ok || !std::isfinite(v))
                v = tempList.field(QStringLiteral("SNR"), j).scalar(0, &ok);
            if (ok && std::isfinite(v)) tp.snr = float(v);

            out->tempPoints.append(tp);
        }
    }

    if (out->metas.isEmpty())
        return fail(error, QStringLiteral("trackList 中未能提取到有效航迹诊断元数据"));
    return true;
}

// ---------------------------------------------------------------------------
//  1. 虚假航迹评估与门限试算沙盒
// ---------------------------------------------------------------------------

QVector<FalseTrackItem> evaluateFalseTracks(const TrackStore &store,
                                            const TrackDiagBundle &bundle,
                                            const FalseTrackConfig &cfg)
{
    const QVector<Track> &tracks = store.tracks();
    QVector<FalseTrackItem> items;
    items.reserve(tracks.size());

    // 供多径检测使用的局部索引映射（itemIdx -> trackIndex）
    for (int i = 0; i < tracks.size(); ++i) {
        const Track &tr = tracks.at(i);
        if (cfg.filterOnlyRadar && tr.source == TrackSource::Adsb)
            continue;

        const TrackDiagMeta *meta = bundle.findMeta(tr);
        FalseTrackItem it;
        it.trackIndex = i;
        it.trackKey = tr.key();
        it.displayId = tr.id;

        // 点数与关联率
        if (meta && meta->valid && meta->totalPointCnt > 0)
            it.totalCount = meta->totalPointCnt;
        else
            it.totalCount = tr.points.size();

        if (meta && meta->valid && meta->asscPointCnt > 0) {
            it.asscCount = meta->asscPointCnt;
        } else if (tr.hasRawPoints()) {
            it.asscCount = tr.rawPoints.size();
        } else if (meta && !meta->asscFlags.isEmpty()) {
            int c = 0;
            for (int f : meta->asscFlags)
                if (f != 0) ++c;
            it.asscCount = c;
        } else {
            it.asscCount = tr.points.size();
        }
        it.asscRatio = it.totalCount > 0
            ? qBound(0.0, double(it.asscCount) / double(it.totalCount), 1.0)
            : 0.0;

        // 连续丢点
        if (meta && meta->valid)
            it.succLoss = qMax(0, meta->succLossCnt);
        else
            it.succLoss = estimateSuccLoss(tr);

        // 平均 SNR
        it.meanSnr = computeMeanSnr(tr);

        // 航程（若 MAT 中 travelLen>0 优先采用，否则按地理点列累计）
        if (meta && meta->valid && meta->travelLenKm > 0.0)
            it.travelLenKm = meta->travelLenKm;
        else
            it.travelLenKm = geoPathLengthKm(tr.points);

        // 平均预测新息（无预测点时退化到平滑残差）
        const QVector<trackanalysis::FilterResidualPoint> res = trackanalysis::computeFilterResiduals(tr);
        if (!res.isEmpty()) {
            double sumPred = 0.0, sumSmooth = 0.0;
            int cntPred = 0, cntSmooth = 0;
            for (const trackanalysis::FilterResidualPoint &r : res) {
                if (r.hasPred && std::isfinite(r.predGeoErrKm)) {
                    sumPred += r.predGeoErrKm;
                    ++cntPred;
                }
                if (r.hasSmooth && std::isfinite(r.smoothGeoErrKm)) {
                    sumSmooth += r.smoothGeoErrKm;
                    ++cntSmooth;
                }
            }
            if (cntPred > 0)
                it.meanInnovKm = sumPred / double(cntPred);
            else if (cntSmooth > 0)
                it.meanInnovKm = sumSmooth / double(cntSmooth);
        }

        // 航迹质量与类型（无 meta 时优雅退化估算质量）
        if (meta && meta->valid) {
            it.quality = qBound(0.0, meta->quality, 1.0);
            it.trackType = meta->trackType;
        } else {
            double estQ = 0.45 * it.asscRatio
                        + 0.25 * qBound(0.0, double(it.asscCount) / 12.0, 1.0)
                        + 0.15 * (1.0 - qBound(0.0, double(it.succLoss) / 8.0, 1.0));
            if (std::isfinite(it.meanSnr))
                estQ += 0.15 * qBound(0.0, (it.meanSnr - 5.0) / 15.0, 1.0);
            else
                estQ += 0.08;
            it.quality = qBound(0.0, estQ, 1.0);
            it.trackType = 0;
        }

        items.append(it);
    }

    // 多径镜像检测：同方位、同多普勒径向速度但距离存在稳定偏移的配对航迹
    const int nItems = items.size();
    QVector<bool> isWeakMultipath(nItems, false);
    for (int a = 0; a < nItems; ++a) {
        const Track &ta = tracks.at(items.at(a).trackIndex);
        if (ta.points.size() < 3)
            continue;
        for (int b = a + 1; b < nItems; ++b) {
            const Track &tb = tracks.at(items.at(b).trackIndex);
            if (tb.points.size() < 3)
                continue;
            const qint64 lo = qMax(ta.minTime(), tb.minTime());
            const qint64 hi = qMin(ta.maxTime(), tb.maxTime());
            if (hi - lo < 20000)
                continue;

            int matched = 0;
            double sumDr = 0.0, sumDr2 = 0.0, sumDaz = 0.0, sumDvr = 0.0;
            for (const TrackPoint &pa : ta.points) {
                if (pa.t < lo || pa.t > hi)
                    continue;
                if (!std::isfinite(pa.prange) || !std::isfinite(pa.paz) || !std::isfinite(pa.pvr))
                    continue;
                TrackPoint pb;
                if (!sampleTrackAtTime(tb.points, pa.t, 15000, &pb))
                    continue;
                if (!std::isfinite(pb.prange) || !std::isfinite(pb.paz) || !std::isfinite(pb.pvr))
                    continue;
                const double dr = rangeToKm(pb.prange) - rangeToKm(pa.prange);
                const double daz = std::fabs(wrapDeg180(double(pb.paz) - double(pa.paz)));
                const double dvr = std::fabs(double(pb.pvr) - double(pa.pvr));
                sumDr += dr;
                sumDr2 += dr * dr;
                sumDaz += daz;
                sumDvr += dvr;
                ++matched;
            }
            if (matched < 3)
                continue;
            const double meanDr = sumDr / double(matched);
            const double absDr = std::fabs(meanDr);
            const double varDr = qMax(0.0, sumDr2 / double(matched) - meanDr * meanDr);
            const double stdDr = std::sqrt(varDr);
            const double meanDaz = sumDaz / double(matched);
            const double meanDvr = sumDvr / double(matched);

            if (meanDaz <= 3.5 && meanDvr <= 10.0 && absDr >= 20.0 && absDr <= 500.0 && stdDr <= 15.0) {
                if (items[a].multipathPeerIndex < 0)
                    items[a].multipathPeerIndex = items[b].trackIndex;
                if (items[b].multipathPeerIndex < 0)
                    items[b].multipathPeerIndex = items[a].trackIndex;
                // 质量/SNR 较低的一方标为疑似多径镜像
                const double scoreA = items[a].quality + (std::isfinite(items[a].meanSnr) ? items[a].meanSnr * 0.02 : 0.0);
                const double scoreB = items[b].quality + (std::isfinite(items[b].meanSnr) ? items[b].meanSnr * 0.02 : 0.0);
                if (scoreA <= scoreB)
                    isWeakMultipath[a] = true;
                else
                    isWeakMultipath[b] = true;
            }
        }
    }

    // 沙盒门限判定 + 综合疑点分计算
    for (int idx = 0; idx < nItems; ++idx) {
        FalseTrackItem &it = items[idx];
        bool filtered = false;

        if (it.quality < cfg.minQuality) {
            filtered = true;
            it.reasons << QStringLiteral("低质量");
        }
        if (it.asscRatio < cfg.minAsscRatio) {
            filtered = true;
            it.reasons << QStringLiteral("低关联率");
        }
        if (it.asscCount < cfg.minAsscPoints) {
            filtered = true;
            it.reasons << QStringLiteral("实测点不足");
        }
        if (it.succLoss > cfg.maxSuccLoss) {
            filtered = true;
            it.reasons << QStringLiteral("连续丢点");
        }
        if (std::isfinite(it.meanSnr) && it.meanSnr < cfg.minMeanSnrDb) {
            filtered = true;
            it.reasons << QStringLiteral("低SNR");
        }
        if (it.travelLenKm < cfg.minTravelKm) {
            filtered = true;
            it.reasons << QStringLiteral("短航程");
        }
        if (std::isfinite(it.meanInnovKm) && it.meanInnovKm > cfg.maxMeanInnovKm) {
            filtered = true;
            it.reasons << QStringLiteral("新息过大");
        }
        if (isWeakMultipath.at(idx))
            it.reasons << QStringLiteral("多径镜像");
        if (it.trackType == 7 && !it.reasons.contains(QStringLiteral("已消亡")))
            it.reasons << QStringLiteral("已消亡");

        it.filteredOut = filtered;

        double s = 0.0;
        s += (1.0 - it.quality) * 32.0;
        s += (1.0 - it.asscRatio) * 18.0;
        s += qBound(0.0, double(it.succLoss) / 10.0, 1.0) * 14.0;
        if (std::isfinite(it.meanSnr) && it.meanSnr < 16.0)
            s += qBound(0.0, (16.0 - it.meanSnr) / 16.0, 1.0) * 12.0;
        if (it.travelLenKm < 10.0)
            s += qBound(0.0, (10.0 - it.travelLenKm) / 10.0, 1.0) * 8.0;
        if (it.asscCount < 6)
            s += qBound(0.0, double(6 - it.asscCount) / 6.0, 1.0) * 8.0;
        if (std::isfinite(it.meanInnovKm))
            s += qBound(0.0, it.meanInnovKm / 50.0, 1.0) * 8.0;
        if (isWeakMultipath.at(idx))
            s += 10.0;
        if (it.trackType == 7)
            s += 6.0;
        else if (it.trackType == 6)
            s += 3.0;

        it.suspicionScore = qBound(0.0, s, 100.0);
    }

    std::stable_sort(items.begin(), items.end(),
                     [](const FalseTrackItem &a, const FalseTrackItem &b) {
                         if (std::fabs(a.suspicionScore - b.suspicionScore) > 1e-9)
                             return a.suspicionScore > b.suspicionScore;
                         return a.trackIndex < b.trackIndex;
                     });
    return items;
}

// ---------------------------------------------------------------------------
//  2a. 异常断航检测
// ---------------------------------------------------------------------------

QVector<BrokenTrackItem> findBrokenTracks(const TrackStore &store,
                                          const TrackDiagBundle &bundle,
                                          const RadarSite *site)
{
    const QVector<Track> &tracks = store.tracks();
    qint64 globalMaxT = 0;
    for (const Track &tr : tracks) {
        if (tr.source == TrackSource::Adsb || tr.points.isEmpty())
            continue;
        globalMaxT = qMax(globalMaxT, tr.maxTime());
    }

    double siteLat = 0.0, siteLon = 0.0;
    const bool hasSiteSector = site && site->isValid() && site->hasSector
                            && site->anchorLatLon(&siteLat, &siteLon);

    QVector<BrokenTrackItem> out;
    for (int i = 0; i < tracks.size(); ++i) {
        const Track &tr = tracks.at(i);
        if (tr.source == TrackSource::Adsb || tr.points.isEmpty())
            continue;

        const TrackDiagMeta *meta = bundle.findMeta(tr);
        const TrackPoint &lastPt = tr.points.last();
        const bool endedEarly = (globalMaxT > 0 && (globalMaxT - lastPt.t) >= 15000);
        const int succLoss = (meta && meta->valid) ? qMax(0, meta->succLossCnt) : estimateSuccLoss(tr);
        const double endQ = (meta && meta->valid) ? qBound(0.0, meta->quality, 1.0)
                                                  : qBound(0.0, 1.0 - double(succLoss) * 0.15, 1.0);
        const bool isTerminated = (meta && meta->valid && meta->trackType == 7);

        if (!endedEarly && !isTerminated && succLoss < 3)
            continue;

        bool inside = true;
        bool nearBoundary = false;
        if (hasSiteSector) {
            double rKm = rangeToKm(lastPt.prange);
            double az = double(lastPt.paz);
            if (!std::isfinite(rKm) || !std::isfinite(az)) {
                rKm = geocalc::vincentyKm(siteLat, siteLon, lastPt.lat, lastPt.lon);
                az = geocalc::initialBearing(siteLat, siteLon, lastPt.lat, lastPt.lon);
            }
            const bool rOk = (site->maxRangeKm <= site->minRangeKm)
                          || (rKm >= site->minRangeKm && rKm <= site->maxRangeKm);
            const bool aOk = azInSector(az, site->startAzDeg, site->endAzDeg);
            inside = rOk && aOk;
            if (inside) {
                const double rSpan = qMax(1.0, site->maxRangeKm - site->minRangeKm);
                const bool nearR = (site->maxRangeKm > site->minRangeKm)
                                && ((rKm - site->minRangeKm) < rSpan * 0.025
                                    || (site->maxRangeKm - rKm) < rSpan * 0.025);
                const double azSpan = std::fabs(site->endAzDeg - site->startAzDeg);
                const bool nearA = (azSpan < 359.5)
                                && (std::fabs(wrapDeg180(az - site->startAzDeg)) < 1.2
                                    || std::fabs(wrapDeg180(az - site->endAzDeg)) < 1.2);
                nearBoundary = nearR || nearA;
            }
        }

        const double tailSnr = computeTrailingSnr(tr, 3);
        QString cause;
        if (!inside || nearBoundary)
            cause = QStringLiteral("飞出探测扇区边界");
        else if (succLoss >= 5 && endQ < 0.2)
            cause = QStringLiteral("连续丢点消亡");
        else if (std::isfinite(tailSnr) && tailSnr < 10.0)
            cause = QStringLiteral("低信噪比失锁");
        else if (endQ < 0.3 && succLoss >= 2)
            cause = QStringLiteral("质量衰减断航");
        else if (endedEarly)
            cause = QStringLiteral("扇区内异常中断");
        else
            cause = QStringLiteral("持续外推预警");

        BrokenTrackItem item;
        item.trackIndex = i;
        item.trackKey = tr.key();
        item.displayId = tr.id;
        item.endTimeMs = lastPt.t;
        item.endLat = lastPt.lat;
        item.endLon = lastPt.lon;
        item.insideSector = inside;
        item.succLoss = succLoss;
        item.endQuality = endQ;
        item.likelyCause = cause;
        out.append(item);
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const BrokenTrackItem &a, const BrokenTrackItem &b) {
                         if (a.insideSector != b.insideSector)
                             return a.insideSector > b.insideSector;
                         if (a.succLoss != b.succLoss)
                             return a.succLoss > b.succLoss;
                         if (std::fabs(a.endQuality - b.endQuality) > 1e-6)
                             return a.endQuality < b.endQuality;
                         return a.endTimeMs > b.endTimeMs;
                     });
    return out;
}

// ---------------------------------------------------------------------------
//  2b. 断航波门尸检
// ---------------------------------------------------------------------------

GateAutopsyResult runGateAutopsy(int trackIndex,
                                 const TrackStore &store,
                                 const TrackDiagBundle &bundle,
                                 const Point3dDataset *detData)
{
    GateAutopsyResult result;
    const QVector<Track> &tracks = store.tracks();
    if (trackIndex < 0 || trackIndex >= tracks.size())
        return result;

    const Track &tr = tracks.at(trackIndex);
    result.trackIndex = trackIndex;
    result.trackKey = tr.key();
    if (tr.points.isEmpty()) {
        result.primaryCause = QStringLiteral("航迹无有效点列");
        return result;
    }

    const TrackDiagMeta *meta = bundle.findMeta(tr);
    const int myBatchNo = (meta && meta->valid && meta->batchNo > 0)
        ? meta->batchNo
        : point3dBatchNo(tr);

    // 1. 构建全量关联点归属索引：
    //    (a) 精确索引：(frameId, pointId) -> batchNo（来自 bundle.metas）
    //    (b) 坐标回退索引：供无 bundle 或跨源时按时间+极坐标匹配
    QHash<quint64, int> exactOwner;
    for (auto it = bundle.metas.constBegin(); it != bundle.metas.constEnd(); ++it) {
        const TrackDiagMeta &m = it.value();
        if (!m.valid || m.batchNo <= 0)
            continue;
        const int n = qMin(m.asscFrameIds.size(), m.asscPointIds.size());
        for (int k = 0; k < n; ++k) {
            if (m.asscFrameIds.at(k) > 0 && m.asscPointIds.at(k) > 0)
                exactOwner.insert(packFramePointKey(m.asscFrameIds.at(k), m.asscPointIds.at(k)), m.batchNo);
        }
    }

    struct RawOwnership {
        int batchNo;
        float range;
        float az;
        float vr;
    };
    QHash<qint64, QVector<RawOwnership> > timeOwners;
    for (const Track &other : tracks) {
        if (other.source == TrackSource::Adsb)
            continue;
        const int bno = point3dBatchNo(other);
        if (bno <= 0)
            continue;
        const QVector<TrackPoint> &col = other.hasRawPoints() ? other.rawPoints : other.points;
        for (const TrackPoint &p : col) {
            if (!std::isfinite(p.prange) || !std::isfinite(p.paz) || !std::isfinite(p.pvr))
                continue;
            RawOwnership ro;
            ro.batchNo = bno;
            ro.range = p.prange;
            ro.az = p.paz;
            ro.vr = p.pvr;
            timeOwners[p.t].append(ro);
        }
    }

    auto findOwnerBatch = [&](int frameId, int pointId, qint64 timeMs,
                              float r, float az, float vr) -> int {
        if (frameId > 0 && pointId > 0) {
            const int b = exactOwner.value(packFramePointKey(frameId, pointId), 0);
            if (b > 0)
                return b;
        }
        // 在 ±2s 窗口内按测量坐标近邻查归属
        for (auto it = timeOwners.constBegin(); it != timeOwners.constEnd(); ++it) {
            if (std::llabs(it.key() - timeMs) > 2000)
                continue;
            for (const RawOwnership &ro : it.value()) {
                if (std::fabs(double(ro.range) - double(r)) <= 0.5
                        && std::fabs(wrapDeg180(double(ro.az) - double(az))) <= 0.15
                        && std::fabs(double(ro.vr) - double(vr)) <= 0.5) {
                    return ro.batchNo;
                }
            }
        }
        return 0;
    };

    // 2. 选取关键尸检帧：航迹尾部最多 6 帧（覆盖末尾失锁段） + 断航后最多 3 帧外推
    struct ProbeSpec {
        qint64 timeMs;
        int frameIndex;
        int frameId;
        double predLat;
        double predLon;
        float predR;
        float predAz;
        float predVr;
        int lostStreak;
        bool postEnd;
    };
    QVector<ProbeSpec> probes;

    const bool hasDet = (detData != nullptr && !detData->frames.isEmpty());
    qint64 frameStepMs = 10000;
    if (hasDet && detData->frames.size() >= 2) {
        qint64 sumGap = 0;
        int gapCnt = 0;
        for (int k = 1; k < detData->frames.size(); ++k) {
            const qint64 g = detData->frames.at(k).timeMs - detData->frames.at(k - 1).timeMs;
            if (g > 0 && g < 120000) {
                sumGap += g;
                ++gapCnt;
            }
        }
        if (gapCnt > 0)
            frameStepMs = sumGap / gapCnt;
    } else if (tr.points.size() >= 2) {
        const qint64 g = tr.points.last().t - tr.points.at(tr.points.size() - 2).t;
        if (g > 0 && g < 120000)
            frameStepMs = g;
    }
    const qint64 matchTolMs = qMax<qint64>(frameStepMs / 2 + 1000, 5000);

    const int tailStart = qMax(0, tr.points.size() - 6);
    QHash<int, int> lifeSuccLostByFrame;
    if (meta && meta->valid) {
        for (const TrackLifeRow &lr : qAsConst(meta->life))
            lifeSuccLostByFrame.insert(lr.frameId, lr.succLostCnt);
    }
    int runningLost = 0;
    // 先把 tailStart 之前的连丢计数接上
    if (meta && meta->valid && meta->asscFlags.size() == tr.points.size()) {
        for (int k = tailStart - 1; k >= 0; --k) {
            if (meta->asscFlags.at(k) == 0) ++runningLost;
            else break;
        }
    }

    for (int k = tailStart; k < tr.points.size(); ++k) {
        const TrackPoint &sp = tr.points.at(k);
        const int pi = findNearestTrackPoint(tr.predPoints, sp.t, 2000);
        const TrackPoint &ref = (pi >= 0) ? tr.predPoints.at(pi) : sp;

        bool associated = false;
        if (meta && meta->valid && k < meta->asscFlags.size())
            associated = (meta->asscFlags.at(k) != 0);
        else
            associated = (findNearestTrackPoint(tr.rawPoints, sp.t, 2000) >= 0);

        if (associated)
            runningLost = 0;
        else
            ++runningLost;

        ProbeSpec ps;
        ps.timeMs = sp.t;
        ps.frameIndex = hasDet ? findNearestDetFrame(detData->frames, sp.t, matchTolMs) : -1;
        if (meta && meta->valid && k < meta->smoothFrameIds.size() && meta->smoothFrameIds.at(k) > 0)
            ps.frameId = meta->smoothFrameIds.at(k);
        else if (ps.frameIndex >= 0)
            ps.frameId = int(std::lround(detData->frames.at(ps.frameIndex).zValue));
        else
            ps.frameId = k + 1;
        if (!associated && lifeSuccLostByFrame.contains(ps.frameId))
            runningLost = qMax(runningLost, lifeSuccLostByFrame.value(ps.frameId));
        ps.predLat = ref.lat;
        ps.predLon = ref.lon;
        ps.predR = std::isfinite(ref.prange) ? ref.prange : sp.prange;
        ps.predAz = std::isfinite(ref.paz) ? ref.paz : sp.paz;
        ps.predVr = std::isfinite(ref.pvr) ? ref.pvr : sp.pvr;
        ps.lostStreak = associated ? 0 : runningLost;
        ps.postEnd = false;
        probes.append(ps);
    }

    // 断航后 1..3 帧外推（仅当 detData 在航迹结束后还有后续帧）
    if (hasDet) {
        const TrackPoint &lastPt = tr.points.last();
        const TailKinematics kin = estimateTailKinematics(tr.points);
        int lastDetIdx = findNearestDetFrame(detData->frames, lastPt.t, matchTolMs);
        if (lastDetIdx < 0) {
            for (int f = 0; f < detData->frames.size(); ++f) {
                if (detData->frames.at(f).timeMs <= lastPt.t)
                    lastDetIdx = f;
            }
        }
        int addedPost = 0;
        for (int f = lastDetIdx + 1; f < detData->frames.size() && addedPost < 3; ++f) {
            const DetPointFrame &df = detData->frames.at(f);
            if (df.timeMs <= lastPt.t)
                continue;
            const double dtSec = double(df.timeMs - lastPt.t) / 1000.0;
            if (dtSec > 60.0)
                break;
            ++addedPost;
            ++runningLost;

            ProbeSpec ps;
            ps.timeMs = df.timeMs;
            ps.frameIndex = f;
            ps.frameId = std::isfinite(df.frameId) ? int(std::lround(df.frameId)) : df.ordinal;
            ps.predLat = lastPt.lat;
            ps.predLon = lastPt.lon;
            if (kin.validGeo && kin.speedKmS > 0.0)
                geocalc::destination(lastPt.lat, lastPt.lon, kin.bearingDeg,
                                     kin.speedKmS * dtSec, &ps.predLat, &ps.predLon);
            if (std::isfinite(lastPt.prange)) {
                const double drRate = kin.validPolar
                    ? kin.drPerSec
                    : (std::isfinite(lastPt.pvr) ? (double(lastPt.pvr) / 1000.0) : 0.0);
                ps.predR = float(double(lastPt.prange) + drRate * dtSec);
            } else {
                ps.predR = kNaNf;
            }
            if (std::isfinite(lastPt.paz))
                ps.predAz = float(wrapDeg360(double(lastPt.paz) + (kin.validPolar ? kin.dazPerSec * dtSec : 0.0)));
            else
                ps.predAz = kNaNf;
            if (std::isfinite(lastPt.pvr))
                ps.predVr = float(double(lastPt.pvr) + (kin.validPolar ? kin.dvrPerSec * dtSec : 0.0));
            else
                ps.predVr = kNaNf;
            ps.lostStreak = runningLost;
            ps.postEnd = true;
            probes.append(ps);
        }
    }

    // 3. 逐帧在 2x 波门内扫描候选点迹并给出死因判定
    int noDetCount = 0;
    int stolenCount = 0;
    int outsideGateCount = 0;
    int unassignedInGateCount = 0;
    int lostProbeCount = 0;

    result.frames.reserve(probes.size());
    for (const ProbeSpec &ps : qAsConst(probes)) {
        GateAutopsyFrame af;
        af.frameIndex = ps.frameIndex;
        af.frameId = ps.frameId;
        af.timeMs = ps.timeMs;
        af.predLat = ps.predLat;
        af.predLon = ps.predLon;
        af.predR = ps.predR;
        af.predAz = ps.predAz;
        af.predVr = ps.predVr;
        effectiveGate(meta, ps.lostStreak, &af.gateR, &af.gateAz, &af.gateVr);

        const bool predPolarOk = std::isfinite(af.predR) && std::isfinite(af.predAz) && std::isfinite(af.predVr);
        const double gR = qMax(0.5, double(af.gateR));
        const double gA = qMax(0.1, double(af.gateAz));
        const double gV = qMax(0.5, double(af.gateVr));

        bool hasSelfAssc = false;
        bool hasStolen = false;
        bool hasUnassignedIn = false;
        bool hasOutside2x = false;

        if (hasDet && ps.frameIndex >= 0 && ps.frameIndex < detData->frames.size() && predPolarOk) {
            const DetPointFrame &df = detData->frames.at(ps.frameIndex);
            const int detFid = std::isfinite(df.frameId) ? int(std::lround(df.frameId)) : af.frameId;
            for (const DetPointItem &dp : df.points) {
                const double dR = double(dp.range) - double(af.predR);
                const double dAz = wrapDeg180(double(dp.az) - double(af.predAz));
                const double dVr = double(dp.vr) - double(af.predVr);
                if (std::fabs(dR) > 2.0 * gR || std::fabs(dAz) > 2.0 * gA || std::fabs(dVr) > 2.0 * gV)
                    continue;

                GateCandidate c;
                c.frameIndex = ps.frameIndex;
                c.frameId = detFid;
                c.timeMs = df.timeMs;
                c.pointId = dp.pointID;
                c.range = dp.range;
                c.az = dp.az;
                c.vr = dp.vr;
                c.snr = dp.snr;
                c.dR = dR;
                c.dAz = dAz;
                c.dVr = dVr;
                c.insideGate = (std::fabs(dR) <= gR && std::fabs(dAz) <= gA && std::fabs(dVr) <= gV);
                c.ownerBatchNo = findOwnerBatch(detFid, dp.pointID, df.timeMs, dp.range, dp.az, dp.vr);

                if (c.insideGate && myBatchNo > 0 && c.ownerBatchNo == myBatchNo) {
                    c.verdict = QStringLiteral("[门内-本批已关联]");
                    hasSelfAssc = true;
                } else if (c.insideGate && c.ownerBatchNo > 0 && c.ownerBatchNo != myBatchNo) {
                    c.verdict = QStringLiteral("[门内-被批号%1抢占]").arg(c.ownerBatchNo);
                    hasStolen = true;
                } else if (c.insideGate) {
                    c.verdict = QStringLiteral("[门内未关联]");
                    hasUnassignedIn = true;
                } else {
                    c.verdict = QStringLiteral("[门外超限]");
                    hasOutside2x = true;
                }
                af.candidates.append(c);
            }
        } else if (!hasDet && predPolarOk && !bundle.tempPoints.isEmpty()) {
            // 未加载 DetPoint 时，回退扫描 tempTrackList 临时池点
            for (const TempTrackPoint &tp : bundle.tempPoints) {
                if (std::llabs(tp.timeMs - ps.timeMs) > matchTolMs)
                    continue;
                if (!std::isfinite(tp.prange) || !std::isfinite(tp.paz) || !std::isfinite(tp.pvr))
                    continue;
                const double dR = double(tp.prange) - double(af.predR);
                const double dAz = wrapDeg180(double(tp.paz) - double(af.predAz));
                const double dVr = double(tp.pvr) - double(af.predVr);
                if (std::fabs(dR) > 2.0 * gR || std::fabs(dAz) > 2.0 * gA || std::fabs(dVr) > 2.0 * gV)
                    continue;

                GateCandidate c;
                c.frameIndex = -1;
                c.frameId = tp.frameId;
                c.timeMs = tp.timeMs;
                c.pointId = tp.pointId;
                c.range = tp.prange;
                c.az = tp.paz;
                c.vr = tp.pvr;
                c.snr = std::isfinite(tp.snr) ? tp.snr : 0.0f;
                c.dR = dR;
                c.dAz = dAz;
                c.dVr = dVr;
                c.insideGate = (std::fabs(dR) <= gR && std::fabs(dAz) <= gA && std::fabs(dVr) <= gV);
                c.ownerBatchNo = 0;
                if (c.insideGate) {
                    c.verdict = QStringLiteral("[门内未关联]");
                    hasUnassignedIn = true;
                } else {
                    c.verdict = QStringLiteral("[门外超限]");
                    hasOutside2x = true;
                }
                af.candidates.append(c);
            }
        }

        // 候选点按归一化波门距离升序排序，保留最近 12 个
        std::stable_sort(af.candidates.begin(), af.candidates.end(),
                         [gR, gA, gV](const GateCandidate &a, const GateCandidate &b) {
                             const double da = (a.dR / gR) * (a.dR / gR)
                                             + (a.dAz / gA) * (a.dAz / gA)
                                             + (a.dVr / gV) * (a.dVr / gV);
                             const double db = (b.dR / gR) * (b.dR / gR)
                                             + (b.dAz / gA) * (b.dAz / gA)
                                             + (b.dVr / gV) * (b.dVr / gV);
                             return da < db;
                         });
        if (af.candidates.size() > 12)
            af.candidates.resize(12);

        af.hasDetectionIn2xGate = !af.candidates.isEmpty();

        const QString tag = ps.postEnd ? QStringLiteral("[断后外推]") : QStringLiteral("[航迹尾段]");
        if (!hasDet && af.candidates.isEmpty()) {
            af.summary = tag + QStringLiteral(" 未加载 DetPoint，无法核查原始点迹");
        } else if (hasSelfAssc) {
            af.summary = tag + QStringLiteral(" 本批成功关联（2x门内候选 %1 个）").arg(af.candidates.size());
        } else if (hasStolen) {
            af.summary = tag + QStringLiteral(" 门内点迹被邻批抢占");
        } else if (hasUnassignedIn) {
            af.summary = tag + QStringLiteral(" 1x门内有未关联点迹（可能门限/质量判定拒绝）");
        } else if (hasOutside2x) {
            af.summary = tag + QStringLiteral(" 仅 1x~2x 门外存在点迹（波门偏小或机动超限）");
        } else {
            af.summary = tag + QStringLiteral(" 2x波门内无检测点（雷达漏检）");
        }

        if (ps.lostStreak > 0 || ps.postEnd) {
            ++lostProbeCount;
            if (hasStolen)
                ++stolenCount;
            else if (hasUnassignedIn)
                ++unassignedInGateCount;
            else if (hasOutside2x)
                ++outsideGateCount;
            else if (!hasSelfAssc)
                ++noDetCount;
        }

        result.frames.append(af);
    }

    if (!hasDet) {
        if (stolenCount > 0 || unassignedInGateCount > 0 || outsideGateCount > 0)
            result.primaryCause = QStringLiteral("未加载 DetPoint（临时池发现 %1 帧邻近点迹）")
                                      .arg(unassignedInGateCount + outsideGateCount);
        else
            result.primaryCause = QStringLiteral("未加载原始检测点迹（请先加载 DetPoint 目录执行完整波门尸检）");
    } else if (lostProbeCount == 0) {
        result.primaryCause = QStringLiteral("尾段持续关联正常，无明显断航失锁帧");
    } else if (stolenCount >= outsideGateCount && stolenCount >= unassignedInGateCount && stolenCount > 0) {
        result.primaryCause = QStringLiteral("关联竞争：门内检测点被邻批抢占（%1/%2 失锁帧）")
                                  .arg(stolenCount).arg(lostProbeCount);
    } else if (outsideGateCount >= unassignedInGateCount && outsideGateCount > 0 && outsideGateCount >= noDetCount) {
        result.primaryCause = QStringLiteral("波门偏小/目标机动超限（%1/%2 失锁帧落于 1x~2x 门外）")
                                  .arg(outsideGateCount).arg(lostProbeCount);
    } else if (unassignedInGateCount > 0 && unassignedInGateCount >= noDetCount) {
        result.primaryCause = QStringLiteral("门内存在未关联检测点（%1/%2 失锁帧，疑似关联门限/质量逻辑拒绝）")
                                  .arg(unassignedInGateCount).arg(lostProbeCount);
    } else {
        result.primaryCause = QStringLiteral("前端雷达漏检（%1/%2 失锁帧在 2x 波门内均无检测点）")
                                  .arg(noDetCount).arg(lostProbeCount);
    }

    return result;
}

// ---------------------------------------------------------------------------
//  2c. 断批重起配对
// ---------------------------------------------------------------------------

QVector<TrackFragmentPair> findFragmentPairs(const TrackStore &store,
                                             qint64 maxGapMs,
                                             double maxDistKm)
{
    const QVector<Track> &tracks = store.tracks();
    QVector<TrackFragmentPair> pairs;
    if (tracks.size() < 2 || maxGapMs <= 0 || maxDistKm <= 0.0)
        return pairs;

    // 预计算每条雷达航迹的尾段运动学，避免 O(N^2) 重复计算
    QVector<int> radarIndices;
    QVector<TailKinematics> tailKins;
    radarIndices.reserve(tracks.size());
    tailKins.reserve(tracks.size());
    for (int i = 0; i < tracks.size(); ++i) {
        const Track &tr = tracks.at(i);
        if (tr.source == TrackSource::Adsb || tr.points.isEmpty())
            continue;
        radarIndices.append(i);
        tailKins.append(estimateTailKinematics(tr.points));
    }

    const int n = radarIndices.size();
    for (int ia = 0; ia < n; ++ia) {
        const int idxA = radarIndices.at(ia);
        const Track &ta = tracks.at(idxA);
        const TrackPoint &endA = ta.points.last();
        const TailKinematics &kinA = tailKins.at(ia);

        for (int ib = 0; ib < n; ++ib) {
            if (ia == ib)
                continue;
            const int idxB = radarIndices.at(ib);
            const Track &tb = tracks.at(idxB);
            const TrackPoint &startB = tb.points.first();

            const qint64 gapMs = startB.t - endA.t;
            if (gapMs < 0 || gapMs > maxGapMs)
                continue;

            const double dtSec = double(gapMs) / 1000.0;
            double exLat = endA.lat;
            double exLon = endA.lon;
            if (kinA.validGeo && kinA.speedKmS > 0.0 && dtSec > 0.0)
                geocalc::destination(endA.lat, endA.lon, kinA.bearingDeg,
                                     kinA.speedKmS * dtSec, &exLat, &exLon);

            const double dExtrap = geocalc::vincentyKm(exLat, exLon, startB.lat, startB.lon);
            const double dRaw = geocalc::vincentyKm(endA.lat, endA.lon, startB.lat, startB.lon);
            const double distKm = kinA.validGeo ? qMin(dExtrap, dRaw * 1.15) : dRaw;
            if (!std::isfinite(distKm) || distKm > maxDistKm)
                continue;

            double dVr = kNaN;
            if (std::isfinite(endA.pvr) && std::isfinite(startB.pvr)) {
                dVr = double(startB.pvr) - double(endA.pvr);
                if (std::fabs(dVr) > 45.0)
                    continue;
            }
            double dAz = kNaN;
            if (std::isfinite(endA.paz) && std::isfinite(startB.paz)) {
                dAz = wrapDeg180(double(startB.paz) - double(endA.paz));
                if (std::fabs(dAz) > 10.0)
                    continue;
            }

            const double sigmaDist = qMax(5.0, maxDistKm * 0.35);
            const double sDist = std::exp(-0.5 * (distKm / sigmaDist) * (distKm / sigmaDist));
            const double sVr = std::isfinite(dVr) ? std::exp(-0.5 * (dVr / 10.0) * (dVr / 10.0)) : 0.55;
            const double sAz = std::isfinite(dAz) ? std::exp(-0.5 * (dAz / 2.5) * (dAz / 2.5)) : 0.55;
            const double sGap = 1.0 - 0.5 * (double(gapMs) / double(maxGapMs));
            const double score = qBound(0.0, 45.0 * sDist + 25.0 * sVr + 15.0 * sAz + 15.0 * sGap, 100.0);

            TrackFragmentPair fp;
            fp.firstIndex = idxA;
            fp.secondIndex = idxB;
            fp.firstId = ta.id;
            fp.secondId = tb.id;
            fp.gapMs = gapMs;
            fp.extrapDistKm = distKm;
            fp.dVr = dVr;
            fp.dAz = dAz;
            fp.score = score;
            pairs.append(fp);
        }
    }

    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const TrackFragmentPair &a, const TrackFragmentPair &b) {
                         if (std::fabs(a.score - b.score) > 1e-9)
                             return a.score > b.score;
                         return a.gapMs < b.gapMs;
                     });
    return pairs;
}

// ---------------------------------------------------------------------------
//  2d. 漏批挖掘：未关联强连续检测点迹链
// ---------------------------------------------------------------------------

QVector<OrphanDetCluster> findOrphanDetClusters(const Point3dDataset &detData,
                                                const TrackStore &store,
                                                const TrackDiagBundle &bundle,
                                                double minContinuity,
                                                int minPoints)
{
    QVector<OrphanDetCluster> clusters;
    if (detData.frames.isEmpty() || minPoints < 2)
        return clusters;

    // 若外部未计算 continuity，则在本地副本上自动补算
    bool hasContinuity = false;
    for (const DetPointFrame &f : detData.frames) {
        if (f.continuity.size() == f.points.size()) {
            for (float c : f.continuity) {
                if (c > 0.0f) {
                    hasContinuity = true;
                    break;
                }
            }
        }
        if (hasContinuity)
            break;
    }
    QVector<DetPointFrame> localFrames;
    const QVector<DetPointFrame> *framesPtr = &detData.frames;
    if (!hasContinuity) {
        localFrames = detData.frames;
        computeContinuity(localFrames, ContinuityConfig());
        framesPtr = &localFrames;
    }
    const QVector<DetPointFrame> &frames = *framesPtr;
    const int nFrames = frames.size();

    // 1. 标记所有已被确认航迹关联的检测点
    QSet<quint64> exactAssc;
    for (auto it = bundle.metas.constBegin(); it != bundle.metas.constEnd(); ++it) {
        const TrackDiagMeta &m = it.value();
        if (!m.valid)
            continue;
        const int n = qMin(m.asscFrameIds.size(), m.asscPointIds.size());
        for (int k = 0; k < n; ++k) {
            if (m.asscFrameIds.at(k) > 0 && m.asscPointIds.at(k) > 0)
                exactAssc.insert(packFramePointKey(m.asscFrameIds.at(k), m.asscPointIds.at(k)));
        }
    }

    QVector<QVector<bool> > usedByTrack(nFrames);
    for (int fi = 0; fi < nFrames; ++fi)
        usedByTrack[fi].fill(false, frames.at(fi).points.size());

    // 先用 exactAssc (frameId, pointId) 精确标记
    for (int fi = 0; fi < nFrames; ++fi) {
        const DetPointFrame &df = frames.at(fi);
        if (!std::isfinite(df.frameId))
            continue;
        const int fid = int(std::lround(df.frameId));
        for (int pi = 0; pi < df.points.size(); ++pi) {
            if (exactAssc.contains(packFramePointKey(fid, df.points.at(pi).pointID)))
                usedByTrack[fi][pi] = true;
        }
    }

    // 再用 store 中所有雷达航迹的关联/平滑点按时间+极坐标邻域标记
    for (const Track &tr : store.tracks()) {
        if (tr.source == TrackSource::Adsb)
            continue;
        const QVector<TrackPoint> &col = tr.hasRawPoints() ? tr.rawPoints : tr.points;
        for (const TrackPoint &tp : col) {
            if (!std::isfinite(tp.prange) || !std::isfinite(tp.paz) || !std::isfinite(tp.pvr))
                continue;
            const int fi = findNearestDetFrame(frames, tp.t, 5000);
            if (fi < 0)
                continue;
            const DetPointFrame &df = frames.at(fi);
            for (int pi = 0; pi < df.points.size(); ++pi) {
                if (usedByTrack[fi][pi])
                    continue;
                const DetPointItem &dp = df.points.at(pi);
                if (std::fabs(double(dp.range) - double(tp.prange)) <= 6.0
                        && std::fabs(wrapDeg180(double(dp.az) - double(tp.paz))) <= 0.8
                        && std::fabs(double(dp.vr) - double(tp.pvr)) <= 3.5) {
                    usedByTrack[fi][pi] = true;
                }
            }
        }
    }

    // 2. 筛选未关联且 continuity >= minContinuity 的孤立候选点
    struct CandNode {
        int fi;
        int pi;
        float range;
        float az;
        float vr;
        float snr;
        float cont;
        int nextNode;
        int prevNode;
    };
    QVector<CandNode> nodes;
    QVector<QVector<int> > frameNodes(nFrames);
    for (int fi = 0; fi < nFrames; ++fi) {
        const DetPointFrame &df = frames.at(fi);
        const int nPts = df.points.size();
        for (int pi = 0; pi < nPts; ++pi) {
            if (usedByTrack[fi][pi])
                continue;
            const float cont = (pi < df.continuity.size()) ? df.continuity.at(pi) : 0.0f;
            if (double(cont) < minContinuity)
                continue;
            CandNode cn;
            cn.fi = fi;
            cn.pi = pi;
            cn.range = df.points.at(pi).range;
            cn.az = df.points.at(pi).az;
            cn.vr = df.points.at(pi).vr;
            cn.snr = df.points.at(pi).snr;
            cn.cont = cont;
            cn.nextNode = -1;
            cn.prevNode = -1;
            const int nid = nodes.size();
            nodes.append(cn);
            frameNodes[fi].append(nid);
        }
    }

    // 3. 逐帧贪心 1-to-1 时序链式连接（跨越 1..3 帧），防止杂波合并成巨团
    for (int fi = 0; fi < nFrames - 1; ++fi) {
        for (int nid : qAsConst(frameNodes[fi])) {
            const CandNode &src = nodes.at(nid);
            int bestNext = -1;
            double bestCost = std::numeric_limits<double>::infinity();
            for (int df = 1; df <= 3 && (fi + df) < nFrames; ++df) {
                const double rGate = qMax(15.0, 12.0 * double(df));
                const double aGate = 1.5 * double(df);
                const double vGate = 8.0 * double(df);
                for (int candId : qAsConst(frameNodes[fi + df])) {
                    const CandNode &dst = nodes.at(candId);
                    if (dst.prevNode >= 0)
                        continue;
                    const double dr = std::fabs(double(dst.range) - double(src.range));
                    const double da = std::fabs(wrapDeg180(double(dst.az) - double(src.az)));
                    const double dv = std::fabs(double(dst.vr) - double(src.vr));
                    if (dr > rGate || da > aGate || dv > vGate)
                        continue;
                    const double cost = (dr / rGate) * (dr / rGate)
                                      + (da / aGate) * (da / aGate)
                                      + (dv / vGate) * (dv / vGate)
                                      + 0.15 * double(df - 1);
                    if (cost < bestCost) {
                        bestCost = cost;
                        bestNext = candId;
                    }
                }
                if (bestNext >= 0)
                    break;  // 优先连接最近邻帧
            }
            if (bestNext >= 0) {
                nodes[nid].nextNode = bestNext;
                nodes[bestNext].prevNode = nid;
            }
        }
    }

    // 4. 提取链条并过滤长度 >= minPoints
    for (int nid = 0; nid < nodes.size(); ++nid) {
        if (nodes.at(nid).prevNode >= 0)
            continue;
        QVector<int> chain;
        int cur = nid;
        while (cur >= 0) {
            chain.append(cur);
            cur = nodes.at(cur).nextNode;
        }
        if (chain.size() < minPoints)
            continue;

        OrphanDetCluster cl;
        cl.pointCount = chain.size();
        cl.framePointIndices.reserve(chain.size());
        const DetPointFrame &fStart = frames.at(nodes.at(chain.first()).fi);
        const DetPointFrame &fEnd = frames.at(nodes.at(chain.last()).fi);
        cl.frameStart = std::isfinite(fStart.frameId) ? int(std::lround(fStart.frameId)) : fStart.ordinal;
        cl.frameEnd = std::isfinite(fEnd.frameId) ? int(std::lround(fEnd.frameId)) : fEnd.ordinal;
        cl.t0 = fStart.timeMs;
        cl.t1 = fEnd.timeMs;

        double sumR = 0.0, sumSin = 0.0, sumCos = 0.0, sumVr = 0.0, sumSnr = 0.0, sumCont = 0.0;
        for (int cid : qAsConst(chain)) {
            const CandNode &cn = nodes.at(cid);
            cl.framePointIndices.append(qMakePair(cn.fi, cn.pi));
            sumR += double(cn.range);
            sumSin += std::sin(double(cn.az) * kDegToRad);
            sumCos += std::cos(double(cn.az) * kDegToRad);
            sumVr += double(cn.vr);
            sumSnr += double(cn.snr);
            sumCont += double(cn.cont);
        }
        const double invN = 1.0 / double(chain.size());
        cl.meanRange = float(sumR * invN);
        cl.meanAz = float(wrapDeg360(std::atan2(sumSin, sumCos) / kDegToRad));
        cl.meanVr = float(sumVr * invN);
        cl.meanSnr = float(sumSnr * invN);
        cl.meanContinuity = float(sumCont * invN);
        clusters.append(cl);
    }

    std::stable_sort(clusters.begin(), clusters.end(),
                     [](const OrphanDetCluster &a, const OrphanDetCluster &b) {
                         const double sa = double(a.pointCount) * double(a.meanContinuity);
                         const double sb = double(b.pointCount) * double(b.meanContinuity);
                         if (std::fabs(sa - sb) > 1e-6)
                             return sa > sb;
                         return a.t0 < b.t0;
                     });
    for (int i = 0; i < clusters.size(); ++i)
        clusters[i].clusterId = i + 1;

    return clusters;
}

// ---------------------------------------------------------------------------
//  2e. 四级漏斗逐帧追踪（L1 DetPoint -> L2 tempTrackList -> L3 assc -> L4 smooth）
// ---------------------------------------------------------------------------

QVector<FunnelFrameStat> traceTrackFunnel(const Track &refTrack,
                                          const TrackStore &store,
                                          const TrackDiagBundle &bundle,
                                          const Point3dDataset *detData,
                                          double searchRadiusKm)
{
    QVector<FunnelFrameStat> stats;
    if (refTrack.points.isEmpty() || searchRadiusKm <= 0.0)
        return stats;

    const qint64 tMin = refTrack.minTime();
    const qint64 tMax = refTrack.maxTime();
    const bool hasDet = (detData != nullptr && !detData->frames.isEmpty());

    struct TimeSlot {
        int frameIndex;
        int frameId;
        qint64 timeMs;
    };
    QVector<TimeSlot> slotsList;

    if (hasDet) {
        for (int fi = 0; fi < detData->frames.size(); ++fi) {
            const DetPointFrame &df = detData->frames.at(fi);
            if (df.timeMs < tMin - 15000 || df.timeMs > tMax + 15000)
                continue;
            TimeSlot ts;
            ts.frameIndex = fi;
            ts.frameId = std::isfinite(df.frameId) ? int(std::lround(df.frameId)) : df.ordinal;
            ts.timeMs = df.timeMs;
            slotsList.append(ts);
        }
    }
    if (slotsList.isEmpty()) {
        const TrackDiagMeta *refMeta = bundle.findMeta(refTrack);
        slotsList.reserve(refTrack.points.size());
        for (int i = 0; i < refTrack.points.size(); ++i) {
            TimeSlot ts;
            ts.frameIndex = -1;
            ts.frameId = (refMeta && i < refMeta->smoothFrameIds.size() && refMeta->smoothFrameIds.at(i) > 0)
                ? refMeta->smoothFrameIds.at(i)
                : (i + 1);
            ts.timeMs = refTrack.points.at(i).t;
            slotsList.append(ts);
        }
    }

    const QVector<Track> &allTracks = store.tracks();
    stats.reserve(slotsList.size());

    for (const TimeSlot &ts : qAsConst(slotsList)) {
        TrackPoint refPt;
        if (!sampleTrackAtTime(refTrack.points, ts.timeMs, 30000, &refPt))
            continue;

        FunnelFrameStat st;
        st.frameIndex = ts.frameIndex;
        st.frameId = ts.frameId;
        st.timeMs = ts.timeMs;

        const bool refPolarOk = std::isfinite(refPt.prange) && std::isfinite(refPt.paz);

        // L1: 原始检测点迹（DetPoint）
        if (hasDet && ts.frameIndex >= 0 && ts.frameIndex < detData->frames.size() && refPolarOk) {
            const DetPointFrame &df = detData->frames.at(ts.frameIndex);
            double bestDist = std::numeric_limits<double>::infinity();
            float bestSnr = kNaNf;
            for (const DetPointItem &dp : df.points) {
                const double dKm = polarDistKm(dp.range, dp.az, refPt.prange, refPt.paz);
                if (dKm <= searchRadiusKm) {
                    ++st.l1DetCount;
                    if (dKm < bestDist) {
                        bestDist = dKm;
                        bestSnr = dp.snr;
                    }
                }
            }
            if (st.l1DetCount > 0) {
                st.bestDetDistKm = bestDist;
                st.bestDetSnr = bestSnr;
            }
        }

        // L2: 临时池点（tempTrackList）
        for (const TempTrackPoint &tp : bundle.tempPoints) {
            if (std::llabs(tp.timeMs - ts.timeMs) > 5000)
                continue;
            double dKm = std::numeric_limits<double>::infinity();
            if (validLatLon(tp.lat, tp.lon) && (tp.lat != 0.0 || tp.lon != 0.0))
                dKm = geocalc::vincentyKm(refPt.lat, refPt.lon, tp.lat, tp.lon);
            else if (refPolarOk && std::isfinite(tp.prange) && std::isfinite(tp.paz))
                dKm = polarDistKm(tp.prange, tp.paz, refPt.prange, refPt.paz);
            if (dKm <= searchRadiusKm)
                ++st.l2TempCount;
        }

        // L3 (assc / rawPoints) 与 L4 (smooth / points)
        for (const Track &tr : allTracks) {
            if (tr.source == TrackSource::Adsb)
                continue;
            if (tr.minTime() > ts.timeMs + 5000 || tr.maxTime() < ts.timeMs - 5000)
                continue;

            const QVector<TrackPoint> &asscCol = tr.hasRawPoints()
                ? tr.rawPoints
                : (tr.source == TrackSource::RadarRaw ? tr.points : QVector<TrackPoint>());
            const int ai = findNearestTrackPoint(asscCol, ts.timeMs, 5000);
            if (ai >= 0) {
                const TrackPoint &ap = asscCol.at(ai);
                if (geocalc::vincentyKm(refPt.lat, refPt.lon, ap.lat, ap.lon) <= searchRadiusKm)
                    ++st.l3AsscCount;
            }

            if (tr.source == TrackSource::Radar) {
                const int si = findNearestTrackPoint(tr.points, ts.timeMs, 5000);
                if (si >= 0) {
                    const TrackPoint &sp = tr.points.at(si);
                    if (geocalc::vincentyKm(refPt.lat, refPt.lon, sp.lat, sp.lon) <= searchRadiusKm)
                        ++st.l4SmoothCount;
                }
            }
        }

        stats.append(st);
    }

    return stats;
}

} // namespace trackdiag
