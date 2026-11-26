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
                    // 顶点小圆球：默认很小，且可在设置面板「套索顶点」里调
                    check(qAbs(g->lassoVertexPx() - 3.0) < 1e-9,
                          QStringLiteral("套索顶点圆球默认半径 %1 px（很小）").arg(g->lassoVertexPx()));
                    g->setLassoVertexPx(9.0);
                    check(qAbs(g->lassoVertexPx() - 9.0) < 1e-9,
                          QStringLiteral("套索顶点圆球半径可调（9 px）"));
                    g->setLassoVertexPx(app::settings()
                                            .value(QStringLiteral("display.lasso_vertex_px"), 3.0).toDouble());
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

} // namespace selftest
