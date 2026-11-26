#include "selftest.h"

#include "mainwindow.h"
#include "globewidget.h"
#include "track.h"
#include "trackparsers.h"
#include "tracklayer.h"
#include "replaycontroller.h"
#include "trackpointdialog.h"
#include "appstatusbar.h"
#include "apppaths.h"
#include "drawingmanager.h"
#include "uiwidgets.h"
#include "geocalc.h"
#include "geo.h"
#include "managepanel.h"
#include "maptools.h"
#include "sidepanels.h"
#include "spatiallasso.h"
#include "trackdb.h"
#include "theme.h"
#include "settingspanel.h"
#include "trackimporter.h"
#include "point3dparsers.h"
#include "point3dcanvas.h"
#include "point3dwindow.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolTip>
#include <QUndoStack>
#include <QVector3D>
#include <QWindow>
#include <QtEndian>
#include <QtMath>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>

namespace selftest {

// 点迹可视化自检：HelloVscode.exe --pointtest <目录|文件> [--track <航迹.mat>]
// 校验批量解析、时间换算、连续性评分、视图坐标与航迹测量坐标转换，写 pointtest.log
int runPointTest(const QString &input, const QString &trackFile)
{
    QFile log(QCoreApplication::applicationDirPath() + QStringLiteral("/pointtest.log"));
    log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    QTextStream out(&log);
    out.setCodec("UTF-8");
    out.setRealNumberPrecision(15);
    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &what) {
        out << (ok ? "PASS " : "FAIL ") << what << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    // ---- 收集输入文件（任意数量，含单个）----
    QStringList files;
    const QFileInfo info(input);
    if (info.isDir()) {
        const QDir dir(input);
        files = dir.entryList(QStringList() << QStringLiteral("detPoint_*.mat")
                                            << QStringLiteral("det_*.mat"),
                              QDir::Files, QDir::Name);
        if (files.isEmpty())
            files = dir.entryList(QStringList() << QStringLiteral("*.mat"), QDir::Files, QDir::Name);
        for (QString &f : files)
            f = dir.filePath(f);
    } else if (info.isFile()) {
        files << input;
    }
    out << "input=" << input << " files=" << files.size() << "\n";
    check(!files.isEmpty(), QStringLiteral("找到点迹文件"));

    // ---- 批量解析 ----
    QElapsedTimer clock;
    clock.start();
    Point3dDataset data;
    QStringList errors;
    const bool parsed = parseDetPointFiles(files, &data, &errors);
    for (const QString &e : qAsConst(errors))
        out << "  parse error: " << e << "\n";
    check(parsed, QStringLiteral("批量解析成功"));
    check(data.frames.size() + errors.size() == files.size(),
          QStringLiteral("帧数+失败数等于文件数（%1+%2=%3）")
              .arg(data.frames.size()).arg(errors.size()).arg(files.size()));
    out << "frames=" << data.frames.size() << " elapsed=" << clock.elapsed() << " ms\n";

    int totalPoints = 0;
    bool timesAscending = true;
    bool fieldsFinite = true;
    for (int i = 0; i < data.frames.size(); ++i) {
        const DetPointFrame &f = data.frames.at(i);
        totalPoints += f.points.size();
        if (i > 0 && !(f.timeMs > data.frames.at(i - 1).timeMs))
            timesAscending = false;
        for (const DetPointItem &p : f.points)
            if (!std::isfinite(p.range) || !std::isfinite(p.vr) || !std::isfinite(p.az)
                    || !std::isfinite(p.snr) || p.range < 0.0f)
                fieldsFinite = false;
    }
    check(totalPoints > 0, QStringLiteral("点迹总数 > 0（%1）").arg(totalPoints));
    check(timesAscending, QStringLiteral("帧按时间严格升序"));
    check(fieldsFinite, QStringLiteral("点字段有限且 range >= 0"));
    if (!data.frames.isEmpty()) {
        out << "time range " << data.minTime << " .. " << data.maxTime << " ("
            << formatBeijingTime(data.minTime) << " ~ " << formatBeijingTime(data.maxTime) << ")\n";
        check(data.minTime == data.frames.first().timeMs && data.maxTime == data.frames.last().timeMs,
              QStringLiteral("数据集时间范围与首末帧一致"));
    }

    // ---- 时间换算（固定样本，防止回归）----
    check(datenumToMs(739961.74983796) == 1765360786000LL,
          QStringLiteral("datenum -> Unix 毫秒固定样本"));
    check(formatBeijingTime(1765360786000LL) == QStringLiteral("2025 12 10 17:59:46"),
          QStringLiteral("Unix 毫秒 -> 北京时间固定样本"));

    // ---- 全局时钟：显示集合变化（地图单独显示/筛选）不得把当前时刻重置到起点 ----
    {
        ReplayController r;
        r.setRange(1000, 5000);
        r.seekTime(3000);
        r.setRange(500, 6000);
        check(r.current() == 3000, QStringLiteral("回放范围变化后保留当前时刻（%1）").arg(r.current()));
        r.setRange(500, 6000);
        check(r.current() == 3000, QStringLiteral("回放范围不变时当前时刻不动"));
        r.setRange(4000, 9000);
        check(r.current() == 4000, QStringLiteral("当前时刻超出新范围时夹到起点（%1）").arg(r.current()));
        r.play();
        const bool playingBefore = r.isPlaying();
        r.setRange(3000, 9500);
        check(playingBefore && r.isPlaying() && r.current() == 4000,
              QStringLiteral("范围变化不打断回放、不跳时间"));
    }

    // ---- 连续性评分 ----
    clock.restart();
    ContinuityConfig cfg;
    computeContinuity(data.frames, cfg);
    bool continuityOk = true;
    double globalMax = 0.0;
    for (const DetPointFrame &f : qAsConst(data.frames)) {
        if (f.continuity.size() != f.points.size()) {
            continuityOk = false;
            continue;
        }
        for (float c : f.continuity) {
            if (!std::isfinite(c) || c < 0.0f || c > 1.0f) {
                continuityOk = false;
                break;
            }
            globalMax = qMax(globalMax, double(c));
        }
    }
    check(continuityOk, QStringLiteral("连续性长度与值域 0..1"));
    check(globalMax > 0.0, QStringLiteral("连续性存在非零值"));
    out << "continuity elapsed=" << clock.elapsed() << " ms max=" << globalMax << "\n";

    // ---- 三种视图坐标映射 ----
    double x = 0, y = 0;
    point3dViewCoordinates(Point3dViewMode::RVF, 1000, 12, 33, &x, &y);
    const bool rvfOk = qFuzzyCompare(x, 1000.0) && qFuzzyCompare(y, 12.0);
    point3dViewCoordinates(Point3dViewMode::RAF, 1000, 12, 33, &x, &y);
    const bool rafOk = qFuzzyCompare(x, 1000.0) && qFuzzyCompare(y, 33.0);
    point3dViewCoordinates(Point3dViewMode::VAF, 1000, 12, 33, &x, &y);
    const bool vafOk = qFuzzyCompare(x, 12.0) && qFuzzyCompare(y, 33.0);
    check(rvfOk && rafOk && vafOk, QStringLiteral("RVF/RAF/VAF 坐标映射"));

    // ---- 三方联动匹配：BatchNo + 时间范围（雷达/原始量测两批同属一个 MAT）----
    check(point3dRefsMatch(20050, 1000, 2000, 20050, 1500, 3000),
          QStringLiteral("同编号且时间重叠 -> 同一条航迹"));
    check(!point3dRefsMatch(20050, 1000, 2000, 20051, 1500, 3000),
          QStringLiteral("编号不同 -> 不是同一条航迹"));
    check(!point3dRefsMatch(20050, 1000, 2000, 20050, 5000, 6000),
          QStringLiteral("编号相同但时间不重叠 -> 不是同一条航迹"));

    // ---- 航迹叠加（可选）----
    if (!trackFile.isEmpty()) {
        QVector<Track> tracks;
        QString err;
        const bool trackOk = parseRadarMat(trackFile, false, QStringLiteral("Radar"), &tracks, &err);
        check(trackOk, QStringLiteral("航迹 MAT 解析"));
        if (trackOk) {
            bool anyMeasure = false;
            for (const Track &t : qAsConst(tracks))
                for (const TrackPoint &p : t.points)
                    if (std::isfinite(p.prange) && std::isfinite(p.pvr) && std::isfinite(p.paz)) {
                        anyMeasure = true;
                        break;
                    }
            check(anyMeasure, QStringLiteral("航迹点带测量坐标（prange/pvr/paz）"));

            int overlayTracks = 0, overlayPoints = 0, overlayRaw = 0;
            for (const Track &t : qAsConst(tracks)) {
                TrackOverlay ov;
                if (overlayFromTrack(t, data.frames, &ov)) {
                    ++overlayTracks;
                    overlayPoints += ov.points.size();
                    overlayRaw += ov.rawPoints.size();
                    data.tracks.append(ov);      // 供画布自检绘制叠加航迹
                }
            }
            out << "overlay tracks=" << overlayTracks << " points=" << overlayPoints
                << " raw=" << overlayRaw << "\n";
            check(overlayTracks > 0 && overlayPoints > 0,
                  QStringLiteral("航迹可转换为点云叠加数据"));
            check(overlayRaw > 0,
                  QStringLiteral("同一 MAT 的关联点列（assc）也可叠加（%1 点）").arg(overlayRaw));

            // 时间窗完全对不上的航迹：不能被硬贴到首/末帧画成假航迹
            Track farTrack;
            farTrack.id = QStringLiteral("RADAR-99999");
            farTrack.source = TrackSource::Radar;
            farTrack.fileName = QStringLiteral("far");
            TrackPoint fp;
            fp.t = data.frames.first().timeMs + 6 * 3600 * 1000LL;
            fp.prange = 1000.0f;
            fp.pvr = 10.0f;
            fp.paz = 45.0f;
            farTrack.points << fp;
            TrackOverlay farOv;
            check(!overlayFromTrack(farTrack, data.frames, &farOv),
                  QStringLiteral("时间窗完全对不上的航迹判为不可叠加（一致性校验）"));

            // TrackRangeScale：距离按比例缩放
            if (!tracks.isEmpty()) {
                TrackOverlay base, scaled;
                if (overlayFromTrack(tracks.first(), data.frames, &base)
                        && overlayFromTrack(tracks.first(), data.frames, &scaled, QColor(), 2.0)
                        && !base.points.isEmpty()) {
                    check(qAbs(scaled.points.first().prange - base.points.first().prange * 2.0f) < 1e-2,
                          QStringLiteral("航迹距离缩放按比例生效（%1 -> %2）")
                              .arg(double(base.points.first().prange))
                              .arg(double(scaled.points.first().prange)));
                }
            }
        }
    }

    // ---- 画布渲染 ----
    {
        Point3dCanvas canvas;
        canvas.setDataset(&data);
        canvas.resize(1200, 800);
        canvas.show();
        QApplication::processEvents();
        check(canvas.visiblePointCount() > 0,
              QStringLiteral("画布可见点数 > 0（%1）").arg(canvas.visiblePointCount()));
        const QPixmap shot = canvas.grab();
        check(!shot.isNull() && shot.width() == 1200 && shot.height() == 800,
              QStringLiteral("画布抓图非空且尺寸正确"));
        check(!canvas.inherits("QOpenGLWidget"), QStringLiteral("画布不使用 OpenGL"));

        // 叠加航迹：默认点源必须能看得到，且平滑 / 关联两种点列都能切换
        if (!data.tracks.isEmpty()) {
            canvas.grab();
            check(canvas.visibleTrackCount() > 0,
                  QStringLiteral("默认点源=自动时叠加航迹可见（%1 条 / 共 %2 条）")
                      .arg(canvas.visibleTrackCount()).arg(data.tracks.size()));
            canvas.setTrackSource(Point3dTrackSource::Smooth);
            canvas.grab();
            const int smoothTracks = canvas.visibleTrackCount();
            check(smoothTracks > 0,
                  QStringLiteral("点源=平滑 显示平滑点列（%1 条）").arg(smoothTracks));
            canvas.setTrackSource(Point3dTrackSource::Associated);
            canvas.grab();
            const int assocTracks = canvas.visibleTrackCount();
            check(assocTracks > 0,
                  QStringLiteral("点源=关联 显示关联点列（%1 条）").arg(assocTracks));
            check(assocTracks <= data.tracks.size(),
                  QStringLiteral("关联点列条数不超过叠加航迹总数（%1 ≤ %2）")
                      .arg(assocTracks).arg(data.tracks.size()));
            canvas.grab().save(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/pointtest_assoc.png"));

            // 地图单独显示/筛选联动：只显示选中的批次
            const int firstBatch = data.tracks.first().batchNo;
            check(data.tracks.first().minTime <= data.maxTime
                      && data.tracks.first().maxTime >= data.minTime,
                  QStringLiteral("叠加航迹时间范围与点迹时间窗有交集"));
            QSet<int> only;
            only.insert(firstBatch);
            canvas.setIsolatedBatches(only);
            canvas.grab();
            check(canvas.visibleTrackCount() >= 1 && canvas.visibleTrackCount() < data.tracks.size(),
                  QStringLiteral("只显示选中批次（%1 条）").arg(canvas.visibleTrackCount()));
            canvas.setIsolatedBatches(QSet<int>());
            canvas.grab();
            // 当前点源是「关联」：清空隔离后应恢复到该点源下的全部航迹（部分批次没有关联点列）
            check(canvas.visibleTrackCount() == assocTracks,
                  QStringLiteral("清空隔离后恢复显示全部叠加航迹（%1 条）")
                      .arg(canvas.visibleTrackCount()));

            // 本窗口显示设置：点迹尺寸 / 标签大小（坐标轴刻度数字跟随标签大小）
            canvas.setPointScale(2.0);
            canvas.setLabelScale(2.0);
            check(qFuzzyCompare(canvas.pointScale(), 2.0) && qFuzzyCompare(canvas.labelScale(), 2.0),
                  QStringLiteral("点迹尺寸/标签大小可调（%1 / %2）")
                      .arg(canvas.pointScale()).arg(canvas.labelScale()));
            canvas.grab().save(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/pointtest_label_scale.png"));
            canvas.setPointScale(1.0);
            canvas.setLabelScale(1.0);
            canvas.setTrackSource(Point3dTrackSource::Auto);
            canvas.grab();
        }

        // 色标条：悬停到点迹上时必须始终显示（回归：提示框画刷泄漏，把色标渐变整块盖成深色）
        {
            bool hovered = false;
            QObject::connect(&canvas, &Point3dCanvas::hoverChanged, &canvas,
                             [&hovered]() { hovered = true; });
            const QRectF plot = canvas.plotRect();
            for (int y = int(plot.top()) + 8; y < int(plot.bottom()) - 8 && !hovered; y += 16) {
                for (int x = int(plot.left()) + 8; x < int(plot.right()) - 8 && !hovered; x += 16) {
                    QMouseEvent ev(QEvent::MouseMove, QPointF(x, y),
                                   Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(&canvas, &ev);
                }
            }
            check(hovered, QStringLiteral("鼠标悬停命中点迹（弹出提示框）"));
            const QImage img = canvas.grab().toImage();
            // 色标条紧贴立方体右侧，取样区域按画布给出的矩形
            const QRectF bar = canvas.colorBarRect();
            const int barX0 = int(bar.left()) + 2;
            const int barX1 = int(bar.right()) - 2;
            int colorful = 0;
            for (int y = int(bar.top()) + 4; y < int(bar.bottom()) - 4; y += 3)
                for (int x = barX0; x <= barX1; ++x) {
                    const QColor c = img.pixelColor(x, y);
                    const int hi = qMax(c.red(), qMax(c.green(), c.blue()));
                    const int lo = qMin(c.red(), qMin(c.green(), c.blue()));
                    if (hi - lo > 40)
                        ++colorful;
                }
            check(colorful > 100,
                  QStringLiteral("悬停时色标条仍然显示（彩色像素 %1）").arg(colorful));
            canvas.grab().save(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/pointtest_hover.png"));
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&canvas, &leave);
        }

        // 右键拖拽 = 平移视图（左键拖拽仍是旋转视角）
        {
            double yaw0 = 0, pitch0 = 0, zoom0 = 0, panX0 = 0, panY0 = 0;
            canvas.camera(&yaw0, &pitch0, &zoom0, &panX0, &panY0);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(600, 400),
                              Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &press);
            QMouseEvent move(QEvent::MouseMove, QPointF(640, 430),
                             Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &move);
            QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(640, 430),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &rel);
            double yaw1 = 0, pitch1 = 0, zoom1 = 0, panX1 = 0, panY1 = 0;
            canvas.camera(&yaw1, &pitch1, &zoom1, &panX1, &panY1);
            check(qFuzzyCompare(panX1 - panX0, 40.0) && qFuzzyCompare(panY1 - panY0, 30.0),
                  QStringLiteral("右键拖拽平移视图（Δ=%1,%2）")
                      .arg(panX1 - panX0).arg(panY1 - panY0));
            check(qFuzzyCompare(yaw1, yaw0) && qFuzzyCompare(pitch1, pitch0),
                  QStringLiteral("右键拖拽不旋转视角"));
            canvas.resetCamera();
            canvas.camera(nullptr, nullptr, nullptr, &panX1, &panY1);
            check(qFuzzyCompare(panX1, 0.0) && qFuzzyCompare(panY1, 0.0),
                  QStringLiteral("恢复默认视角同时清零平移"));
        }

        // 航迹优先悬停 + 左键单击航迹发出 trackClicked（窗口据此单独显示并联动地图）
        if (!data.tracks.isEmpty()) {
            canvas.grab();
            const QPointF head = canvas.trackHead(0);
            int hoveredBatch = -1;
            QObject::connect(&canvas, &Point3dCanvas::trackFocusChanged, &canvas,
                             [&hoveredBatch](int b, TrackSource, qint64, qint64) { hoveredBatch = b; });
            QMouseEvent hmove(QEvent::MouseMove, head, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &hmove);
            check(hoveredBatch > 0,
                  QStringLiteral("悬停航迹线时命中航迹（批 %1）").arg(hoveredBatch));

            int clickedBatch = -1;
            QObject::connect(&canvas, &Point3dCanvas::trackClicked, &canvas,
                             [&clickedBatch](int b, TrackSource, qint64, qint64) { clickedBatch = b; });
            QMouseEvent press(QEvent::MouseButtonPress, head, Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(&canvas, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, head, Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(&canvas, &release);
            check(clickedBatch > 0 && clickedBatch == hoveredBatch,
                  QStringLiteral("左键单击航迹发出 trackClicked（批 %1）").arg(clickedBatch));
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&canvas, &leave);
        }

        // 色标 SNR 手动映射 / 提示字号
        {
            canvas.setSnrRange(0.0, 40.0);
            check(qFuzzyCompare(canvas.effectiveSnrMin(), 0.0)
                      && qFuzzyCompare(canvas.effectiveSnrMax(), 40.0),
                  QStringLiteral("色标 SNR 手动映射生效"));
            const double nan = std::numeric_limits<double>::quiet_NaN();
            canvas.setSnrRange(nan, nan);
            check(qFuzzyCompare(canvas.effectiveSnrMin(), data.snrMin)
                      && qFuzzyCompare(canvas.effectiveSnrMax(), data.snrMax),
                  QStringLiteral("色标 SNR 自动映射回到数据范围（%1~%2）")
                      .arg(canvas.effectiveSnrMin()).arg(canvas.effectiveSnrMax()));
            canvas.setHoverTextScale(2.0);
            check(qFuzzyCompare(canvas.hoverTextScale(), 2.0),
                  QStringLiteral("悬停信息字号可调"));
            canvas.setHoverTextScale(1.0);
        }

        // 区域筛选：未应用只画候选框；应用后只显示框内数据；清除后恢复
        {
            const Point3dRegion ext = canvas.regionExtent();
            check(ext.valid && ext.x1 > ext.x0 && ext.y1 > ext.y0 && ext.z1 > ext.z0,
                  QStringLiteral("区域筛选三轴量程有效"));
            const int before = canvas.visiblePointCount();
            Point3dRegion r = ext;
            r.x0 = ext.x0 + (ext.x1 - ext.x0) * 0.25;
            r.x1 = ext.x0 + (ext.x1 - ext.x0) * 0.75;
            r.y0 = ext.y0 + (ext.y1 - ext.y0) * 0.25;
            r.y1 = ext.y0 + (ext.y1 - ext.y0) * 0.75;
            r.valid = true;
            canvas.setRegionBox(r);
            canvas.grab().save(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/pointtest_region_box.png"));
            check(canvas.visiblePointCount() == before,
                  QStringLiteral("只画候选框时不裁剪数据（%1）").arg(canvas.visiblePointCount()));
            canvas.setRegionFilter(r);
            canvas.grab().save(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/pointtest_region_applied.png"));
            check(canvas.visiblePointCount() > 0 && canvas.visiblePointCount() < before,
                  QStringLiteral("应用区域筛选后只显示框内点（%1 -> %2）")
                      .arg(before).arg(canvas.visiblePointCount()));
            canvas.setRegionFilter(Point3dRegion());
            canvas.setRegionBox(Point3dRegion());
            canvas.grab();
            check(canvas.visiblePointCount() == before,
                  QStringLiteral("清除区域筛选后恢复全部点（%1）").arg(canvas.visiblePointCount()));
        }

        // 强连续阈值：只影响"强连续点数"统计
        {
            canvas.setStrongThreshold(0.0);
            canvas.grab();
            const int strongAll = canvas.lastStrongCount();
            canvas.setStrongThreshold(1.0);
            canvas.grab();
            check(strongAll > 0 && canvas.lastStrongCount() == 0
                      && strongAll <= canvas.visiblePointCount(),
                  QStringLiteral("强连续阈值影响统计（阈值0 -> %1，阈值1 -> %2）")
                      .arg(strongAll).arg(canvas.lastStrongCount()));
            canvas.setStrongThreshold(0.70);
        }

        bool viewOk = true;
        const char *const modeNames[] = { "rvf", "raf", "vaf" };
        int modeIndex = 0;
        for (Point3dViewMode mode : { Point3dViewMode::RVF, Point3dViewMode::RAF,
                                      Point3dViewMode::VAF }) {
            canvas.setViewMode(mode);
            QApplication::processEvents();
            const QPixmap pm = canvas.grab();
            if (pm.isNull())
                viewOk = false;
            else
                pm.save(QCoreApplication::applicationDirPath()
                        + QStringLiteral("/pointtest_%1.png").arg(QLatin1String(modeNames[modeIndex])));
            ++modeIndex;
        }
        check(viewOk, QStringLiteral("三种视图切换并重绘"));

        canvas.setWindowLength(20);
        QApplication::processEvents();
        check(canvas.visibleFrameCount() <= 20,
              QStringLiteral("滑动窗口 20 帧（实际 %1）").arg(canvas.visibleFrameCount()));
        canvas.setWindowLength(0);
        QApplication::processEvents();
        check(canvas.visibleFrameCount() == data.frames.size(),
              QStringLiteral("滑动窗口「全部」显示所有帧"));
        canvas.close();
    }

    // ---- 独立窗口（多开）----
    {
        Point3dWindow *w1 = new Point3dWindow(nullptr);
        Point3dWindow *w2 = new Point3dWindow(nullptr);
        w1->setNumber(1);
        w2->setNumber(2);
        w1->resize(1000, 700);
        w2->resize(1000, 700);
        w1->show();
        w2->show();
        QApplication::processEvents();
        check(w1->isWindow() && w2->isWindow(), QStringLiteral("点迹窗口是独立顶层窗口"));
        check(w1->windowTitle() != w2->windowTitle(),
              QStringLiteral("多窗口标题带序号（%1 / %2）").arg(w1->windowTitle(), w2->windowTitle()));
        check(!w1->inherits("QOpenGLWidget") && !w1->canvas()->inherits("QOpenGLWidget"),
              QStringLiteral("窗口与画布均不使用 OpenGL"));
        const QPixmap pm1 = w1->grab();
        check(!pm1.isNull(), QStringLiteral("窗口抓图非空"));
        // 布局：菜单 + 左侧「显示」侧栏 + 底部回放条（对齐地图窗口）
        check(w1->minimumSizeHint().width() < 900,
              QStringLiteral("窗口可自由缩放（最小宽度 %1 px）").arg(w1->minimumSizeHint().width()));
        QScrollArea *side = w1->findChild<QScrollArea *>(QStringLiteral("point3dSide"));
        check(side != nullptr, QStringLiteral("左侧有「显示」侧栏"));
        const int groups = w1->findChildren<QGroupBox *>().size();
        check(groups >= 5, QStringLiteral("侧栏含 %1 个设置分组").arg(groups));
        check(w1->findChild<QMenuBar *>() != nullptr, QStringLiteral("顶部有菜单栏"));
        check(w1->findChild<QScrollArea *>(QStringLiteral("point3dAdvanced")) != nullptr,
              QStringLiteral("左侧有「高级」参数页"));
        QPushButton *setBtn = nullptr, *advBtn = nullptr;
        for (QPushButton *b : w1->findChildren<QPushButton *>()) {
            if (b->text() == QStringLiteral("高级"))
                advBtn = b;
            else if (b->text() == QStringLiteral("设置") && b->isCheckable())
                setBtn = b;
        }
        check(setBtn != nullptr && advBtn != nullptr,
              QStringLiteral("顶部有「设置」「高级」按钮"));
        QStackedWidget *stack = w1->findChild<QStackedWidget *>(QStringLiteral("point3dSideStack"));
        if (advBtn && stack) {
            advBtn->click();
            QApplication::processEvents();
            check(stack->currentIndex() == 1, QStringLiteral("点「高级」切到高级参数页"));
            w1->grab().save(QCoreApplication::applicationDirPath()
                            + QStringLiteral("/pointtest_advanced_panel.png"));
        }
        if (setBtn && stack) {
            setBtn->click();
            QApplication::processEvents();
            check(stack->currentIndex() == 0, QStringLiteral("点「设置」切回设置页"));
        }
        QApplication::processEvents();
        w1->grab().save(QCoreApplication::applicationDirPath()
                        + QStringLiteral("/pointtest_side_panel.png"));
        w1->close();
        w2->close();
        QApplication::processEvents();
    }

    // ---- 窗口后台导入（走真实导入线程）----
    if (files.size() >= 1) {
        Point3dWindow *w = new Point3dWindow(nullptr);
        w->show();
        w->loadDetectionFiles(files.mid(0, qMin(3, files.size())));
        QElapsedTimer wait;
        wait.start();
        while (w->isLoading() && wait.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        const int got = w->dataset()->frames.size();
        check(!w->isLoading() && got == qMin(3, files.size()),
              QStringLiteral("窗口后台导入 %1 个文件（实际 %2 帧）")
                  .arg(qMin(3, files.size())).arg(got));

        // 区域筛选：应用后只显示框内，三个视图各存一套
        {
            w->canvas()->repaint();
            const int fullPoints = w->canvas()->visiblePointCount();
            const Point3dRegion ext = w->canvas()->regionExtent();
            Point3dRegion r = ext;
            r.x0 = ext.x0 + (ext.x1 - ext.x0) * 0.2;
            r.x1 = ext.x1 - (ext.x1 - ext.x0) * 0.2;
            r.y0 = ext.y0 + (ext.y1 - ext.y0) * 0.2;
            r.y1 = ext.y1 - (ext.y1 - ext.y0) * 0.2;
            check(w->setRegionForCurrentView(r, true),
                  QStringLiteral("窗口应用区域筛选"));
            check(w->regionAppliedForView(w->canvas()->viewMode()),
                  QStringLiteral("区域筛选状态为已应用"));
            w->canvas()->repaint();
            check(w->canvas()->visiblePointCount() < fullPoints,
                  QStringLiteral("应用后只显示框内点（%1 -> %2）")
                      .arg(fullPoints).arg(w->canvas()->visiblePointCount()));
            w->setViewMode(Point3dViewMode::RAF);
            check(!w->regionAppliedForView(Point3dViewMode::RAF),
                  QStringLiteral("切换视图后另一套区域参数独立"));
            w->setViewMode(Point3dViewMode::RVF);
            check(w->regionAppliedForView(Point3dViewMode::RVF),
                  QStringLiteral("切回原视图后区域筛选状态恢复"));
        }

        w->close();
        QApplication::processEvents();
    }

    // ---- 联动：地图选中的航迹不在本窗口时，本窗口保持原样 ----
    if (files.size() >= 1) {
        Point3dWindow *w = new Point3dWindow(nullptr);
        w->show();
        w->loadDetectionFiles(files.mid(0, 1));
        QElapsedTimer waitLink;
        waitLink.start();
        while (w->isLoading() && waitLink.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        QVector<LinkedTrackRef> refs;
        LinkedTrackRef r;
        r.batchNo = 20050;
        r.t0 = w->minTime();
        r.t1 = w->maxTime();
        refs << r;
        w->setLinkedSelection(refs);          // 本窗口没有对应叠加航迹
        check(w->isolatedBatches().isEmpty(),
              QStringLiteral("地图选中的航迹不在本窗口时窗口保持不变"));
        w->setLinkedHighlight(20050, w->minTime(), w->maxTime());
        check(w->highlightedBatch() < 0,
              QStringLiteral("悬停的航迹不在本窗口时不产生高亮"));
        w->close();
        QApplication::processEvents();
    }

    // ---- 本窗口独立回放：逐帧推进、倍速、按帧吸附、多窗口互不影响 ----
    if (files.size() >= 2) {
        Point3dWindow *wa = new Point3dWindow(nullptr);
        Point3dWindow *wb = new Point3dWindow(nullptr);
        wa->show();
        wb->show();
        wa->loadDetectionFiles(files.mid(0, qMin(3, files.size())));
        wb->loadDetectionFiles(files.mid(qMin(3, files.size() - 1),
                                          qMin(3, files.size())));
        QElapsedTimer wait2;
        wait2.start();
        while ((wa->isLoading() || wb->isLoading()) && wait2.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);

        const int nA = wa->frameCount();
        check(nA >= 2, QStringLiteral("窗口 A 导入 %1 帧").arg(nA));
        if (nA >= 2) {
            // 拖拽按帧吸附
            wa->seekToFrame(1);
            check(wa->currentFrame() == 1,
                  QStringLiteral("定位到指定帧（帧 %1）").arg(wa->currentFrame()));
            wa->seekToFrame(999999);
            check(wa->currentFrame() == nA - 1,
                  QStringLiteral("越界定位夹到最后一帧（帧 %1）").arg(wa->currentFrame()));
            wa->seekToFrame(-5);
            check(wa->currentFrame() == 0, QStringLiteral("负值定位夹到第一帧"));

            // 倍速：每帧停留 = 相邻帧时间差 / 倍速
            wa->setSpeed(1000.0);
            const qint64 iv = wa->dataset()->frames.at(1).timeMs
                              - wa->dataset()->frames.at(0).timeMs;
            const int expect = qBound(1, int(std::llround(double(iv) / 1000.0)), 600000);
            out << "frame interval=" << iv << " ms hold=" << wa->frameHoldMs()
                << " expect=" << expect << "\n";
            check(qAbs(wa->frameHoldMs() - expect) <= 1,
                  QStringLiteral("每帧停留 = 帧间隔 / 倍速（%1 ms）").arg(wa->frameHoldMs()));

            // 播放：按帧间隔逐帧推进（不插值）
            wb->seekToFrame(wb->frameCount() - 1);
            wa->seekToFrame(0);
            wa->togglePlay();
            check(wa->isPlaying(), QStringLiteral("开始播放"));
            QElapsedTimer play;
            play.start();
            while (play.elapsed() < 5000 && wa->currentFrame() == 0)
                QApplication::processEvents(QEventLoop::AllEvents, 10);
            check(wa->currentFrame() >= 1,
                  QStringLiteral("播放按帧推进（当前帧 %1）").arg(wa->currentFrame()));
            check(!wb->isPlaying() && wb->currentFrame() == wb->frameCount() - 1,
                  QStringLiteral("另一个窗口不跟着播（回放相互独立）"));
            wa->togglePlay();
            check(!wa->isPlaying(), QStringLiteral("暂停"));

            // 播到最后一帧自动停
            wa->seekToFrame(nA - 2);
            wa->togglePlay();
            QElapsedTimer tail;
            tail.start();
            while (tail.elapsed() < 5000 && wa->isPlaying())
                QApplication::processEvents(QEventLoop::AllEvents, 10);
            check(!wa->isPlaying() && wa->currentFrame() == nA - 1,
                  QStringLiteral("播到最后一帧自动停（帧 %1）").arg(wa->currentFrame()));
        }
        wa->close();
        wb->close();
        QApplication::processEvents();
    }

    // ---- 多窗口会话往返 ----
    if (files.size() >= 2) {
        Point3dWindow *wa = new Point3dWindow(nullptr);
        wa->loadDetectionFiles(files.mid(0, 2));
        QElapsedTimer wait3;
        wait3.start();
        while (wa->isLoading() && wait3.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        const QJsonObject session = wa->sessionJson();
        check(session.value(QStringLiteral("files")).toArray().size() == 2,
              QStringLiteral("会话 JSON 保存文件清单"));
        Point3dWindow *wb = new Point3dWindow(nullptr);
        wb->restoreSession(session);
        QElapsedTimer wait4;
        wait4.start();
        while (wb->isLoading() && wait4.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        check(wb->dataset()->frames.size() == wa->dataset()->frames.size()
                  && wb->dataset()->frames.size() == 2,
              QStringLiteral("会话恢复后自动重载数据（%1 帧）")
                  .arg(wb->dataset()->frames.size()));
        wa->close();
        wb->close();
        QApplication::processEvents();
    }

    out << (failures == 0 ? QStringLiteral("RESULT OK")
                          : QStringLiteral("RESULT FAILED (%1)").arg(failures)) << "\n";
    out.flush();
    return failures == 0 ? 0 : 1;
}

} // namespace selftest
