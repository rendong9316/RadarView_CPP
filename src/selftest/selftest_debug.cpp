#include "selftest.h"

#include "apppaths.h"
#include "appstatusbar.h"
#include "diagnosticspanel.h"
#include "globewidget.h"
#include "mainwindow.h"
#include "point3dwindow.h"
#include "radarsite.h"
#include "replaycontroller.h"
#include "track.h"
#include "trackanalysis.h"
#include "trackanalysiswindow.h"
#include "trackdiag.h"
#include "tracklayer.h"
#include "trackparsers.h"
#include "trackpointdialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>
#include <QWindow>
#include <cmath>
#include <functional>
#include <memory>

namespace selftest {

// 调试态势专项自检：RadarView.exe --debugtest <track.mat> <detPointDir> [adsb.csv]
// 使用真实 data/ 数据验证 Phase 1/2/3 新增能力，日志写 debugtest.log
void runDebugTest(MainWindow *w, const QString &mat, const QString &detDir, const QString &adsbCsv)
{
    Q_UNUSED(adsbCsv);
    struct State {
        QFile log;
        QTextStream out;
        int failures = 0;
        QElapsedTimer clock;
    };
    auto st = std::make_shared<State>();
    st->log.setFileName(QCoreApplication::applicationDirPath() + QStringLiteral("/debugtest.log"));
    st->log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    st->out.setDevice(&st->log);
    st->out.setCodec("UTF-8");
    st->clock.start();
    w->setProperty("noDialogs", true);

    GlobeWidget *g = w->globe();
    TrackStore *store = w->trackStore();
    ReplayController *rp = w->replay();
    AppStatusBar *sb = w->appStatusBar();

    auto check = [st](bool ok, const QString &what) {
        st->out << (ok ? "PASS " : "FAIL ") << what << "\n";
        st->out.flush();
        if (!ok)
            ++st->failures;
    };
    auto afterFrames = [g, st](std::function<void()> next) {
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

    // 1. 导入真实雷达数据 track_20251210175946.mat
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = QObject::connect(w, &MainWindow::importFinished, w, [=](bool ok, const QString &msg) {
        QObject::disconnect(*conn);
        check(ok && store->size() > 0, QStringLiteral("导入雷达 MAT：%1（%2 条）").arg(msg).arg(store->size()));
        if (!ok || store->size() < 2) {
            finish();
            return;
        }
        afterFrames([=]() {
            TrackLayer *layer = g->trackLayer();

            // 2. Phase 1: 按批号分色 vs 按文件分色（默认按文件分色）
            check(w->trackColorMode() == TrackLayer::TrackColorMode::ByFile,
                  QStringLiteral("默认着色模式为按数据源文件"));
            const QColor fileCol0 = layer->trackColor(0);
            const QColor fileCol1 = layer->trackColor(1);
            check(fileCol0 == fileCol1, QStringLiteral("按数据源文件模式同文件航迹同色"));
            w->setTrackColorMode(TrackLayer::TrackColorMode::ByTrack);
            const QColor batchCol0 = layer->trackColor(0);
            const QColor batchCol1 = layer->trackColor(1);
            check(batchCol0 != batchCol1, QStringLiteral("按批号分色模式不同航迹颜色区分：%1 vs %2")
                                              .arg(batchCol0.name(), batchCol1.name()));

            // 3. Phase 1: Ctrl+多选对比隔离 + 缩放至航迹
            w->toggleIsolatedTrack(0);
            w->toggleIsolatedTrack(1);
            check(w->isolatedTracks().size() == 2 && w->isolatedTrack() == -1,
                  QStringLiteral("多选隔离 2 条航迹生效"));
            afterFrames([=]() {
                check(layer->visibleTrackCount() == 2,
                      QStringLiteral("多选隔离渲染可见航迹数=%1（期望 2）").arg(layer->visibleTrackCount()));
                w->zoomToTrack(0);
                double camLon = 0, camLat = 0, camAlt = 0;
                g->cameraView(&camLon, &camLat, &camAlt);
                check(camAlt > 0.0 && camAlt < 4000.0,
                      QStringLiteral("缩放至航迹相机视高=%1 km").arg(camAlt, 0, 'f', 1));

                // 4. Phase 1: 回放尾迹时长 + 逐点步进 + A-B 循环
                rp->setTrailSeconds(60.0);
                check(qFuzzyCompare(rp->trailSeconds(), 60.0), QStringLiteral("尾迹时长设置 60s"));
                rp->seekTime(rp->start());
                const qint64 t0 = rp->current();
                w->stepReplay(+1);
                const qint64 t1 = rp->current();
                check(t1 > t0, QStringLiteral("单步前进：%1 -> %2").arg(t0).arg(t1));
                w->stepReplay(-1);
                check(rp->current() == t0, QStringLiteral("单步后退还原至 %1").arg(rp->current()));
                const qint64 loopA = rp->start() + 30000;
                const qint64 loopB = rp->start() + 90000;
                rp->setLoopRange(true, loopA, loopB);
                rp->seekTime(loopB - 1000);
                rp->setSpeed(1.0);
                rp->advance(2500);
                check(rp->isActive() && rp->current() >= loopA && rp->current() < loopB,
                      QStringLiteral("A-B 循环越界回卷至区间内：%1 in [%2,%3]").arg(rp->current()).arg(loopA).arg(loopB));
                rp->clearLoop();
                rp->setTrailSeconds(0.0);

                // 5. Phase 1: 点迹表双击定位地图 + 准星 + 回放跳转
                QDialog *dlg = w->showTrackPoints(0);
                TrackPointDialog *tpd = qobject_cast<TrackPointDialog *>(dlg);
                check(tpd != nullptr && tpd->rowCount() > 0, QStringLiteral("打开点迹数据表"));
                if (tpd && tpd->rowCount() > 2) {
                    tpd->activateRow(2);
                    check(g->hasFocusPoint(), QStringLiteral("点迹表双击行设置地图聚焦准星"));
                    check(rp->isActive(), QStringLiteral("点迹表双击行同步回放时刻"));
                    tpd->close();
                }

                // 6. Phase 1: 点迹三维窗口可选「同步主时钟」
                w->isolateTrack(-1);
                w->setTrackColorMode(TrackLayer::TrackColorMode::ByFile);
                QString layoutRep;
                check(sb->checkLayout(&layoutRep),
                      QStringLiteral("状态栏布局校验：%1").arg(layoutRep.isEmpty() ? QStringLiteral("OK") : layoutRep));

                QDir dir(detDir);
                QStringList detFiles = dir.entryList(QStringList() << QStringLiteral("detPoint_*.mat"),
                                                     QDir::Files, QDir::Name);
                for (QString &f : detFiles)
                    f = dir.filePath(f);
                if (detFiles.isEmpty()) {
                    check(false, QStringLiteral("未找到 DetPoint 文件：%1").arg(detDir));
                    finish();
                    return;
                }
                w->showPoint3dWindow();
                Point3dWindow *pwin = w->point3dWindows().isEmpty() ? nullptr : w->point3dWindows().last();
                check(pwin != nullptr && !pwin->isSyncMasterClock(),
                      QStringLiteral("新建点迹三维窗口默认关闭主时钟同步"));
                if (!pwin) {
                    finish();
                    return;
                }
                pwin->loadDetectionFiles(detFiles);
                auto pollLoad = std::make_shared<QTimer>();
                pollLoad->setInterval(100);
                QObject::connect(pollLoad.get(), &QTimer::timeout, w, [=]() {
                    if (pwin->isLoading())
                        return;
                    pollLoad->stop();
                    check(pwin->frameCount() > 5,
                          QStringLiteral("点迹窗口加载完成：%1 帧").arg(pwin->frameCount()));
                    // 未勾选同步时，主地图跳转不应改变点迹窗口当前帧
                    pwin->seekToFrame(0);
                    const qint64 midTime = pwin->dataset()->frames.at(pwin->frameCount() / 2).timeMs;
                    rp->seekTime(midTime);
                    check(pwin->currentFrame() == 0,
                          QStringLiteral("关闭同步时主地图跳转不影响点迹窗口帧=%1").arg(pwin->currentFrame()));
                    // 开启同步后，主地图跳转应驱动点迹窗口切到对应帧
                    pwin->setSyncMasterClock(true);
                    rp->seekTime(midTime);
                    check(pwin->currentFrame() == pwin->frameCount() / 2,
                          QStringLiteral("开启同步后点迹窗口同步到帧=%1（期望 %2）")
                              .arg(pwin->currentFrame()).arg(pwin->frameCount() / 2));
                    // 反向：点迹窗口手动切帧同步主地图
                    const int targetF = qMin(pwin->frameCount() - 1, pwin->frameCount() / 2 + 3);
                    pwin->seekToFrame(targetF);
                    check(rp->current() == pwin->dataset()->frames.at(targetF).timeMs,
                          QStringLiteral("点迹窗口手动切帧反向驱动主地图时钟"));
                    // 保留点迹窗口以便 Phase 4 诊断面板联动 60 帧 DetPoint，先关闭主时钟同步避免干扰 Phase 2/3
                    pwin->setSyncMasterClock(false);

                    // 7. Phase 2: 滤波残差与两航迹定量误差对比
                    const Track &tr0 = store->tracks().at(0);
                    const QVector<trackanalysis::FilterResidualPoint> res = trackanalysis::computeFilterResiduals(tr0);
                    check(!res.isEmpty() && res.first().hasPred && std::isfinite(res.first().predGeoErrKm),
                          QStringLiteral("Phase 2 滤波残差计算：航迹 %1 得 %2 帧新息（首帧水平误差 %3 km）")
                              .arg(tr0.id).arg(res.size())
                              .arg(res.isEmpty() ? -1.0 : res.first().predGeoErrKm, 0, 'f', 3));

                    const trackanalysis::TrackCompareResult cmpSP =
                        trackanalysis::compareTracks(tr0.points, tr0.predPoints, 30000);
                    check(cmpSP.timeOverlapped && cmpSP.geoDistKm.valid() && cmpSP.samples.size() > 5,
                          QStringLiteral("Phase 2 同航迹平滑 vs 预测对比：N=%1 RMSE=%2 km Max=%3 km")
                              .arg(cmpSP.samples.size())
                              .arg(cmpSP.geoDistKm.rmse, 0, 'f', 3)
                              .arg(cmpSP.geoDistKm.maxAbs, 0, 'f', 3));

                    // 无重叠时间窗安全处理
                    QVector<TrackPoint> shifted = tr0.points;
                    for (TrackPoint &pt : shifted)
                        pt.t += 86400LL * 1000LL * 30LL;
                    const trackanalysis::TrackCompareResult cmpNone = trackanalysis::compareTracks(tr0.points, shifted, 30000);
                    check(!cmpNone.timeOverlapped && cmpNone.samples.isEmpty(),
                          QStringLiteral("Phase 2 非重叠时间窗安全返回空对比"));

                    // 开启滤波叠加对比并渲染验证
                    w->isolateTrack(0);
                    w->zoomToTrack(0);
                    rp->stop();
                    w->setFilterOverlayEnabled(true);
                    TrackAnalysisWindow *awin = w->showTrackAnalysis(0, 0);
                    check(awin != nullptr && awin->seriesChartCount() >= 4,
                          QStringLiteral("Phase 2 打开时序分析窗口并生成 %1 张曲线图")
                              .arg(awin ? awin->seriesChartCount() : 0));
                    if (awin) {
                        awin->openForTrack(0, 1);
                        check(awin->lastCompareResult().samples.size() > 0,
                              QStringLiteral("Phase 2 误差对比页生成配对样本 %1 条")
                                  .arg(awin->lastCompareResult().samples.size()));
                        QTemporaryFile tf;
                        tf.open();
                        const QString csvPath = tf.fileName() + QStringLiteral(".csv");
                        tf.close();
                        QString csvErr;
                        const bool expOk = awin->exportLastCompareCsv(csvPath, &csvErr);
                        check(expOk && QFileInfo(csvPath).size() > 100,
                              QStringLiteral("Phase 2 导出对比 CSV 成功（%1 字节）").arg(QFileInfo(csvPath).size()));
                        QFile::remove(csvPath);
                    }
                    afterFrames([=]() {
                        check(g->filterOverlayItemsDrawn() > 0,
                              QStringLiteral("Phase 2 地图同屏滤波叠加绘制图元=%1").arg(g->filterOverlayItemsDrawn()));
                        check(g->compareLinksDrawn() > 0,
                              QStringLiteral("Phase 2 地图误差对比连线绘制=%1").arg(g->compareLinksDrawn()));
                        if (awin)
                            awin->close();
                        w->setFilterOverlayEnabled(false);
                        w->isolateTrack(-1);

                        // 8. Phase 3: sysPara 自动提取双基地站址/探测扇区 + PPI 网格绘制 + 持久化 + 状态栏相对极坐标
                        RadarSiteStore *rs = w->radarSites();
                        check(rs != nullptr && rs->sites().size() == 1,
                              QStringLiteral("Phase 3 导入 MAT 自动注册雷达站点（count=%1）")
                                  .arg(rs ? rs->sites().size() : 0));
                        const RadarSite *active = rs ? rs->activeSite() : nullptr;
                        check(active != nullptr && active->hasTx && active->hasRx && active->hasSector,
                              QStringLiteral("Phase 3 站点包含双基地 Tx/Rx 与探测扇区"));
                        if (active) {
                            check(std::fabs(active->txLat - 35.28) < 0.01 && std::fabs(active->txLon - 113.93) < 0.01,
                                  QStringLiteral("Phase 3 Tx 坐标 (%1, %2)").arg(active->txLat, 0, 'f', 4).arg(active->txLon, 0, 'f', 4));
                            check(std::fabs(active->rxLat - 33.0108) < 0.01 && std::fabs(active->rxLon - 112.6503) < 0.01,
                                  QStringLiteral("Phase 3 Rx 坐标 (%1, %2)").arg(active->rxLat, 0, 'f', 4).arg(active->rxLon, 0, 'f', 4));
                            check(std::fabs(active->minRangeKm - 2201.25) < 1.0 && std::fabs(active->maxRangeKm - 4200.0) < 1.0,
                                  QStringLiteral("Phase 3 探测距离门 %1 ~ %2 km")
                                      .arg(active->minRangeKm, 0, 'f', 2).arg(active->maxRangeKm, 0, 'f', 1));
                            check(std::fabs(active->startAzDeg - 83.6) < 0.2 && std::fabs(active->endAzDeg - 113.6) < 0.2
                                      && std::fabs(active->beamDirDeg - 113.0) < 0.2,
                                  QStringLiteral("Phase 3 方位扇区 %1°~%2° beamDir=%3°")
                                      .arg(active->startAzDeg, 0, 'f', 1).arg(active->endAzDeg, 0, 'f', 1)
                                      .arg(active->beamDirDeg, 0, 'f', 1));
                            // 持久化往返验证
                            RadarSiteStore reloaded;
                            reloaded.load();
                            check(reloaded.sites().size() == 1 && reloaded.activeSite() != nullptr,
                                  QStringLiteral("Phase 3 RadarSiteStore 持久化与恢复一致"));
                        }
                        // 切视角对准探测扇区，验证 PPI 网格与状态栏布局
                        g->lookAt(125.0, 31.0, 5000.0);
                        afterFrames([=]() {
                            check(g->radarPpiPrimitivesDrawn() > 5,
                                  QStringLiteral("Phase 3 地图绘制 PPI 网格/站址图元=%1").arg(g->radarPpiPrimitivesDrawn()));
                            QString layoutRep2;
                            check(sb->checkLayout(&layoutRep2),
                                  QStringLiteral("Phase 3 启用极坐标状态栏后布局校验：%1")
                                      .arg(layoutRep2.isEmpty() ? QStringLiteral("OK") : layoutRep2));

                            // 9. Phase 4: 航迹诊断面板 + 虚假航迹/消失断航双引擎自检
                            const trackdiag::TrackDiagBundle &bundle = w->diagBundle();
                            int metaWithQuality = 0;
                            int metaWithAssc = 0;
                            int metaWithGate = 0;
                            int metaWithLife = 0;
                            for (auto it = bundle.metas.constBegin(); it != bundle.metas.constEnd(); ++it) {
                                const trackdiag::TrackDiagMeta &m = it.value();
                                if (m.valid && std::isfinite(m.quality) && m.quality >= 0.0 && m.quality <= 1.0)
                                    ++metaWithQuality;
                                if (!m.asscFlags.isEmpty())
                                    ++metaWithAssc;
                                if (m.gate.valid)
                                    ++metaWithGate;
                                if (!m.life.isEmpty())
                                    ++metaWithLife;
                            }
                            check(bundle.metas.size() == 84
                                      && metaWithQuality == 84
                                      && metaWithAssc == 84
                                      && metaWithGate == 84
                                      && metaWithLife > 0,
                                  QStringLiteral("Phase 4 自动解析 TrackDiagBundle 元数据：共 %1 条（Quality=%2 asscFlags=%3 gate=%4 lifeList=%5）")
                                      .arg(bundle.metas.size()).arg(metaWithQuality).arg(metaWithAssc)
                                      .arg(metaWithGate).arg(metaWithLife));
                            check(bundle.tempPoints.size() == 1857,
                                  QStringLiteral("Phase 4 自动解析 tempTrackList=%1 条（期望 1857）")
                                      .arg(bundle.tempPoints.size()));

                            // 9a. 虚假航迹引擎：84 条降序评分 + 沙盒门限与 TrackLayer mutedTracks 同步
                            trackdiag::FalseTrackConfig fcfg;
                            const QVector<trackdiag::FalseTrackItem> falseItems =
                                trackdiag::evaluateFalseTracks(*store, bundle, fcfg);
                            bool scoreDesc = true;
                            for (int i = 1; i < falseItems.size(); ++i) {
                                if (falseItems.at(i - 1).suspicionScore + 1e-6 < falseItems.at(i).suspicionScore) {
                                    scoreDesc = false;
                                    break;
                                }
                            }
                            check(falseItems.size() == 84 && scoreDesc,
                                  QStringLiteral("Phase 4 evaluateFalseTracks 产出 %1 条且按疑点分降序（最高=%2 最低=%3）")
                                      .arg(falseItems.size())
                                      .arg(falseItems.isEmpty() ? 0.0 : falseItems.first().suspicionScore, 0, 'f', 1)
                                      .arg(falseItems.isEmpty() ? 0.0 : falseItems.last().suspicionScore, 0, 'f', 1));

                            w->activatePanel(PanelId::Diagnostics);
                            DiagnosticsPanel *dp = w->diagnosticsPanel();
                            check(dp != nullptr && w->activePanel() == int(PanelId::Diagnostics),
                                  QStringLiteral("Phase 4 打开航迹诊断面板（activePanel=%1）").arg(w->activePanel()));
                            if (!dp) {
                                pwin->close();
                                finish();
                                return;
                            }
                            dp->setCurrentTab(0);
                            check(dp->currentTab() == 0 && dp->falseTrackCount() == 84,
                                  QStringLiteral("Phase 4 诊断面板 Tab 1 虚假航迹列表=%1 条").arg(dp->falseTrackCount()));
                            dp->setSandboxMinQuality(0.5);
                            const int filteredCnt = dp->filteredFalseCount();
                            const int mutedCnt = layer->mutedTracks().size();
                            check(filteredCnt > 0 && mutedCnt == filteredCnt,
                                  QStringLiteral("Phase 4 调整沙盒 minQuality=0.5：拟滤除=%1 条，TrackLayer mutedTracks=%2 条")
                                      .arg(filteredCnt).arg(mutedCnt));
                            dp->selectFalseRow(0);

                            // 9b. 消失/断航引擎：断航 + 波门尸检 + 断批重起 + 漏批挖掘 + 四级漏斗
                            const Point3dDataset *detDs = pwin->dataset();
                            check(detDs != nullptr && detDs->frames.size() == 60,
                                  QStringLiteral("Phase 4 诊断联动 DetPoint 帧数=%1（期望 60）")
                                      .arg(detDs ? detDs->frames.size() : 0));

                            const QVector<trackdiag::BrokenTrackItem> broken =
                                trackdiag::findBrokenTracks(*store, bundle, active);
                            check(!broken.isEmpty(),
                                  QStringLiteral("Phase 4 findBrokenTracks 检出异常断航=%1 条").arg(broken.size()));

                            bool gateValid = false;
                            bool hasVerdict = false;
                            int autopsyIdx = 0;
                            trackdiag::GateAutopsyResult sampleAutopsy;
                            for (int bi = 0; bi < qMin(broken.size(), 15); ++bi) {
                                const trackdiag::GateAutopsyResult ar =
                                    trackdiag::runGateAutopsy(broken.at(bi).trackIndex, *store, bundle, detDs);
                                bool curGate = false;
                                bool curCand = false;
                                for (const trackdiag::GateAutopsyFrame &fr : ar.frames) {
                                    if (fr.gateR > 0.0f && fr.gateAz > 0.0f)
                                        curGate = true;
                                    for (const trackdiag::GateCandidate &c : fr.candidates) {
                                        if (!c.verdict.isEmpty())
                                            curCand = true;
                                    }
                                }
                                if (bi == 0) {
                                    sampleAutopsy = ar;
                                    autopsyIdx = bi;
                                }
                                if (curGate && curCand) {
                                    sampleAutopsy = ar;
                                    autopsyIdx = bi;
                                    gateValid = true;
                                    hasVerdict = true;
                                    break;
                                }
                                if (curGate)
                                    gateValid = true;
                            }
                            check(!sampleAutopsy.frames.isEmpty() && !sampleAutopsy.primaryCause.isEmpty()
                                      && gateValid && hasVerdict,
                                  QStringLiteral("Phase 4 runGateAutopsy 有效波门与候选点判定：frames=%1 cause=%2")
                                      .arg(sampleAutopsy.frames.size()).arg(sampleAutopsy.primaryCause));

                            const QVector<trackdiag::TrackFragmentPair> frags = trackdiag::findFragmentPairs(*store);
                            check(!frags.isEmpty(),
                                  QStringLiteral("Phase 4 findFragmentPairs 检出断批重起=%1 对").arg(frags.size()));

                            const QVector<trackdiag::OrphanDetCluster> orphans =
                                detDs ? trackdiag::findOrphanDetClusters(*detDs, *store, bundle)
                                      : QVector<trackdiag::OrphanDetCluster>();
                            check(!orphans.isEmpty(),
                                  QStringLiteral("Phase 4 findOrphanDetClusters 检出未关联漏批链=%1 条").arg(orphans.size()));

                            const QVector<trackdiag::FunnelFrameStat> funnel =
                                trackdiag::traceTrackFunnel(store->tracks().at(0), *store, bundle, detDs);
                            int funnelHits = 0;
                            for (const trackdiag::FunnelFrameStat &fs : funnel) {
                                if (fs.l1DetCount > 0 || fs.l2TempCount > 0 || fs.l3AsscCount > 0 || fs.l4SmoothCount > 0)
                                    ++funnelHits;
                            }
                            check(!funnel.isEmpty() && funnelHits > 0,
                                  QStringLiteral("Phase 4 traceTrackFunnel 输出 %1 帧（有效命中 %2 帧）")
                                      .arg(funnel.size()).arg(funnelHits));

                            // 9c. 切换 Tab 2 与三子视图，选中异常行触发地图诊断叠加绘制
                            dp->setCurrentTab(1);
                            check(dp->currentTab() == 1
                                      && dp->brokenTrackCount() > 0
                                      && dp->fragmentPairCount() > 0
                                      && dp->orphanClusterCount() > 0,
                                  QStringLiteral("Phase 4 诊断面板 Tab 2 统计：断航=%1 配对=%2 漏批链=%3")
                                      .arg(dp->brokenTrackCount())
                                      .arg(dp->fragmentPairCount())
                                      .arg(dp->orphanClusterCount()));

                            dp->setDisappearSubView(0);
                            dp->selectBrokenRow(autopsyIdx);
                            check(!dp->lastGateAutopsy().frames.isEmpty(),
                                  QStringLiteral("Phase 4 选中异常断航行触发波门尸检（%1 帧）")
                                      .arg(dp->lastGateAutopsy().frames.size()));
                            if (autopsyIdx >= 0 && autopsyIdx < broken.size())
                                g->lookAt(broken.at(autopsyIdx).endLon, broken.at(autopsyIdx).endLat, 500.0);

                            afterFrames([=]() {
                                const int gateDrawn = g->diagnosticsItemsDrawn();
                                check(gateDrawn > 0,
                                      QStringLiteral("Phase 4 地图波门尸检叠加绘制图元=%1").arg(gateDrawn));

                                // 断批重起子视图：先清除波门叠加再选中首对，独立验证双选隔离与桥接虚线绘制
                                g->clearDiagnosticsOverlay();
                                dp->setDisappearSubView(1);
                                dp->selectFragmentRow(0);
                                afterFrames([=]() {
                                    const int fragDrawn = g->diagnosticsItemsDrawn();
                                    check(fragDrawn > 0 && w->isolatedTracks().size() == 2,
                                          QStringLiteral("Phase 4 断批重起叠加绘制图元=%1，双选隔离=%2 条")
                                              .arg(fragDrawn).arg(w->isolatedTracks().size()));

                                    // 漏批挖掘 & 四级漏斗子视图：先清除桥接叠加再选中漏批链与漏斗航迹
                                    g->clearDiagnosticsOverlay();
                                    dp->setDisappearSubView(2);
                                    dp->selectOrphanRow(0);
                                    dp->selectFunnelTrack(0);
                                    check(dp->funnelFrameCount() > 0,
                                          QStringLiteral("Phase 4 面板四级漏斗表=%1 帧").arg(dp->funnelFrameCount()));
                                    afterFrames([=]() {
                                        const int orphanDrawn = g->diagnosticsItemsDrawn();
                                        check(orphanDrawn > 0,
                                              QStringLiteral("Phase 4 漏批点迹链叠加绘制图元=%1").arg(orphanDrawn));

                                        // 恢复清理并核验全程零 GL 错误
                                        g->clearDiagnosticsOverlay();
                                        w->isolateTrack(-1);
                                        layer->setMutedTracks(QSet<int>());
                                        w->closeSidebar();
                                        pwin->close();
                                        check(g->glErrorCount() == 0,
                                              QStringLiteral("全程 OpenGL 错误数=%1").arg(g->glErrorCount()));
                                        finish();
                                    });
                                });
                            });
                        });
                    });
                });
                pollLoad->start();
            });
        });
    });
    w->importFile(TrackSource::Radar, mat);
}

} // namespace selftest
