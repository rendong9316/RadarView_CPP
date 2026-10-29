#include "mainwindow.h"
#include "globewidget.h"

#include <QApplication>
#include <QDateTime>
#include <QTimer>
#include <QElapsedTimer>
#include <QTextStream>
#include <QFile>
#include <QElapsedTimer>
#include <QVector3D>
#include <QtMath>
#include <functional>
#include <cmath>

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

#include <QMouseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLocale>
#include <QWindow>
#include <QTemporaryFile>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QToolBar>
#include <QToolTip>
#include <QUndoStack>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>

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

    {
        check(UiScale::instance()->basePx() == ui::kFontDefaultPx, QStringLiteral("默认界面字号 22px"));
        ui::RadarImportDialog dlg(QStringLiteral("radar.mat"), QStringLiteral("Radar"), QColor("#FF0000"),
                                  QStringList() << QStringLiteral("已有名称"), w);
        QLineEdit *name = dlg.findChild<QLineEdit *>(QStringLiteral("radarImportName"));
        QDialogButtonBox *buttons = dlg.findChild<QDialogButtonBox *>();
        QPushButton *blue = dlg.findChild<QPushButton *>(QStringLiteral("radarColor_7"));
        check(name && buttons && blue, QStringLiteral("雷达导入包含名称、标准色和确认控件"));
        if (name && buttons && blue) {
            name->setText(QStringLiteral("   "));
            check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), QStringLiteral("雷达名称不能为空"));
            name->setText(QStringLiteral("已有名称"));
            check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), QStringLiteral("雷达批次名称不能重复"));
            name->setText(QStringLiteral("  东部雷达  "));
            blue->click();
            check(dlg.trackName() == QStringLiteral("东部雷达") && dlg.trackColor() == QColor("#0070C0")
                  && buttons->button(QDialogButtonBox::Ok)->isEnabled(), QStringLiteral("雷达自定义名称与标准蓝色选择"));
            if (qEnvironmentVariableIsSet("RV_SHOT")) {
                dlg.show();
                QCoreApplication::processEvents();
                dlg.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/radar-import-preview.png"));
            }
            buttons->button(QDialogButtonBox::Cancel)->click();
            check(dlg.result() == QDialog::Rejected, QStringLiteral("取消导入对话框"));
        }
    }

    {
        ui::RadarImportDialog dlg(QStringLiteral("adsb.csv"), QStringLiteral("上午 ADS-B"), QColor(),
                                  QStringList() << QStringLiteral("已有批次"), w, true);
        QLineEdit *name = dlg.findChild<QLineEdit *>(QStringLiteral("radarImportName"));
        QDialogButtonBox *buttons = dlg.findChild<QDialogButtonBox *>();
        check(name && buttons && !dlg.findChild<QPushButton *>(QStringLiteral("radarColor_0")),
              QStringLiteral("ADS-B 导入只选择批次名称，不显示颜色盘"));
        if (name && buttons) {
            name->setText(QStringLiteral("已有批次"));
            check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), QStringLiteral("ADS-B 拒绝重复批次名"));
            name->setText(QStringLiteral("  下午 ADS-B  "));
            check(dlg.trackName() == QStringLiteral("下午 ADS-B") && buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                  QStringLiteral("ADS-B 接受自定义中文批次名并去掉首尾空格"));
            if (qEnvironmentVariableIsSet("RV_SHOT")) {
                dlg.show();
                QCoreApplication::processEvents();
                dlg.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/adsb-import-preview.png"));
            }
            dlg.reject();
        }
        QTemporaryFile csv(QCoreApplication::applicationDirPath() + QStringLiteral("/named-adsb-XXXXXX.csv"));
        check(csv.open(), QStringLiteral("创建 ADS-B 命名回归数据"));
        csv.write("ABC123,30,120,90,10000,250,,RX,A320,B-1234,2026-04-27 10:00:00,PEK,SHA,CA1234,,0,CCA1234,,AirChina\n");
        csv.close();
        TrackStore named;
        for (const QString &batch : { QStringLiteral("上午 ADS-B"), QStringLiteral("下午 ADS-B") }) {
            TrackImporter importer(TrackSource::Adsb, csv.fileName(), batch);
            importer.start();
            importer.wait();
            const bool ok = importer.ok() && importer.tracks().size() == 1 && importer.tracks().first().fileName == batch;
            check(ok, QStringLiteral("导入器保留批次名：%1").arg(batch));
            if (ok) {
                named.addTracks(importer.tracks());
                DbSaveJob save(batch, TrackSource::Adsb, importer.tracks());
                save.start();
                save.wait();
                check(save.ok(), QStringLiteral("自定义 ADS-B 批次入库：%1").arg(batch));
            }
        }
        check(named.size() == 2 && named.tracks()[0].key() != named.tracks()[1].key(),
              QStringLiteral("相同 ICAO 的两批 ADS-B 航迹保持独立"));
        const QVector<Track> restored = trackdb::loadAll();
        check(restored.size() == 2 && restored[0].fileName != restored[1].fileName,
              QStringLiteral("数据库恢复后仍保留两个独立自定义批次"));
        for (const BatchInfo &batch : trackdb::batches())
            trackdb::deleteBatch(batch.id);
    }

    {
        SettingsPanel *sp = w->settingsPanel();
        QSpinBox *font = w->findChild<QSpinBox *>(QStringLiteral("menuFontSize"));
        QDoubleSpinBox *width = w->findChild<QDoubleSpinBox *>(QStringLiteral("menuLineWidth"));
        QDoubleSpinBox *dots = w->findChild<QDoubleSpinBox *>(QStringLiteral("menuPointScale"));
        check(font && width && dots && w->findChild<QMenu *>(QStringLiteral("displaySettingsMenu")),
              QStringLiteral("顶部设置菜单包含字号、线宽、点迹大小"));
        if (font && width && dots) {
            check(width->value() == 3.0 && g->trackLayer()->lineFullWidthPx() == 3.0f, QStringLiteral("默认航迹线宽加粗为 3px"));
            width->setValue(5.0);
            dots->setValue(1.8);
            font->setValue(24);
            check(sp->lineWidthValue() == 5.0 && g->trackLayer()->lineFullWidthPx() == 5.0f
                  && std::fabs(g->trackLayer()->pointDotPx() - 12.6f) < 0.01f && sp->fontValue() == 24
                  && app::settings().value(QStringLiteral("display.line_width")).toDouble() == 5.0,
                  QStringLiteral("菜单设置即时应用到地图、侧栏和持久化配置"));
            sp->setLineWidthValue(3.0);
            sp->setPointScaleValue(1.0);
            sp->setFontValue(ui::kFontDefaultPx);
            check(width->value() == 3.0 && dots->value() == 1.0 && font->value() == ui::kFontDefaultPx,
                  QStringLiteral("侧栏设置同步回顶部菜单"));
            check(font->cleanText() == QStringLiteral("22") && width->cleanText() == QStringLiteral("3.0")
                  && dots->cleanText() == QStringLiteral("1.0"), QStringLiteral("设置数字不受系统区域设置影响"));
            if (qEnvironmentVariableIsSet("RV_SHOT")) {
                QMenu *menu = w->findChild<QMenu *>(QStringLiteral("displaySettingsMenu"));
                menu->popup(w->mapToGlobal(QPoint(150, 35)));
                QCoreApplication::processEvents();
                menu->grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/settings-menu-preview.png"));
                menu->hide();
            }
        }
    }

    // ---- 测地圆 / 绘图数据 / 距离圆弹窗（纯逻辑，不依赖 GL）----
    {
        // 正算与反算必须互逆：沿 45° 走 100 km，反算回来还是 100 km / 45°
        double lat2 = 0.0, lon2 = 0.0;
        const bool ok = geocalc::destination(30.0, 120.0, 45.0, 100.0, &lat2, &lon2);
        const double back = geocalc::vincentyKm(30.0, 120.0, lat2, lon2);
        // 反算得到的方位是「回程起始方位」= 去程终点方位 + 180°，与去程方位差一个子午线
        // 收敛角（30°N 走 100 km 约 0.15°），所以只能断言到 1° 量级，不能拿 45° 直接比
        const double rev = geocalc::initialBearing(lat2, lon2, 30.0, 120.0);
        check(ok && std::fabs(back - 100.0) < 0.01 && std::fabs(rev - 225.0) < 1.0
              && lat2 > 30.0 && lon2 > 120.0,
              QStringLiteral("测地正算：100 km / 45° → (%1, %2)，反算 %3 km / 回程 %4°")
                  .arg(lat2, 0, 'f', 4).arg(lon2, 0, 'f', 4).arg(back, 0, 'f', 3).arg(rev, 0, 'f', 3));

        // 等距圆：一圈点到圆心的测地距离都等于半径（不是经纬度平面上的椭圆）
        const QVector<QPointF> ring = geocalc::geodesicCircle(31.2, 121.5, 250.0, 180);
        double maxErr = 0.0;
        for (const QPointF &p : ring)
            maxErr = qMax(maxErr, std::fabs(geocalc::vincentyKm(31.2, 121.5, p.y(), p.x()) - 250.0));
        check(ring.size() == 180 && maxErr < 0.01,
              QStringLiteral("测地圆：%1 点，半径误差 %2 m").arg(ring.size()).arg(maxErr * 1000.0, 0, 'f', 2));

        DrawingManager dm;
        const int cid = dm.addCircle(31.2, 121.5, 250.0, QColor("#0070C0"), QStringLiteral("旗标 1"));
        const DrawingFeature *cf = dm.feature(cid);
        check(cid > 0 && dm.count() == 1 && cf && cf->pts.size() == 181 && cf->pts.first() == cf->pts.last()
              && cf->kind == DrawKind::Circle && cf->widthPx == 3.0,
              QStringLiteral("距离圆要素：%1 个顶点，首尾闭合").arg(cf ? cf->pts.size() : -1));

        // GeoJSON 往返：颜色、半径、圆心、顶点数都不能丢
        DrawingManager dm2;
        QString err;
        const bool loaded = dm2.fromJson(dm.toJson(), &err);
        const DrawingFeature *cf2 = loaded ? dm2.feature(cid) : nullptr;
        check(loaded && dm2.count() == 1 && cf2 && cf2->pts.size() == 181
              && cf2->color == QColor("#0070C0") && std::fabs(cf2->radiusKm - 250.0) < 1e-6
              && std::fabs(cf2->centerLat - 31.2) < 1e-6 && cf2->kind == DrawKind::Circle,
              QStringLiteral("GeoJSON 往返：%1 个要素 %2").arg(dm2.count()).arg(err));
        check(dm2.removeFeature(cid) && dm2.count() == 0, QStringLiteral("删除绘图要素"));

        // 站点圆点（kind=dot）：单个锚点，屏幕尺寸固定，导入导出都要认
        QJsonObject dotRoot;
        {
            QJsonArray coords;
            coords.append(92.5);
            coords.append(31.45);
            QJsonObject geom;
            geom.insert(QStringLiteral("type"), QStringLiteral("Point"));
            geom.insert(QStringLiteral("coordinates"), coords);
            QJsonObject props;
            props.insert(QStringLiteral("id"), 9);
            props.insert(QStringLiteral("kind"), QStringLiteral("dot"));
            props.insert(QStringLiteral("color"), QStringLiteral("#FF3B30"));
            props.insert(QStringLiteral("width"), 1.5);
            props.insert(QStringLiteral("fill"), QStringLiteral("#FF3B30"));
            QJsonObject feat;
            feat.insert(QStringLiteral("type"), QStringLiteral("Feature"));
            feat.insert(QStringLiteral("geometry"), geom);
            feat.insert(QStringLiteral("properties"), props);
            QJsonArray feats;
            feats.append(feat);
            dotRoot.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
            dotRoot.insert(QStringLiteral("features"), feats);
        }
        DrawingManager dm3;
        QString dotErr;
        const bool dotLoaded = dm3.fromJson(dotRoot, &dotErr);
        const DrawingFeature *dot = dotLoaded && dm3.count() == 1 ? &dm3.features().first() : nullptr;
        check(dot && dot->kind == DrawKind::Dot && dot->pts.size() == 1
              && std::fabs(dot->pts.first().x() - 92.5) < 1e-6
              && std::fabs(dot->pts.first().y() - 31.45) < 1e-6,
              QStringLiteral("站点圆点导入：%1").arg(dotErr));
        const QJsonObject dotBack = dm3.toJson().value(QStringLiteral("features")).toArray().first().toObject();
        check(dotBack.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("type")).toString()
                  == QLatin1String("Point")
              && dotBack.value(QStringLiteral("properties")).toObject().value(QStringLiteral("kind")).toString()
                  == QLatin1String("dot"),
              QStringLiteral("站点圆点导出为 Point + kind=dot"));

        // 距离圆弹窗：版式与导入弹窗一致，复用同一套标准色盘
        ui::DistanceCircleDialog dlg(QStringLiteral("旗标 1"), 31.2, 121.5, QColor("#FFC000"), w);
        QDoubleSpinBox *radius = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("circleRadius"));
        QDialogButtonBox *buttons = dlg.findChild<QDialogButtonBox *>();
        QPushButton *blue = dlg.findChild<QPushButton *>(QStringLiteral("radarColor_7"));
        check(radius && buttons && blue, QStringLiteral("距离圆弹窗包含半径、标准色盘和确认控件"));
        if (radius && buttons && blue) {
            radius->setValue(250.0);
            blue->click();
            check(dlg.radiusKm() == 250.0 && dlg.color() == QColor("#0070C0")
                  && buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                  QStringLiteral("距离圆弹窗：半径 %1 km、颜色 %2").arg(dlg.radiusKm()).arg(dlg.color().name().toUpper()));
            radius->setMinimum(0.0);
            radius->setValue(0.0);
            check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), QStringLiteral("距离圆半径 0 时禁止提交"));
            dlg.reject();
        }
    }

    // ---- 直线 / 矩形 / 多边形 / 文本 / 填充（纯逻辑）----
    {
        // 大圆加密：两点之间插值出密集顶点，端点保持不动
        const QVector<QPointF> path = geocalc::geodesicPath(30.0, 120.0, 31.0, 121.0);
        // 约 1.4° 的边按 0.25° 步长 -> 6 段 7 点；断言端点不变、点数够密
        check(path.size() >= 6 && std::fabs(path.first().x() - 120.0) < 1e-6
              && std::fabs(path.last().x() - 121.0) < 1e-6
              && std::fabs(path.last().y() - 31.0) < 1e-6,
              QStringLiteral("大圆加密：%1 个顶点").arg(path.size()));

        DrawingManager mgr;
        const int lid = mgr.addLine(30, 120, 31, 121, QColor("#00B0F0"));
        const int rid = mgr.addRect(28, 118, 32, 122, QColor("#00B050"));
        QVector<QPointF> verts;
        verts << QPointF(119, 29) << QPointF(121, 29) << QPointF(120, 30.5);
        const int pid = mgr.addPolygon(verts, QColor("#FF0000"));
        const int tid = mgr.addText(30, 120, QStringLiteral("重点区域"), QColor("#FFFF00"), 20);
        check(lid && rid && pid && tid && mgr.count() == 4, QStringLiteral("直线/矩形/多边形/文本各建一个"));
        const DrawingFeature *lf = mgr.feature(lid);
        const DrawingFeature *rf = mgr.feature(rid);
        const DrawingFeature *tf = mgr.feature(tid);
        check(lf && lf->kind == DrawKind::Line && lf->pts.size() > 2,
              QStringLiteral("直线按大圆加密成 %1 个顶点").arg(lf ? lf->pts.size() : -1));
        check(rf && rf->kind == DrawKind::Rect && rf->isClosed() && rf->pts.first() == rf->pts.last(),
              QStringLiteral("矩形闭合，%1 个顶点").arg(rf ? rf->pts.size() : -1));
        check(tf && tf->isText() && tf->text == QStringLiteral("重点区域") && tf->pts.size() == 1,
              QStringLiteral("文本框保存锚点与内容"));

        // 油漆桶：填充色带 alpha；GeoJSON 往返不能丢透明度、文本和字号
        check(mgr.setFill(rid, QColor(255, 192, 0, 90)), QStringLiteral("矩形填充"));
        DrawingManager mgr2;
        QString err;
        const bool ok = mgr2.fromJson(mgr.toJson(), &err);
        const DrawingFeature *rf2 = ok ? mgr2.feature(rid) : nullptr;
        const DrawingFeature *tf2 = ok ? mgr2.feature(tid) : nullptr;
        check(ok && mgr2.count() == 4 && rf2 && rf2->fill.isValid()
              && qAbs(rf2->fill.alpha() - 90) <= 1 && tf2 && tf2->text == QStringLiteral("重点区域")
              && tf2->textSizePx == 20,
              QStringLiteral("GeoJSON 往返：填充 alpha %1、文本字号 %2 %3")
                  .arg(rf2 ? rf2->fill.alpha() : -1).arg(tf2 ? int(tf2->textSizePx) : -1).arg(err));
    }

    // ---- 撤销/重做、打断/裁剪、圆的往返（纯逻辑）----
    {
        // 大圆交点：东西向与南北向两段交于 (30,120)
        const QVector<QPointF> x = geocalc::greatCircleIntersections(30, 119, 30, 121, 29, 120, 31, 120);
        const double hx = x.isEmpty() ? 0.0 : x.first().x();
        const double hy = x.isEmpty() ? 0.0 : x.first().y();
        // 经度必须正好是 120；纬度比 30 略高（≈30.0038°）——因为东西向那段是大圆不是纬圈，
        // 中点会向极地鼓出 tan30°/cos1°，这正好反证交点是按大圆算的，不是按经纬度直线算的
        check(x.size() == 1 && std::fabs(hx - 120.0) < 1e-6
              && hy > 30.0 && hy < 30.01,
              QStringLiteral("大圆交点：%1 个，(%2, %3)（大圆向北鼓出）").arg(x.size())
                  .arg(hx, 0, 'f', 4).arg(hy, 0, 'f', 4));
        // 交点在端点上也必须认（T 形相接）
        check(geocalc::greatCircleIntersections(30, 120, 30, 122, 29, 120, 31, 120).size() == 1,
              QStringLiteral("大圆交点：T 形相接（交点在端点）"));
        // 两条弧段范围外不相交
        check(geocalc::greatCircleIntersections(30, 119, 30, 121, 32, 119, 32, 121).isEmpty(),
              QStringLiteral("大圆交点：两段不相交时返回空"));

        QUndoStack stack;
        stack.setUndoLimit(200);
        DrawingManager um;
        um.setUndoStack(&stack);
        const int u1 = um.addLine(30, 120, 31, 121, QColor("#00B0F0"));
        check(um.count() == 1 && stack.canUndo(), QStringLiteral("撤销栈：画线后可以撤销"));
        stack.undo();
        check(um.count() == 0, QStringLiteral("撤销栈：撤销后剩 %1 个图形").arg(um.count()));
        stack.redo();
        check(um.count() == 1 && um.feature(u1) != nullptr && um.feature(u1)->id == u1,
              QStringLiteral("撤销栈：重做后同一个 id 回来了"));

        // 填充 -> 撤销 -> 重做
        const int ur = um.addRect(28, 118, 32, 122, QColor("#00B050"));
        um.setFill(ur, QColor(0, 176, 80, 90));
        check(um.feature(ur) && um.feature(ur)->fill.isValid(), QStringLiteral("撤销栈：矩形已填充"));
        stack.undo();
        check(um.feature(ur) && !um.feature(ur)->fill.isValid(),
              QStringLiteral("撤销栈：撤销填充后填充色消失"));
        stack.redo();
        check(um.feature(ur) && um.feature(ur)->fill.isValid(),
              QStringLiteral("撤销栈：重做后填充色回来"));

        // 改属性（颜色 + 线宽）也要能撤销
        DrawingFeature styled = *um.feature(ur);
        styled.color = QColor("#FF0000");
        styled.widthPx = 7.0;
        um.updateFeature(styled);
        check(um.feature(ur)->color == QColor("#FF0000") && um.feature(ur)->widthPx == 7.0,
              QStringLiteral("撤销栈：改颜色和线宽生效"));
        stack.undo();
        check(um.feature(ur)->widthPx != 7.0, QStringLiteral("撤销栈：撤销改属性"));

        // 删除 -> 撤销：位置和 id 都要还原
        um.removeFeature(ur);
        check(um.count() == 1, QStringLiteral("撤销栈：删除后剩 %1 个").arg(um.count()));
        stack.undo();
        check(um.count() == 2 && um.feature(ur) != nullptr && um.indexOf(ur) == 1,
              QStringLiteral("撤销栈：撤销删除后原位置还原"));

        // 打断：一条直线在中间顶点处一分为二，撤销还原成一条
        DrawingManager bm;
        QUndoStack bstack;
        bm.setUndoStack(&bstack);
        const int bl = bm.addLine(30, 119, 30, 121, QColor("#FF4444"));
        const int bv = bm.nearestVertex(bl, 30.0, 120.0);
        const int btotal = bm.feature(bl) ? bm.feature(bl)->pts.size() : 0;
        check(bv > 0 && bv < btotal - 1, QStringLiteral("打断：找到中间的断点（第 %1/%2 个顶点）")
              .arg(bv).arg(btotal));
        check(bm.breakFeature(bl, bv) && bm.count() == 2,
              QStringLiteral("打断：一条线变成 %1 段").arg(bm.count()));
        bstack.undo();
        check(bm.count() == 1 && bm.feature(bl) != nullptr && bm.feature(bl)->pts.size() == btotal,
              QStringLiteral("打断：撤销后还原成一条"));

        // 闭合图形（矩形）也能打断：剪开后变成一条首尾在断点重合的开放曲线，并留下断点标记
        DrawingManager rm;
        QUndoStack rstack;
        rm.setUndoStack(&rstack);
        const int rectId = rm.addRect(28, 118, 32, 122, QColor("#00B050"));
        const int rv = rm.nearestVertex(rectId, 32.0, 120.0);      // 北边中间那个顶点
        check(rm.canBreak(rectId), QStringLiteral("打断：闭合图形也可以打断"));
        check(rm.breakFeature(rectId, rv) && rm.count() == 1,
              QStringLiteral("打断闭合图形：剪开后仍是 %1 个要素").arg(rm.count()));
        {
            const DrawingFeature &opened = rm.features().first();
            check(opened.kind == DrawKind::Polyline && !opened.isClosed()
                  && opened.pts.first() == opened.pts.last() && opened.marks.size() == 1,
                  QStringLiteral("打断闭合图形：首尾在断点重合，带 %1 个断点标记")
                      .arg(opened.marks.size()));
            // 断点标记要能存进 GeoJSON 再读回来
            DrawingManager rm2;
            QString rerr;
            rm2.fromJson(rm.toJson(), &rerr);
            check(rm2.count() == 1 && rm2.features().first().marks.size() == 1,
                  QStringLiteral("断点标记随 GeoJSON 存取：%1 个 %2")
                      .arg(rm2.count()).arg(rerr));
        }
        rstack.undo();
        check(rm.count() == 1 && rm.features().first().isClosed(),
              QStringLiteral("打断闭合图形：撤销后恢复成闭合矩形"));

        // 闭合图形裁剪：横线穿过矩形，剪掉中间一段，剩一条开放曲线 + 两个断点
        DrawingManager cm1;
        QUndoStack cstack1;
        cm1.setUndoStack(&cstack1);
        cm1.addRect(28, 118, 32, 122, QColor("#00B050"));
        const int cut2 = cm1.trimCrossing(30, 116, 30, 124);
        check(cut2 == 1 && cm1.count() == 1 && !cm1.features().first().isClosed()
              && cm1.features().first().marks.size() == 2,
              QStringLiteral("裁剪闭合图形：剩 %1 段、%2 个断点").arg(cm1.count())
                  .arg(cm1.features().isEmpty() ? -1 : cm1.features().first().marks.size()));

        // 裁剪：U 形折线被一条横线穿过两次，中间那段被删掉，剩下两个残段
        DrawingManager tm;
        QUndoStack tstack;
        tm.setUndoStack(&tstack);
        QVector<QPointF> u;
        u << QPointF(119, 30) << QPointF(119, 32) << QPointF(121, 32) << QPointF(121, 30);
        const int ul = tm.addPolyline(u, QColor("#FF4444"));
        check(ul > 0, QStringLiteral("裁剪：建好 U 形折线"));
        const int cut = tm.trimCrossing(31, 118, 31, 122);
        check(cut == 1 && tm.count() == 2,
              QStringLiteral("裁剪：切掉中间一段后剩 %1 段").arg(tm.count()));
        if (tm.count() == 2) {
            int total = 0;
            for (const DrawingFeature &f : tm.features())
                total += f.pts.size();
            check(total <= 5, QStringLiteral("裁剪：中间的点被删掉了（剩 %1 个顶点）").arg(total));
        }
        tstack.undo();
        check(tm.count() == 1 && tm.feature(ul) && tm.feature(ul)->pts.size() == 4,
              QStringLiteral("裁剪：撤销后还原成原来的 U 形"));

        // 圆的 GeoJSON 往返：写 shapeType/radius，导入后仍是圆心+半径，半径还能再调
        DrawingManager cm;
        const int cc = cm.addCircle(31.2, 121.5, 250.0, QColor("#0070C0"));
        const QJsonObject cj = cm.toJson();
        const QJsonObject cp = cj.value(QStringLiteral("features")).toArray().at(0)
                                 .toObject().value(QStringLiteral("properties")).toObject();
        check(cp.value(QStringLiteral("shapeType")).toString() == QLatin1String("circle")
              && std::fabs(cp.value(QStringLiteral("radius")).toDouble() - 250.0) < 1e-9,
              QStringLiteral("圆序列化：shapeType=circle、radius=%1")
                  .arg(cp.value(QStringLiteral("radius")).toDouble()));
        DrawingManager cm2;
        QString cerr;
        cm2.fromJson(cj, &cerr);
        const DrawingFeature *cc2 = cm2.feature(cc);
        check(cc2 && cc2->isCircle() && cc2->pts.size() == 181
              && std::fabs(cc2->radiusKm - 250.0) < 1e-6,
              QStringLiteral("圆导入：还原成圆心+半径，%1 个顶点 %2")
                  .arg(cc2 ? cc2->pts.size() : -1).arg(cerr));
        const bool grown = cm2.setCircleRadius(cc, 500.0);
        const DrawingFeature *cc3 = cm2.feature(cc);
        const double r = (cc3 && cc3->pts.size() > 10)
            ? geocalc::vincentyKm(31.2, 121.5, cc3->pts.at(10).y(), cc3->pts.at(10).x()) : 0.0;
        check(grown && cc3 && std::fabs(cc3->radiusKm - 500.0) < 1e-9 && std::fabs(r - 500.0) < 0.01,
              QStringLiteral("圆导入：半径改成 500 km 后点环仍是等距圆（实测 %1 km）").arg(r, 0, 'f', 3));
    }

    // ---- 空间套索几何自检（纯函数，不依赖 GL）----
    {
        QVector<LassoVertex> poly;
        poly << LassoVertex(0, 30.0, 100.0) << LassoVertex(1, 30.0, 120.0)
             << LassoVertex(2, 40.0, 120.0) << LassoVertex(3, 40.0, 100.0);
        check(lassogeo::pointInPolygon(35.0, 110.0, poly), QStringLiteral("套索：多边形内部点判定"));
        check(!lassogeo::pointInPolygon(50.0, 110.0, poly), QStringLiteral("套索：多边形外部点判定"));
        check(lassogeo::pointInPolygon(35.0, 110.0, poly) == !lassogeo::pointInPolygon(35.0, 90.0, poly),
              QStringLiteral("套索：内外点判定互补"));

        Track inside;
        TrackPoint p1;
        p1.lat = 35.0;
        p1.lon = 110.0;
        inside.points.append(p1);
        check(lassogeo::trackIntersectsPolygon(inside, poly), QStringLiteral("套索：航迹点落入多边形"));

        Track outside;
        TrackPoint p2;
        p2.lat = 0.0;
        p2.lon = 0.0;
        outside.points.append(p2);
        check(!lassogeo::trackIntersectsPolygon(outside, poly), QStringLiteral("套索：航迹完全在外"));

        // 两端在框外、中间穿过：靠线段相交判定
        Track crossing;
        TrackPoint a, b;
        a.lat = 35.0;
        a.lon = 90.0;
        b.lat = 35.0;
        b.lon = 130.0;
        crossing.points << a << b;
        check(lassogeo::trackIntersectsPolygon(crossing, poly), QStringLiteral("套索：航迹线段穿越多边形"));

        check(lassogeo::polygonAreaSqKm(poly) > 1.0, QStringLiteral("套索：面积计算 %1 km²")
                                                         .arg(lassogeo::polygonAreaSqKm(poly), 0, 'f', 1));
        check(lassogeo::polygonPerimeterKm(poly) > 1000.0,
              QStringLiteral("套索：周长计算 %1 km").arg(lassogeo::polygonPerimeterKm(poly), 0, 'f', 1));

        // 简化：给多边形加密，简化后顶点数应减少但不少于 3
        QVector<LassoVertex> dense;
        for (int i = 0; i < 100; ++i) {
            const double t = i / 100.0;
            dense.append(LassoVertex(i, 30.0 + 10.0 * t, 100.0));
        }
        dense << LassoVertex(100, 40.0, 120.0) << LassoVertex(101, 30.0, 120.0);
        const QVector<LassoVertex> simplified = lassogeo::simplifyPolygon(dense, 5.0);
        check(simplified.size() >= 3 && simplified.size() < dense.size(),
              QStringLiteral("套索：多边形简化 %1 → %2 顶点").arg(dense.size()).arg(simplified.size()));

        // 状态机：激活 → 加点 → 闭合
        SpatialLasso lasso;
        lasso.activate();
        lasso.addVertex(30.0, 100.0);
        lasso.addVertex(30.0, 120.0);
        lasso.addVertex(40.0, 120.0);
        lasso.closePolygon();
        check(lasso.isClosed() && !lasso.isActive() && lasso.vertices().size() == 3,
              QStringLiteral("套索：激活/加点/闭合状态机"));

        // 自由手绘：起笔 → 轨迹点 → 松手自动闭合
        SpatialLasso freehand;
        freehand.beginFreehand(30.0, 100.0);
        freehand.addFreehandPoint(30.0, 120.0);
        freehand.addFreehandPoint(40.0, 120.0);
        freehand.endFreehand();
        check(freehand.isClosed() && !freehand.isActive() && freehand.vertices().size() == 3,
              QStringLiteral("套索：自由手绘松手自动闭合"));

        // 地心纬度 / 大地纬度：geodesicLines 直接 asin(z) 会把整条线南偏约 0.2°（≈21 km），
        // 表现为「连线与顶点错位、看着在框内的航迹却没被选中」
        {
            const double latIn = 39.9, lonIn = 116.4;
            const QVector3D d = geo::ecefDeg(lonIn, latIn, 0.0).normalized();
            const double p = std::sqrt(double(d.x()) * d.x() + double(d.y()) * d.y());
            const double latOk = qRadiansToDegrees(geo::geodeticLatFromDir(double(d.z()), p));
            const double latNaive = qRadiansToDegrees(std::asin(qBound(-1.0, double(d.z()), 1.0)));
            check(std::fabs(latOk - latIn) < 1e-6 && std::fabs(latNaive - latIn) > 0.1,
                  QStringLiteral("地心→大地纬度 %1°（旧算法 %2°，偏差 %3 km）")
                      .arg(latOk, 0, 'f', 6).arg(latNaive, 0, 'f', 4)
                      .arg(std::fabs(latNaive - latIn) * 111.0, 0, 'f', 1));
        }
    }

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

    // 等至少再画 n 帧：叠加层顶点计数在 paintGL 后才更新
    auto waitFrames = std::make_shared<std::function<void(int, std::function<void()>)>>();
    *waitFrames = [g](int n, std::function<void()> next) {
        const quint64 target = g->frameCount() + quint64(n);
        QElapsedTimer t;
        t.start();
        auto poll = std::make_shared<std::function<void()>>();
        *poll = [g, target, next, poll, t]() {
            if (g->frameCount() >= target || t.elapsed() > 5000) {
                next();
                return;
            }
            g->update();
            QTimer::singleShot(30, *poll);
        };
        QTimer::singleShot(30, *poll);
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

        // 缩放到约 10 km 高度，应细分到瓦片最大层级。
        // 近岸走廊类的离线包只在海岸带存到高层级，内陆本来就取不到瓦片，
        // 所以判据放宽到 min(maxZoom, 14)：仍能证明放大时确实在向下细分，
        // 又不会因为瓦片覆盖不全而误报。
        g->resetView();
        g->zoomBy(30);
        (*waitIdle)([=]() {
            st->out << "zoomed: " << g->debugInfo() << "\n";
            const int maxZ = g->tileMaxZoom();
            const int wantZ = qMin(maxZ, 14);
            check(maxZ > 0 && g->drawnMaxZoom() >= wantZ,
                  QStringLiteral("拉近后绘制层级 %1（瓦片最大 %2，期望 ≥ %3）")
                      .arg(g->drawnMaxZoom()).arg(maxZ).arg(wantZ));
            check(g->altitudeKm() < 50.0, QStringLiteral("拉近后高度 %1 km").arg(g->altitudeKm(), 0, 'f', 1));
            check(!g->isLoading(), QStringLiteral("瓦片加载完成"));

            g->zoomBy(-60);
            (*waitIdle)([=]() {
                st->out << "zoomed out: " << g->debugInfo() << "\n";
                check(g->drawnMaxZoom() <= 3, QStringLiteral("拉远后绘制层级 %1").arg(g->drawnMaxZoom()));
                check(g->glErrorCount() == 0, QStringLiteral("OpenGL 错误数 %1").arg(g->glErrorCount()));

                // 空间套索：激活 → 三次单击放点 → 双击闭合 → 叠加层画出顶点
                SpatialLasso *lasso = w->spatialLasso();
                lasso->activate();
                auto sendClick = [g](const QPointF &p) {
                    QMouseEvent pr(QEvent::MouseButtonPress, p, g->mapToGlobal(p.toPoint()),
                                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(g, &pr);
                    QMouseEvent rl(QEvent::MouseButtonRelease, p, g->mapToGlobal(p.toPoint()),
                                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(g, &rl);
                };
                const QPointF lp1(g->width() * 0.44, g->height() * 0.44);
                const QPointF lp2(g->width() * 0.56, g->height() * 0.44);
                const QPointF lp3(g->width() * 0.56, g->height() * 0.56);
                sendClick(lp1);
                sendClick(lp2);
                sendClick(lp3);
                check(lasso->vertices().size() == 3,
                      QStringLiteral("套索：三次单击放下 %1 个顶点").arg(lasso->vertices().size()));
                QMouseEvent dbl(QEvent::MouseButtonDblClick, lp3, g->mapToGlobal(lp3.toPoint()),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(g, &dbl);
                check(lasso->isClosed() && !lasso->isActive(),
                      QStringLiteral("套索：双击闭合后退出绘制"));
                // 顶点回投影必须落在点击处（否则叠加层会与鼠标位置错位）
                {
                    const QVector<LassoVertex> &vv = lasso->vertices();
                    const QPointF clicks[3] = { lp1, lp2, lp3 };
                    QStringList diffs;
                    bool same = vv.size() >= 3;
                    for (int i = 0; i < 3 && i < vv.size(); ++i) {
                        QPointF back;
                        if (!g->screenPos(vv[i].lon, vv[i].lat, 0.0, &back)) {
                            same = false;
                            diffs << QStringLiteral("不可投影");
                            continue;
                        }
                        const QPointF d = back - clicks[i];
                        if (d.manhattanLength() > 3)
                            same = false;
                        diffs << QStringLiteral("(%1,%2)Δ(%3,%4)").arg(back.x(), 0, 'f', 1).arg(back.y(), 0, 'f', 1)
                                                                     .arg(d.x(), 0, 'f', 1).arg(d.y(), 0, 'f', 1);
                    }
                    check(same, QStringLiteral("套索：顶点回投影与点击位置一致 %1").arg(diffs.join(QStringLiteral(" "))));
                }
                // 边界采样必须从顶点出发：填充、边线、查询多边形共用同一批点，不能各画各的
                {
                    const QVector<LassoVertex> &vv = lasso->vertices();
                    bool onVertex = vv.size() >= 3;
                    for (int i = 0; i < vv.size() && onVertex; ++i) {
                        const int j = (i + 1) % vv.size();
                        QVector<QPointF> pts;
                        QVector<bool> vis;
                        g->sampleLonLatLine(vv[i].lat, vv[i].lon, vv[j].lat, vv[j].lon, &pts, &vis);
                        QPointF a;
                        onVertex = g->screenPos(vv[i].lon, vv[i].lat, 0.0, &a) && !pts.isEmpty()
                                   && (pts.first() - a).manhattanLength() <= 1.0;
                    }
                    check(onVertex, QStringLiteral("套索：边界采样从顶点出发（看到的边界=查询边界）"));
                }
                (*waitFrames)(2, [=]() {
                    check(g->lassoVerticesDrawn() >= 3,
                          QStringLiteral("套索：叠加层画出 %1 个顶点").arg(g->lassoVerticesDrawn()));
                    // 清理测试多边形，避免写进用户设置
                    w->filterPanel()->clickSpatialClear();
                    lasso->clearAll();
                    // 绘图工具栏默认收起：先记下状态，点开后再检查（两行按钮）
                    QToolBar *bar0 = w->findChild<QToolBar *>(QStringLiteral("drawToolBar"));
                    const bool drawBarCollapsed = bar0 && bar0->isHidden();
                    w->setDrawingBarVisible(true);
                    // 贴地圆：加到视野中心，叠加层必须画出来（背面顶点会被断开，这里整圈都可见）
                    const int cid = w->drawing()->addCircle(30.0, 110.0, 800.0, QColor("#FFC000"),
                                                            QStringLiteral("自检"));
                    check(cid > 0, QStringLiteral("距离圆加入绘图管理器"));
                    (*waitFrames)(2, [=]() {
                        check(g->featuresDrawn() == 1,
                              QStringLiteral("地图上画出距离圆 %1 个").arg(g->featuresDrawn()));
                        // 绘图工具栏平时不显示，点标题栏「帮助」右边的「绘图」才出现（一行）
                        QToolBar *bar = w->findChild<QToolBar *>(QStringLiteral("drawToolBar"));
                        check(drawBarCollapsed && bar && bar->isVisible() && bar->height() > 10
                              && bar->actions().size() >= 16
                              && w->undoStack() && w->undoStack()->undoLimit() == 200
                              && g->drawTool() == DrawTool::None,
                              QStringLiteral("绘图工具栏默认收起，点开后 %1 项，撤销上限 %2")
                                  .arg(bar ? bar->actions().size() : 0)
                                  .arg(w->undoStack() ? w->undoStack()->undoLimit() : -1));

                        auto send = [g](QEvent::Type type, const QPointF &p, Qt::MouseButton btn,
                                        Qt::MouseButtons buttons) {
                            QMouseEvent ev(type, p, g->mapToGlobal(p.toPoint()), btn, buttons, Qt::NoModifier);
                            QCoreApplication::sendEvent(g, &ev);
                        };

                        // 直线：按下 → 拖动 → 松开
                        w->setDrawTool(DrawTool::Line);
                        QPointF pa, pb;
                        if (g->screenPos(109.0, 29.0, 0.0, &pa) && g->screenPos(111.0, 31.0, 0.0, &pb)) {
                            send(QEvent::MouseButtonPress, pa, Qt::LeftButton, Qt::LeftButton);
                            send(QEvent::MouseMove, (pa + pb) / 2.0, Qt::NoButton, Qt::LeftButton);
                            send(QEvent::MouseButtonRelease, pb, Qt::LeftButton, Qt::NoButton);
                        }
                        check(w->drawing()->count() == 2,
                              QStringLiteral("直线落笔后共 %1 个图形").arg(w->drawing()->count()));

                        // 橡皮：点直线中点删掉它
                        w->setDrawTool(DrawTool::Eraser);
                        QPointF mid;
                        if (g->screenPos(110.0, 30.0, 0.0, &mid)) {
                            send(QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
                            send(QEvent::MouseButtonRelease, mid, Qt::LeftButton, Qt::NoButton);
                        }
                        check(w->drawing()->count() == 1,
                              QStringLiteral("橡皮擦掉直线后剩 %1 个").arg(w->drawing()->count()));

                        // 矩形：按住左键拖出一个框
                        w->setDrawTool(DrawTool::Rect);
                        QPointF r1, r2;
                        if (g->screenPos(108.0, 28.0, 0.0, &r1) && g->screenPos(112.0, 32.0, 0.0, &r2)) {
                            send(QEvent::MouseButtonPress, r1, Qt::LeftButton, Qt::LeftButton);
                            send(QEvent::MouseMove, r2, Qt::NoButton, Qt::LeftButton);
                            send(QEvent::MouseButtonRelease, r2, Qt::LeftButton, Qt::NoButton);
                        }
                        check(w->drawing()->count() == 2,
                              QStringLiteral("矩形落笔后共 %1 个图形").arg(w->drawing()->count()));

                        // 框选删除：整屏拉框，相交的都要删掉
                        w->setDrawTool(DrawTool::BoxSelect);
                        send(QEvent::MouseButtonPress, QPointF(2, 2), Qt::LeftButton, Qt::LeftButton);
                        send(QEvent::MouseMove, QPointF(g->width() - 2, g->height() - 2),
                             Qt::NoButton, Qt::LeftButton);
                        send(QEvent::MouseButtonRelease, QPointF(g->width() - 2, g->height() - 2),
                             Qt::LeftButton, Qt::NoButton);
                        check(w->drawing()->count() == 0,
                              QStringLiteral("框选删除后剩 %1 个图形").arg(w->drawing()->count()));

                        // 框选删除也要能撤销/重做
                        w->undoStack()->undo();
                        check(w->drawing()->count() == 2,
                              QStringLiteral("撤销框选删除后回来 %1 个图形").arg(w->drawing()->count()));
                        w->undoStack()->redo();
                        check(w->drawing()->count() == 0,
                              QStringLiteral("重做框选删除后剩 %1 个图形").arg(w->drawing()->count()));

                        // 选择 + Delete + 撤销：画一条线，点中它，删掉，再撤销回来
                        w->setDrawTool(DrawTool::Line);
                        QPointF s1, s2;
                        if (g->screenPos(108.0, 29.0, 0.0, &s1) && g->screenPos(112.0, 31.0, 0.0, &s2)) {
                            send(QEvent::MouseButtonPress, s1, Qt::LeftButton, Qt::LeftButton);
                            send(QEvent::MouseMove, s2, Qt::NoButton, Qt::LeftButton);
                            send(QEvent::MouseButtonRelease, s2, Qt::LeftButton, Qt::NoButton);
                        }
                        const int lineId = w->drawing()->count() == 1
                            ? w->drawing()->features().first().id : 0;
                        w->setDrawTool(DrawTool::Select);
                        QPointF mid2;
                        bool midOk = false;
                        if (const DrawingFeature *lf = w->drawing()->feature(lineId)) {
                            const QPointF &mp = lf->pts.at(lf->pts.size() / 2);   // 正中那个顶点，点得准
                            midOk = g->screenPos(mp.x(), mp.y(), 0.0, &mid2);
                        }
                        if (midOk) {
                            send(QEvent::MouseButtonPress, mid2, Qt::LeftButton, Qt::LeftButton);
                            send(QEvent::MouseButtonRelease, mid2, Qt::LeftButton, Qt::NoButton);
                        }
                        check(lineId > 0 && g->selectedFeature() == lineId,
                              QStringLiteral("选择工具：选中图形 %1（当前 %2）")
                                  .arg(lineId).arg(g->selectedFeature()));
                        check(g->deleteSelected() && w->drawing()->count() == 0,
                              QStringLiteral("Delete 删除选中的图形"));
                        w->undoStack()->undo();
                        check(w->drawing()->count() == 1 && g->selectedFeature() == 0,
                              QStringLiteral("撤销删除后图形回来，选中状态已清空"));

                        // 吸附：从已有端点旁边 6px 起笔，起点必须精确吸到那个端点
                        const DrawingFeature *base = w->drawing()->count() == 1
                            ? &w->drawing()->features().first() : nullptr;
                        QPointF endPos, farPos;
                        const bool canSnap = base && !base->pts.isEmpty()
                            && g->screenPos(base->pts.last().x(), base->pts.last().y(), 0.0, &endPos)
                            && g->screenPos(107.0, 28.0, 0.0, &farPos);
                        if (canSnap) {
                            const QPointF expect = base->pts.last();
                            // 注意别用 near / far 当变量名：windows.h 里是宏
                            const QPointF snapPress = endPos + QPointF(6.0, 4.0);   // 7.2px，在 10px 吸附圈内
                            w->setDrawTool(DrawTool::Line);
                            send(QEvent::MouseButtonPress, snapPress, Qt::LeftButton, Qt::LeftButton);
                            send(QEvent::MouseMove, farPos, Qt::NoButton, Qt::LeftButton);
                            send(QEvent::MouseButtonRelease, farPos, Qt::LeftButton, Qt::NoButton);
                            const DrawingFeature &added = w->drawing()->features().last();
                            check(w->drawing()->count() == 2 && added.pts.first() == expect,
                                  QStringLiteral("吸附：新线起点吸到已有端点（%1 个顶点）")
                                      .arg(added.pts.size()));
                        } else {
                            check(false, QStringLiteral("吸附：测试点不在屏幕上"));
                        }

                        w->setDrawTool(DrawTool::None);
                        finish();
                    });
                });
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
                // 在预设倍速下点「自定义...」必须出现输入框，键入后倍速生效
                sb->chooseCustomSpeed();
                check(sb->isCustomSpeedShown(), QStringLiteral("点「自定义...」后显示输入框：%1").arg(sb->speedText()));
                sb->typeCustomSpeed(QStringLiteral("37.5"));
                check(rp->speed() == 37.5 && sb->isCustomSpeedShown(),
                      QStringLiteral("自定义倍速 37.5 生效：speed=%1 显示 %2").arg(rp->speed()).arg(sb->speedText()));
                sb->chooseCustomSpeed();
                sb->typeCustomSpeed(QStringLiteral("50"));
                check(rp->speed() == 50 && !sb->isCustomSpeedShown() && sb->speedText() == QStringLiteral("50x"),
                      QStringLiteral("自定义输入预设值 50 收回下拉：%1").arg(sb->speedText()));
                rp->setSpeed(500);
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
            const int hit = g->trackAt(sp.toPoint()); // 鼠标事件使用整数像素，与实际点击位置一致
            check(hit >= 0, QStringLiteral("中心点拾取到航迹 %1").arg(hit));

            // 悬停：移动鼠标到航迹上，下一帧应画出高亮
            sendMouse(QEvent::MouseMove, sp, Qt::NoButton);
            afterFrames([=]() {
                check(layer->hoveredTrack() == hit && layer->hoverDrawn(),
                      QStringLiteral("悬停高亮 hovered=%1 drawn=%2").arg(layer->hoveredTrack()).arg(layer->hoverDrawn()));
                check(QToolTip::text().contains(QStringLiteral("font-size:14px")),
                      QStringLiteral("航迹悬停信息使用独立的 14px 字号"));
                if (qEnvironmentVariableIsSet("RV_SHOT"))
                    for (QWidget *tip : QApplication::topLevelWidgets())
                        if (tip->inherits("QTipLabel") && tip->isVisible())
                            tip->grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/hover-preview.png"));
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
            // 阈值 80 ms：同一份二进制在这台机器上实测 21–56 ms（CPU 降频/后台负载影响），
            // 30 ms 会误报；真正的回归（例如每个点都重建几何）会远高于此
            check(perPick < 80.0, QStringLiteral("全视野拾取平均 %1 ms（高度 %2 km，视口 %3×%4）")
                                      .arg(perPick, 0, 'f', 2).arg(g->altitudeKm(), 0, 'f', 1)
                                      .arg(g->width()).arg(g->height()));
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
            bool customColorApplied = false;
            for (int i = 0; i < store->size(); ++i)
                if (store->tracks()[i].source == TrackSource::Radar) {
                    customColorApplied = store->tracks()[i].fileName == QStringLiteral("Radar")
                                         && layer->trackColor(i) == QColor("#0070C0");
                    break;
                }
            check(c1 == QColor("#0070C0") && customColorApplied, QStringLiteral("雷达自定义名称与蓝色已应用到真实导入航迹"));
            check(app::settings().value(QStringLiteral("display.custom_file_color")).toString().contains(QStringLiteral("#0070c0")),
                  QStringLiteral("雷达自定义颜色已持久保存"));
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

            // 空间套索 → 航迹管理系统执行查询（是查询条件，不是切换地图可见性状态）
            {
                FilterPanel *fp0 = w->filterPanel();
                SpatialLasso *ls = w->spatialLasso();
                int ti = -1;
                for (int i = 0; i < store->size() && ti < 0; ++i)
                    if (store->tracks()[i].source == TrackSource::Adsb && store->tracks()[i].points.size() >= 100)
                        ti = i;
                const Track &tt = store->tracks()[ti];
                const TrackPoint mid = tt.points[tt.points.size() / 2];
                const double d = 0.05;      // 小方框：只圈住航迹中点附近，保证命中数远小于总数
                QVector<LassoVertex> poly;
                poly << LassoVertex(0, mid.lat - d, mid.lon - d) << LassoVertex(1, mid.lat - d, mid.lon + d)
                     << LassoVertex(2, mid.lat + d, mid.lon + d) << LassoVertex(3, mid.lat + d, mid.lon - d);
                int expect = 0;
                for (const Track &tr : store->tracks())
                    if (lassogeo::trackIntersectsPolygon(tr, poly))
                        ++expect;
                ls->activate();
                for (const LassoVertex &v : poly)
                    ls->addVertex(v.lat, v.lon);
                ls->closePolygon();        // closed() → 写筛选状态 → applyDisplay → 管理系统查询
                check(expect >= 1 && expect < store->size(),
                      QStringLiteral("套索区域命中 %1 条（库内共 %2 条）").arg(expect).arg(store->size()));
                check(mp->totalCount() == expect,
                      QStringLiteral("管理系统空间查询 共 %1 条（期望 %2）").arg(mp->totalCount()).arg(expect));
                bool allIn = mp->rowCount() > 0;
                for (const ManageRow &r : mp->rows()) {
                    const int idx = store->indexOf(r.trackKey());
                    allIn = allIn && idx >= 0 && lassogeo::trackIntersectsPolygon(store->tracks()[idx], poly);
                }
                check(allIn, QStringLiteral("管理系统本页 %1 行全部落在套索区域内").arg(mp->rowCount()));
                check(ms->visibleKeys().isEmpty() && mp->spatialChipVisible()
                          && mp->spatialChipText().contains(QString::number(expect)),
                      QStringLiteral("查询状态而非可见性：可见集合 %1 条，标签 [%2]")
                          .arg(ms->visibleKeys().size()).arg(mp->spatialChipText()));
                // 搜索叠加在空间查询之上，两者是 AND 关系
                mp->setSearchText(QStringLiteral("zzzz没有这条"));
                check(mp->totalCount() == 0 && mp->emptyText().contains(QStringLiteral("套索区域内没有匹配的航迹")),
                      QStringLiteral("空间查询 + 搜索无匹配：%1").arg(mp->emptyText()));
                mp->setSearchText(QString());
                check(mp->totalCount() == expect, QStringLiteral("清空搜索后回到空间查询结果 %1 条").arg(mp->totalCount()));
                // 大范围套索（把全部数据都圈进来）必须一条不漏
                {
                    double mnLat = 90, mxLat = -90, mnLon = 180, mxLon = -180;
                    for (const Track &tr : store->tracks())
                        for (const TrackPoint &p : tr.points) {
                            mnLat = qMin(mnLat, p.lat);
                            mxLat = qMax(mxLat, p.lat);
                            mnLon = qMin(mnLon, p.lon);
                            mxLon = qMax(mxLon, p.lon);
                        }
                    const double pad = 1.0;
                    QVector<LassoVertex> big;
                    big << LassoVertex(0, mnLat - pad, mnLon - pad) << LassoVertex(1, mnLat - pad, mxLon + pad)
                        << LassoVertex(2, mxLat + pad, mxLon + pad) << LassoVertex(3, mxLat + pad, mnLon - pad);
                    int bigExpect = 0;
                    for (const Track &tr : store->tracks())
                        if (lassogeo::trackIntersectsPolygon(tr, big))
                            ++bigExpect;
                    ls->activate();
                    for (const LassoVertex &v : big)
                        ls->addVertex(v.lat, v.lon);
                    ls->closePolygon();
                    check(bigExpect == store->size() && mp->totalCount() == store->size(),
                          QStringLiteral("大范围套索：内存命中 %1 / 面板 %2 / 总数 %3")
                              .arg(bigExpect).arg(mp->totalCount()).arg(store->size()));
                }
                // 清除空间查询 → 恢复全部
                fp0->clickSpatialClear();
                check(mp->totalCount() == store->size() && !mp->spatialChipVisible(),
                      QStringLiteral("清除空间查询后恢复 %1 条").arg(mp->totalCount()));
            }

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
                    check(layer->pointDotColor(ti) == layer->trackColor(ti), QStringLiteral("点迹使用所属航迹的实际颜色"));
                    const QString batchKey = trackSourceName(store->tracks()[ti].source) + QStringLiteral("::") + store->tracks()[ti].fileName;
                    layer->setFileColorOverride(batchKey, QColor("#0070C0"));
                    const QImage dotsImage = g->grabFramebuffer();
                    if (qEnvironmentVariableIsSet("RV_SHOT"))
                        dotsImage.save(QCoreApplication::applicationDirPath() + QStringLiteral("/dots-preview.png"));
                    check(layer->pointDotColor(ti) == QColor("#0070C0") && layer->trackColor(ti) == QColor("#0070C0"),
                          QStringLiteral("更换批次颜色后点迹与航迹一起更新"));
                    QPointF sp;
                    g->screenPos(mid.lon, mid.lat, TrackLayer::kAltitudeM, &sp);
                    sendMouse(QEvent::MouseMove, sp, Qt::NoButton);
                    int ht = -1, hp = -1;
                    g->pointHover(&ht, &hp);
                    check(ht == ti && hp >= 0, QStringLiteral("悬停点迹 航迹 %1 点 %2").arg(ht).arg(hp));
                    w->togglePointDots(ti);
                    check(!w->pointDotsShown(ti), QStringLiteral("单条查看可以临时隐藏点迹"));
                    w->isolateTrack(-1);
                    check(w->pointDotsShown(ti), QStringLiteral("返回总览恢复原点迹选择"));
                    const int other = ti == 0 ? 1 : 0;
                    w->isolateTrack(other);
                    w->onEscape();
                    check(w->pointDotsShown(ti) && layer->hasPointDots(ti), QStringLiteral("查看另一航迹并 Esc 返回仍保留原点迹"));
                    afterFrames([=]() {
                        check(w->pointDotsShown(ti) && layer->pointDotCount() == store->tracks()[ti].points.size(),
                              QStringLiteral("返回总览后实际绘制原航迹全部点迹"));
                        w->togglePointDots(ti);
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
                        // 宽松格式输入：斜杠日期、紧凑格式、只填时间、只填日期
                        {
                            const QDateTime b = QDateTime::fromMSecsSinceEpoch(lo + 8LL * 3600 * 1000, Qt::UTC);
                            const QString d1 = QLocale::c().toString(b, QStringLiteral("yyyy/M/d H:mm"));
                            const QString d2 = QLocale::c().toString(b.addSecs(3600), QStringLiteral("yyyyMMdd HHmm"));
                            fp->setInputTexts(d1, d2);
                            fp->clickApply();
                            check(fp->errorText().isEmpty() && w->displayedTrackCount() == expect,
                                  QStringLiteral("宽松格式「%1」~「%2」：%3 条（期望 %4）%5").arg(d1, d2).arg(w->displayedTrackCount())
                                      .arg(expect).arg(fp->errorText()));
                            fp->setInputTexts(QLocale::c().toString(b, QStringLiteral("yyyy-MM-dd HH:mm")), QLocale::c().toString(b.addSecs(3600), QStringLiteral("H:mm")));
                            fp->clickApply();
                            check(fp->errorText().isEmpty() && w->displayedTrackCount() == expect,
                                  QStringLiteral("结束只填时间：%1 条 %2").arg(w->displayedTrackCount()).arg(fp->errorText()));
                            fp->setInputTexts(QLocale::c().toString(b, QStringLiteral("yyyy年M月d日")), QLocale::c().toString(b, QStringLiteral("yyyy-MM-dd")));
                            fp->clickApply();
                            check(fp->errorText().isEmpty() && w->displayedTrackCount() > 0,
                                  QStringLiteral("只填日期（整天）：%1 条 %2").arg(w->displayedTrackCount()).arg(fp->errorText()));
                            fp->setInputTexts(QStringLiteral("abc"), QStringLiteral("2026-13-40"));
                            fp->clickApply();
                            check(fp->errorText().contains(QStringLiteral("格式无法识别")),
                                  QStringLiteral("非法格式提示：%1").arg(fp->errorText()));
                            fp->clickClear();
                        }
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
                                        w->activatePanel(PanelId::TimeFilter);
                                        w->filterPanel()->openCalendar(0);
                                        settle();
                                        w->grab().save(dir + QStringLiteral("/shot50_filter.png"));
                                        sp->setFontValue(14);
                                        settle();
                                        w->grab().save(dir + QStringLiteral("/shot14_filter.png"));
                                        w->filterPanel()->openCalendar(0);   // 收起
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
                                    // loadDefaultTiles 的优先级是「离线高德近岸 > natural_earth4 > maxZoom 最高」，
                                    // 打包目录里带了 amap_coast 时就该选中它（这条断言原来只认 natural_earth4，已过时）
                                    {
                                        const QString tn = g->tileName();
                                        check(tn.contains(QStringLiteral("amap"), Qt::CaseInsensitive)
                                                  || tn.contains(QStringLiteral("Natural Earth")),
                                              QStringLiteral("默认瓦片源：%1").arg(tn));
                                    }
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
        w->importFile(j.kind, j.path, j.kind == TrackSource::Radar ? QStringLiteral("Radar") : QString(),
                      j.kind == TrackSource::Radar ? QColor("#0070C0") : QColor());
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

// ---------------------------------------------------------------
//  状态持久化自检：HelloVscode.exe --statecheck
//  第一遍造状态并关闭窗口（走 closeEvent 保存），第二遍新窗口应完整恢复。
//  结果写到 exe 旁 statecheck.log
// ---------------------------------------------------------------
static int runStateCheck()
{
    QFile log(QCoreApplication::applicationDirPath() + QStringLiteral("/statecheck.log"));
    log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    QTextStream out(&log);
    out.setCodec("UTF-8");
    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &what) {
        out << (ok ? "PASS " : "FAIL ") << what << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    // 先造一份旧版 ini（Windows 上是注册表），第一次读设置时应自动搬进数据库
    {
        QSettings legacy(QCoreApplication::applicationDirPath() + QStringLiteral("/test_settings.ini"),
                         QSettings::IniFormat);
        legacy.setValue(QStringLiteral("display.line_width"), 5.5);
        legacy.setValue(QStringLiteral("theme"), QStringLiteral("light"));
        legacy.sync();
    }

    const double kLon = 118.5, kLat = 31.2, kAlt = 4200.0;
    {
        MainWindow w;
        check(qAbs(app::settings().value(QStringLiteral("display.line_width"), 0.0).toDouble() - 5.5) < 1e-6,
              QStringLiteral("旧 QSettings 设置迁移到数据库"));
        w.globe()->lookAt(kLon, kLat, kAlt);
        w.globe()->setShowLabels(true);
        w.setDrawingBarVisible(true);
        w.replay()->setSpeed(1234.0);
        w.ruler()->activate();
        w.ruler()->addWaypoint(31.0, 118.0);
        w.ruler()->addWaypoint(32.0, 119.0);
        w.drawing()->addCircle(31.5, 118.5, 300.0, QColor(QStringLiteral("#FF0000")));
        w.drawing()->addLine(30.0, 117.0, 33.0, 120.0, QColor(QStringLiteral("#00FF00")));
        // 数据源文件显隐：造一条雷达航迹再把它的分组关掉
        Track t;
        t.id = QStringLiteral("STATETEST");
        t.source = TrackSource::Radar;
        t.fileName = QStringLiteral("StateTest");
        TrackPoint p;
        p.t = 1000;
        p.lat = 31.0;
        p.lon = 118.0;
        t.points.append(p);
        w.trackStore()->addTracks(QVector<Track>() << t);
        w.globe()->trackLayer()->setGroupVisible(QStringLiteral("Radar::StateTest"), false);
        w.activatePanel(PanelId::Settings);
        w.close();                       // closeEvent 里保存并落盘
    }
    // 关掉设置连接再重开，第二遍必须从数据库重新读，不能靠进程内缓存
    app::closeSettings();

    // 设置确实落在数据库里（不是注册表/ini），库结构版本号也对
    {
        const QString conn = QStringLiteral("statecheck_probe");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(app::databasePath());
            if (db.open()) {
                QSqlQuery q(db);
                q.exec(QStringLiteral("SELECT COUNT(*) FROM app_settings"));
                check(q.next() && q.value(0).toInt() > 5, QStringLiteral("设置写入数据库 app_settings 表"));
                q.exec(QStringLiteral("SELECT COUNT(*) FROM app_settings WHERE key = 'camera.state'"));
                check(q.next() && q.value(0).toInt() == 1, QStringLiteral("相机状态在数据库里"));
                q.exec(QStringLiteral("SELECT COUNT(*) FROM app_settings WHERE key = 'app.migrated_from_qsettings'"));
                check(q.next() && q.value(0).toInt() == 1, QStringLiteral("旧设置只迁移一次"));
                q.exec(QStringLiteral("SELECT type FROM app_settings WHERE key = 'window.geometry'"));
                check(q.next() && q.value(0).toString() == QLatin1String("bytes"),
                      QStringLiteral("窗口几何按二进制类型存取"));
                q.exec(QStringLiteral("PRAGMA user_version"));
                check(q.next() && q.value(0).toInt() == 2, QStringLiteral("库结构版本 user_version=2"));
                db.close();
            } else {
                check(false, QStringLiteral("打开设置数据库"));
            }
        }
        QSqlDatabase::removeDatabase(conn);
    }

    {
        MainWindow w;
        double lon = 0.0, lat = 0.0, alt = 0.0;
        w.globe()->cameraView(&lon, &lat, &alt);
        check(qAbs(lon - kLon) < 0.05 && qAbs(lat - kLat) < 0.05 && qAbs(alt - kAlt) < 60.0,
              QStringLiteral("相机视角恢复"));
        check(w.globe()->showLabels(), QStringLiteral("航迹标签开关恢复"));
        check(w.drawingBarVisible(), QStringLiteral("绘图工具栏展开状态恢复"));
        check(qAbs(w.replay()->speed() - 1234.0) < 0.001, QStringLiteral("回放倍速恢复"));
        check(w.ruler()->isActive() && w.ruler()->waypoints().size() == 2, QStringLiteral("标尺航点恢复"));
        check(w.drawing()->count() == 2, QStringLiteral("绘图内容恢复"));
        check(w.activePanel() == int(PanelId::Settings), QStringLiteral("侧栏面板恢复"));
        check(!w.globe()->trackLayer()->isGroupVisible(QStringLiteral("Radar::StateTest")),
              QStringLiteral("数据源文件显隐恢复"));
        w.close();
    }

    out << (failures == 0 ? QStringLiteral("RESULT OK")
                          : QStringLiteral("RESULT FAIL (%1)").arg(failures)) << "\n";
    out.flush();
    return failures == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    loadBundledFonts();
    const QStringList args = app.arguments();
    const int pt = args.indexOf(QStringLiteral("--parsetest"));
    if (pt >= 0 && pt + 2 < args.size())
        return runParseTest(args.at(pt + 1), args.at(pt + 2));

    if (args.contains(QStringLiteral("--statecheck"))) {
        app::setTestMode(true);
        ui::setAutoConfirm(true);
        return runStateCheck();
    }

    // 只用临时数据目录跑一遍（截图/回归用，不碰真实数据库和设置）
    if (args.contains(QStringLiteral("--testmode"))) {
        app::setTestMode(true);
        ui::setAutoConfirm(true);
    }

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

    // 命令行直接指定在线底图 key（也方便自动化验证）：--online-key <天地图 key>
    const int ok = args.indexOf(QStringLiteral("--online-key"));
    if (ok >= 0 && ok + 1 < args.size())
        w.openOnlineMap(args.at(ok + 1), args.contains(QStringLiteral("--amap")));

    // 指定视角后再抓帧（回归验证用）：--look 经度,纬度,视高km。
    // 延到事件循环开始后再设，否则会被 loadPersisted() 里「自动飞到航迹中心」覆盖掉。
    const int lookIdx = args.indexOf(QStringLiteral("--look"));
    if (lookIdx >= 0 && lookIdx + 1 < args.size()) {
        const QStringList p = args.at(lookIdx + 1).split(QLatin1Char(','));
        if (p.size() >= 3) {
            const double lon = p[0].toDouble(), lat = p[1].toDouble(), alt = p[2].toDouble();
            QTimer::singleShot(0, &w, [&w, lon, lat, alt]() { w.globe()->lookAt(lon, lat, alt); });
        }
    }

    // 启动时加载一份绘图 GeoJSON（回归截图用）：--drawings <file>
    const int drIdx = args.indexOf(QStringLiteral("--drawings"));
    if (drIdx >= 0 && drIdx + 1 < args.size()) {
        QString drawErr;
        if (!w.drawing()->loadFromFile(args.at(drIdx + 1), &drawErr))
            qWarning("加载绘图失败：%s", qPrintable(drawErr));
    }

    // 绘图演示（回归截图用）：--draw-demo，往地图上放几个典型图形
    if (args.contains(QStringLiteral("--draw-demo"))) {
        w.setDrawingBarVisible(true);          // 截图里要能看到工具栏
        DrawingManager *d = w.drawing();
        const int rid = d->addRect(30.0, 95.0, 40.0, 115.0, QColor("#00B0F0"));
        d->setFill(rid, QColor(0x00, 0xb0, 0xf0, 70));
        d->addCircle(35.0, 105.0, 600.0, QColor("#FFC000"));
        const int demoLine = d->addLine(32.0, 100.0, 38.0, 112.0, QColor("#FF4444"));
        // 顺手演示打断：断点处会画一个白心黑边的小圆点
        if (const DrawingFeature *dl = d->feature(demoLine))
            d->breakFeature(demoLine, dl->pts.size() / 2);
        d->addText(33.0, 101.0, QStringLiteral("重点区域"), QColor("#FFFF00"), 22);
    }

    // 抓一帧后退出（回归验证用）：--shot <png> [--shot-delay 毫秒] [--shot-window]
    //   默认只抓 GL 帧（地球+航迹）；--shot-window 抓整窗，连旗标/标签/状态栏一起，
    //   因为叠加层是 GlobeOverlay 子控件，grabFramebuffer 拍不到它
    const int shotIdx = args.indexOf(QStringLiteral("--shot"));
    if (shotIdx >= 0 && shotIdx + 1 < args.size()) {
        const QString path = args.at(shotIdx + 1);
        int delayMs = 12000;
        const int dIdx = args.indexOf(QStringLiteral("--shot-delay"));
        if (dIdx >= 0 && dIdx + 1 < args.size())
            delayMs = args.at(dIdx + 1).toInt();
        const bool wholeWindow = args.contains(QStringLiteral("--shot-window"));
        QTimer::singleShot(delayMs, &w, [&w, path, wholeWindow]() {
            if (wholeWindow)
                w.grab().save(path);
            else if (!w.globe()->grabFramebuffer().save(path))
                qWarning("抓帧保存失败：%s", qPrintable(path));
            QCoreApplication::quit();
        });
    }

    if (app.arguments().contains(QStringLiteral("--selftest")))
        runSelfTest(&w);
    const int tt = args.indexOf(QStringLiteral("--tracktest"));
    if (tt >= 0 && tt + 2 < args.size())
        runTrackTest(&w, args.at(tt + 1), args.at(tt + 2));

    return app.exec();
}
