#include "apppaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

namespace app {

namespace {
bool g_test = false;
std::unique_ptr<QSettings> g_settings;
QTimer *g_syncTimer = nullptr;

QString testDir()
{
    return QCoreApplication::applicationDirPath();
}

// 合并落盘：写操作只把 QSettings 标记为脏，300ms 内没有新的写入才真正 sync
void ensureSyncTimer()
{
    if (g_syncTimer)
        return;
    g_syncTimer = new QTimer();
    g_syncTimer->setSingleShot(true);
    g_syncTimer->setInterval(300);
    QObject::connect(g_syncTimer, &QTimer::timeout, []() {
        if (g_settings)
            g_settings->sync();
    });
    // 退出时兜底落盘（Linux 下 ini 后端不 sync 会丢掉最近改动）
    if (QCoreApplication::instance()) {
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                         []() { flushSettings(); });
    }
}

QString dataDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/RadarView/HelloVscode");
    QDir().mkpath(dir);
    return dir;
}
} // namespace

void setTestMode(bool on)
{
    if (g_settings)
        g_settings->sync();
    g_test = on;
    g_settings.reset();
    if (on) {
        const QString db = testDir() + QStringLiteral("/test_radarview.db");
        for (const QString &suffix : { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") })
            QFile::remove(db + suffix);
        QFile::remove(testDir() + QStringLiteral("/test_settings.ini"));
        QFile::remove(testDir() + QStringLiteral("/test_drawings.json"));
    }
}

bool testMode()
{
    return g_test;
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

QSettings &settings()
{
    if (!g_settings) {
        if (g_test) {
            g_settings.reset(new QSettings(testDir() + QStringLiteral("/test_settings.ini"), QSettings::IniFormat));
            g_settings->setIniCodec("UTF-8");
        } else {
            g_settings.reset(new QSettings(QStringLiteral("RadarView"), QStringLiteral("HelloVscode")));
        }
        ensureSyncTimer();
    }
    return *g_settings;
}

void setValue(const QString &key, const QVariant &value)
{
    settings().setValue(key, value);
    if (g_syncTimer)
        g_syncTimer->start();
}

void removeValue(const QString &key)
{
    settings().remove(key);
    if (g_syncTimer)
        g_syncTimer->start();
}

void flushSettings()
{
    if (g_syncTimer)
        g_syncTimer->stop();
    if (g_settings)
        g_settings->sync();
}

} // namespace app
