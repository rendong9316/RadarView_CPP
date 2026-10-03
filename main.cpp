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
#include "trackpointdialog.h"
#include "appstatusbar.h"
#include "apppaths.h"
#include "uiwidgets.h"
#include "geocalc.h"
#include "managepanel.h"
#include "maptools.h"
#include "sidepanels.h"
#include "trackdb.h"
#include "theme.h"
#include "settingspanel.h"

#include <QMouseEvent>
#include <QDialog>
#include <QWindow>

#include <QFileInfo>
#include <QDir>
#include <QFontDatabase>
#include <memory>

namespace {

// 自检用：丢掉来自窗口系统的鼠标事件，只保留测试代码发出的。
// Enter/Leave 由 QApplication 转发时不是 spontaneous，所以一律拦下，只放行 allowed 指向的那个
class RealMouseBlocker : public QObject
{
public:
    using QObject::QObject;
    QEvent *allowed = nullptr;
    bool eventFilter(QObject *, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::Enter: case QEvent::Leave:
            return e != allowed;
        case QEvent::MouseMove: case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: case QEvent::ContextMenu: case QEvent::Wheel:
            return e->spontaneous();
        default:
            return false;
        }
    }
};

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
    auto afterFrames = [g, st](std::function<void()> next) {
        const quint64 target = g->frameCount() + 3;
        auto poll = std::make_shared<std::function<void()>>();
        QElapsedTimer t;
        t.start();
        *poll = [g, st, target, next, poll, t]() {
            if (g->frameCount() >= target || t.elapsed() > 10000) {
                if (g->frameCount() < target) {
                    QWindow *win = g->window()->windowHandle();
                    st->out << "WAIT timeout: frames " << g->frameCount() << " < " << target
                            << " at " << st->clock.elapsed() << " ms; exposed=" << (win ? win->isExposed() : false)
                            << " visible=" << g->isVisible() << " minimized=" << g->window()->isMinimized()
                            << " active=" << g->window()->isActiveWindow() << "\n";
                }
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

    // 状态栏与回放时钟一致：进度条、时间文字都必须等于 ReplayController 当前值；回放控件任何时候都显示
    AppStatusBar *sb = w->appStatusBar();
    auto statusConsistent = [=](const QString &when) {
        ReplayController *rp = w->replay();
        const bool hasData = rp->end() > rp->start();
        const QString expectTime = hasData ? formatBeijingTime(rp->current()) + QStringLiteral(" / ") + formatBeijingTime(rp->end())
                                           : QString();
        const double expectProg = hasData ? rp->progress() : 0.0;
        QString layout;
        const bool layoutOk = sb->checkLayout(&layout);
        const bool ok = sb->areReplayControlsShown() && sb->isTimeShown() == hasData && sb->timeText() == expectTime
                        && std::fabs(sb->seekProgress() - expectProg) < 1e-9 && layoutOk;
        check(ok, QStringLiteral("状态栏一致（%1）：控件=%2 时间=[%3] 期望=[%4] 进度=%5/%6 布局=%7")
                      .arg(when).arg(sb->areReplayControlsShown()).arg(sb->timeText(), expectTime)
                      .arg(sb->seekProgress(), 0, 'f', 6).arg(expectProg, 0, 'f', 6).arg(layoutOk ? QStringLiteral("OK") : layout));
    };
    statusConsistent(QStringLiteral("无数据"));
    check(sb->trackCountText() == QStringLiteral("航迹: 0") && sb->sourceTexts() == QStringList(QStringLiteral("ADS-B:0")),
          QStringLiteral("无数据时：%1 | %2").arg(sb->trackCountText(), sb->sourceTexts().join(QLatin1Char(' '))));
    check(sb->heightText().startsWith(QStringLiteral("高: ")) && sb->heightText().endsWith(QStringLiteral(" km"))
              && sb->lonLatText().startsWith(QStringLiteral("经纬: ")) && sb->fpsText().startsWith(QStringLiteral("FPS: ")),
          QStringLiteral("视图项：%1 | %2 | %3").arg(sb->heightText(), sb->lonLatText(), sb->fpsText()));

    // 依次导入 ADS-B、雷达、雷达原始量测
    struct Job { TrackSource kind; QString path; int expectTracks; };
    auto jobs = std::make_shared<QVector<Job>>(QVector<Job>{
        { TrackSource::Adsb, csv, 2900 },
        { TrackSource::Radar, mat, 311 },
        { TrackSource::RadarRaw, mat, 311 } });
    auto step = std::make_shared<int>(0);
    auto conn = std::make_shared<QMetaObject::Connection>();
    auto runNext = std::make_shared<std::function<void()>>();
    auto testFeatures = std::make_shared<std::function<void()>>();   // 管理面板 / 入库 / 标签 / 点迹 / 筛选 / 旗标 / 标尺

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
        statusConsistent(QStringLiteral("跳转后"));
        afterFrames([=]() {
            check(rp->isActive() && !rp->isPlaying(), QStringLiteral("跳转后处于回放暂停状态"));
            check(layer->visibleTrackCount() == expectStarted,
                  QStringLiteral("t=%1 可见端点 %2（期望 %3）").arg(formatBeijingTime(mid))
                      .arg(layer->visibleTrackCount()).arg(expectStarted));
            // 播放 1 秒真实时间（500x）应前进约 500 秒
            rp->setSpeed(500);
            check(sb->speedText() == QStringLiteral("500x") && !sb->isCustomSpeedShown(),
                  QStringLiteral("倍速显示 %1").arg(sb->speedText()));
            const qint64 before = rp->current();
            rp->play();
            // 播放中每 100 ms 采样一次：状态栏必须与回放时钟同步
            auto samples = std::make_shared<int>(0);
            auto sampleFails = std::make_shared<int>(0);
            QTimer *sampler = new QTimer(w);
            QObject::connect(sampler, &QTimer::timeout, w, [=]() {
                ++*samples;
                const QString expect = formatBeijingTime(rp->current()) + QStringLiteral(" / ") + formatBeijingTime(rp->end());
                if (!sb->areReplayControlsShown() || sb->timeText() != expect
                        || std::fabs(sb->seekProgress() - rp->progress()) > 1e-9)
                    ++*sampleFails;
            });
            sampler->start(100);
            QTimer::singleShot(1000, w, [=]() {
                sampler->stop();
                sampler->deleteLater();
                check(*samples >= 5 && *sampleFails == 0,
                      QStringLiteral("播放中状态栏与时钟同步：采样 %1 次，不一致 %2 次").arg(*samples).arg(*sampleFails));
                rp->pause();
                statusConsistent(QStringLiteral("暂停后"));
                rp->setSpeed(123);
                check(sb->isCustomSpeedShown() && sb->speedText() == QStringLiteral("自定义... 123"),
                      QStringLiteral("非预设倍速显示自定义输入 %1").arg(sb->speedText()));
                rp->setSpeed(500);
                check(!sb->isCustomSpeedShown(), QStringLiteral("回到预设倍速后收起输入框"));
                const double gained = (rp->current() - before) / 1000.0;
                check(gained > 350 && gained < 650, QStringLiteral("播放 1 s 前进 %1 s（期望约 500）").arg(gained, 0, 'f', 1));
                // 一次推进到结尾
                rp->play();
                rp->advance(rp->end() - rp->start());
                afterFrames([=]() {
                    check(!rp->isActive() && !rp->isPlaying() && rp->current() == rp->end(),
                          QStringLiteral("到达结尾自动退出回放"));
                    statusConsistent(QStringLiteral("播放到结尾"));
                    check(layer->visibleTrackCount() == store->size(),
                          QStringLiteral("退出回放后恢复全部端点 %1").arg(layer->visibleTrackCount()));
                    // 再次播放应从头开始
                    rp->play();
                    check(rp->current() == rp->start() && rp->isPlaying(), QStringLiteral("结尾处再播放从头开始"));
                    rp->stop();
                    statusConsistent(QStringLiteral("停止后"));
                    // 窗口很窄时状态栏不能把主窗口撑大，回放控件仍要显示
                    const QSize oldSize = w->size();
                    w->resize(700, oldSize.height());
                    afterFrames([=]() {
                        statusConsistent(QStringLiteral("窗口宽 %1").arg(w->width()));
                        check(sb->width() <= w->width(), QStringLiteral("状态栏宽 %1 不超过窗口宽 %2").arg(sb->width()).arg(w->width()));
                        w->resize(oldSize);
                        afterFrames([=]() {
                            check(g->glErrorCount() == 0, QStringLiteral("回放后 OpenGL 错误数 %1").arg(g->glErrorCount()));
                            (*testFeatures)();
                        });
                    });
                });
            });
        });
    };

    // 地图交互：拾取、悬停高亮、单击单独显示 / 单击空白返回、点迹表与导出。
    // 自检期间屏蔽真实鼠标（spontaneous 事件），否则桌面上鼠标的实际位置会触发 Leave/Move 干扰结果
    RealMouseBlocker *blocker = new RealMouseBlocker(g);
    g->installEventFilter(blocker);
    auto sendMouse = [g](QEvent::Type type, const QPointF &pos, Qt::MouseButton button) {
        const Qt::MouseButtons buttons = type == QEvent::MouseButtonPress ? Qt::MouseButtons(button) : Qt::NoButton;
        QMouseEvent ev(type, pos, g->mapToGlobal(pos.toPoint()), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(g, &ev);
    };
    auto testInteraction = [=]() {
        TrackLayer *layer = g->trackLayer();
        // 选一条点数较多的 ADS-B 航迹，镜头对准它的中间点
        int ti = -1;
        for (int i = 0; i < store->size() && ti < 0; ++i)
            if (store->tracks()[i].source == TrackSource::Adsb && store->tracks()[i].points.size() >= 100)
                ti = i;
        check(ti >= 0, QStringLiteral("找到测试航迹 %1").arg(ti));
        if (ti < 0) {
            finish();
            return;
        }
        const Track &t = store->tracks()[ti];
        const TrackPoint mid = t.points[t.points.size() / 2];
        g->lookAt(mid.lon, mid.lat, 300.0);
        afterFrames([=]() {
            QPointF sp;
            const bool onScreen = g->screenPos(mid.lon, mid.lat, TrackLayer::kAltitudeM, &sp);
            const QPointF center(g->width() / 2.0, g->height() / 2.0);
            check(onScreen && (sp - center).manhattanLength() < 4.0,
                  QStringLiteral("航迹点投影到屏幕中心 (%1, %2)").arg(sp.x(), 0, 'f', 1).arg(sp.y(), 0, 'f', 1));
            const int hit = g->trackAt(sp);
            check(hit >= 0, QStringLiteral("中心点拾取到航迹 %1").arg(hit));

            // 悬停：移动鼠标到航迹上，下一帧应画出高亮
            sendMouse(QEvent::MouseMove, sp, Qt::NoButton);
            afterFrames([=]() {
                check(layer->hoveredTrack() == hit && layer->hoverDrawn(),
                      QStringLiteral("悬停高亮 hovered=%1 drawn=%2").arg(layer->hoveredTrack()).arg(layer->hoverDrawn()));
                // 单击：单独显示
                sendMouse(QEvent::MouseButtonPress, sp, Qt::LeftButton);
                sendMouse(QEvent::MouseButtonRelease, sp, Qt::LeftButton);
                afterFrames([=]() {
                    if (hit < 0) {               // 前面已记 FAIL；没有命中就不再往下测，避免越界
                        finish();
                        return;
                    }
                    const Track &ht = store->tracks()[hit];
                    check(w->isolatedTrack() == hit && layer->visibleTrackCount() == 1,
                          QStringLiteral("单击后单独显示 %1，可见端点 %2").arg(w->isolatedTrack()).arg(layer->visibleTrackCount()));
                    check(w->replay()->start() == ht.minTime() && w->replay()->end() == ht.maxTime(),
                          QStringLiteral("单独显示时回放范围为该航迹 %1 ~ %2")
                              .arg(formatBeijingTime(w->replay()->start()), formatBeijingTime(w->replay()->end())));
                    check(g->trackAt(sp) == hit, QStringLiteral("单独显示时只能拾取到该航迹"));
                    statusConsistent(QStringLiteral("单独显示"));

                    // 点迹表 + CSV 导出
                    QDialog *dlg = w->showTrackPoints(hit);
                    TrackPointDialog *pd = qobject_cast<TrackPointDialog *>(dlg);
                    check(pd && pd->rowCount() == ht.points.size(),
                          QStringLiteral("点迹表行数 %1（期望 %2）").arg(pd ? pd->rowCount() : -1).arg(ht.points.size()));
                    const QString csvOut = QCoreApplication::applicationDirPath() + QStringLiteral("/points_export_test.csv");
                    QString err;
                    const bool exported = pd && pd->exportCsv(csvOut, &err);
                    QFile f(csvOut);
                    int lines = 0;
                    bool bom = false;
                    if (exported && f.open(QIODevice::ReadOnly)) {
                        const QByteArray all = f.readAll();
                        bom = all.startsWith("\xEF\xBB\xBF");
                        lines = all.count('\n');
                        f.close();
                    }
                    QFile::remove(csvOut);
                    check(exported && bom && lines == ht.points.size() + 1,
                          QStringLiteral("导出 CSV：%1 行（期望 %2），BOM=%3 %4")
                              .arg(lines).arg(ht.points.size() + 1).arg(bom).arg(err));
                    if (dlg)
                        dlg->close();

                    // 单击空白处（屏幕外缘远离该航迹的位置）返回全部
                    QPointF empty(4, 4);
                    for (const QPointF &c : { QPointF(4, 4), QPointF(g->width() - 4.0, 4), QPointF(4, g->height() - 4.0) })
                        if (g->trackAt(c) < 0) {
                            empty = c;
                            break;
                        }
                    sendMouse(QEvent::MouseButtonPress, empty, Qt::LeftButton);
                    sendMouse(QEvent::MouseButtonRelease, empty, Qt::LeftButton);
                    // 鼠标离开地图：悬停应取消（返回全部后 empty 处可能正好压着别的航迹，所以用 Leave 测）
                    QEvent leave(QEvent::Leave);
                    blocker->allowed = &leave;
                    QCoreApplication::sendEvent(g, &leave);
                    blocker->allowed = nullptr;
                    afterFrames([=]() {
                        check(w->isolatedTrack() < 0 && layer->visibleTrackCount() == store->size(),
                              QStringLiteral("单击空白处返回全部，可见端点 %1").arg(layer->visibleTrackCount()));
                        check(layer->hoveredTrack() < 0, QStringLiteral("移开后取消悬停"));
                        // 拖动不应触发单击
                        sendMouse(QEvent::MouseButtonPress, sp, Qt::LeftButton);
                        sendMouse(QEvent::MouseMove, sp + QPointF(60, 0), Qt::NoButton);
                        sendMouse(QEvent::MouseButtonRelease, sp + QPointF(60, 0), Qt::LeftButton);
                        check(w->isolatedTrack() < 0, QStringLiteral("拖动不触发单独显示"));
                        check(g->glErrorCount() == 0, QStringLiteral("交互后 OpenGL 错误数 %1").arg(g->glErrorCount()));
                        testReplay();
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
        // 拾取耗时（全部航迹都在视野内时最慢；每次鼠标移动都会做一次）
        {
            QElapsedTimer pt;
            pt.start();
            const int n = 20;
            for (int k = 0; k < n; ++k)
                g->trackAt(QPointF(g->width() * (k + 1) / (n + 1.0), g->height() / 2.0));
            const double perPick = pt.nsecsElapsed() / 1e6 / n;
            check(perPick < 30.0, QStringLiteral("全视野拾取平均 %1 ms").arg(perPick, 0, 'f', 2));
        }

        // 状态栏数据源项：ADS-B 整体一项，雷达 / 原始量测按文件各一项（与 RadarView 一致）
        const QStringList expectSources = { QStringLiteral("ADS-B:2900"), QStringLiteral("Radar:311"), QStringLiteral("RadarRaw:311") };
        check(sb->sourceTexts() == expectSources && sb->trackCountText() == QStringLiteral("航迹: 3522"),
              QStringLiteral("状态栏数据源 %1 | %2").arg(sb->sourceTexts().join(QLatin1Char(' ')), sb->trackCountText()));
        check(sb->errorText().isEmpty(), QStringLiteral("没有错误提示"));

        // 点击状态栏 ADS-B 项隐藏，可见端点应只剩雷达两组；再点一次恢复
        sb->clickSource(0);
        afterFrames([=]() {
            check(layer->visibleTrackCount() == 622 && sb->sourceTexts().value(0) == QStringLiteral("off ADS-B:2900"),
                  QStringLiteral("点击状态栏隐藏 ADS-B 后可见端点 %1（期望 622），%2")
                      .arg(layer->visibleTrackCount()).arg(sb->sourceTexts().value(0)));
            sb->clickSource(1);
            afterFrames([=]() {
                check(layer->visibleTrackCount() == 311, QStringLiteral("再隐藏 Radar 后可见端点 %1（期望 311）").arg(layer->visibleTrackCount()));
                sb->clickSource(0);
                sb->clickSource(1);
                afterFrames([=]() {
                    check(layer->visibleTrackCount() == store->size() && sb->sourceTexts() == expectSources,
                          QStringLiteral("恢复显示后可见端点 %1").arg(layer->visibleTrackCount()));
                    // 主题循环 dark -> light -> hc -> dark
                    const QString t0 = sb->theme().id;
                    sb->clickTheme();
                    const QString t1 = sb->theme().id;
                    sb->clickTheme();
                    const QString t2 = sb->theme().id;
                    sb->clickTheme();
                    const QStringList order = { QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("hc") };
                    const int i0 = order.indexOf(t0);
                    check(i0 >= 0 && t1 == order[(i0 + 1) % 3] && t2 == order[(i0 + 2) % 3] && sb->theme().id == t0,
                          QStringLiteral("主题切换 %1 -> %2 -> %3 -> %4").arg(t0, t1, t2, sb->theme().id));
                    testInteraction();
                });
            });
        });
    };

    // ---- 新功能：入库与启动加载、管理面板、标签、点迹、筛选、旗标、标尺 ----
    *testFeatures = [=]() {
        // 等后台入库结束（最多 60 秒）
        auto waitPersist = std::make_shared<std::function<void(std::function<void()>)>>();
        *waitPersist = [w](std::function<void()> next) {
            QElapsedTimer t;
            t.start();
            auto poll = std::make_shared<std::function<void()>>();
            *poll = [w, t, next, poll]() {
                if (!w->isPersisting() || t.elapsed() > 60000) {
                    next();
                    return;
                }
                QTimer::singleShot(50, *poll);
            };
            QTimer::singleShot(0, *poll);
        };
        (*waitPersist)([=]() {
            TrackLayer *layer = g->trackLayer();
            check(!w->isPersisting() && sb->loadingText().isEmpty(), QStringLiteral("后台入库完成，状态栏不再显示保存中"));
            const ManageStats ds = trackdb::stats();
            check(ds.totalTracks == 3522 && ds.totalBatches == 3,
                  QStringLiteral("数据库：%1 条航迹，%2 个批次（期望 3522 / 3）").arg(ds.totalTracks).arg(ds.totalBatches));
            // 启动加载：从数据库读回的内容与内存一致
            const QVector<Track> back = trackdb::loadAll();
            qint64 pts = 0;
            for (const Track &t : back)
                pts += t.points.size();
            check(back.size() == store->size() && pts == store->pointCount(),
                  QStringLiteral("数据库读回 %1 条 %2 点（内存 %3 条 %4 点）").arg(back.size()).arg(pts).arg(store->size()).arg(store->pointCount()));
            // 同一数据源的不同文件颜色互不相同：Radar 与 RadarRaw 来自同一个 .mat，再导入一个 Radar 文件
            const QColor c1 = store->fileColor(TrackSource::Radar, QStringLiteral("Radar"));
            TrackStore probe;
            probe.setFileColors(store->fileColors());
            QVector<Track> extra;
            Track et;
            et.id = QStringLiteral("X");
            et.source = TrackSource::Radar;
            et.fileName = QStringLiteral("Radar2");
            TrackPoint tp;
            tp.t = 1;
            et.points << tp;
            extra << et;
            et.fileName = QStringLiteral("Radar3");
            extra << et;
            probe.addTracks(extra);
            const QColor c2 = probe.fileColor(TrackSource::Radar, QStringLiteral("Radar2"));
            const QColor c3 = probe.fileColor(TrackSource::Radar, QStringLiteral("Radar3"));
            check(c1 != c2 && c2 != c3 && c1 != c3, QStringLiteral("同源文件轮换颜色 %1 / %2 / %3").arg(c1.name(), c2.name(), c3.name()));

            // 管理面板：统计、分页、搜索、排序
            w->activatePanel(PanelId::Manage);
            ManagePanel *mp = w->managePanel();
            ManageState *ms = w->manageState();
            check(w->activePanel() == int(PanelId::Manage) && mp->totalCount() == 3522 && mp->rowCount() == 100,
                  QStringLiteral("管理面板 共 %1 条，本页 %2 行，%3").arg(mp->totalCount()).arg(mp->rowCount()).arg(mp->pageText()));
            check(mp->statsText().startsWith(QStringLiteral("总计 3522 条 | ADS-B 2900 雷达 311 原始 311 | ")),
                  QStringLiteral("统计栏：%1").arg(mp->statsText()));
            const QString icao = mp->rows().value(0).icao;
            mp->setSearchText(icao.toLower());
            bool allMatch = mp->totalCount() >= 1;
            for (const ManageRow &r : mp->rows())
                allMatch = allMatch && (r.icao.contains(icao, Qt::CaseInsensitive) || r.flightNo.contains(icao, Qt::CaseInsensitive)
                                        || r.registration.contains(icao, Qt::CaseInsensitive));
            check(allMatch, QStringLiteral("搜索 %1：匹配 %2 条").arg(icao).arg(mp->totalCount()));
            // 与 ManageDataTable.vue 相同：匹配总数为 0 时显示「暂无航迹数据」，有匹配但本页为空才是「未找到匹配」
            mp->setSearchText(QStringLiteral("没有这条航迹zz"));
            check(mp->totalCount() == 0 && mp->emptyText() == QStringLiteral("暂无航迹数据，请先导入文件"),
                  QStringLiteral("无匹配提示：%1").arg(mp->emptyText()));
            mp->clickResetFilters();
            mp->clickSort(ManageModel::Pts);
            bool asc = mp->rowCount() == 100;
            for (int i = 1; i < mp->rows().size(); ++i)
                asc = asc && mp->rows()[i - 1].pointCount <= mp->rows()[i].pointCount;
            mp->clickSort(ManageModel::Pts);
            bool desc = mp->rows().size() > 1 && mp->rows()[0].pointCount >= mp->rows()[1].pointCount;
            check(asc && desc, QStringLiteral("点数列排序 升序=%1 降序=%2").arg(asc).arg(desc));

            // 勾选显示：地图只显示可见集合，回放范围跟随
            const ManageRow r0 = mp->rows()[0];
            mp->clickEye(0);
            const int ti0 = store->indexOf(r0.trackKey());
            check(w->displayedTrackCount() == 1 && ti0 >= 0
                      && w->replay()->start() == store->tracks()[ti0].minTime() && w->replay()->end() == store->tracks()[ti0].maxTime(),
                  QStringLiteral("勾选 1 条后地图显示 %1 条，回放范围为该航迹").arg(w->displayedTrackCount()));
            check(mp->toolbarText() == QStringLiteral("匹配 3522 条 · 本页 100 条 · 地图可见 1 条"), mp->toolbarText());
            mp->clickClearMap();
            check(w->displayedTrackCount() == 3522, QStringLiteral("清空地图后显示 %1 条").arg(w->displayedTrackCount()));

            // 删除（软删除）与撤销
            const QString delKey = mp->rows()[0].trackKey();
            mp->deleteRow(0);
            check(store->size() == 3521 && store->indexOf(delKey) < 0 && mp->totalCount() == 3521
                      && w->undoToast()->text().startsWith(QStringLiteral("已删除")),
                  QStringLiteral("删除后内存 %1 条，面板 %2 条，提示 [%3]").arg(store->size()).arg(mp->totalCount()).arg(w->undoToast()->text()));
            check(trackdb::loadAll().size() == 3521, QStringLiteral("删除的航迹下次启动不加载"));
            ms->undoDelete();
            check(store->size() == 3522 && store->indexOf(delKey) >= 0 && mp->totalCount() == 3522
                      && ms->isVisible(delKey) && w->undoToast()->text().isEmpty(),
                  QStringLiteral("撤销后恢复 %1 条，并加入地图可见集合").arg(store->size()));
            ms->clearVisible();
            mp->clickResetFilters();

            // 标签
            w->toggleLabels();
            afterFrames([=]() {
                check(g->showLabels() && g->labelsDrawn() > 0, QStringLiteral("标签显示 %1 个").arg(g->labelsDrawn()));
                check(GlobeWidget::trackLabel(store->tracks()[0]).size() > 0, QStringLiteral("标签文字 %1").arg(GlobeWidget::trackLabel(store->tracks()[0])));
                w->toggleLabels();
                // 点迹：选一条点数较多的 ADS-B 航迹
                int ti = -1;
                for (int i = 0; i < store->size() && ti < 0; ++i)
                    if (store->tracks()[i].source == TrackSource::Adsb && store->tracks()[i].points.size() >= 100)
                        ti = i;
                const Track &t = store->tracks()[ti];
                w->togglePointDots(ti);
                w->isolateTrack(ti);     // 单独显示，保证鼠标处拾取到的就是这条（ADS-B 航迹很密）
                const TrackPoint mid = t.points[t.points.size() / 2];
                g->lookAt(mid.lon, mid.lat, 300.0);
                afterFrames([=]() {
                    check(w->pointDotsShown(ti) && layer->pointDotCount() == store->tracks()[ti].points.size(),
                          QStringLiteral("点迹 %1 个（期望 %2）").arg(layer->pointDotCount()).arg(store->tracks()[ti].points.size()));
                    QPointF sp;
                    g->screenPos(mid.lon, mid.lat, TrackLayer::kAltitudeM, &sp);
                    sendMouse(QEvent::MouseMove, sp, Qt::NoButton);
                    int ht = -1, hp = -1;
                    g->pointHover(&ht, &hp);
                    check(ht == ti && hp >= 0, QStringLiteral("悬停点迹 航迹 %1 点 %2").arg(ht).arg(hp));
                    w->togglePointDots(ti);
                    w->isolateTrack(-1);
                    afterFrames([=]() {
                        check(!w->pointDotsShown(ti) && layer->pointDotCount() == 0, QStringLiteral("隐藏点迹"));
                        // 时间筛选
                        FilterPanel *fp = w->filterPanel();
                        const qint64 midT = 1777253315000LL + (1777282200000LL - 1777253315000LL) / 2;
                        const qint64 lo = midT / 60000 * 60000 - 1800000, hi = lo + 3600000;
                        int expect = 0;
                        for (const Track &tr : store->tracks()) {
                            bool any = false;
                            for (const TrackPoint &p : tr.points)
                                if (p.t >= lo && p.t <= hi) {
                                    any = true;
                                    break;
                                }
                            expect += any;
                        }
                        fp->setInputs(hi, lo);
                        fp->clickApply();
                        check(fp->errorText() == QStringLiteral("起始时间必须早于结束时间"), QStringLiteral("时间顺序校验：%1").arg(fp->errorText()));
                        fp->setInputs(lo, hi);
                        fp->clickApply();
                        check(w->displayedTrackCount() == expect && w->replay()->start() >= lo && w->replay()->end() <= hi,
                              QStringLiteral("时间筛选后显示 %1 条（期望 %2），回放 %3 ~ %4").arg(w->displayedTrackCount()).arg(expect)
                                  .arg(formatBeijingTime(w->replay()->start()), formatBeijingTime(w->replay()->end())));
                        fp->clickClear();
                        // 点数筛选（按原始总点数）
                        int expectPc = 0;
                        for (const Track &tr : store->tracks())
                            expectPc += tr.source != TrackSource::Adsb || tr.points.size() >= 100;
                        fp->setPointCount(TrackSource::Adsb, true, 100, -1);
                        check(w->displayedTrackCount() == expectPc,
                              QStringLiteral("ADS-B ≥100 点筛选后显示 %1 条（期望 %2）").arg(w->displayedTrackCount()).arg(expectPc));
                        fp->setPointCount(TrackSource::Adsb, false, -1, -1);
                        check(w->displayedTrackCount() == 3522, QStringLiteral("取消点数筛选后显示 %1 条").arg(w->displayedTrackCount()));
                        afterFrames([=]() {
                            check(layer->visibleTrackCount() == 3522, QStringLiteral("筛选清除后端点 %1").arg(layer->visibleTrackCount()));
                            // 旗标：双击放置 / 双击删除；勾选两个测距
                            FlagStore *fs = w->flagStore();
                            const QPointF c(g->width() / 2.0, g->height() / 2.0);
                            sendMouse(QEvent::MouseButtonDblClick, c, Qt::LeftButton);
                            check(fs->flags().size() == 1 && fs->flags()[0].label == QStringLiteral("旗标 1"),
                                  QStringLiteral("双击放置旗标 %1 个").arg(fs->flags().size()));
                            fs->addFlag(mid.lat + 0.5, mid.lon + 0.5);
                            check(fs->flags().size() == 2 && fs->flags()[1].label == QStringLiteral("旗标 2"), QStringLiteral("第二个旗标编号"));
                            fs->toggleSelect(fs->flags()[0].id);
                            fs->toggleSelect(fs->flags()[1].id);
                            const MapFlag a = fs->flags()[0], b = fs->flags()[1];
                            const double km = geocalc::vincentyKm(a.lat, a.lon, b.lat, b.lon);
                            check(w->flagPanel()->geoText().startsWith(QStringLiteral("距离: %1 km").arg(km, 0, 'f', 1)),
                                  QStringLiteral("两旗标测距：%1").arg(w->flagPanel()->geoText().replace(QLatin1Char('\n'), QLatin1Char(' '))));
                            w->flagPanel()->placeFlag(QStringLiteral("95"), QStringLiteral("100"));
                            check(w->flagPanel()->coordError() == QStringLiteral("纬度范围 -90 ~ 90"), QStringLiteral("纬度校验"));
                            afterFrames([=]() {
                                check(g->flagsDrawn() == 2, QStringLiteral("地图上画出旗标 %1 个").arg(g->flagsDrawn()));
                                sendMouse(QEvent::MouseButtonDblClick, c + QPointF(0, -10), Qt::LeftButton);
                                check(w->flagStore()->flags().size() == 1, QStringLiteral("双击旗标删除，剩 %1 个").arg(w->flagStore()->flags().size()));
                                w->flagStore()->clearAll();
                                // 标尺：单击放两个航点
                                RulerState *rs = w->ruler();
                                rs->toggle();
                                const QPointF p1 = c + QPointF(-100, 0), p2 = c + QPointF(100, 60);
                                sendMouse(QEvent::MouseButtonPress, p1, Qt::LeftButton);
                                sendMouse(QEvent::MouseButtonRelease, p1, Qt::LeftButton);
                                sendMouse(QEvent::MouseButtonPress, p2, Qt::LeftButton);
                                sendMouse(QEvent::MouseButtonRelease, p2, Qt::LeftButton);
                                check(rs->isActive() && rs->waypoints().size() == 2 && w->isolatedTrack() < 0,
                                      QStringLiteral("标尺航点 %1 个，单击不选航迹").arg(rs->waypoints().size()));
                                const double d = rs->segments().value(0).distanceKm;
                                check(w->flagPanel()->rulerTotalText() == formatRulerDistance(d) && w->flagPanel()->segmentTexts().size() == 1,
                                      QStringLiteral("标尺总距离 %1，%2").arg(w->flagPanel()->rulerTotalText(), w->flagPanel()->segmentTexts().value(0)));
                                afterFrames([=]() {
                                    check(g->rulerMarkersDrawn() == 2, QStringLiteral("地图上画出航点 %1 个").arg(g->rulerMarkersDrawn()));
                                    w->onEscape();
                                    check(!w->ruler()->isActive() && w->ruler()->waypoints().isEmpty(), QStringLiteral("Esc 关闭标尺"));
                                    // 设置面板：字号全局缩放（10–20 钳制），回到 14 后 rem 换算还原
                                    w->activatePanel(PanelId::Settings);
                                    SettingsPanel *sp = w->settingsPanel();
                                    check(int(PanelId::Count) == 4 && w->activePanel() == int(PanelId::Settings),
                                          QStringLiteral("侧栏面板 %1 个，设置面板已打开").arg(int(PanelId::Count)));
                                    sp->setFontValue(18);
                                    check(UiScale::instance()->basePx() == 18 && ui::px(1.0) == 18 && ui::px(0.786) == 14,
                                          QStringLiteral("字号 18：1rem=%1px 0.786rem=%2px").arg(ui::px(1.0)).arg(ui::px(0.786)));
                                    check(Theme::instance()->qss(QStringLiteral("a { font-size: 11px; }")).contains(QStringLiteral("font-size: 14px")),
                                          QStringLiteral("QSS 字号随根字号缩放"));
                                    sp->setFontValue(60);
                                    check(UiScale::instance()->basePx() == ui::kFontMaxPx,
                                          QStringLiteral("字号上限钳制为 %1").arg(UiScale::instance()->basePx()));
                                    QCoreApplication::processEvents();
                                    QCoreApplication::processEvents();
                                    {
                                        QString layout;
                                        const bool lok = w->appStatusBar()->checkLayout(&layout);
                                        check(lok, QStringLiteral("字号 50 时状态栏各项不重叠：%1").arg(layout));
                                    }
                                    if (qEnvironmentVariableIsSet("RV_SHOT")) {
                                        auto settle = []() {
                                            QElapsedTimer t;
                                            t.start();
                                            while (t.elapsed() < 500)
                                                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                                        };
                                        const QString dir = QCoreApplication::applicationDirPath();
                                        w->resize(1600, 1000);
                                        w->activatePanel(PanelId::Manage);
                                        settle();
                                        w->grab().save(dir + QStringLiteral("/shot50_manage.png"));
                                        w->activatePanel(PanelId::Settings);
                                        settle();
                                        w->grab().save(dir + QStringLiteral("/shot50_settings.png"));
                                        sp->setFontValue(14);
                                        w->activatePanel(PanelId::Manage);
                                        settle();
                                        w->grab().save(dir + QStringLiteral("/shot14_manage.png"));
                                    }
                                    sp->setFontValue(14);
                                    check(ui::px(0.786) == 11, QStringLiteral("字号还原 14：0.786rem=%1px").arg(ui::px(0.786)));
                                    // 批量数据管理：按批次从数据库硬删除 Radar 批次
                                    qint64 radarId = -1;
                                    int radarCount = 0;
                                    for (const BatchInfo &b : trackdb::batches())
                                        if (b.source == TrackSource::Radar) {
                                            radarId = b.id;
                                            radarCount = b.trackCount;
                                        }
                                    const int before = store->size();
                                    QString derr;
                                    const bool delOk = radarId >= 0 && w->manageState()->hardDeleteBatch(radarId, &derr);
                                    const ManageStats after = trackdb::stats();
                                    int radarLeft = 0;
                                    for (const Track &tr : store->tracks())
                                        radarLeft += tr.source == TrackSource::Radar;
                                    check(delOk && after.totalBatches == 2 && after.totalTracks == 3522 - radarCount
                                              && store->size() == before - radarCount && radarLeft == 0
                                              && trackdb::loadBatchTracks(radarId).isEmpty(),
                                          QStringLiteral("硬删除 Radar 批次 %1 条：库内剩 %2 条 / %3 批次，内存 %4 条 %5")
                                              .arg(radarCount).arg(after.totalTracks).arg(after.totalBatches).arg(store->size()).arg(derr));
                                    check(g->tileName().contains(QStringLiteral("Natural Earth")),
                                          QStringLiteral("默认瓦片源 natural_earth4：%1").arg(g->tileName()));
                                    afterFrames([=]() {
                                        check(g->glErrorCount() == 0, QStringLiteral("新功能测试后 OpenGL 错误数 %1").arg(g->glErrorCount()));
                                        finish();
                                    });
                                });
                            });
                        });
                    });
                });
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
        check(sb->isLoadingShown(), QStringLiteral("导入 %1 时显示进度").arg(trackSourceName(j.kind)));
        statusConsistent(QStringLiteral("导入中"));
    };
    *conn = QObject::connect(w, &MainWindow::importFinished, w, [=](bool ok, const QString &msg) {
        const Job &j = jobs->at(*step);
        check(ok, QStringLiteral("导入 %1：%2").arg(trackSourceName(j.kind), msg));
        // 解析完成后进度百分比消失；后台入库期间显示「保存中」（StatusBar.vue persisting）
        check(sb->loadingText() == (w->isPersisting() ? QStringLiteral("保存中") : QString()),
              QStringLiteral("导入完成后进度项 [%1]（入库中=%2）").arg(sb->loadingText()).arg(w->isPersisting()));
        statusConsistent(QStringLiteral("导入 %1 后").arg(trackSourceName(j.kind)));
        ++*step;
        QTimer::singleShot(0, *runNext);
    });
    QTimer::singleShot(200, *runNext);
}

// 加载 exe 旁 fonts/ 目录里的字体。Linux 上把它设为界面默认字体，保证中文能显示；
// Windows 自带雅黑，只注册不替换
void loadBundledFonts()
{
    const QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/fonts"));
    QStringList families;
    const QFileInfoList files = dir.entryInfoList(
        QStringList() << QStringLiteral("*.ttf") << QStringLiteral("*.ttc") << QStringLiteral("*.otf"), QDir::Files);
    for (const QFileInfo &fi : files) {
        const int id = QFontDatabase::addApplicationFont(fi.absoluteFilePath());
        if (id >= 0)
            families += QFontDatabase::applicationFontFamilies(id);
    }
#ifdef Q_OS_LINUX
    if (!families.isEmpty()) {
        QFont f = QApplication::font();
        f.setFamily(families.first());
        QApplication::setFont(f);
    }
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    loadBundledFonts();
    const QStringList args = app.arguments();
    const int pt = args.indexOf(QStringLiteral("--parsetest"));
    if (pt >= 0 && pt + 2 < args.size())
        return runParseTest(args.at(pt + 1), args.at(pt + 2));

    // 自检：数据库和设置用 exe 旁的临时文件（启动时清空），确认框自动通过
    const bool testRun = args.contains(QStringLiteral("--selftest")) || args.contains(QStringLiteral("--tracktest"));
    if (testRun) {
        app::setTestMode(true);
        ui::setAutoConfirm(true);
    }
    UiScale::instance()->applyAppFont();   // 按保存的根字号缩放默认字体（自检时读的是临时设置）

    MainWindow w;
    // 启动自动最大化（保留任务栏）；自检/tracktest 保持普通窗口，便于断言布局
    if (testRun)
        w.show();
    else
        w.showMaximized();
    w.loadDefaultTiles();

    if (app.arguments().contains(QStringLiteral("--selftest")))
        runSelfTest(&w);
    const int tt = args.indexOf(QStringLiteral("--tracktest"));
    if (tt >= 0 && tt + 2 < args.size())
        runTrackTest(&w, args.at(tt + 1), args.at(tt + 2));

    return app.exec();
}
