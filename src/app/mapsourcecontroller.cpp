#include "mapsourcecontroller.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>

#include "apppaths.h"
#include "globewidget.h"
#include "tilesource.h"

namespace {

// 内置天地图 key（个人版）。需要换 key 时改这里，或用启动参数 --online-key 覆盖
const char *const kTdtToken = "5e2d0f18562e921064ef494dd4cd76eb";

} // namespace

MapSourceController::MapSourceController(GlobeWidget *globe, QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_globe(globe)
    , m_dialogParent(dialogParent)
{
}

void MapSourceController::populateSourceMenu(QMenu *menu)
{
    auto addSource = [this, menu](const QString &id, const QString &text) {
        QAction *a = menu->addAction(text);
        a->setCheckable(true);
        connect(a, &QAction::triggered, this, [this, id]() { setMapSource(id); });
        return a;
    };
    m_srcOffline = addSource(QStringLiteral("offline"), tr("离线瓦片 (.mbtiles)"));
    m_srcTdt = addSource(QStringLiteral("tdt"), tr("天地图（在线 · 街道级 z18）"));
    m_srcAmap = addSource(QStringLiteral("amap"), tr("高德（在线 · 更快）"));
    m_srcAmap->setToolTip(tr("高德瓦片的坐标基准和 GPS 不同，程序内部自动对齐；\n"
                             "界面显示、数据库和导出的坐标始终是 WGS84"));
    m_srcAmapOffline = addSource(QStringLiteral("amap_offline"), tr("高德（离线 · 近岸 z1-z16）"));
    m_srcAmapOffline->setToolTip(tr("中国东部+东南沿海近岸走廊 z16 离线瓦片；\n"
                                    "需先运行 tools/download_tiles.py 生成 amap_coast_z1_z16.mbtiles"));
    syncSourceActions();
}

bool MapSourceController::openTiles(const QString &path)
{
    QString err;
    if (!m_globe->openTiles(path, &err)) {
        QMessageBox::warning(m_dialogParent, tr("打开瓦片失败"), err);
        return false;
    }
    return true;
}

// 启动时自动加载：优先 exe 旁 tiles/ 目录，其次 exe 同目录；
// 优先级：离线高德近岸 z16 > natural_earth4 > 最高 maxZoom
void MapSourceController::loadDefaultTiles()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QFileInfoList files;
    for (const QString &dir : { appDir + QStringLiteral("/tiles"), appDir })
        files += QDir(dir).entryInfoList(QStringList() << QStringLiteral("*.mbtiles"), QDir::Files);

    QString best;
    int bestZoom = -1;
    QString amapOfflinePath;
    QString natural4Path;
    for (const QFileInfo &fi : qAsConst(files)) {
        if (amapOfflinePath.isEmpty() && fi.fileName() == QLatin1String("amap_coast_z1_z16.mbtiles"))
            amapOfflinePath = fi.absoluteFilePath();
        if (natural4Path.isEmpty() && fi.fileName() == QLatin1String("natural_earth4.mbtiles"))
            natural4Path = fi.absoluteFilePath();
        TileSource probe;
        if (probe.open(fi.absoluteFilePath()) && probe.maxZoom() > bestZoom) {
            bestZoom = probe.maxZoom();
            best = fi.absoluteFilePath();
        }
    }
    if (!amapOfflinePath.isEmpty())
        best = amapOfflinePath;
    else if (!natural4Path.isEmpty())
        best = natural4Path;
    if (!best.isEmpty())
        openTiles(best);

    // 上次退出时用的底图源就恢复；离线/无网络时本地瓦片照常可用
    const QString src = app::settings().value(QStringLiteral("map.source"), QStringLiteral("offline")).toString();
    if (src == QLatin1String("tdt") || src == QLatin1String("amap")) {
        QString token = app::settings().value(QStringLiteral("map.tdt_token")).toString();
        if (token.trimmed().isEmpty())
            token = QString::fromLatin1(kTdtToken);
        if (!openOnlineMap(token, src == QLatin1String("amap")))
            app::setValue(QStringLiteral("map.source"), QStringLiteral("offline"));
    } else if (src == QLatin1String("amap_offline")) {
        m_globe->closeOnlineMap();
        QString found;
        for (const QString &dir : { appDir + QStringLiteral("/tiles"), appDir }) {
            const QString p = dir + QStringLiteral("/amap_coast_z1_z16.mbtiles");
            if (QFile::exists(p)) {
                found = p;
                break;
            }
        }
        if (!found.isEmpty())
            openTiles(found);
        else
            app::setValue(QStringLiteral("map.source"), QStringLiteral("offline"));
    }
    syncSourceActions();
}

void MapSourceController::syncSourceActions()
{
    const QString src = app::settings().value(QStringLiteral("map.source"), QStringLiteral("offline")).toString();
    struct { QAction *action; const char *id; } items[] = {
        { m_srcOffline, "offline" }, { m_srcTdt, "tdt" },
        { m_srcAmap, "amap" }, { m_srcAmapOffline, "amap_offline" }
    };
    for (auto &it : items) {
        if (!it.action)
            continue;
        QSignalBlocker blocker(it.action);
        it.action->setChecked(src == QLatin1String(it.id));
    }
}

bool MapSourceController::openOnlineMap(const QString &token, bool useAmap)
{
    QString err;
    if (!m_globe->openOnlineMap(token, useAmap, &err)) {
        QMessageBox::warning(m_dialogParent, tr("在线底图"), err);
        return false;
    }
    app::setValue(QStringLiteral("map.tdt_token"), token.trimmed());
    return true;
}

// 底图源切换：offline=本地 mbtiles，tdt=天地图，amap=高德，amap_offline=离线高德近岸
void MapSourceController::setMapSource(const QString &id)
{
    if (id == QLatin1String("amap_offline")) {
        m_globe->closeOnlineMap();
        const QString appDir = QCoreApplication::applicationDirPath();
        QString found;
        for (const QString &dir : { appDir + QStringLiteral("/tiles"), appDir }) {
            const QString p = dir + QStringLiteral("/amap_coast_z1_z16.mbtiles");
            if (QFile::exists(p)) {
                found = p;
                break;
            }
        }
        if (found.isEmpty()) {
            QMessageBox::warning(m_dialogParent, tr("离线高德瓦片"),
                tr("未找到 amap_coast_z1_z16.mbtiles。\n"
                   "请先运行 python tools/download_tiles.py 生成瓦片包。"));
            syncSourceActions();
            return;
        }
        openTiles(found);
        app::setValue(QStringLiteral("map.source"), id);
        syncSourceActions();
        return;
    }

    if (id == QLatin1String("offline")) {
        m_globe->closeOnlineMap();
        // 先把用户意图记成 offline 再加载默认瓦片：loadDefaultTiles() 末尾会按
        // map.source 恢复在线源，只有先置成 offline 才不会被拉回天地图/高德。
        app::setValue(QStringLiteral("map.source"), id);
        loadDefaultTiles();               // 始终切回目录里的默认离线瓦片
        if (m_globe->tileName().isEmpty())
            QMessageBox::warning(m_dialogParent, tr("离线瓦片"),
                tr("未找到可用的 .mbtiles 瓦片文件。\n"
                   "请将瓦片包放入程序目录或 tiles/ 子目录，或用「打开瓦片…」手动选择。"));
        syncSourceActions();
        return;
    }

    // key 已内置；启动参数 --online-key 可临时换
    QString token = app::settings().value(QStringLiteral("map.tdt_token")).toString();
    if (token.trimmed().isEmpty())
        token = QString::fromLatin1(kTdtToken);

    if (!openOnlineMap(token, id == QLatin1String("amap"))) {
        syncSourceActions();                  // 失败则勾选状态回到原来的源
        return;
    }
    app::setValue(QStringLiteral("map.source"), id);
    syncSourceActions();
}

void MapSourceController::openTileFile()
{
    const QString path = QFileDialog::getOpenFileName(
        m_dialogParent, tr("打开瓦片"), QCoreApplication::applicationDirPath(),
        tr("MBTiles 瓦片 (*.mbtiles);;所有文件 (*)"));
    if (!path.isEmpty()) {
        openTiles(path);
        app::setValue(QStringLiteral("map.source"), QStringLiteral("offline"));
        syncSourceActions();
    }
}
