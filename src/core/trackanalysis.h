#ifndef TRACKANALYSIS_H
#define TRACKANALYSIS_H

#include <QString>
#include <QVector>
#include <limits>

#include "track.h"

namespace trackanalysis {

// 同条雷达航迹在某帧的滤波新息/残差（预测 -> 关联，平滑 -> 关联）
struct FilterResidualPoint {
    qint64 timeMs = 0;
    double asscLat = 0.0, asscLon = 0.0;
    double predLat = 0.0, predLon = 0.0;
    double smoothLat = 0.0, smoothLon = 0.0;
    bool hasPred = false;
    bool hasSmooth = false;
    double predGeoErrKm = std::numeric_limits<double>::quiet_NaN();    // 预测 vs 关联 水平距离(km)
    double smoothGeoErrKm = std::numeric_limits<double>::quiet_NaN();  // 平滑 vs 关联 水平距离(km)
    double dRange = std::numeric_limits<double>::quiet_NaN();          // assc.prange - pred.prange
    double dAz = std::numeric_limits<double>::quiet_NaN();             // wrap180(assc.paz - pred.paz)
    double dVr = std::numeric_limits<double>::quiet_NaN();             // assc.pvr - pred.pvr
    double smoothDRange = std::numeric_limits<double>::quiet_NaN();    // assc.prange - smooth.prange
    double smoothDAz = std::numeric_limits<double>::quiet_NaN();       // wrap180(assc.paz - smooth.paz)
    double smoothDVr = std::numeric_limits<double>::quiet_NaN();       // assc.pvr - smooth.pvr
};

// 两航迹时间对齐后的逐点误差样本（B 相对 A 的偏差）
struct TrackDiffSample {
    qint64 timeMs = 0;
    double latA = 0.0, lonA = 0.0;
    double latB = 0.0, lonB = 0.0;
    double geoDistKm = 0.0;
    double dRange = std::numeric_limits<double>::quiet_NaN();
    double dAz = std::numeric_limits<double>::quiet_NaN();
    double dVr = std::numeric_limits<double>::quiet_NaN();
    double dAltM = std::numeric_limits<double>::quiet_NaN();
    double dSpeedKt = std::numeric_limits<double>::quiet_NaN();
};

struct ErrorMetric {
    int count = 0;
    double rmse = 0.0;
    double mae = 0.0;
    double p95 = 0.0;
    double maxAbs = 0.0;
    double bias = 0.0;
    bool valid() const { return count > 0; }
};

struct TrackCompareResult {
    bool timeOverlapped = false;
    qint64 overlapStartMs = 0;
    qint64 overlapEndMs = 0;
    double coverageB = 0.0;      // B 中成功匹配到 A 的点数比例 (0..1)
    int maxGeoErrorIndex = -1;
    QVector<TrackDiffSample> samples;
    ErrorMetric geoDistKm;
    ErrorMetric dRange;
    ErrorMetric dAz;
    ErrorMetric dVr;
    ErrorMetric dAltM;
    ErrorMetric dSpeedKt;
};

// 角度差归一化到 (-180, 180]
double wrapAngleDiffDeg(double deg);

// 计算单条雷达航迹各关联帧上的预测新息与平滑残差
QVector<FilterResidualPoint> computeFilterResiduals(const Track &track, qint64 maxMatchGapMs = 1500);

// 两航迹定量误差对比：以 a 为时间插值基准，对 b 中每个采样点对齐并计算各项误差统计
TrackCompareResult compareTracks(const QVector<TrackPoint> &a,
                                 const QVector<TrackPoint> &b,
                                 qint64 maxInterpGapMs = 30000);

// 导出对比样本为 UTF-8（带 BOM）CSV
bool exportCompareCsv(const TrackCompareResult &res, const QString &path, QString *error = nullptr);

} // namespace trackanalysis

#endif // TRACKANALYSIS_H
