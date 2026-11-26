#include "point3dcoordinator.h"

#include <QAction>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <algorithm>

#include "apppaths.h"
#include "globewidget.h"
#include "point3dtypes.h"
#include "point3dwindow.h"
#include "tracklayer.h"

Point3dCoordinator::Point3dCoordinator(TrackStore *store, GlobeWidget *globe, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_globe(globe)
{
}

// 新建一个点迹三维窗口。可多开；每个窗口独立数据集
Point3dWindow *Point3dCoordinator::createWindow()
{
    Point3dWindow *win = new Point3dWindow(nullptr);
    win->setNumber(++m_counter);
    // 点云航迹颜色与地图上的文件颜色一致
    win->setTrackColorProvider([this](TrackSource s, const QString &fileName) {
        return m_store->fileColor(s, fileName);
    });
    m_windows.append(QPointer<Point3dWindow>(win));
    connect(win, &Point3dWindow::closed, this, [this, win]() {
        m_windows.removeAll(QPointer<Point3dWindow>(win));
        if (!m_closingAll) {
            saveSessions();
            emit replayRangeInvalidated();
        }
    });
    // 点迹数据变化后重算全局时间范围并保存现场
    connect(win, &Point3dWindow::dataChanged, this, [this]() {
        emit replayRangeInvalidated();
        if (!m_closingAll)
            saveSessions();
    });
    // 点云 <-> 地图全链路联动
    connect(win, &Point3dWindow::trackFocusChanged, this, &Point3dCoordinator::onPoint3dFocus);
    connect(win, &Point3dWindow::trackActivated, this, &Point3dCoordinator::onPoint3dActivate);
    connect(win, &Point3dWindow::isolationCleared, this, [this]() {
        emit isolateTrackRequested(-1);
    });
    connect(win, &Point3dWindow::frameTimeActivated, this, &Point3dCoordinator::masterSeekRequested);
    return win;
}

void Point3dCoordinator::showNewWindow()
{
    Point3dWindow *win = createWindow();
    win->show();
    win->raise();
    win->activateWindow();
}

QList<Point3dWindow *> Point3dCoordinator::windows() const
{
    QList<Point3dWindow *> out;
    for (const QPointer<Point3dWindow> &win : m_windows)
        if (win)
            out.append(win.data());
    return out;
}

void Point3dCoordinator::saveSessions()
{
    QJsonArray arr;
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
        if (win)
            arr.append(win->sessionJson());
    app::setValue(QStringLiteral("point3d.windows"),
                  QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

// 启动时按上次的会话恢复点迹窗口（可在高级参数里关闭 auto_reload）
void Point3dCoordinator::restoreSessions()
{
    if (app::testMode())
        return;
    if (!app::settings().value(QStringLiteral("point3d.auto_reload"), true).toBool())
        return;
    const QByteArray json = app::settings().value(QStringLiteral("point3d.windows")).toString().toUtf8();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    if (arr.isEmpty())
        return;
    for (const QJsonValue &v : arr) {
        Point3dWindow *win = createWindow();
        win->restoreSession(v.toObject());
        win->show();
    }
    emit replayRangeInvalidated();
}

void Point3dCoordinator::closeAllAndSave()
{
    m_closingAll = true;
    saveSessions();               // 先存现场，再关窗口
    const QList<QPointer<Point3dWindow>> wins = m_windows;
    for (const QPointer<Point3dWindow> &win : wins)
        if (win)
            win->close();
    m_windows.clear();
}

// 单独显示（点地图上某条航迹）或管理面板筛选出的航迹 → 点迹窗口里对应的叠加航迹同步单独显示。
// 判定用 BatchNo + 时间范围：同一 MAT 按「雷达航迹」和「雷达原始测量」分别导入时
// id 前缀不同（RADAR-/RAW-），但编号与时间一致，三方据此互相对应。
void Point3dCoordinator::syncLinkedSelection(const QSet<QString> &visibleKeys, int isolatedIndex)
{
    QSet<int> iso;
    if (isolatedIndex >= 0)
        iso.insert(isolatedIndex);
    syncLinkedSelection(visibleKeys, iso);
}

void Point3dCoordinator::syncLinkedSelection(const QSet<QString> &visibleKeys, const QSet<int> &isolatedIndices)
{
    QVector<LinkedTrackRef> refs;
    if (!visibleKeys.isEmpty()) {
        for (int i = 0; i < m_store->size(); ++i) {
            const Track &t = m_store->tracks()[i];
            if (visibleKeys.contains(t.key())) {
                LinkedTrackRef r;
                r.batchNo = point3dBatchNo(t);
                r.t0 = t.minTime();
                r.t1 = t.maxTime();
                refs.append(r);
            }
        }
    } else if (!isolatedIndices.isEmpty()) {
        for (int idx : isolatedIndices) {
            if (idx < 0 || idx >= m_store->size())
                continue;
            const Track &t = m_store->tracks().at(idx);
            LinkedTrackRef r;
            r.batchNo = point3dBatchNo(t);
            r.t0 = t.minTime();
            r.t1 = t.maxTime();
            refs.append(r);
        }
    }
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
        if (win)
            win->setLinkedSelection(refs);

    // 单条单独显示的航迹同时作为高亮；多选或返回全部时清掉单条高亮
    if (isolatedIndices.size() == 1) {
        const int idx = *isolatedIndices.constBegin();
        if (idx >= 0 && idx < m_store->size()) {
            const Track &t = m_store->tracks().at(idx);
            for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
                if (win)
                    win->setLinkedHighlight(point3dBatchNo(t), t.minTime(), t.maxTime());
            return;
        }
    }
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
        if (win)
            win->setLinkedHighlight(-1, 0, 0);
}

void Point3dCoordinator::onMasterTimeChanged(qint64 timeMs)
{
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
        if (win)
            win->syncToMasterTime(timeMs);
}

// 地图上悬停的航迹 → 所有点迹窗口里"同一条航迹"同步高亮（id 前缀可能不同，用编号+时间匹配）
void Point3dCoordinator::onMapHoverTrack(int index)
{
    int batchNo = -1;
    qint64 t0 = 0, t1 = 0;
    if (index >= 0 && index < m_store->size()) {
        const Track &t = m_store->tracks().at(index);
        batchNo = point3dBatchNo(t);
        t0 = t.minTime();
        t1 = t.maxTime();
    }
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
        if (win)
            win->setLinkedHighlight(batchNo, t0, t1);
}

void Point3dCoordinator::refreshOverlayColors()
{
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
        if (win)
            win->refreshOverlayColors();
}

void Point3dCoordinator::populateWindowMenu(QMenu *menu)
{
    menu->clear();
    m_windows.erase(std::remove_if(m_windows.begin(), m_windows.end(),
                                   [](const QPointer<Point3dWindow> &p) { return p.isNull(); }),
                    m_windows.end());
    if (m_windows.isEmpty()) {
        QAction *empty = menu->addAction(tr("（没有打开的点迹窗口）"));
        empty->setEnabled(false);
        return;
    }
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows)) {
        if (!win)
            continue;
        const QString title = win->windowTitle();
        menu->addAction(title, this, [win]() {
            if (win) {
                win->showNormal();
                win->raise();
                win->activateWindow();
            }
        });
    }
    menu->addSeparator();
    menu->addAction(tr("关闭全部点迹窗口"), this, [this]() {
        const QList<QPointer<Point3dWindow>> wins = m_windows;
        for (const QPointer<Point3dWindow> &win : wins)
            if (win)
                win->close();
    });
}

int Point3dCoordinator::trackIndexForBatch(int batchNo, TrackSource source, qint64 t0, qint64 t1) const
{
    if (batchNo <= 0)
        return -1;
    int best = -1;
    qint64 bestOverlap = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass) {
        for (int i = 0; i < m_store->size(); ++i) {
            const Track &t = m_store->tracks().at(i);
            if (point3dBatchNo(t) != batchNo)
                continue;
            if (pass == 0 && t.source != source)
                continue;
            const qint64 overlap = qMax<qint64>(
                0, qMin(t.maxTime(), t1) - qMax(t.minTime(), t0));
            // 编号在别的 MAT 里可能重复：有时间信息时必须真的重叠才算同一条
            if (overlap <= 0 && (t0 > 0 || t1 > 0))
                continue;
            if (best < 0 || overlap > bestOverlap) {
                best = i;
                bestOverlap = overlap;
            }
        }
    }
    return best;
}

void Point3dCoordinator::onPoint3dFocus(int batchNo, TrackSource source, qint64 t0, qint64 t1)
{
    const int idx = batchNo < 0 ? -1 : trackIndexForBatch(batchNo, source, t0, t1);
    m_globe->trackLayer()->setHoveredTrack(idx);
    m_globe->update();
    // 所有点迹窗口保持同一高亮（外部入口不再发信号，不会形成回路）
    for (const QPointer<Point3dWindow> &win : qAsConst(m_windows))
        if (win)
            win->setLinkedHighlight(batchNo, t0, t1);
}

void Point3dCoordinator::onPoint3dActivate(int batchNo, TrackSource source, qint64 t0, qint64 t1)
{
    const int idx = trackIndexForBatch(batchNo, source, t0, t1);
    if (idx >= 0)
        emit isolateTrackRequested(idx);
}
