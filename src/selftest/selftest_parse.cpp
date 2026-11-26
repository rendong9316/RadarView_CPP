#include "selftest.h"

#include "track.h"
#include "trackparsers.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

namespace selftest {

// 解析自检：HelloVscode.exe --parsetest <adsb.csv> <radar.mat>
// 输出各数据源的航迹数、点数、时间范围和首尾点，写到 exe 旁 parsetest.log
int runParseTest(const QString &csv, const QString &mat)
{
    QFile log(QCoreApplication::applicationDirPath() + QStringLiteral("/parsetest.log"));
    log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    QTextStream out(&log);
    out.setCodec("UTF-8");
    out.setRealNumberPrecision(15);
    int failures = 0;

    auto dumpPoint = [&out](const char *tag, const Track &t, const TrackPoint &p) {
        out << tag << " " << t.id << " n=" << t.points.size() << " t=" << p.t
            << " lat=" << p.lat << " lon=" << p.lon << " alt=" << p.alt
            << " hdg=" << p.heading << " spd=" << p.speed << " vr=" << p.vrate << "\n";
    };
    auto report = [&](const char *name, bool ok, const QVector<Track> &tracks,
                      const QString &err, qint64 ms) {
        if (!ok) {
            out << name << " FAIL: " << err << "\n";
            ++failures;
            return;
        }
        TrackStore store;
        store.addTracks(tracks);
        out << name << " tracks=" << store.size() << " points=" << store.pointCount()
            << " min=" << store.minTime() << " max=" << store.maxTime()
            << " (" << formatBeijingTime(store.minTime()) << " ~ " << formatBeijingTime(store.maxTime())
            << ") " << ms << " ms\n";
        const Track &first = store.tracks().first();
        const Track &last = store.tracks().last();
        dumpPoint("  first", first, first.points.first());
        dumpPoint("  last ", last, last.points.last());
    };

    QElapsedTimer clock;
    QVector<Track> tracks;
    QString err;

    clock.start();
    bool ok = parseAdsbCsv(csv, &tracks, &err);
    report("ADSB", ok, tracks, err, clock.elapsed());

    clock.restart();
    ok = parseRadarMat(mat, false, QStringLiteral("Radar"), &tracks, &err);
    report("RADAR", ok, tracks, err, clock.elapsed());

    clock.restart();
    ok = parseRadarMat(mat, true, QStringLiteral("RadarRaw"), &tracks, &err);
    report("RAW", ok, tracks, err, clock.elapsed());

    // 合并去重：同一文件导入两次，航迹数和点数不应变化
    TrackStore store;
    parseAdsbCsv(csv, &tracks, &err);
    store.addTracks(tracks);
    const qint64 before = store.pointCount();
    const int added = store.addTracks(tracks);
    out << "MERGE added=" << added << " pointsBefore=" << before << " pointsAfter=" << store.pointCount() << "\n";
    if (added != 0 || before != store.pointCount())
        ++failures;

    out << (failures == 0 ? "RESULT OK" : "RESULT FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}

} // namespace selftest
