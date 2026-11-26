#ifndef POINT3DTYPES_H
#define POINT3DTYPES_H

#include <QColor>
#include <QString>
#include <QVector>

#include "track.h"

// 点迹三维可视化的纯数据类型（对应 MATLAB track_visualization_demo 的数据结构）

// 三维视图坐标组合（ViewMode）
enum class Point3dViewMode { RVF = 0, RAF = 1, VAF = 2 };

// 航迹点源（TrackPointSource）：自动 / 平滑 / 关联 / 预测
enum class Point3dTrackSource { Auto = 0, Smooth = 1, Associated = 2, Predicted = 3 };

// 单个点迹（DetPoint 元素里绘图需要的标量字段）
struct DetPointItem {
    int pointID = 0;        // 缺失时用 1 基序号
    float range = 0.0f;     // 距离，m
    float vr = 0.0f;        // 多普勒径向速度，m/s
    float az = 0.0f;        // 方位角，度
    float snr = 0.0f;       // dB
};

// 一帧 = 一个 detPoint_*.mat
struct DetPointFrame {
    QString path;
    QString fileName;
    double datenum = 0.0;   // 原始 MATLAB datenum（北京墙钟）
    double frameId = 0.0;   // 原始 frameID（整型宽度不定，按 double 保存）
    qint64 timeMs = 0;      // datenumToMs
    int ordinal = 0;        // 按时间排序后的 1 基序号
    double zValue = 0.0;    // Z 轴帧值：frameID 严格递增时用 frameID，否则用 ordinal
    QVector<DetPointItem> points;
    QVector<float> continuity;   // 与 points 等长，0..1
};

// 航迹叠加点（测量坐标；来自数据库 saved_tracks.points）
struct TrackOverlayPoint {
    qint64 timeMs = 0;
    double zValue = 0.0;    // 匹配到的点迹帧 Z 值
    float prange = 0.0f;
    float pvr = 0.0f;
    float paz = 0.0f;
};

struct TrackOverlay {
    QString id;             // RADAR-0001 / RAW-0001
    int batchNo = 0;
    QString fileName;
    TrackSource source = TrackSource::Radar;
    QColor color;
    QVector<TrackOverlayPoint> points;      // 平滑点列（Radar 批次的主点列）
    QVector<TrackOverlayPoint> rawPoints;   // 关联点列（同一 MAT 的 asscPointList / RadarRaw 批次）
    QVector<TrackOverlayPoint> predPoints;  // 预测点列（同一 MAT 的 predictRes）
    qint64 minTime = 0, maxTime = 0;        // 三套点列合起来的时间范围（联动匹配用）
};

// 叠加航迹的稳定键（与 Track::key() 口径一致）
inline QString point3dOverlayKey(const TrackOverlay &ov)
{
    return ov.id + QStringLiteral("::") + trackSourceName(ov.source)
         + QStringLiteral("::") + ov.fileName;
}

// 地图侧的一条航迹在联动里怎么指代：BatchNo（MAT 里的航迹编号）+ 时间范围。
// 同一 MAT 分别按「雷达航迹」「雷达原始测量」导入时，id 前缀不同（RADAR-/RAW-）
// 但编号和时间一致，靠这两项就能判断"指的是同一条航迹"。
struct LinkedTrackRef {
    int batchNo = 0;
    qint64 t0 = 0, t1 = 0;
};

// 两条航迹是否在联动意义上同一条：编号相同且时间范围有重叠
inline bool point3dRefsMatch(int batchNo, qint64 t0, qint64 t1,
                             int otherBatchNo, qint64 o0, qint64 o1)
{
    if (batchNo <= 0 || batchNo != otherBatchNo)
        return false;
    return qMin(t1, o1) >= qMax(t0, o0);
}

// 连续性评分参数（对应 Continuity* 四个 cfg）
struct ContinuityConfig {
    int frameGap = 3;
    double rangeScale = 80.0;
    double dopplerScale = 10.0;
    double azimuthScale = 6.0;
};

// 一个点云窗口的完整数据集
struct Point3dDataset {
    QVector<DetPointFrame> frames;   // 按时间升序
    QVector<TrackOverlay> tracks;
    qint64 minTime = 0;
    qint64 maxTime = 0;
    double snrMin = 0.0;
    double snrMax = 1.0;

    void clear()
    {
        frames.clear();
        tracks.clear();
        minTime = maxTime = 0;
        snrMin = 0.0;
        snrMax = 1.0;
    }
    bool isEmpty() const { return frames.isEmpty(); }
};

// 区域筛选框：坐标就是"当前视图坐标"（X/Y 随视图模式变化）+ 帧值 Z。
// valid=false 表示不筛选（全量）。
struct Point3dRegion {
    bool valid = false;
    double x0 = 0.0, x1 = 0.0;
    double y0 = 0.0, y1 = 0.0;
    double z0 = 0.0, z1 = 0.0;
    bool inside(double x, double y, double z) const
    {
        return !valid || (x >= x0 - 1e-9 && x <= x1 + 1e-9
                          && y >= y0 - 1e-9 && y <= y1 + 1e-9
                          && z >= z0 - 1e-9 && z <= z1 + 1e-9);
    }
};

// 视图坐标：仅做三选一映射，与 MATLAB viewCoordinates 一致
inline void point3dViewCoordinates(Point3dViewMode mode, double range, double vr, double az,
                                   double *x, double *y)
{
    switch (mode) {
    case Point3dViewMode::RVF: *x = range; *y = vr; break;
    case Point3dViewMode::RAF: *x = range; *y = az; break;
    case Point3dViewMode::VAF: *x = vr; *y = az; break;
    }
}

// 某一视图模式下三个轴的数据量程（区域筛选三行的初值/满量程）
inline Point3dRegion point3dRegionExtent(const Point3dDataset &data, Point3dViewMode mode)
{
    Point3dRegion r;
    bool any = false;
    auto add = [&](double range, double vr, double az, double z) {
        double x = 0, y = 0;
        point3dViewCoordinates(mode, range, vr, az, &x, &y);
        if (!any) {
            r.x0 = r.x1 = x;
            r.y0 = r.y1 = y;
            r.z0 = r.z1 = z;
            any = true;
        } else {
            r.x0 = qMin(r.x0, x);
            r.x1 = qMax(r.x1, x);
            r.y0 = qMin(r.y0, y);
            r.y1 = qMax(r.y1, y);
            r.z0 = qMin(r.z0, z);
            r.z1 = qMax(r.z1, z);
        }
    };
    for (const DetPointFrame &f : data.frames)
        for (const DetPointItem &p : f.points)
            add(p.range, p.vr, p.az, f.zValue);
    for (const TrackOverlay &t : data.tracks)
        for (const TrackOverlayPoint &op : t.points)
            add(op.prange, op.pvr, op.paz, op.zValue);
    for (const TrackOverlay &t : data.tracks)
        for (const TrackOverlayPoint &op : t.rawPoints)
            add(op.prange, op.pvr, op.paz, op.zValue);
    for (const TrackOverlay &t : data.tracks)
        for (const TrackOverlayPoint &op : t.predPoints)
            add(op.prange, op.pvr, op.paz, op.zValue);
    r.valid = any && r.x1 >= r.x0 && r.y1 >= r.y0 && r.z1 >= r.z0;
    return r;
}

// 从航迹 id 解析批次号（"RADAR-0001" / "RAW-0001" -> 1）；解析失败返回 0
inline int point3dBatchNoFromId(const QString &id)
{
    const int dash = id.lastIndexOf(QLatin1Char('-'));
    if (dash < 0)
        return 0;
    bool ok = false;
    const int v = id.mid(dash + 1).toInt(&ok);
    return ok ? v : 0;
}

inline int point3dBatchNo(const Track &t)
{
    return point3dBatchNoFromId(t.id);
}

// 点源选择：同一条叠加航迹可能同时带平滑、关联、预测三套点列，按当前模式取对应的一列。
// 返回 nullptr 表示这条航迹在当前模式下没有可画的点。
inline const QVector<TrackOverlayPoint> *point3dOverlayPointsFor(const TrackOverlay &ov,
                                                                Point3dTrackSource mode)
{
    switch (mode) {
    case Point3dTrackSource::Smooth:
        return ov.points.isEmpty() ? nullptr : &ov.points;
    case Point3dTrackSource::Associated:
        return ov.rawPoints.isEmpty() ? nullptr : &ov.rawPoints;
    case Point3dTrackSource::Predicted:
        return ov.predPoints.isEmpty() ? nullptr : &ov.predPoints;
    case Point3dTrackSource::Auto:
        break;
    }
    // 自动：优先平滑，其次关联，再次预测
    if (!ov.points.isEmpty())
        return &ov.points;
    if (!ov.rawPoints.isEmpty())
        return &ov.rawPoints;
    return ov.predPoints.isEmpty() ? nullptr : &ov.predPoints;
}

// 该航迹是否带某种点列（供窗口判断当前点源过滤是否把所选航迹全藏起来）
inline bool point3dOverlayHasColumn(const TrackOverlay &ov, Point3dTrackSource mode)
{
    switch (mode) {
    case Point3dTrackSource::Smooth: return !ov.points.isEmpty();
    case Point3dTrackSource::Associated: return !ov.rawPoints.isEmpty();
    case Point3dTrackSource::Predicted: return !ov.predPoints.isEmpty();
    case Point3dTrackSource::Auto: break;
    }
    return !ov.points.isEmpty() || !ov.rawPoints.isEmpty() || !ov.predPoints.isEmpty();
}

#endif // POINT3DTYPES_H
