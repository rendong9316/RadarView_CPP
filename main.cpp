#include "mainwindow.h"
#include "globewidget.h"

#include <QApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QTextStream>
#include <QFile>
#include <QElapsedTimer>
#include <functional>
#include <cmath>

#include "track.h"
#include "trackparsers.h"
#include "tracklayer.h"
#include "replaycontroller.h"

#include <QFileInfo>

namespace {

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

// 自检：等待瓦片加载完成后检查拾取、缩放细分、拖拽，结果写到 exe 旁 selftest.log
void runSelfTest(MainWindow *w)
{
    struct State {
        QFile log;
        QTextStream out;
        int failures = 0;
        QElapsedTimer clock;
    };
    auto st = std::make_shared<State>();
    st->log.setFileName(QCoreApplication::applicationDirPath() + QStringLiteral("/selftest.log"));
    st->log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    st->out.setDevice(&st->log);
    st->out.setCodec("UTF-8");
    st->clock.start();

    GlobeWidget *g = w->globe();
    auto check = [st](bool ok, const QString &what) {
        st->out << (ok ? "PASS " : "FAIL ") << what << "\n";
        st->out.flush();
        if (!ok)
            ++st->failures;
    };

    // 等到不再有待加载瓦片（最多 15 秒）再执行下一步
    auto waitIdle = std::make_shared<std::function<void(std::function<void()>)>>();
    *waitIdle = [g, st, waitIdle](std::function<void()> next) {
        QElapsedTimer t;
        t.start();
        auto poll = std::make_shared<std::function<void()>>();
        *poll = [g, t, next, poll]() {
            if ((!g->isLoading() && t.elapsed() > 300) || t.elapsed() > 15000) {
                next();
                return;
            }
            g->update();
            QTimer::singleShot(50, *poll);
        };
        QTimer::singleShot(50, *poll);
    };

    auto finish = [st]() {
        st->out << (st->failures == 0 ? "RESULT OK" : "RESULT FAILED")
                << " (" << st->failures << " failures, " << st->clock.elapsed() << " ms)\n";
        st->out.flush();
        st->log.close();
        QCoreApplication::exit(st->failures == 0 ? 0 : 1);
    };

    (*waitIdle)([=]() {
        st->out << "start: " << g->debugInfo() << "\n";
        check(g->isGlReady(), QStringLiteral("OpenGL 着色器初始化"));
        check(!g->tileName().isEmpty(), QStringLiteral("已加载瓦片：%1").arg(g->tileName()));
        check(g->drawnTileCount() > 0, QStringLiteral("绘制瓦片数 %1").arg(g->drawnTileCount()));

        const QPoint c(g->width() / 2, g->height() / 2);
        double lon = 0, lat = 0;
        const bool hit = g->geoAt(QPointF(g->width() / 2.0, g->height() / 2.0), &lon, &lat);
        check(hit && std::fabs(lon - 105.0) < 0.01 && std::fabs(lat - 35.0) < 0.01,
              QStringLiteral("屏幕中心拾取 = %1, %2（期望 105, 35）").arg(lon, 0, 'f', 4).arg(lat, 0, 'f', 4));
        double lon2 = 0, lat2 = 0;
        check(!g->geoAt(QPoint(2, 2), &lon2, &lat2), QStringLiteral("左上角在地球外，拾取应失败"));

        // 拖拽：把中心右侧 100px 处的地面拖到中心，中心经度应变大
        double lonR = 0, latR = 0;
        g->geoAt(c + QPoint(100, 0), &lonR, &latR);
        g->panPixels(c + QPoint(100, 0), c);
        double lonAfter = 0, latAfter = 0;
        g->geoAt(c, &lonAfter, &latAfter);
        check(std::fabs(lonAfter - lonR) < 0.2 && std::fabs(latAfter - latR) < 0.2,
              QStringLiteral("拖拽后中心 = %1, %2（期望 ≈ %3, %4）")
                  .arg(lonAfter, 0, 'f', 3).arg(latAfter, 0, 'f', 3).arg(lonR, 0, 'f', 3).arg(latR, 0, 'f', 3));

        // 缩放到约 10 km 高度，应细分到瓦片最大层级
        g->resetView();
        g->zoomBy(30);
        (*waitIdle)([=]() {
            st->out << "zoomed: " << g->debugInfo() << "\n";
            const int maxZ = g->tileMaxZoom();
            check(g->drawnMaxZoom() >= maxZ && maxZ > 0,
                  QStringLiteral("拉近后绘制层级 %1（瓦片最大 %2）").arg(g->drawnMaxZoom()).arg(maxZ));
            check(g->altitudeKm() < 50.0, QStringLiteral("拉近后高度 %1 km").arg(g->altitudeKm(), 0, 'f', 1));
            check(!g->isLoading(), QStringLiteral("瓦片加载完成"));

            g->zoomBy(-60);
            (*waitIdle)([=]() {
                st->out << "zoomed out: " << g->debugInfo() << "\n";
                check(g->drawnMaxZoom() <= 3, QStringLiteral("拉远后绘制层级 %1").arg(g->drawnMaxZoom()));
                check(g->glErrorCount() == 0, QStringLiteral("OpenGL 错误数 %1").arg(g->glErrorCount()));
                finish();
            });
        });
    });
}


// 航迹自检：HelloVscode.exe --tracktest <adsb.csv> <radar.mat>
// 走真实的菜单导入流程（后台线程），检查航迹数、上传线段数、可见端点数、图层开关和 GL 错误
void runTrackTest(MainWindow *w, const QString &csv, const QString &mat)
{
    struct State {
        QFile log;
        QTextStream out;
        int failures = 0;
        QElapsedTimer clock;
    };
    auto st = std::make_shared<State>();
    st->log.setFileName(QCoreApplication::applicationDirPath() + QStringLiteral("/tracktest.log"));
    st->log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    st->out.setDevice(&st->log);
    st->out.setCodec("UTF-8");
    st->clock.start();
    w->setProperty("noDialogs", true);

    GlobeWidget *g = w->globe();
    TrackStore *store = w->trackStore();
    auto check = [st](bool ok, const QString &what) {
        st->out << (ok ? "PASS " : "FAIL ") << what << "\n";
        st->out.flush();
        if (!ok)
            ++st->failures;
    };
    // 等到至少再画 3 帧，保证图层状态已刷新
    auto afterFrames = [g](std::function<void()> next) {
        const quint64 target = g->frameCount() + 3;
        auto poll = std::make_shared<std::function<void()>>();
        QElapsedTimer t;
        t.start();
        *poll = [g, target, next, poll, t]() {
            if (g->frameCount() >= target || t.elapsed() > 10000) {
                next();
                return;
            }
            g->update();
            QTimer::singleShot(30, *poll);
        };
        QTimer::singleShot(30, *poll);
    };
    auto finish = [st]() {
        st->out << (st->failures == 0 ? "RESULT OK" : "RESULT FAILED")
                << " (" << st->failures << " failures, " << st->clock.elapsed() << " ms)\n";
        st->out.flush();
        st->log.close();
        QCoreApplication::exit(st->failures == 0 ? 0 : 1);
    };

    // 依次导入 ADS-B、雷达、雷达原始量测
    struct Job { TrackSource kind; QString path; int expectTracks; };
    auto jobs = std::make_shared<QVector<Job>>(QVector<Job>{
        { TrackSource::Adsb, csv, 2900 },
        { TrackSource::Radar, mat, 311 },
        { TrackSource::RadarRaw, mat, 311 } });
    auto step = std::make_shared<int>(0);
    auto conn = std::make_shared<QMetaObject::Connection>();
    auto runNext = std::make_shared<std::function<void()>>();

    // 回放：跳到中间时刻，可见端点应等于「已开始」的航迹数；推进到结尾应自动退出回放
    auto testReplay = [=]() {
        ReplayController *rp = w->replay();
        TrackLayer *layer = g->trackLayer();
        check(rp->start() == store->minTime() && rp->end() == store->maxTime(),
              QStringLiteral("回放范围 %1 ~ %2").arg(formatBeijingTime(rp->start()), formatBeijingTime(rp->end())));
        // ADS-B 数据时段的中点（雷达数据早于它，已全部结束）
        const qint64 mid = 1777253315000LL + (1777282200000LL - 1777253315000LL) / 2;
        int expectStarted = 0;
        for (const Track &t : store->tracks())
            if (t.minTime() <= mid)
                ++expectStarted;
        rp->seekTime(mid);
        afterFrames([=]() {
            check(rp->isActive() && !rp->isPlaying(), QStringLiteral("跳转后处于回放暂停状态"));
            check(layer->visibleTrackCount() == expectStarted,
                  QStringLiteral("t=%1 可见端点 %2（期望 %3）").arg(formatBeijingTime(mid))
                      .arg(layer->visibleTrackCount()).arg(expectStarted));
            // 播放 1 秒真实时间（500x）应前进约 500 秒
            rp->setSpeed(500);
            const qint64 before = rp->current();
            rp->play();
            QTimer::singleShot(1000, w, [=]() {
                rp->pause();
                const double gained = (rp->current() - before) / 1000.0;
                check(gained > 350 && gained < 650, QStringLiteral("播放 1 s 前进 %1 s（期望约 500）").arg(gained, 0, 'f', 1));
                // 一次推进到结尾
                rp->play();
                rp->advance(rp->end() - rp->start());
                afterFrames([=]() {
                    check(!rp->isActive() && !rp->isPlaying() && rp->current() == rp->end(),
                          QStringLiteral("到达结尾自动退出回放"));
                    check(layer->visibleTrackCount() == store->size(),
                          QStringLiteral("退出回放后恢复全部端点 %1").arg(layer->visibleTrackCount()));
                    // 再次播放应从头开始
                    rp->play();
                    check(rp->current() == rp->start() && rp->isPlaying(), QStringLiteral("结尾处再播放从头开始"));
                    rp->stop();
                    afterFrames([=]() {
                        check(g->glErrorCount() == 0, QStringLiteral("回放后 OpenGL 错误数 %1").arg(g->glErrorCount()));
                        finish();
                    });
                });
            });
        });
    };

    auto verify = [=]() {
        TrackLayer *layer = g->trackLayer();
        st->out << "layer: segments=" << layer->segmentCount() << " visible=" << layer->visibleTrackCount()
                << " | " << g->debugInfo() << "\n";
        check(g->isGlReady(), QStringLiteral("GL 就绪（地球 + 航迹着色器）"));
        check(store->size() == 2900 + 311 + 311, QStringLiteral("总航迹数 %1（期望 3522）").arg(store->size()));
        check(store->pointCount() == 274081 + 6098 + 3776,
              QStringLiteral("总点数 %1（期望 283955）").arg(store->pointCount()));
        // 下界：相邻两点位置不同的线段数（长线段还会细分，所以实际上传数 >= 下界）
        int minSegs = 0;
        for (const Track &t : store->tracks())
            for (int j = 1; j < t.points.size(); ++j)
                if (t.points[j].lat != t.points[j - 1].lat || t.points[j].lon != t.points[j - 1].lon)
                    ++minSegs;
        check(layer->segmentCount() >= minSegs && layer->segmentCount() < minSegs * 2,
              QStringLiteral("已上传线段 %1（非重复线段 %2）").arg(layer->segmentCount()).arg(minSegs));
        check(layer->visibleTrackCount() == store->size(),
              QStringLiteral("可见端点 %1").arg(layer->visibleTrackCount()));

        // 隐藏 ADS-B 文件组后，可见端点应只剩雷达两组
        const QString adsbKey = QStringLiteral("ADS-B::") + QFileInfo(csv).fileName();
        layer->setGroupVisible(adsbKey, false);
        g->update();
        afterFrames([=]() {
            check(layer->visibleTrackCount() == 622,
                  QStringLiteral("隐藏 ADS-B 后可见端点 %1（期望 622）").arg(layer->visibleTrackCount()));
            layer->setGroupVisible(adsbKey, true);
            g->update();
            afterFrames([=]() {
                check(layer->visibleTrackCount() == store->size(),
                      QStringLiteral("恢复显示后可见端点 %1").arg(layer->visibleTrackCount()));
                testReplay();
            });
        });
    };

    *runNext = [=]() {
        if (*step >= jobs->size()) {
            QObject::disconnect(*conn);
            afterFrames(verify);
            return;
        }
        const Job &j = jobs->at(*step);
        w->importFile(j.kind, j.path);
    };
    *conn = QObject::connect(w, &MainWindow::importFinished, w, [=](bool ok, const QString &msg) {
        const Job &j = jobs->at(*step);
        check(ok, QStringLiteral("导入 %1：%2").arg(trackSourceName(j.kind), msg));
        ++*step;
        QTimer::singleShot(0, *runNext);
    });
    QTimer::singleShot(200, *runNext);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int pt = args.indexOf(QStringLiteral("--parsetest"));
    if (pt >= 0 && pt + 2 < args.size())
        return runParseTest(args.at(pt + 1), args.at(pt + 2));

    MainWindow w;
    w.show();
    w.loadDefaultTiles();

    if (app.arguments().contains(QStringLiteral("--selftest")))
        runSelfTest(&w);
    const int tt = args.indexOf(QStringLiteral("--tracktest"));
    if (tt >= 0 && tt + 2 < args.size())
        runTrackTest(&w, args.at(tt + 1), args.at(tt + 2));

    return app.exec();
}
