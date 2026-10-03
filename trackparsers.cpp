#include "trackparsers.h"
#include "matfile.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <cmath>
#include <cstring>

namespace {

const double kFeetToMeter = 0.3048;

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

// 数值字段：空串或非法返回 def，ok 指示是否解析成功
double toNum(const char *s, int len, bool *ok = nullptr, double def = 0.0)
{
    if (len <= 0) {
        if (ok)
            *ok = false;
        return def;
    }
    bool good = false;
    const double v = QByteArray(s, len).toDouble(&good);
    if (ok)
        *ok = good && std::isfinite(v);
    return good && std::isfinite(v) ? v : def;
}

QString toStr(const char *s, int len)
{
    return len > 0 ? QString::fromUtf8(s, len).trimmed() : QString();
}

// MATLAB datenum（北京时间）-> Unix 毫秒；与 RadarView 一致取整到秒
qint64 datenumToMs(double dn)
{
    const qint64 sec = qint64(std::llround((dn - 719529.0) * 86400.0));
    return sec * 1000 - 8LL * 3600 * 1000;
}

bool validLatLon(double lat, double lon)
{
    return std::isfinite(lat) && std::isfinite(lon)
        && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

} // namespace

// ---------------------------------------------------------------
//  ADS-B CSV
//  0 ICAO | 1 纬度 | 2 经度 | 3 航向 | 4 高度(ft) | 5 地速(kt) | 6 保留 | 7 接收机
//  8 机型 | 9 注册号 | 10 时间 | 11 起飞地 | 12 目的地 | 13 航班号(IATA) | 14 标志
//  15 升降率(ft/min) | 16 航班号(ICAO) | 17 标志 | 18 航司
// ---------------------------------------------------------------
bool parseAdsbCsv(const QString &path, QVector<Track> *out, QString *error, const ProgressFn &progress)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开文件：%1").arg(f.errorString()));
    const QByteArray data = f.readAll();
    f.close();
    if (progress)
        progress(10);

    const QString fileName = QFileInfo(path).fileName();
    QMap<QString, Track> groups;    // 按 ICAO 排序，与 RadarView 一致
    const char *p = data.constData();
    const char *const end = p + data.size();
    const qint64 total = qMax(1, data.size());
    int lastPct = 10;
    int badRows = 0;

    const char *fs[19];
    int fl[19];
    while (p < end) {
        const char *eol = static_cast<const char *>(std::memchr(p, '\n', size_t(end - p)));
        const char *lineEnd = eol ? eol : end;
        const char *next = eol ? eol + 1 : end;
        // 去掉行首尾空白（含 \r）
        while (p < lineEnd && (*p == ' ' || *p == '\t'))
            ++p;
        while (lineEnd > p && (lineEnd[-1] == '\r' || lineEnd[-1] == ' ' || lineEnd[-1] == '\t'))
            --lineEnd;
        if (p == lineEnd) {
            p = next;
            continue;
        }

        int n = 0;
        const char *s = p;
        for (const char *c = p; c <= lineEnd && n < 19; ++c) {
            if (c == lineEnd || *c == ',') {
                fs[n] = s;
                fl[n] = int(c - s);
                ++n;
                s = c + 1;
            }
        }
        p = next;

        bool okLat = false, okLon = false;
        qint64 t = 0;
        if (n < 19 || fl[0] == 0) {
            ++badRows;
            continue;
        }
        const double lat = toNum(fs[1], fl[1], &okLat);
        const double lon = toNum(fs[2], fl[2], &okLon);
        if (!okLat || !okLon || !validLatLon(lat, lon) || !parseBeijingTime(fs[10], fl[10], &t)) {
            ++badRows;
            continue;
        }

        const QString icao = QString::fromLatin1(fs[0], fl[0]).trimmed();
        auto it = groups.find(icao);
        if (it == groups.end()) {
            Track tr;
            tr.id = icao;
            tr.source = TrackSource::Adsb;
            tr.fileName = fileName;
            tr.aircraftType = toStr(fs[8], fl[8]);
            tr.registration = toStr(fs[9], fl[9]);
            tr.origin = toStr(fs[11], fl[11]);
            tr.destination = toStr(fs[12], fl[12]);
            tr.flightNo = toStr(fs[13], fl[13]);
            tr.icaoFlightNo = toStr(fs[16], fl[16]);
            tr.airline = toStr(fs[18], fl[18]);
            it = groups.insert(icao, tr);
        }
        TrackPoint pt;
        pt.t = t;
        pt.lat = lat;
        pt.lon = lon;
        pt.heading = float(toNum(fs[3], fl[3]));
        pt.alt = float(toNum(fs[4], fl[4]) * kFeetToMeter);
        pt.speed = float(toNum(fs[5], fl[5]));
        pt.vrate = float(toNum(fs[15], fl[15]));
        it->points.append(pt);

        if (progress) {
            const int pct = 10 + int(80 * (p - data.constData()) / total);
            if (pct >= lastPct + 5) {
                lastPct = pct;
                progress(pct);
            }
        }
    }

    out->clear();
    out->reserve(groups.size());
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        it->normalize();
        out->append(std::move(it.value()));
    }
    if (progress)
        progress(100);
    if (out->isEmpty())
        return fail(error, QStringLiteral("文件中没有有效的 ADS-B 数据（%1 行无效）").arg(badRows));
    return true;
}

// ---------------------------------------------------------------
//  雷达 MAT
// ---------------------------------------------------------------
bool parseRadarMat(const QString &path, bool raw, const QString &displayName,
                   QVector<Track> *out, QString *error, const ProgressFn &progress)
{
    MatFile mat;
    if (!mat.open(path, error))
        return false;
    if (progress)
        progress(5);
    const MatArray list = mat.variable(QStringLiteral("trackList"), error);
    if (!list.isValid())
        return false;
    if (!list.isStruct())
        return fail(error, QStringLiteral("trackList 不是结构体数组"));
    if (progress)
        progress(20);

    const QStringList ptFields = raw ? QStringList{QStringLiteral("asscPointList")}
                                     : QStringList{QStringLiteral("smoothPointList"),
                                                   QStringLiteral("outputPointList")};
    const QString prefix = raw ? QStringLiteral("RAW") : QStringLiteral("RADAR");
    const QString fileName = displayName.isEmpty() ? QFileInfo(path).fileName() : displayName;

    out->clear();
    const int nTracks = list.numel();
    for (int i = 0; i < nTracks; ++i) {
        MatArray pts;
        for (const QString &fName : ptFields) {
            const MatArray cand = list.field(fName, i);
            if (cand.isStruct() && cand.numel() > 0) {
                pts = cand;
                break;
            }
        }
        if (!pts.isValid())
            continue;

        const int batchNo = int(list.field(QStringLiteral("BatchNo"), i).scalar());
        const int type = int(list.field(QStringLiteral("Type"), i).scalar());

        Track tr;
        tr.id = QStringLiteral("%1-%2").arg(prefix).arg(batchNo, 4, 10, QLatin1Char('0'));
        tr.flightNo = QStringLiteral("TGT-%1").arg(batchNo, 4, 10, QLatin1Char('0'));
        tr.aircraftType = type == 1 ? QStringLiteral("RADAR") : QStringLiteral("UNKNOWN");
        tr.source = raw ? TrackSource::RadarRaw : TrackSource::Radar;
        tr.fileName = fileName;

        const int nPts = pts.numel();
        tr.points.reserve(nPts);
        for (int j = 0; j < nPts; ++j) {
            bool okT = false, okLat = false, okLon = false;
            const double dn = pts.field(QStringLiteral("time"), j).scalar(0, &okT);
            const double lat = pts.field(QStringLiteral("lat"), j).scalar(0, &okLat);
            const double lon = pts.field(QStringLiteral("lon"), j).scalar(0, &okLon);
            if (!okT || !okLat || !okLon || dn <= 0.0 || !validLatLon(lat, lon))
                continue;
            TrackPoint pt;
            pt.t = datenumToMs(dn);
            pt.lat = lat;
            pt.lon = lon;
            tr.points.append(pt);
        }
        tr.normalize();
        if (!tr.points.isEmpty())
            out->append(std::move(tr));

        if (progress && nTracks > 10 && i % qMax(1, nTracks / 10) == 0)
            progress(20 + 75 * i / nTracks);
    }
    if (progress)
        progress(100);
    if (out->isEmpty())
        return fail(error, QStringLiteral("trackList 中没有有效航迹点"));
    return true;
}
