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

// 数据库迁移自检：HelloVscode.exe --dbtest
// 造一个 v2 旧库（40 字节点格式）→ 打开触发 v3~v6 迁移 → 校验点数据、测量坐标、SNR
// 与版本号，写 dbtest.log
int runDbMigrationTest()
{
    QFile log(QCoreApplication::applicationDirPath() + QStringLiteral("/dbtest.log"));
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

    app::setTestMode(true);                 // 清空并固定使用 exe 旁的临时库（test_radarview.db）
    const QString path = app::databasePath();
    QFile::remove(path);
    const QString conn = QStringLiteral("dbtest_setup");

    // ---- 造 v2 旧库：2 个点 × 40 字节 ----
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(path);
        if (!db.open()) {
            out << "FAIL 创建测试库: " << db.lastError().text() << "\n";
            return 1;
        }
        QSqlQuery q(db);
        const char *const stmts[] = {
            "CREATE TABLE batches (id INTEGER PRIMARY KEY AUTOINCREMENT, file_name TEXT NOT NULL,"
            " source TEXT NOT NULL, track_count INTEGER NOT NULL, imported_at TEXT NOT NULL)",
            "CREATE TABLE saved_tracks (icao_address TEXT NOT NULL, batch_id INTEGER NOT NULL,"
            " source TEXT NOT NULL, flight_no TEXT, icao_flight_no TEXT, registration TEXT,"
            " aircraft_type TEXT, airline TEXT, origin TEXT, destination TEXT,"
            " min_timestamp TEXT, max_timestamp TEXT, min_ms INTEGER, max_ms INTEGER,"
            " point_count INTEGER DEFAULT 0, deleted INTEGER DEFAULT 0, points BLOB NOT NULL,"
            " PRIMARY KEY (icao_address, batch_id))",
            "INSERT INTO batches (id, file_name, source, track_count, imported_at)"
            " VALUES (1, 'Radar', 'Radar', 1, '2026-01-01 00:00:00')",
            "PRAGMA user_version=2",
        };
        bool okAll = true;
        for (const char *s : stmts)
            if (!q.exec(QLatin1String(s)))
                okAll = false;
        check(okAll, QStringLiteral("构造 v2 旧库"));

        QByteArray blob(2 * 40, 0);
        char *p = blob.data();
        for (int i = 0; i < 2; ++i) {
            const qint64 t = 1765360182000LL + i * 10000;
            qToLittleEndian<qint64>(t, p);
            double lat = 30.0 + i, lon = 110.0 + i;
            quint64 u;
            std::memcpy(&u, &lat, 8);
            qToLittleEndian<quint64>(u, p + 8);
            std::memcpy(&u, &lon, 8);
            qToLittleEndian<quint64>(u, p + 16);
            const float fs[4] = { 1000.0f, 90.0f, 200.0f, 0.0f };
            for (int k = 0; k < 4; ++k) {
                quint32 v;
                std::memcpy(&v, &fs[k], 4);
                qToLittleEndian<quint32>(v, p + 24 + 4 * k);
            }
            p += 40;
        }
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO saved_tracks (icao_address, batch_id, source, min_timestamp, max_timestamp,"
            " min_ms, max_ms, point_count, deleted, points)"
            " VALUES ('RADAR-0001', 1, 'Radar', '', '', 1765360182000, 1765360192000, 2, 0, ?)"));
        ins.addBindValue(blob);
        check(ins.exec(), QStringLiteral("写入旧格式航迹"));
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);

    // ---- 打开触发迁移 ----
    QString err;
    const bool opened = trackdb::open(&err);
    check(opened, QStringLiteral("打开数据库并触发 v3 迁移：%1").arg(err));
    if (opened) {
        QVector<Track> tracks = trackdb::loadAll(&err);
        check(tracks.size() == 1, QStringLiteral("迁移后航迹数 = 1（实际 %1）").arg(tracks.size()));
        if (!tracks.isEmpty()) {
            const Track &t = tracks.first();
            check(t.points.size() == 2, QStringLiteral("迁移后点数 = 2（实际 %1）").arg(t.points.size()));
            if (t.points.size() == 2) {
                check(qFuzzyCompare(t.points.first().lat, 30.0)
                          && qFuzzyCompare(t.points.last().lon, 111.0),
                      QStringLiteral("旧数据经纬度未损坏"));
                check(!std::isfinite(t.points.first().prange)
                          && !std::isfinite(t.points.first().pvr)
                          && !std::isfinite(t.points.first().paz)
                          && !std::isfinite(t.points.first().snr),
                      QStringLiteral("旧数据测量坐标与 SNR 补为 NaN"));
            }
        }
        const QVector<DbTrackSummary> sums = trackdb::trackSummaries(QString(), &err);
        check(sums.size() == 1 && !sums.first().hasMeasure,
              QStringLiteral("has_measure = 0（旧数据不可用于点云叠加）"));

        QSqlDatabase db = QSqlDatabase::database(QStringLiteral("radarview_main"), false);
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next())
            check(q.value(0).toInt() == 6, QStringLiteral("user_version = 6"));
        else
            check(false, QStringLiteral("读取 user_version"));
        if (q.exec(QStringLiteral("SELECT length(points) FROM saved_tracks")) && q.next())
            check(q.value(0).toInt() == 2 * 56, QStringLiteral("点格式已重编码为 56 字节/点"));
        else
            check(false, QStringLiteral("读取点 BLOB 长度"));
        if (q.exec(QStringLiteral("SELECT COUNT(*) FROM saved_tracks"
                                  " WHERE (has_raw IS NULL OR has_raw = 0) AND raw_points IS NULL"))
                && q.next())
            check(q.value(0).toInt() == 1,
                  QStringLiteral("v4 迁移给旧航迹加空关联点列（has_raw=0）"));
        else
            check(false, QStringLiteral("检查 raw_points 列"));
        if (q.exec(QStringLiteral("SELECT COUNT(*) FROM saved_tracks WHERE has_snr = 0")) && q.next())
            check(q.value(0).toInt() == 1,
                  QStringLiteral("v6 迁移给旧航迹加 has_snr=0（重导后可原地补 SNR）"));
        else
            check(false, QStringLiteral("检查 has_snr 列"));

        // 再插入一条新格式航迹（带测量坐标与 SNR），验证选择器能区分新旧数据
        {
            QByteArray blob(2 * 56, 0);
            char *p = blob.data();
            for (int i = 0; i < 2; ++i) {
                const qint64 t = 1765360182000LL + i * 10000;
                qToLittleEndian<qint64>(t, p);
                double lat = 31.0 + i, lon = 111.0 + i;
                quint64 u;
                std::memcpy(&u, &lat, 8);
                qToLittleEndian<quint64>(u, p + 8);
                std::memcpy(&u, &lon, 8);
                qToLittleEndian<quint64>(u, p + 16);
                const float fs[8] = { 1000.0f, 90.0f, 200.0f, 0.0f,
                                      1000.0f + i * 100.0f, 12.0f, 90.0f,
                                      20.5f + i };
                for (int k = 0; k < 8; ++k) {
                    quint32 v;
                    std::memcpy(&v, &fs[k], 4);
                    qToLittleEndian<quint32>(v, p + 24 + 4 * k);
                }
                p += 56;
            }
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO saved_tracks (icao_address, batch_id, source, min_timestamp,"
                " max_timestamp, min_ms, max_ms, point_count, has_measure, has_snr,"
                " raw_point_count, has_raw, deleted, points, raw_points)"
                " VALUES ('RADAR-0002', 1, 'Radar', '', '', 1765360182000, 1765360192000, 2, 1, 1, 3, 1, 0, ?, ?)"));
            ins.addBindValue(blob);
            // 关联点列（同一 MAT 的 asscPointList）：3 个点，时间落在上面 2 个平滑点之间
            QByteArray rawBlob(3 * 56, 0);
            char *rp = rawBlob.data();
            const qint64 rawTimes[3] = { 1765360183000LL, 1765360190000LL, 1765360191500LL };
            for (int i = 0; i < 3; ++i) {
                qToLittleEndian<qint64>(rawTimes[i], rp);
                double lat = 31.5 + i * 0.1, lon = 111.5 + i * 0.1;
                quint64 u;
                std::memcpy(&u, &lat, 8);
                qToLittleEndian<quint64>(u, rp + 8);
                std::memcpy(&u, &lon, 8);
                qToLittleEndian<quint64>(u, rp + 16);
                const float fs[8] = { 1000.0f, 90.0f, 200.0f, 0.0f,
                                      2000.0f + i * 10.0f, 8.0f, 91.0f,
                                      30.0f + i };
                for (int k = 0; k < 8; ++k) {
                    quint32 v;
                    std::memcpy(&v, &fs[k], 4);
                    qToLittleEndian<quint32>(v, rp + 24 + 4 * k);
                }
                rp += 56;
            }
            ins.addBindValue(rawBlob);
            check(ins.exec(), QStringLiteral("写入带测量坐标与关联点列的新格式航迹"));
        }
        const QVector<DbTrackSummary> sums2 = trackdb::trackSummaries(QString(), &err);
        int withMeasure = 0, withRaw = 0, rawCount = 0;
        for (const DbTrackSummary &s : sums2) {
            withMeasure += s.hasMeasure ? 1 : 0;
            withRaw += s.hasRaw ? 1 : 0;
            rawCount += s.rawCount;
        }
        check(sums2.size() == 2 && withMeasure == 1 && withRaw == 1 && rawCount == 3,
              QStringLiteral("航迹选择器摘要：2 条，1 条带平滑+关联两套点列（关联 %1 点）").arg(rawCount));
        const QVector<Track> loadedNew = trackdb::loadTracks(
            QStringList() << QStringLiteral("RADAR-0002::Radar::Radar"));
        check(loadedNew.size() == 1 && !loadedNew.first().points.isEmpty()
                  && std::isfinite(loadedNew.first().points.first().prange),
              QStringLiteral("新格式航迹可读出测量坐标"));
        check(loadedNew.size() == 1 && !loadedNew.first().points.isEmpty()
                  && qFuzzyCompare(loadedNew.first().points.first().snr, 20.5f),
              QStringLiteral("新格式航迹可读出 SNR（snr=%1）")
                  .arg(loadedNew.isEmpty() ? 0.0 : double(loadedNew.first().points.first().snr)));
        check(loadedNew.size() == 1 && loadedNew.first().rawPoints.size() == 3
                  && std::isfinite(loadedNew.first().rawPoints.first().prange)
                  && qFuzzyCompare(loadedNew.first().rawPoints.first().snr, 30.0f),
              QStringLiteral("关联点列随航迹一起读出（%1 点，SNR 一并保留）")
                  .arg(loadedNew.isEmpty() ? -1 : loadedNew.first().rawPoints.size()));

        // 真实链路：库里的航迹 → 点云叠加（按时间映射到点迹帧的 Z 值）
        {
            QVector<DetPointFrame> frames;
            for (int i = 0; i < 3; ++i) {
                DetPointFrame f;
                f.timeMs = 1765360182000LL + i * 10000;
                f.frameId = 100 + i;
                f.zValue = 100 + i;
                frames.append(f);
            }
            TrackOverlay ov;
            const bool ok = !loadedNew.isEmpty()
                    && overlayFromTrack(loadedNew.first(), frames, &ov);
            check(ok && ov.points.size() == 2 && ov.rawPoints.size() == 3 && ov.batchNo == 2,
                  QStringLiteral("数据库航迹转换为点云叠加（平滑 %1 / 关联 %2 点 / 批 %3）")
                      .arg(ov.points.size()).arg(ov.rawPoints.size()).arg(ov.batchNo));
            if (ok && ov.points.size() == 2)
                check(qFuzzyCompare(ov.points.at(1).prange, 1100.0f)
                          && ov.points.at(1).zValue == 101.0,
                      QStringLiteral("平滑叠加点带测量坐标与帧 Z 值（z=%1）")
                          .arg(ov.points.at(1).zValue));
            if (ok && ov.rawPoints.size() == 3)
                check(qFuzzyCompare(ov.rawPoints.at(1).prange, 2010.0f)
                          && ov.rawPoints.at(0).zValue == 100.0
                          && ov.rawPoints.at(1).zValue == 101.0,
                      QStringLiteral("关联叠加点按时间映射到帧 Z 值（z=%1/%2）")
                          .arg(ov.rawPoints.at(0).zValue).arg(ov.rawPoints.at(1).zValue));
        }

        // 老库原地升级：同一批次重导同一个 MAT 时，已存在但缺列（测量坐标/关联点列）的
        // 航迹要被补上，而不是被"已存在"直接跳过
        {
            q.finish();          // 结束上面那条 SELECT：否则主连接一直持有读快照，看不到后台写入
            Track t;
            t.id = QStringLiteral("RADAR-0001");      // v2 迁移过来的那条，has_measure=0/has_raw=0
            t.source = TrackSource::Radar;
            t.fileName = QStringLiteral("Radar");
            TrackPoint p1, p2;
            p1.t = 1765360182000LL;
            p2.t = 1765360192000LL;
            p1.prange = 500.0f; p1.pvr = 10.0f; p1.paz = 45.0f;
            p2.prange = 520.0f; p2.pvr = 11.0f; p2.paz = 46.0f;
            t.points << p1 << p2;
            TrackPoint r1;
            r1.t = 1765360185000LL;
            r1.prange = 900.0f; r1.pvr = 5.0f; r1.paz = 50.0f; r1.snr = 18.0f;
            t.rawPoints << r1;
            DbSaveJob job(QStringLiteral("Radar"), TrackSource::Radar, QVector<Track>() << t);
            job.start();
            job.wait();
            check(job.ok() && job.savedCount() == 1 && job.appended(),
                  QStringLiteral("重导同批次时原地补上缺失列（写入 %1 条）").arg(job.savedCount()));
            const QVector<Track> up = trackdb::loadTracks(
                QStringList() << QStringLiteral("RADAR-0001::Radar::Radar"));
            check(up.size() == 1 && up.first().rawPoints.size() == 1
                      && std::isfinite(up.first().points.first().prange),
                  QStringLiteral("旧航迹升级后同时带平滑与关联点列"));
            check(up.size() == 1 && up.first().rawPoints.size() == 1
                      && qFuzzyCompare(up.first().rawPoints.first().snr, 18.0f),
                  QStringLiteral("旧航迹重导后补上 SNR（%1）")
                      .arg(up.isEmpty() || up.first().rawPoints.isEmpty()
                               ? 0.0 : double(up.first().rawPoints.first().snr)));
        }

        // 数据源重命名（schema v5）：只改显示名，file_name 不变；搜索按新名与原名都能命中
        {
            qint64 batchId = -1;
            for (const BatchInfo &b : trackdb::batches())
                if (b.fileName == QStringLiteral("Radar"))
                    batchId = b.id;
            QString rerr;
            check(batchId >= 0 && trackdb::setBatchDisplayName(batchId, QStringLiteral("雷达主站"), &rerr),
                  QStringLiteral("数据源重命名：%1").arg(rerr));
            const QVector<BatchInfo> bs = trackdb::batches();
            check(bs.size() == 1 && bs.first().label() == QStringLiteral("雷达主站")
                      && bs.first().fileName == QStringLiteral("Radar"),
                  QStringLiteral("重命名只改显示名（显示 %1，文件名仍是 %2）")
                      .arg(bs.isEmpty() ? QString() : bs.first().label(),
                           bs.isEmpty() ? QString() : bs.first().fileName));
            const QHash<QString, QString> names = trackdb::batchDisplayNames();
            check(names.value(QStringLiteral("Radar::Radar")) == QStringLiteral("雷达主站"),
                  QStringLiteral("显示名按 来源::文件名 提供（状态栏 chip 用）"));
            const QVector<DbTrackSummary> renamed = trackdb::trackSummaries();
            check(!renamed.isEmpty() && renamed.first().displayName == QStringLiteral("雷达主站"),
                  QStringLiteral("点云航迹选择器拿到新显示名"));
            ManageFilter f;
            f.searchText = QStringLiteral("雷达主站");
            int totalNew = 0;
            trackdb::query(f, 10, 0, &totalNew);
            f.searchText = QStringLiteral("Radar");
            int totalOld = 0;
            trackdb::query(f, 10, 0, &totalOld);
            check(totalNew == 2 && totalOld == 2,
                  QStringLiteral("搜索同时匹配新名与原名（新名 %1 / 原名 %2）").arg(totalNew).arg(totalOld));
            trackdb::setBatchDisplayName(batchId, QString(), &rerr);
            check(trackdb::batches().first().label() == QStringLiteral("Radar"),
                  QStringLiteral("清空显示名恢复为文件名"));
        }

        // 「删除全部数据」：清空批次与航迹
        {
            QString derr;
            const bool ok = trackdb::deleteAllBatches(&derr);
            const ManageStats st = trackdb::stats();
            check(ok && st.totalBatches == 0 && st.totalTracks == 0 && trackdb::batches().isEmpty()
                      && trackdb::loadAll().isEmpty(),
                  QStringLiteral("删除全部数据后库为空（批次 %1 / 航迹 %2）%3")
                      .arg(st.totalBatches).arg(st.totalTracks).arg(derr));
        }
    }
    trackdb::close();

    // ---- 728 字节边界：v5 库里 14 个 52 字节点正好 728 字节，也是 13 个 56 字节。
    // 迁移必须按「库版本 < 6 一律是 52 字节点」处理，不能按长度猜（否则这条航迹会被误读成 13 点）
    {
        QFile::remove(path);
        const QString conn5 = QStringLiteral("dbtest_setup_v5");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn5);
            db.setDatabaseName(path);
            if (!db.open()) {
                out << "FAIL 创建 v5 边界测试库: " << db.lastError().text() << "\n";
                return 1;
            }
            QSqlQuery q(db);
            const char *const stmts[] = {
                "CREATE TABLE batches (id INTEGER PRIMARY KEY AUTOINCREMENT, file_name TEXT NOT NULL,"
                " source TEXT NOT NULL, display_name TEXT DEFAULT NULL,"
                " track_count INTEGER NOT NULL, imported_at TEXT NOT NULL)",
                "CREATE TABLE saved_tracks (icao_address TEXT NOT NULL, batch_id INTEGER NOT NULL,"
                " source TEXT NOT NULL, flight_no TEXT, icao_flight_no TEXT, registration TEXT,"
                " aircraft_type TEXT, airline TEXT, origin TEXT, destination TEXT,"
                " min_timestamp TEXT, max_timestamp TEXT, min_ms INTEGER, max_ms INTEGER,"
                " point_count INTEGER DEFAULT 0, deleted INTEGER DEFAULT 0,"
                " has_measure INTEGER DEFAULT 0, raw_point_count INTEGER DEFAULT 0,"
                " has_raw INTEGER DEFAULT 0, raw_points BLOB, points BLOB NOT NULL,"
                " PRIMARY KEY (icao_address, batch_id))",
                "INSERT INTO batches (id, file_name, source, track_count, imported_at)"
                " VALUES (1, 'Radar', 'Radar', 1, '2026-01-01 00:00:00')",
                "PRAGMA user_version=5",
            };
            bool okAll = true;
            for (const char *s : stmts)
                if (!q.exec(QLatin1String(s)))
                    okAll = false;
            check(okAll, QStringLiteral("构造 v5 库（14 点 × 52 字节 = 728 字节）"));
            QByteArray blob(14 * 52, 0);
            char *p = blob.data();
            for (int i = 0; i < 14; ++i) {
                qToLittleEndian<qint64>(1765360182000LL + i * 1000, p);
                double lat = 30.0 + i, lon = 110.0 + i;
                quint64 u;
                std::memcpy(&u, &lat, 8);
                qToLittleEndian<quint64>(u, p + 8);
                std::memcpy(&u, &lon, 8);
                qToLittleEndian<quint64>(u, p + 16);
                const float fs[7] = { 1000.0f, 90.0f, 200.0f, 0.0f,
                                      1000.0f + i, 12.0f, 90.0f };
                for (int k = 0; k < 7; ++k) {
                    quint32 v;
                    std::memcpy(&v, &fs[k], 4);
                    qToLittleEndian<quint32>(v, p + 24 + 4 * k);
                }
                p += 52;
            }
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral(
                "INSERT INTO saved_tracks (icao_address, batch_id, source, point_count,"
                " deleted, min_ms, max_ms, points) VALUES ('RADAR-0014', 1, 'Radar', 14, 0,"
                " 1765360182000, 1765360195000, ?)"));
            ins.addBindValue(blob);
            check(ins.exec(), QStringLiteral("写入 728 字节旧格式航迹"));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn5);
        QString e5;
        const bool opened5 = trackdb::open(&e5);
        check(opened5, QStringLiteral("打开 v5 库触发 v6 迁移：%1").arg(e5));
        const QVector<Track> amb = trackdb::loadAll(&e5);
        check(amb.size() == 1 && amb.first().points.size() == 14,
              QStringLiteral("728 字节旧点列迁移后仍是 14 点（实际 %1）")
                  .arg(amb.isEmpty() ? -1 : amb.first().points.size()));
        check(!amb.isEmpty() && amb.first().points.size() == 14
                  && qFuzzyCompare(amb.first().points.last().lat, 43.0)
                  && qFuzzyCompare(amb.first().points.last().prange, 1013.0f),
              QStringLiteral("边界库经纬度/测量坐标未错位"));
        QSqlDatabase mdb = QSqlDatabase::database(QStringLiteral("radarview_main"), false);
        QSqlQuery lq(mdb);
        check(lq.exec(QStringLiteral("SELECT length(points) FROM saved_tracks")) && lq.next()
                  && lq.value(0).toInt() == 14 * 56,
              QStringLiteral("728 字节点列已升级为 14 × 56 字节"));
    }
    trackdb::close();

    out << (failures == 0 ? QStringLiteral("RESULT OK")
                          : QStringLiteral("RESULT FAILED (%1)").arg(failures)) << "\n";
    out.flush();
    return failures == 0 ? 0 : 1;
}

} // namespace selftest
