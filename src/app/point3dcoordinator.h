#ifndef POINT3DCOORDINATOR_H
#define POINT3DCOORDINATOR_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include "track.h"

class GlobeWidget;
class Point3dWindow;
class QMenu;

// 点迹三维多窗口协调器：窗口生命周期、会话恢复、窗口菜单填充及 地图 <-> 点迹 双向联动
class Point3dCoordinator : public QObject
{
    Q_OBJECT
public:
    Point3dCoordinator(TrackStore *store, GlobeWidget *globe, QObject *parent = nullptr);

    Point3dWindow *createWindow();
    void showNewWindow();
    QList<Point3dWindow *> windows() const;

    void saveSessions();
    void restoreSessions();
    void closeAllAndSave();

    void syncLinkedSelection(const QSet<QString> &visibleKeys, int isolatedIndex);
    void onMapHoverTrack(int index);
    void refreshOverlayColors();
    void populateWindowMenu(QMenu *menu);

signals:
    void isolateTrackRequested(int index);
    void replayRangeInvalidated();

private:
    int trackIndexForBatch(int batchNo, TrackSource source, qint64 t0, qint64 t1) const;
    void onPoint3dFocus(int batchNo, TrackSource source, qint64 t0, qint64 t1);
    void onPoint3dActivate(int batchNo, TrackSource source, qint64 t0, qint64 t1);

    TrackStore *m_store = nullptr;
    GlobeWidget *m_globe = nullptr;
    QList<QPointer<Point3dWindow>> m_windows;   // 已打开的点云窗口（可多开）
    int m_counter = 0;
    bool m_closingAll = false;                  // 批量关闭时抑制重复写会话
};

#endif // POINT3DCOORDINATOR_H
