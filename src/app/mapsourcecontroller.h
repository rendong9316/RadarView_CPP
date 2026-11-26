#ifndef MAPSOURCECONTROLLER_H
#define MAPSOURCECONTROLLER_H

#include <QObject>
#include <QString>

class GlobeWidget;
class QAction;
class QMenu;
class QWidget;

// 底图源控制器：本地 MBTiles 自动探测/打开、天地图/高德在线底图、近岸离线高德切换与菜单勾选同步
class MapSourceController : public QObject
{
    Q_OBJECT
public:
    MapSourceController(GlobeWidget *globe, QWidget *dialogParent, QObject *parent = nullptr);

    void populateSourceMenu(QMenu *menu);
    bool openTiles(const QString &path);
    void loadDefaultTiles();
    void setMapSource(const QString &id);          // offline / tdt / amap / amap_offline
    bool openOnlineMap(const QString &token, bool useAmap = false);
    void openTileFile();
    void syncSourceActions();

private:
    GlobeWidget *m_globe = nullptr;
    QWidget *m_dialogParent = nullptr;
    QAction *m_srcOffline = nullptr;
    QAction *m_srcTdt = nullptr;
    QAction *m_srcAmap = nullptr;
    QAction *m_srcAmapOffline = nullptr;
};

#endif // MAPSOURCECONTROLLER_H
