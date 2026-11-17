#ifndef POINT3DPARSERS_H
#define POINT3DPARSERS_H

#include <functional>

#include <QString>
#include <QStringList>

#include "point3dtypes.h"

// 点迹 MAT 解析与连续性评分（对应 MATLAB track_visualization_demo 的
// loadRealDataset / pointContinuityScores）。全部为纯函数，可在工作线程调用。

typedef std::function<void(int)> Point3dProgressFn;   // 0..100
typedef std::function<bool()> Point3dCancelFn;        // 返回 true 表示请求取消

// 默认变量名 / 字段名（对应 cfg.DetPointVariable / SysParaVariable / AzimuthField）
extern const char *const kDefaultDetVariable;      // "DetPoint"
extern const char *const kDefaultSysVariable;      // "sysPara"
extern const char *const kDefaultAzimuthField;     // "az"

// 解析单个 detPoint MAT。成功返回 true；失败写 error。
bool parseDetPointFile(const QString &path, DetPointFrame *frame, QString *error,
                       const QString &detVar = QStringLiteral("DetPoint"),
                       const QString &sysVar = QStringLiteral("sysPara"),
                       const QString &azimuthField = QStringLiteral("az"));

// 批量解析（任意数量，含单个）：逐文件收集错误，成功的按时间排序，
// 计算 ordinal / zValue / 时间范围 / SNR 范围。maxFrames<=0 表示不限制。
bool parseDetPointFiles(const QStringList &paths, Point3dDataset *out, QStringList *errors,
                        const Point3dProgressFn &progress = Point3dProgressFn(),
                        const Point3dCancelFn &cancelled = Point3dCancelFn(),
                        const QString &detVar = QStringLiteral("DetPoint"),
                        const QString &sysVar = QStringLiteral("sysPara"),
                        const QString &azimuthField = QStringLiteral("az"),
                        int maxFrames = 0);

// 连续性评分：原地写入每个 frame.continuity（与 points 等长，0..1）。
// 已取消的帧评分置 0，不影响已有数据。
void computeContinuity(QVector<DetPointFrame> &frames, const ContinuityConfig &cfg,
                       const Point3dProgressFn &progress = Point3dProgressFn(),
                       const Point3dCancelFn &cancelled = Point3dCancelFn());

// 数据库航迹 -> 点云叠加：只保留带测量坐标的点，Z 值按时间匹配最近点迹帧。
// 返回 false 表示没有任何可用点（例如旧数据缺少 prange/pvr/paz）。
bool overlayFromTrack(const Track &track, const QVector<DetPointFrame> &frames,
                      TrackOverlay *out, const QColor &color = QColor());

// 重新计算数据集派生量（排序结果、时间范围、SNR 范围）
void rebuildDatasetDerived(Point3dDataset *data);

#endif // POINT3DPARSERS_H
