#include "point3dparsers.h"

#include "matfile.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>

const char *const kDefaultDetVariable = "DetPoint";
const char *const kDefaultSysVariable = "sysPara";
const char *const kDefaultAzimuthField = "az";

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// 角度差取 ±180° 内最短路径（对应 MATLAB wrappedAzimuthDifference）
double wrapAzimuth(double d)
{
    double r = std::fmod(d + 180.0, 360.0);
    if (r < 0.0)
        r += 360.0;
    return r - 180.0;
}

struct NeighbourFit {
    double fit = 0.0;
    double rate[3] = { 0.0, 0.0, 0.0 };
};

// 在若干邻帧中找与当前点最连续的候选（对应 MATLAB bestTemporalNeighbour）
NeighbourFit bestTemporalNeighbour(double curRange, double curDoppler, double curAzimuth,
                                   double curFrame, const QVector<int> &frameIndices,
                                   const QVector<DetPointFrame> &frames, const ContinuityConfig &cfg)
{
    NeighbourFit best;
    double bestD2 = std::numeric_limits<double>::infinity();
    for (int fi : frameIndices) {
        const DetPointFrame &nf = frames.at(fi);
        const double frameDelta = nf.zValue - curFrame;
        if (frameDelta == 0.0)
            continue;
        for (const DetPointItem &p : nf.points) {
            const double rangeRate = (double(p.range) - curRange) / (frameDelta * cfg.rangeScale);
            const double dopplerRate = (double(p.vr) - curDoppler) / (frameDelta * cfg.dopplerScale);
            const double azimuthRate = wrapAzimuth(double(p.az) - curAzimuth)
                                     / (frameDelta * cfg.azimuthScale);
            const double d2 = rangeRate * rangeRate + dopplerRate * dopplerRate
                            + azimuthRate * azimuthRate;
            if (d2 < bestD2) {
                bestD2 = d2;
                best.fit = std::exp(-0.5 * d2);
                best.rate[0] = rangeRate;
                best.rate[1] = dopplerRate;
                best.rate[2] = azimuthRate;
            }
        }
    }
    return best;
}

} // namespace

bool parseDetPointFile(const QString &path, DetPointFrame *frame, QString *error,
                       const QString &detVar, const QString &sysVar,
                       const QString &azimuthField)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!frame)
        return fail(QStringLiteral("内部错误：frame 为空"));

    MatFile mat;
    if (!mat.open(path, error))
        return false;

    QString err;
    const MatArray list = mat.variable(detVar, &err);
    if (!list.isValid())
        return fail(err.isEmpty() ? QStringLiteral("缺少变量 %1").arg(detVar) : err);
    if (!list.isStruct())
        return fail(QStringLiteral("%1 不是结构体数组").arg(detVar));
    const int n = list.numel();
    if (n <= 0)
        return fail(QStringLiteral("%1 为空").arg(detVar));

    // sysPara：波束指向与帧时间
    double beamDir = 0.0;
    bool hasBeam = false, hasDatenum = false;
    double frameDatenum = 0.0;
    if (!sysVar.isEmpty()) {
        const MatArray sys = mat.variable(sysVar, &err);
        if (sys.isValid() && sys.isStruct()) {
            bool ok = false;
            const double b = sys.field(QStringLiteral("beamDir"), 0).scalar(0, &ok);
            if (ok && std::isfinite(b)) {
                beamDir = b;
                hasBeam = true;
            }
            ok = false;
            const double d = sys.field(QStringLiteral("datenum"), 0).scalar(0, &ok);
            if (ok && std::isfinite(d) && d > 0.0) {
                frameDatenum = d;
                hasDatenum = true;
            }
        }
    }
    if (!hasDatenum) {
        bool ok = false;
        const double d = list.field(QStringLiteral("datenum"), 0).scalar(0, &ok);
        if (ok && std::isfinite(d) && d > 0.0) {
            frameDatenum = d;
            hasDatenum = true;
        }
    }
    if (!hasDatenum)
        return fail(QStringLiteral("找不到帧时间（sysPara.datenum / DetPoint.datenum）"));

    double frameId = kNaN;
    {
        bool ok = false;
        const double f = list.field(QStringLiteral("frameID"), 0).scalar(0, &ok);
        if (ok && std::isfinite(f))
            frameId = f;
    }

    frame->path = path;
    frame->fileName = QFileInfo(path).fileName();
    frame->datenum = frameDatenum;
    frame->frameId = frameId;
    frame->timeMs = datenumToMs(frameDatenum);
    frame->points.clear();
    frame->points.reserve(n);
    frame->continuity.clear();

    const bool useTheta = (azimuthField == QLatin1String("theta"));
    for (int i = 0; i < n; ++i) {
        bool okR = false, okV = false, okS = false;
        const double range = list.field(QStringLiteral("range"), i).scalar(0, &okR);
        const double vr = list.field(QStringLiteral("vr"), i).scalar(0, &okV);
        const double snr = list.field(QStringLiteral("SNR"), i).scalar(0, &okS);
        if (!okR || !okV || !okS || !std::isfinite(range) || !std::isfinite(vr)
                || !std::isfinite(snr) || range < 0.0)
            continue;

        double az = 0.0;
        bool okA = false;
        if (useTheta) {
            az = list.field(QStringLiteral("theta"), i).scalar(0, &okA);
        } else {
            az = list.field(QStringLiteral("az"), i).scalar(0, &okA);
            if (!okA || !std::isfinite(az)) {
                // az 缺失时按实测关系 az = theta + beamDir 回退
                bool okT = false;
                const double theta = list.field(QStringLiteral("theta"), i).scalar(0, &okT);
                if (okT && std::isfinite(theta) && hasBeam) {
                    az = theta + beamDir;
                    okA = true;
                }
            }
        }
        if (!okA || !std::isfinite(az))
            continue;

        DetPointItem item;
        item.range = float(range);
        item.vr = float(vr);
        item.az = float(az);
        item.snr = float(snr);
        bool okId = false;
        const double pid = list.field(QStringLiteral("pointID"), i).scalar(0, &okId);
        item.pointID = okId ? int(pid) : i + 1;
        frame->points.append(item);
    }
    frame->continuity.resize(frame->points.size());
    if (frame->points.isEmpty())
        return fail(QStringLiteral("没有有效点迹（range/vr/az/SNR 无效）"));
    return true;
}

bool parseDetPointFiles(const QStringList &paths, Point3dDataset *out, QStringList *errors,
                        const Point3dProgressFn &progress, const Point3dCancelFn &cancelled,
                        const QString &detVar, const QString &sysVar,
                        const QString &azimuthField, int maxFrames)
{
    if (!out)
        return false;
    out->clear();
    if (errors)
        errors->clear();
    if (paths.isEmpty()) {
        if (errors)
            *errors << QStringLiteral("未选择点迹文件");
        return false;
    }

    const int total = paths.size();
    int lastPct = -1;
    for (int i = 0; i < total; ++i) {
        if (cancelled && cancelled())
            return false;
        DetPointFrame f;
        QString err;
        if (parseDetPointFile(paths.at(i), &f, &err, detVar, sysVar, azimuthField))
            out->frames.append(std::move(f));
        else if (errors)
            errors->append(QStringLiteral("%1: %2").arg(QFileInfo(paths.at(i)).fileName(), err));

        const int pct = int(qint64(100) * (i + 1) / total);
        if (progress && pct != lastPct) {
            lastPct = pct;
            progress(pct);
        }
    }
    if (out->frames.isEmpty())
        return false;

    QVector<DetPointFrame> &frames = out->frames;
    std::stable_sort(frames.begin(), frames.end(),
                     [](const DetPointFrame &a, const DetPointFrame &b) { return a.timeMs < b.timeMs; });
    if (maxFrames > 0 && frames.size() > maxFrames)
        frames.resize(maxFrames);

    // Z 轴：全部 frameID 有限且严格递增时用 frameID，否则用 1..N 序号（与 MATLAB 一致）
    bool idsOk = true;
    for (int i = 0; i < frames.size(); ++i) {
        if (!std::isfinite(frames[i].frameId)) {
            idsOk = false;
            break;
        }
        if (i > 0 && !(frames[i].frameId > frames[i - 1].frameId)) {
            idsOk = false;
            break;
        }
    }
    for (int i = 0; i < frames.size(); ++i) {
        frames[i].ordinal = i + 1;
        frames[i].zValue = idsOk ? frames[i].frameId : double(i + 1);
        if (frames[i].continuity.size() != frames[i].points.size())
            frames[i].continuity.resize(frames[i].points.size());
    }
    rebuildDatasetDerived(out);
    if (progress)
        progress(100);
    return true;
}

void rebuildDatasetDerived(Point3dDataset *data)
{
    if (!data)
        return;
    QVector<DetPointFrame> &frames = data->frames;
    std::stable_sort(frames.begin(), frames.end(),
                     [](const DetPointFrame &a, const DetPointFrame &b) { return a.timeMs < b.timeMs; });
    for (int i = 0; i < frames.size(); ++i)
        frames[i].ordinal = i + 1;

    data->minTime = frames.isEmpty() ? 0 : frames.first().timeMs;
    data->maxTime = frames.isEmpty() ? 0 : frames.last().timeMs;

    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (const DetPointFrame &f : qAsConst(frames))
        for (const DetPointItem &p : f.points) {
            lo = qMin(lo, double(p.snr));
            hi = qMax(hi, double(p.snr));
        }
    if (!(lo <= hi)) {
        lo = 0.0;
        hi = 1.0;
    } else if (hi - lo < 1e-6) {
        hi = lo + 1.0;
    }
    data->snrMin = lo;
    data->snrMax = hi;
}

void computeContinuity(QVector<DetPointFrame> &frames, const ContinuityConfig &cfg,
                       const Point3dProgressFn &progress, const Point3dCancelFn &cancelled)
{
    const int n = frames.size();
    if (n == 0)
        return;
    const int gap = qMax(1, cfg.frameGap);
    int lastPct = -1;

    for (int k = 0; k < n; ++k) {
        if (cancelled && cancelled()) {
            // 取消：剩余帧保持 0，不清空已算好的结果
            for (int j = k; j < n; ++j)
                frames[j].continuity.fill(0.0f, frames[j].points.size());
            return;
        }

        DetPointFrame &cur = frames[k];
        cur.continuity.resize(cur.points.size());
        QVector<int> prevIdx, nextIdx;
        for (int j = qMax(0, k - gap); j < k; ++j)
            prevIdx.append(j);
        for (int j = k + 1; j <= qMin(n - 1, k + gap); ++j)
            nextIdx.append(j);

        const double curFrame = cur.zValue;
        for (int i = 0; i < cur.points.size(); ++i) {
            const DetPointItem &p = cur.points.at(i);
            const NeighbourFit prev = bestTemporalNeighbour(p.range, p.vr, p.az, curFrame,
                                                           prevIdx, frames, cfg);
            const NeighbourFit next = bestTemporalNeighbour(p.range, p.vr, p.az, curFrame,
                                                           nextIdx, frames, cfg);
            const double snrScore = 1.0 / (1.0 + std::exp(-(double(p.snr) - 7.0) / 3.0));
            const double snrWeight = 0.35 + 0.65 * snrScore;
            double score = 0.0;
            if (prev.fit > 0.0 && next.fit > 0.0) {
                const double dr = next.rate[0] - prev.rate[0];
                const double dv = next.rate[1] - prev.rate[1];
                const double da = next.rate[2] - prev.rate[2];
                const double smoothness = std::exp(-1.6 * (dr * dr + dv * dv + da * da));
                const double temporalFit = std::sqrt(prev.fit * next.fit);
                score = temporalFit * (0.20 + 0.80 * smoothness) * snrWeight;
            } else {
                score = 0.35 * qMax(prev.fit, next.fit) * snrWeight;
            }
            cur.continuity[i] = float(qBound(0.0, score, 1.0));
        }

        const int pct = int(qint64(100) * (k + 1) / n);
        if (progress && pct != lastPct) {
            lastPct = pct;
            progress(pct);
        }
    }
}

bool overlayFromTrack(const Track &track, const QVector<DetPointFrame> &frames,
                      TrackOverlay *out, const QColor &color)
{
    if (!out || frames.isEmpty())
        return false;

    out->id = track.id;
    out->batchNo = point3dBatchNo(track);
    out->fileName = track.fileName;
    out->source = track.source;
    out->color = color.isValid() ? color : trackSourceColor(track.source);
    out->points.clear();
    out->points.reserve(track.points.size());

    for (const TrackPoint &p : track.points) {
        if (!std::isfinite(p.prange) || !std::isfinite(p.pvr) || !std::isfinite(p.paz))
            continue;

        // 按时间二分找最近帧（帧已按时间升序）
        int lo = 0, hi = frames.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (frames.at(mid).timeMs < p.t)
                lo = mid + 1;
            else
                hi = mid;
        }
        int best = -1;
        qint64 bestDiff = 0;
        for (int k : { lo - 1, lo }) {
            if (k < 0 || k >= frames.size())
                continue;
            const qint64 diff = qAbs(frames.at(k).timeMs - p.t);
            if (best < 0 || diff < bestDiff) {
                best = k;
                bestDiff = diff;
            }
        }
        if (best < 0)
            continue;

        TrackOverlayPoint op;
        op.timeMs = p.t;
        op.zValue = frames.at(best).zValue;
        op.prange = p.prange;
        op.pvr = p.pvr;
        op.paz = p.paz;
        out->points.append(op);
    }
    return !out->points.isEmpty();
}
