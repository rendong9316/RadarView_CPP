#include "apppaths.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>
#include <memory>

namespace app {

namespace {

const char *const kSettingsConn = "radarview_settings";
bool g_test = false;
QTimer *g_flushTimer = nullptr;

QString testDir()
{
    return QCoreApplication::applicationDirPath();
}

bool copyFileAtomic(const QString &src, const QString &dst)
{
    if (!QFileInfo::exists(src) || QFileInfo::exists(dst))
        return true;
    const QString tmp = dst + QStringLiteral(".migrating");
    QFile::remove(tmp);
    if (!QFile::copy(src, tmp)) {
        qWarning("[apppaths] 复制旧文件失败: %s -> %s", qPrintable(src), qPrintable(dst));
        return false;
    }
    if (!QFile::rename(tmp, dst)) {
        QFile::remove(tmp);
        qWarning("[apppaths] 落盘迁移文件失败: %s", qPrintable(dst));
        return false;
    }
    return true;
}

bool copyDirRecursive(const QString &srcPath, const QString &dstPath)
{
    QDir srcDir(srcPath);
    if (!srcDir.exists())
        return true;
    if (!QDir().mkpath(dstPath))
        return false;
    bool ok = true;
    const QFileInfoList entries = srcDir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString dstEntry = dstPath + QLatin1Char('/') + entry.fileName();
        if (entry.isDir()) {
            if (!copyDirRecursive(entry.absoluteFilePath(), dstEntry))
                ok = false;
        } else {
            if (!copyFileAtomic(entry.absoluteFilePath(), dstEntry))
                ok = false;
        }
    }
    return ok;
}

QString dataDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/RadarView/RadarView");
    static bool migrated = false;
    if (!migrated) {
        migrated = true;
        migrateLegacyDataDir(legacyDataDir(), dir);
    } else {
        QDir().mkpath(dir);
    }
    return dir;
}

// ---- 值编码：value 存明文，type 记住 QVariant 类型，SQL 里能直接读 ----
QString encodeType(const QVariant &v)
{
    switch (v.type()) {
    case QVariant::Bool: return QStringLiteral("bool");
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong: return QStringLiteral("int");
    case QVariant::Double: return QStringLiteral("double");
    case QVariant::String: return QStringLiteral("string");
    case QVariant::StringList: return QStringLiteral("stringlist");
    case QVariant::ByteArray: return QStringLiteral("bytes");
    default: return QStringLiteral("variant");
    }
}

QString encodeValue(const QVariant &v)
{
    switch (v.type()) {
    case QVariant::Bool: return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong: return QString::number(v.toLongLong());
    case QVariant::Double: return QString::number(v.toDouble(), 'g', 17);
    case QVariant::String: return v.toString();
    case QVariant::StringList:
        return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(v.toStringList()))
                                     .toJson(QJsonDocument::Compact));
    case QVariant::ByteArray: return QString::fromLatin1(v.toByteArray().toBase64());
    default: {
        QByteArray blob;
        QDataStream ds(&blob, QIODevice::WriteOnly);
        ds.setVersion(QDataStream::Qt_5_6);
        ds << v;
        return QString::fromLatin1(blob.toBase64());
    }
    }
}

QVariant decodeValue(const QString &value, const QString &type)
{
    if (type == QLatin1String("bool"))
        return value == QLatin1String("true");
    if (type == QLatin1String("int"))
        return value.toLongLong();
    if (type == QLatin1String("double"))
        return value.toDouble();
    if (type == QLatin1String("stringlist")) {
        QStringList list;
        for (const QJsonValue &v : QJsonDocument::fromJson(value.toUtf8()).array())
            list << v.toString();
        return list;
    }
    if (type == QLatin1String("bytes"))
        return QByteArray::fromBase64(value.toLatin1());
    if (type == QLatin1String("variant")) {
        QByteArray blob = QByteArray::fromBase64(value.toLatin1());
        QDataStream ds(&blob, QIODevice::ReadOnly);
        ds.setVersion(QDataStream::Qt_5_6);
        QVariant v;
        ds >> v;
        return v;
    }
    return value;
}

// 合并落库：写操作先落缓存，300ms 内没有新写入才真正写数据库
void ensureFlushTimer()
{
    if (g_flushTimer)
        return;
    g_flushTimer = new QTimer();
    g_flushTimer->setSingleShot(true);
    g_flushTimer->setInterval(300);
    QObject::connect(g_flushTimer, &QTimer::timeout, []() { settings().sync(); });
    if (QCoreApplication::instance()) {
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                         []() { flushSettings(); });
    }
}

// 老版本把设置写在 QSettings（Windows 注册表 / Linux ini）里，第一次跑新版本时搬过来
void migrateFromQSettings(Settings &st)
{
    std::unique_ptr<QSettings> legacy;
    if (g_test)
        legacy.reset(new QSettings(testDir() + QStringLiteral("/test_settings.ini"), QSettings::IniFormat));
    else
        legacy.reset(new QSettings(QStringLiteral("RadarView"), QStringLiteral("HelloVscode")));
    const QStringList keys = legacy->allKeys();
    if (keys.isEmpty())
        return;
    for (const QString &key : keys)
        st.setValue(key, legacy->value(key));
    st.setValue(QStringLiteral("app.migrated_from_qsettings"), true);
    st.sync();
    qInfo("[settings] 已从旧 QSettings 迁移 %d 项到数据库", keys.size());
}

} // namespace

void setTestMode(bool on)
{
    closeSettings();
    g_test = on;
    if (on) {
        const QString db = testDir() + QStringLiteral("/test_radarview.db");
        for (const QString &suffix : { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") })
            QFile::remove(db + suffix);
        QFile::remove(testDir() + QStringLiteral("/test_settings.ini"));
        QFile::remove(testDir() + QStringLiteral("/test_drawings.json"));
        QDir(testDir() + QStringLiteral("/test_tilecache")).removeRecursively();
    }
}

bool testMode()
{
    return g_test;
}

QString legacyDataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
         + QStringLiteral("/RadarView/HelloVscode");
}

bool migrateLegacyDataDir(const QString &legacyDir, const QString &newDir)
{
    if (newDir.isEmpty())
        return false;
    QDir().mkpath(newDir);
    if (legacyDir.isEmpty() || QDir::cleanPath(legacyDir) == QDir::cleanPath(newDir))
        return true;
    if (!QDir(legacyDir).exists())
        return true;

    bool ok = true;

    // 1) 航迹与设置数据库（含 SQLite WAL 附属文件）：先迁 -wal/-shm 再原子落盘主库，旧文件保留不动
    const QString legacyDb = legacyDir + QStringLiteral("/radarview.db");
    const QString newDb = newDir + QStringLiteral("/radarview.db");
    if (QFileInfo::exists(legacyDb) && !QFileInfo::exists(newDb)) {
        for (const QString &suffix : { QStringLiteral("-wal"), QStringLiteral("-shm") }) {
            if (!copyFileAtomic(legacyDb + suffix, newDb + suffix))
                ok = false;
        }
        if (copyFileAtomic(legacyDb, newDb))
            qInfo("[apppaths] 已从旧目录迁移数据库到 %s", qPrintable(newDb));
        else
            ok = false;
    }

    // 2) 绘图内容 GeoJSON：仅当新目录尚无时复制，保留旧文件
    const QString legacyDraw = legacyDir + QStringLiteral("/drawings.json");
    const QString newDraw = newDir + QStringLiteral("/drawings.json");
    if (QFileInfo::exists(legacyDraw) && !QFileInfo::exists(newDraw)) {
        if (copyFileAtomic(legacyDraw, newDraw))
            qInfo("[apppaths] 已从旧目录迁移绘图到 %s", qPrintable(newDraw));
        else
            ok = false;
    }

    // 3) 在线瓦片缓存：同卷优先原子重命名（避免海量 PNG 拷贝），失败则递归复制
    const QString legacyCache = legacyDir + QStringLiteral("/tilecache");
    const QString newCache = newDir + QStringLiteral("/tilecache");
    if (QDir(legacyCache).exists() && !QDir(newCache).exists()) {
        if (QDir().rename(legacyCache, newCache)) {
            qInfo("[apppaths] 已从旧目录迁移瓦片缓存到 %s", qPrintable(newCache));
        } else if (copyDirRecursive(legacyCache, newCache)) {
            qInfo("[apppaths] 已从旧目录复制瓦片缓存到 %s", qPrintable(newCache));
        } else {
            ok = false;
        }
    }

    return ok;
}

QString databasePath()
{
    if (g_test)
        return testDir() + QStringLiteral("/test_radarview.db");
    return dataDir() + QStringLiteral("/radarview.db");
}

QString drawingPath()
{
    if (g_test)
        return testDir() + QStringLiteral("/test_drawings.json");
    return dataDir() + QStringLiteral("/drawings.json");
}

QString tileCacheDir()
{
    if (g_test)
        return testDir() + QStringLiteral("/test_tilecache");
    return dataDir() + QStringLiteral("/tilecache");
}

// ---------------------------------------------------------------
//  Settings
// ---------------------------------------------------------------

static void migrateSettingsSchema(QSqlDatabase &db, QSqlQuery &q)
{
    bool needMigrate = false;
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(app_settings)"))) {
        while (info.next()) {
            if (info.value(1).toString() == QLatin1String("value") && info.value(3).toInt() == 1)
                needMigrate = true;
        }
    }
    if (needMigrate) {
        q.exec(QStringLiteral("ALTER TABLE app_settings RENAME TO app_settings_old"));
        q.exec(QStringLiteral("CREATE TABLE app_settings (key TEXT PRIMARY KEY, value TEXT, type TEXT NOT NULL DEFAULT 'string')"));
        q.exec(QStringLiteral("INSERT INTO app_settings(key, value, type) SELECT key, value, type FROM app_settings_old"));
        q.exec(QStringLiteral("DROP TABLE app_settings_old"));
    }
}

void Settings::ensureLoaded()
{
    if (m_loaded)
        return;
    m_loaded = true;

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QLatin1String(kSettingsConn));
    db.setDatabaseName(databasePath());
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=30000"));
    if (!db.open()) {
        qWarning("[settings] 打开设置库失败: %s", qPrintable(db.lastError().text()));
        return;
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA busy_timeout=30000"));
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS app_settings ("
                          " key TEXT PRIMARY KEY, value TEXT, type TEXT NOT NULL DEFAULT 'string')"));
    migrateSettingsSchema(db, q);
    if (q.exec(QStringLiteral("SELECT key, value, type FROM app_settings"))) {
        while (q.next())
            m_cache.insert(q.value(0).toString(), decodeValue(q.value(1).toString(), q.value(2).toString()));
    }
    if (m_cache.isEmpty())
        migrateFromQSettings(*this);
}

QVariant Settings::value(const QString &key, const QVariant &defaultValue)
{
    ensureLoaded();
    const auto it = m_cache.constFind(key);
    return it == m_cache.constEnd() ? defaultValue : it.value();
}

void Settings::setValue(const QString &key, const QVariant &v)
{
    ensureLoaded();
    m_cache.insert(key, v);
    m_removed.remove(key);
    m_dirty.insert(key);
    ensureFlushTimer();
    if (g_flushTimer)
        g_flushTimer->start();
}

void Settings::remove(const QString &key)
{
    ensureLoaded();
    m_cache.remove(key);
    m_dirty.remove(key);
    m_removed.insert(key);
    ensureFlushTimer();
    if (g_flushTimer)
        g_flushTimer->start();
}

bool Settings::contains(const QString &key)
{
    ensureLoaded();
    return m_cache.contains(key);
}

QStringList Settings::keys()
{
    ensureLoaded();
    return m_cache.keys();
}

void Settings::sync()
{
    if (m_dirty.isEmpty() && m_removed.isEmpty())
        return;
    QSqlDatabase db = QSqlDatabase::database(QLatin1String(kSettingsConn), false);
    if (!db.isOpen())
        return;                                   // 库没打开：留着脏值，别丢
    db.transaction();
    // 每条记录用独立的 QSqlQuery：复用同一查询对象并反复 addBindValue 会让绑定值累加，
    // 导致第 1 次 exec 之后的写全部失败（静默丢失），持久化看起来“卡在旧状态”。
    for (const QString &key : m_removed) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM app_settings WHERE key = ?"));
        del.addBindValue(key);
        if (!del.exec())
            qWarning("[settings] 删除设置失败 %s: %s", qPrintable(key), qPrintable(del.lastError().text()));
    }
    for (const QString &key : m_dirty) {
        const auto it = m_cache.constFind(key);
        if (it == m_cache.constEnd())
            continue;
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral("INSERT OR REPLACE INTO app_settings (key, value, type) VALUES (?, ?, ?)"));
        ins.addBindValue(key);
        ins.addBindValue(encodeValue(it.value()));
        ins.addBindValue(encodeType(it.value()));
        if (!ins.exec())
            qWarning("[settings] 写入设置失败 %s: %s", qPrintable(key), qPrintable(ins.lastError().text()));
    }
    if (!db.commit()) {
        qWarning("[settings] 写设置失败: %s", qPrintable(db.lastError().text()));
        db.rollback();
        return;
    }
    m_dirty.clear();
    m_removed.clear();
}

Settings &settings()
{
    static Settings s;
    return s;
}

void setValue(const QString &key, const QVariant &value)
{
    settings().setValue(key, value);
}

void removeValue(const QString &key)
{
    settings().remove(key);
}

void flushSettings()
{
    if (g_flushTimer)
        g_flushTimer->stop();
    settings().sync();
}

void closeSettings()
{
    if (g_flushTimer)
        g_flushTimer->stop();
    {
        Settings &s = settings();
        s.sync();
        // 让缓存跟着连接一起作废：下次 ensureLoaded 重新读库
        s = Settings();
    }
    {
        QSqlDatabase db = QSqlDatabase::database(QLatin1String(kSettingsConn), false);
        if (db.isOpen())
            db.close();
    }
    if (QSqlDatabase::contains(QLatin1String(kSettingsConn)))
        QSqlDatabase::removeDatabase(QLatin1String(kSettingsConn));
}

} // namespace app
