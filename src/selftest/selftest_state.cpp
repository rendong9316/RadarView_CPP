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

// ---------------------------------------------------------------
//  状态持久化自检：RadarView.exe --statecheck
//  第一遍造状态并关闭窗口（走 closeEvent 保存），第二遍新窗口应完整恢复。
//  结果写到 exe 旁 statecheck.log
// ---------------------------------------------------------------
int runStateCheck()
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
        w.setRadarPointMode(RadarPointMode::Predicted);
        w.activatePanel(PanelId::Settings);
        w.setSourceStripOpen(true);      // 展开数据源条，第二遍应恢复成展开
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
                check(q.next() && q.value(0).toInt() == 7, QStringLiteral("库结构版本 user_version=7"));
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
        check(w.radarPointMode() == RadarPointMode::Predicted
                  && w.globe()->trackLayer()->radarPointMode() == RadarPointMode::Predicted,
              QStringLiteral("雷达点源模式恢复（预测点）"));
        check(!w.sourceStrip()->isHidden() && w.appStatusBar()->sourceStripOpen(),
              QStringLiteral("数据源条展开状态恢复"));
        w.close();
    }

    // ---- 旧版 HelloVscode 数据目录迁移自检（完全隔离在 exe 旁临时目录，不碰真实用户目录）----
    {
        const QString tmpRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/test_legacy_migrate");
        const QString legacyDir = tmpRoot + QStringLiteral("/HelloVscode");
        const QString newDir = tmpRoot + QStringLiteral("/RadarView");
        QDir(tmpRoot).removeRecursively();
        QDir().mkpath(legacyDir + QStringLiteral("/tilecache/amap/5/10"));

        // 构造旧版数据库、绘图文件与瓦片缓存
        const QString legacyDbPath = legacyDir + QStringLiteral("/radarview.db");
        {
            const QString conn = QStringLiteral("statecheck_legacy_seed");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(legacyDbPath);
                if (db.open()) {
                    QSqlQuery q(db);
                    q.exec(QStringLiteral("CREATE TABLE app_settings (key TEXT PRIMARY KEY, value TEXT, type TEXT NOT NULL DEFAULT 'string')"));
                    q.exec(QStringLiteral("INSERT INTO app_settings (key, value, type) VALUES ('migrate.sentinel', 'from_legacy', 'string')"));
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(conn);
        }
        {
            QFile f(legacyDir + QStringLiteral("/drawings.json"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write("{\"type\":\"FeatureCollection\",\"features\":[{\"id\":1}]}");
        }
        const QByteArray mockTile("PNG_MOCK_TILE_DATA");
        {
            QFile f(legacyDir + QStringLiteral("/tilecache/amap/5/10/12.png"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(mockTile);
        }

        const bool migratedOk = app::migrateLegacyDataDir(legacyDir, newDir);
        check(migratedOk, QStringLiteral("旧目录 HelloVscode -> RadarView 迁移成功"));
        check(QFileInfo::exists(legacyDbPath), QStringLiteral("旧目录数据库保留不删除"));

        // 校验新目录数据库可读且内容完整
        {
            const QString conn = QStringLiteral("statecheck_legacy_verify");
            QString sentinel;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(newDir + QStringLiteral("/radarview.db"));
                if (db.open()) {
                    QSqlQuery q(db);
                    if (q.exec(QStringLiteral("SELECT value FROM app_settings WHERE key = 'migrate.sentinel'")) && q.next())
                        sentinel = q.value(0).toString();
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(conn);
            check(sentinel == QLatin1String("from_legacy"), QStringLiteral("迁移后数据库内容完整"));
        }

        // 校验绘图文件与瓦片缓存已迁移
        {
            QFile f(newDir + QStringLiteral("/drawings.json"));
            const bool okDraw = f.open(QIODevice::ReadOnly)
                             && f.readAll().contains("FeatureCollection");
            check(okDraw, QStringLiteral("迁移后 drawings.json 完整"));
        }
        {
            QFile f(newDir + QStringLiteral("/tilecache/amap/5/10/12.png"));
            const bool okTile = f.open(QIODevice::ReadOnly) && f.readAll() == mockTile;
            check(okTile, QStringLiteral("迁移后 tilecache 完整"));
        }

        // 幂等性：新目录已有更新数据时，再次迁移绝不覆盖
        {
            QFile f(newDir + QStringLiteral("/drawings.json"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write("{\"newer\":true}");
        }
        app::migrateLegacyDataDir(legacyDir, newDir);
        {
            QFile f(newDir + QStringLiteral("/drawings.json"));
            const bool kept = f.open(QIODevice::ReadOnly) && f.readAll() == QByteArray("{\"newer\":true}");
            check(kept, QStringLiteral("重复迁移不覆盖新目录已有文件"));
        }

        QDir(tmpRoot).removeRecursively();
    }

    out << (failures == 0 ? QStringLiteral("RESULT OK")
                          : QStringLiteral("RESULT FAIL (%1)").arg(failures)) << "\n";
    out.flush();
    return failures == 0 ? 0 : 1;
}

} // namespace selftest
