#ifndef TRACKPARSERS_H
#define TRACKPARSERS_H

#include "radarsite.h"
#include "track.h"

#include <functional>

class MatFile;

// 进度回调：0..100，可在工作线程中调用
using ProgressFn = std::function<void(int)>;

// ADS-B CSV：无表头、19 列逗号分隔（列定义见 RadarView docs/adsb-format.md），按 ICAO 聚合
bool parseAdsbCsv(const QString &path, QVector<Track> *out, QString *error,
                  const ProgressFn &progress = ProgressFn());

// 从 MAT 的 sysPara 结构体提取雷达收发站址（tx_BLH / rx_BLH）与探测扇区（rIndex/aIndex/beamDir/T_inter）
bool parseRadarSysPara(const MatFile &mat, const QString &siteName, RadarSite *out);

// 雷达 MAT（v5）：读取 trackList；raw=false 用 smoothPointList（缺失时 outputPointList），
// raw=true 用 asscPointList。displayName 写入 Track::fileName；若 siteOut 非空且存在 sysPara 则顺带提取站址
bool parseRadarMat(const QString &path, bool raw, const QString &displayName,
                   QVector<Track> *out, QString *error,
                   const ProgressFn &progress = ProgressFn(),
                   RadarSite *siteOut = nullptr);

#endif // TRACKPARSERS_H
