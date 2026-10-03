#include "apppaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <memory>

namespace app {

namespace {
bool g_test = false;
std::unique_ptr<QSettings> g_settings;

QString testDir()
{
    return QCoreApplication::applicationDirPath();
}
} // namespace

void setTestMode(bool on)
{
    g_test = on;
    g_settings.reset();
    if (on) {
        const QString db = testDir() + QStringLiteral("/test_radarview.db");
        for (const QString &suffix : { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") })
            QFile::remove(db + suffix);
        QFile::remove(testDir() + QStringLiteral("/test_settings.ini"));
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
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/RadarView/HelloVscode");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/radarview.db");
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
    }
    return *g_settings;
}

} // namespace app
