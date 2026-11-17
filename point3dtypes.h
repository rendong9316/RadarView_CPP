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
    QVector<TrackOverlayPoint> points;
};

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

// 点源过滤语义（画布绘制与窗口提示共用，避免两处判断不一致）：
// 数据库里 Radar 批次存平滑点列、RadarRaw 批次存关联点列，预测点列未入库。
inline bool point3dTrackSourceAdmits(Point3dTrackSource filter, TrackSource source)
{
    switch (filter) {
    case Point3dTrackSource::Smooth: return source == TrackSource::Radar;
    case Point3dTrackSource::Associated: return source == TrackSource::RadarRaw;
    case Point3dTrackSource::Predicted: return false;
    case Point3dTrackSource::Auto: break;
    }
    return true;
}

#endif // POINT3DTYPES_H
