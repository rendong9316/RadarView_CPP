#ifndef TRACKPARSERS_H
#define TRACKPARSERS_H

#include "track.h"

#include <functional>

// 进度回调：0..100，可在工作线程中调用
using ProgressFn = std::function<void(int)>;

// ADS-B CSV：无表头、19 列逗号分隔（列定义见 RadarView docs/adsb-format.md），按 ICAO 聚合
bool parseAdsbCsv(const QString &path, QVector<Track> *out, QString *error,
                  const ProgressFn &progress = ProgressFn());

// 雷达 MAT（v5）：读取 trackList；raw=false 用 smoothPointList（缺失时 outputPointList），
// raw=true 用 asscPointList。displayName 写入 Track::fileName
bool parseRadarMat(const QString &path, bool raw, const QString &displayName,
                   QVector<Track> *out, QString *error,
                   const ProgressFn &progress = ProgressFn());

#endif // TRACKPARSERS_H
