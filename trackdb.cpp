#include "trackdb.h"
#include "apppaths.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtEndian>
#include <cstring>

namespace {

const char *const kMainConn = "radarview_main";
const int kPointBytes = 40;   // t(8) lat(8) lon(8) alt heading speed vrate(4×4)

QSqlDatabase mainDb()
{
    return QSqlDatabase::database(QLatin1String(kMainConn), false);
}

bool exec(QSqlQuery &q, const QString &sql, QString *error)
{
    if (q.exec(sql))
        return true;
    if (error)
        *error = q.lastError().text();
    return false;
}

bool createSchema(QSqlDatabase db, QString *error)
{
    QSqlQuery q(db);
    const char *const stmts[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "CREATE TABLE IF NOT EXISTS batches ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, file_name TEXT NOT NULL, source TEXT NOT NULL,"
        " track_count INTEGER NOT NULL, imported_at TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS saved_tracks ("
        " icao_address TEXT NOT NULL, batch_id INTEGER NOT NULL, source TEXT NOT NULL,"
        " flight_no TEXT, icao_flight_no TEXT, registration TEXT, aircraft_type TEXT, airline TEXT,"
        " origin TEXT, destination TEXT, min_timestamp TEXT, max_timestamp TEXT,"
        " min_ms INTEGER, max_ms INTEGER, point_count INTEGER DEFAULT 0, deleted INTEGER DEFAULT 0,"
        " points BLOB NOT NULL, PRIMARY KEY (icao_address, batch_id))",
        "CREATE INDEX IF NOT EXISTS idx_tracks_source ON saved_tracks(source)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_time ON saved_tracks(min_timestamp, max_timestamp)",
        "CREATE INDEX IF NOT EXISTS idx_tracks_points ON saved_tracks(point_count)",
        "CREATE INDEX IF NOT EXISTS idx_batches_file ON batches(source, file_name)",
    };
    for (const char *s : stmts)
        if (!exec(q, QLatin1String(s), error))
            return false;
    return true;
}

QByteArray encodePoints(const QVector<TrackPoint> &pts)
{
    QByteArray out(pts.size() * kPointBytes, Qt::Uninitialized);
    char *p = out.data();
    for (const TrackPoint &tp : pts) {
        qToLittleEndian<qint64>(tp.t, p);
        quint64 u;
        std::memcpy(&u, &tp.lat, 8);
        qToLittleEndian<quint64>(u, p + 8);
        std::memcpy(&u, &tp.lon, 8);
        qToLittleEndian<quint64>(u, p + 16);
        const float fs[4] = { tp.alt, tp.heading, tp.speed, tp.vrate };
        for (int k = 0; k < 4; ++k) {
            quint32 v;
            std::memcpy(&v, &fs[k], 4);
            qToLittleEndian<quint32>(v, p + 24 + 4 * k);
        }
        p += kPointBytes;
    }
    return out;
}

QVector<TrackPoint> decodePoints(const QByteArray &blob)
{
    const int n = blob.size() / kPointBytes;
    QVector<TrackPoint> pts(n);
    const char *p = blob.constData();
    for (int i = 0; i < n; ++i, p += kPointBytes) {
        TrackPoint &tp = pts[i];
        tp.t = qFromLittleEndian<qint64>(p);
        quint64 u = qFromLittleEndian<quint64>(p + 8);
        std::memcpy(&tp.lat, &u, 8);
        u = qFromLittleEndian<quint64>(p + 16);
        std::memcpy(&tp.lon, &u, 8);
        float fs[4];
        for (int k = 0; k < 4; ++k) {
            const quint32 v = qFromLittleEndian<quint32>(p + 24 + 4 * k);
            std::memcpy(&fs[k], &v, 4);
        }
        tp.alt = fs[0];
        tp.heading = fs[1];
        tp.speed = fs[2];
        tp.vrate = fs[3];
    }
    return pts;
}

const char *const kTrackCols =
    "st.icao_address, st.source, b.file_name, st.flight_no, st.icao_flight_no, st.aircraft_type,"
    " st.registration, st.airline, st.origin, st.destination, st.points";

Track trackFromQuery(const QSqlQuery &q)
{
    Track t;
    t.id = q.value(0).toString();
    trackSourceFromName(q.value(1).toString(), &t.source);
    t.fileName = q.value(2).toString();
    t.flightNo = q.value(3).toString();
    t.icaoFlightNo = q.value(4).toString();
    t.aircraftType = q.value(5).toString();
    t.registration = q.value(6).toString();
    t.airline = q.value(7).toString();
    t.origin = q.value(8).toString();
    t.destination = q.value(9).toString();
    t.points = decodePoints(q.value(10).toByteArray());
    return t;
}

// key = icao::source::fileName（icao 本身不含 "::"）
bool splitKey(const QString &key, QString *icao, QString *source, QString *file)
{
    const int a = key.indexOf(QLatin1String("::"));
    if (a < 0)
        return false;
    const int b = key.indexOf(QLatin1String("::"), a + 2);
    if (b < 0)
        return false;
    *icao = key.left(a);
    *source = key.mid(a + 2, b - a - 2);
    *file = key.mid(b + 2);
    return true;
}

} // namespace

// ---------------------------------------------------------------
QJsonObject ManageFilter::filterJson() const
{
    QJsonObject o;
    if (!source.isEmpty()) o.insert(QStringLiteral("source"), source);
    if (batchId >= 0) o.insert(QStringLiteral("batchId"), double(batchId));
    if (!searchText.isEmpty()) o.insert(QStringLiteral("searchText"), searchText);
    if (!airline.isEmpty()) o.insert(QStringLiteral("airline"), airline);
    if (!aircraftType.isEmpty()) o.insert(QStringLiteral("aircraftType"), aircraftType);
    if (minPoints >= 0) o.insert(QStringLiteral("minPoints"), minPoints);
    if (maxPoints >= 0) o.insert(QStringLiteral("maxPoints"), maxPoints);
    if (minTimeMs > 0) o.insert(QStringLiteral("minTimeMs"), double(minTimeMs));
    if (maxTimeMs > 0) o.insert(QStringLiteral("maxTimeMs"), double(maxTimeMs));
    return o;
}

QJsonObject ManageFilter::sortJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("column"), sortBy);
    o.insert(QStringLiteral("desc"), sortDesc);
    return o;
}

void ManageFilter::loadJson(const QJsonObject &o, const QJsonObject &s)
{
    resetAll();
    source = o.value(QStringLiteral("source")).toString();
    batchId = o.contains(QStringLiteral("batchId")) ? qint64(o.value(QStringLiteral("batchId")).toDouble()) : -1;
    searchText = o.value(QStringLiteral("searchText")).toString();
    airline = o.value(QStringLiteral("airline")).toString();
    aircraftType = o.value(QStringLiteral("aircraftType")).toString();
    minPoints = o.value(QStringLiteral("minPoints")).toInt(-1);
    maxPoints = o.value(QStringLiteral("maxPoints")).toInt(-1);
    minTimeMs = qint64(o.value(QStringLiteral("minTimeMs")).toDouble(0));
    maxTimeMs = qint64(o.value(QStringLiteral("maxTimeMs")).toDouble(0));
    if (s.contains(QStringLiteral("column"))) {
        sortBy = s.value(QStringLiteral("column")).toString();
        sortDesc = s.value(QStringLiteral("desc")).toBool();
    }
}

QString ManageRow::trackKey() const
{
    return icao + QStringLiteral("::") + trackSourceName(source) + QStringLiteral("::") + fileName;
}

// ---------------------------------------------------------------
namespace trackdb {

QString beijingText(qint64 ms)
{
    if (ms <= 0)
        return QString();
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::OffsetFromUTC, 8 * 3600)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

bool open(QString *error)
{
    if (isOpen())
        return true;
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QLatin1String(kMainConn));
    db.setDatabaseName(app::databasePath());
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=30000"));
    if (!db.open()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    return createSchema(db, error);
}

void close()
{
    {
        QSqlDatabase db = mainDb();
        if (db.isValid())
            db.close();
    }
    QSqlDatabase::removeDatabase(QLatin1String(kMainConn));
}

bool isOpen()
{
    return mainDb().isOpen();
}

QVector<Track> loadAll(QString *error)
{
    QVector<Track> out;
    QSqlQuery q(mainDb());
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT %1 FROM saved_tracks st JOIN batches b ON st.batch_id = b.id"
                               " WHERE st.deleted = 0 ORDER BY b.id, st.rowid").arg(QLatin1String(kTrackCols)))) {
        if (error)
            *error = q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(trackFromQuery(q));
    return out;
}

QVector<Track> loadTracks(const QStringList &keys)
{
    QVector<Track> out;
    QSqlQuery q(mainDb());
    q.prepare(QStringLiteral("SELECT %1 FROM saved_tracks st JOIN batches b ON st.batch_id = b.id"
                             " WHERE st.icao_address = ? AND st.source = ? AND b.file_name = ?")
                  .arg(QLatin1String(kTrackCols)));
    for (const QString &k : keys) {
        QString icao, src, file;
        if (!splitKey(k, &icao, &src, &file))
            continue;
        q.addBindValue(icao);
        q.addBindValue(src);
        q.addBindValue(file);
        if (q.exec() && q.next())
            out.append(trackFromQuery(q));
    }
    return out;
}

QVector<BatchInfo> batches()
{
    QVector<BatchInfo> out;
    QSqlQuery q(mainDb());
    q.exec(QStringLiteral("SELECT id, file_name, source, track_count, imported_at FROM batches ORDER BY id DESC"));
    while (q.next()) {
        BatchInfo b;
        b.id = q.value(0).toLongLong();
        b.fileName = q.value(1).toString();
        trackSourceFromName(q.value(2).toString(), &b.source);
        b.trackCount = q.value(3).toInt();
        b.importedAt = q.value(4).toString();
        out.append(b);
    }
    return out;
}

// 统计不含软删除的航迹（RadarView 的统计来自数据库全量，软删除只在前端过滤；这里删可见后统计随之减少更直观）
QVector<Track> loadBatchTracks(qint64 batchId)
{
    QVector<Track> out;
    QSqlQuery q(mainDb());
    q.prepare(QStringLiteral("SELECT %1 FROM saved_tracks st JOIN batches b ON st.batch_id = b.id"
                             " WHERE st.batch_id = ? ORDER BY st.rowid").arg(QLatin1String(kTrackCols)));
    q.addBindValue(batchId);
    if (q.exec())
        while (q.next())
            out.append(trackFromQuery(q));
    return out;
}

QStringList batchKeys(qint64 batchId)
{
    QStringList out;
    QSqlQuery q(mainDb());
    q.prepare(QStringLiteral("SELECT st.icao_address, st.source, b.file_name FROM saved_tracks st"
                             " JOIN batches b ON st.batch_id = b.id WHERE st.batch_id = ?"));
    q.addBindValue(batchId);
    if (!q.exec())
        return out;
    while (q.next())
        out << q.value(0).toString() + QStringLiteral("::") + q.value(1).toString() + QStringLiteral("::")
            + q.value(2).toString();
    return out;
}

ManageStats stats()
{
    ManageStats s;
    QSqlQuery q(mainDb());
    q.exec(QStringLiteral("SELECT source, COUNT(*) FROM saved_tracks WHERE deleted = 0 GROUP BY source"));
    while (q.next()) {
        s.bySource.insert(q.value(0).toString(), q.value(1).toInt());
        s.totalTracks += q.value(1).toInt();
    }
    q.exec(QStringLiteral("SELECT COUNT(*) FROM batches"));
    if (q.next())
        s.totalBatches = q.value(0).toInt();
    q.exec(QStringLiteral("SELECT COUNT(DISTINCT icao_address), MIN(min_ms), MAX(max_ms) FROM saved_tracks WHERE deleted = 0"));
    if (q.next()) {
        s.uniqueIcao = q.value(0).toInt();
        s.timeMin = q.value(1).toLongLong();
        s.timeMax = q.value(2).toLongLong();
    }
    return s;
}

QStringList distinctValues(const QString &column, const QString &source)
{
    QStringList out;
    if (column != QLatin1String("airline") && column != QLatin1String("aircraft_type"))
        return out;
    QSqlQuery q(mainDb());
    QString sql = QStringLiteral("SELECT DISTINCT %1 FROM saved_tracks WHERE deleted = 0 AND %1 IS NOT NULL AND %1 != ''").arg(column);
    TrackSource s;
    if (trackSourceFromKey(source, &s))
        sql += QStringLiteral(" AND source = ?");
    sql += QStringLiteral(" ORDER BY 1");
    q.prepare(sql);
    if (trackSourceFromKey(source, &s))
        q.addBindValue(trackSourceName(s));
    q.exec();
    while (q.next())
        out << q.value(0).toString();
    return out;
}

// 与 db.rs sort_column 相同的白名单
static QString sortColumn(const QString &by)
{
    static const char *const map[][2] = {
        {"icao_address", "st.icao_address"}, {"flight_no", "st.flight_no"}, {"registration", "st.registration"},
        {"aircraft_type", "st.aircraft_type"}, {"airline", "st.airline"}, {"origin", "st.origin"},
        {"destination", "st.destination"}, {"point_count", "st.point_count"},
        {"min_timestamp", "st.min_timestamp"}, {"max_timestamp", "st.max_timestamp"},
        {"batch_file_name", "b.file_name"}, {"batch_imported_at", "b.imported_at"},
    };
    for (const auto &m : map)
        if (by == QLatin1String(m[0]))
            return QLatin1String(m[1]);
    return QStringLiteral("b.imported_at");
}

QVector<ManageRow> query(const ManageFilter &f, int limit, int offset, int *total)
{
    QStringList where;
    QVariantList binds;
    where << QStringLiteral("st.deleted = 0");
    TrackSource src;
    if (trackSourceFromKey(f.source, &src)) {
        where << QStringLiteral("st.source = ?");
        binds << trackSourceName(src);
    }
    if (f.minTimeMs > 0) {
        where << QStringLiteral("st.max_timestamp >= ?");
        binds << beijingText(f.minTimeMs);
    }
    if (f.maxTimeMs > 0) {
        where << QStringLiteral("st.min_timestamp <= ?");
        binds << beijingText(f.maxTimeMs);
    }
    if (f.minPoints >= 0) {
        where << QStringLiteral("st.point_count >= ?");
        binds << f.minPoints;
    }
    if (f.maxPoints >= 0) {
        where << QStringLiteral("st.point_count <= ?");
        binds << f.maxPoints;
    }
    if (f.batchId >= 0) {
        where << QStringLiteral("st.batch_id = ?");
        binds << f.batchId;
    }
    if (!f.airline.isEmpty()) {
        where << QStringLiteral("st.airline = ?");
        binds << f.airline;
    }
    if (!f.aircraftType.isEmpty()) {
        where << QStringLiteral("st.aircraft_type = ?");
        binds << f.aircraftType;
    }
    const QString txt = f.searchText.trimmed().toLower();
    if (!txt.isEmpty()) {
        const QString like = QLatin1Char('%') + txt + QLatin1Char('%');
        QStringList ors;
        for (const char *c : { "st.icao_address", "st.flight_no", "st.icao_flight_no", "st.registration",
                               "st.aircraft_type", "st.airline", "st.origin", "st.destination" }) {
            ors << QStringLiteral("LOWER(%1) LIKE ?").arg(QLatin1String(c));
            binds << like;
        }
        where << QLatin1Char('(') + ors.join(QStringLiteral(" OR ")) + QLatin1Char(')');
    }
    const QString whereSql = where.join(QStringLiteral(" AND "));

    QSqlQuery q(mainDb());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM saved_tracks st JOIN batches b ON st.batch_id = b.id WHERE ") + whereSql);
    for (const QVariant &v : binds)
        q.addBindValue(v);
    *total = q.exec() && q.next() ? q.value(0).toInt() : 0;

    QVector<ManageRow> rows;
    q.prepare(QStringLiteral("SELECT st.icao_address, st.batch_id, st.source, st.flight_no, st.icao_flight_no,"
                             " st.registration, st.aircraft_type, st.airline, st.origin, st.destination,"
                             " st.min_timestamp, st.max_timestamp, st.point_count, b.file_name, b.imported_at"
                             " FROM saved_tracks st JOIN batches b ON st.batch_id = b.id WHERE %1"
                             " ORDER BY %2 %3, st.rowid LIMIT ? OFFSET ?")
                  .arg(whereSql, sortColumn(f.sortBy), f.sortDesc ? QStringLiteral("DESC") : QStringLiteral("ASC")));
    for (const QVariant &v : binds)
        q.addBindValue(v);
    q.addBindValue(limit);
    q.addBindValue(offset);
    if (!q.exec())
        return rows;
    while (q.next()) {
        ManageRow r;
        r.icao = q.value(0).toString();
        r.batchId = q.value(1).toLongLong();
        trackSourceFromName(q.value(2).toString(), &r.source);
        r.flightNo = q.value(3).toString();
        r.icaoFlightNo = q.value(4).toString();
        r.registration = q.value(5).toString();
        r.aircraftType = q.value(6).toString();
        r.airline = q.value(7).toString();
        r.origin = q.value(8).toString();
        r.destination = q.value(9).toString();
        r.minTs = q.value(10).toString();
        r.maxTs = q.value(11).toString();
        r.pointCount = q.value(12).toInt();
        r.fileName = q.value(13).toString();
        r.importedAt = q.value(14).toString();
        rows.append(r);
    }
    return rows;
}

bool setDeleted(const QStringList &keys, bool deleted)
{
    QSqlDatabase db = mainDb();
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE saved_tracks SET deleted = ? WHERE icao_address = ? AND source = ?"
                             " AND batch_id IN (SELECT id FROM batches WHERE file_name = ? AND source = ?)"));
    for (const QString &k : keys) {
        QString icao, src, file;
        if (!splitKey(k, &icao, &src, &file))
            continue;
        q.addBindValue(deleted ? 1 : 0);
        q.addBindValue(icao);
        q.addBindValue(src);
        q.addBindValue(file);
        q.addBindValue(src);
        q.exec();
    }
    return db.commit();
}

QSet<QString> deletedKeys()
{
    QSet<QString> out;
    QSqlQuery q(mainDb());
    q.exec(QStringLiteral("SELECT st.icao_address, st.source, b.file_name FROM saved_tracks st"
                          " JOIN batches b ON st.batch_id = b.id WHERE st.deleted = 1"));
    while (q.next())
        out.insert(q.value(0).toString() + QStringLiteral("::") + q.value(1).toString() + QStringLiteral("::")
                   + q.value(2).toString());
    return out;
}

// 批次级硬删除：删该批次全部航迹 + 批次行。点序列存在 BLOB 里，无独立点表。不可撤销。
bool deleteBatch(qint64 batchId, QString *error)
{
    QSqlDatabase db = mainDb();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM saved_tracks WHERE batch_id = ?"));
    q.addBindValue(batchId);
    if (!q.exec()) {
        db.rollback();
        if (error)
            *error = q.lastError().text();
        return false;
    }
    q.prepare(QStringLiteral("DELETE FROM batches WHERE id = ?"));
    q.addBindValue(batchId);
    if (!q.exec()) {
        db.rollback();
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return db.commit();
}

} // namespace trackdb

// ---------------------------------------------------------------
DbSaveJob::DbSaveJob(const QString &fileName, TrackSource source, const QVector<Track> &tracks, QObject *parent)
    : QThread(parent), m_path(app::databasePath()), m_fileName(fileName), m_source(source), m_tracks(tracks)
{
}

void DbSaveJob::run()
{
    const QString conn = QStringLiteral("radarview_save_%1").arg(quintptr(this));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(m_path);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=30000"));
        if (!db.open()) {
            m_error = db.lastError().text();
        } else if (createSchema(db, &m_error)) {
            const QString src = trackSourceName(m_source);
            QSqlQuery q(db);
            db.transaction();
            qint64 batchId = -1;
            q.prepare(QStringLiteral("SELECT id FROM batches WHERE file_name = ? AND source = ? ORDER BY id LIMIT 1"));
            q.addBindValue(m_fileName);
            q.addBindValue(src);
            if (q.exec() && q.next()) {
                batchId = q.value(0).toLongLong();
                m_appended = true;
            }
            QSet<QString> existing;
            bool okAll = true;
            if (batchId < 0) {
                q.prepare(QStringLiteral("INSERT INTO batches (file_name, source, track_count, imported_at) VALUES (?, ?, ?, ?)"));
                q.addBindValue(m_fileName);
                q.addBindValue(src);
                q.addBindValue(m_tracks.size());
                q.addBindValue(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                okAll = q.exec();
                batchId = q.lastInsertId().toLongLong();
            } else {
                // 重新导入：恢复该批次被软删除的航迹，只补缺失的
                q.prepare(QStringLiteral("UPDATE saved_tracks SET deleted = 0 WHERE batch_id = ?"));
                q.addBindValue(batchId);
                q.exec();
                q.prepare(QStringLiteral("SELECT icao_address FROM saved_tracks WHERE batch_id = ?"));
                q.addBindValue(batchId);
                if (q.exec())
                    while (q.next())
                        existing.insert(q.value(0).toString());
            }
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO saved_tracks (icao_address, batch_id, source, flight_no, icao_flight_no,"
                " registration, aircraft_type, airline, origin, destination, min_timestamp, max_timestamp,"
                " min_ms, max_ms, point_count, deleted, points) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?)"));
            for (int i = 0; okAll && i < m_tracks.size(); ++i) {
                Track &t = m_tracks[i];
                if (existing.contains(t.id))
                    continue;
                t.normalize();
                if (t.points.isEmpty())
                    continue;
                ins.addBindValue(t.id);
                ins.addBindValue(batchId);
                ins.addBindValue(src);
                ins.addBindValue(t.flightNo);
                ins.addBindValue(t.icaoFlightNo);
                ins.addBindValue(t.registration);
                ins.addBindValue(t.aircraftType);
                ins.addBindValue(t.airline);
                ins.addBindValue(t.origin);
                ins.addBindValue(t.destination);
                ins.addBindValue(trackdb::beijingText(t.minTime()));
                ins.addBindValue(trackdb::beijingText(t.maxTime()));
                ins.addBindValue(t.minTime());
                ins.addBindValue(t.maxTime());
                ins.addBindValue(t.points.size());
                ins.addBindValue(encodePoints(t.points));
                if (!ins.exec()) {
                    m_error = ins.lastError().text();
                    okAll = false;
                }
                ++m_saved;
            }
            if (okAll && m_appended && m_saved > 0) {
                q.prepare(QStringLiteral("UPDATE batches SET track_count = track_count + ? WHERE id = ?"));
                q.addBindValue(m_saved);
                q.addBindValue(batchId);
                q.exec();
            }
            if (okAll && db.commit()) {
                m_ok = true;
            } else {
                if (m_error.isEmpty())
                    m_error = db.lastError().text();
                db.rollback();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}
