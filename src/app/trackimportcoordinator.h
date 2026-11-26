#ifndef TRACKIMPORTCOORDINATOR_H
#define TRACKIMPORTCOORDINATOR_H

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include "radarsite.h"
#include "track.h"

class DbSaveJob;
class GlobeWidget;
class QAction;
class QWidget;
class TrackImporter;

// 航迹导入与后台入库协调器：文件解析线程、命名/选色弹窗、入库串行队列与文件颜色持久化
class TrackImportCoordinator : public QObject
{
    Q_OBJECT
public:
    TrackImportCoordinator(TrackStore *store, GlobeWidget *globe, QWidget *dialogParent,
                           QObject *parent = nullptr);

    void setImportActions(const QList<QAction *> &actions);
    void setDefaultPointMode(RadarPointMode mode) { m_defaultPointMode = mode; }
    void importFile(TrackSource kind, const QString &path,
                    const QString &displayName = QString(), const QColor &color = QColor());

    bool isImporting() const { return m_importer != nullptr; }
    bool isPersisting() const { return m_saveJob != nullptr || !m_saveQueue.isEmpty(); }

    void loadFileColors();
    void saveFileColors();
    void setCustomFileColor(const QString &groupKey, const QColor &color);
    void waitAndShutdown();

signals:
    void loadingStateChanged(bool loading, int percent);
    void persistingStateChanged(bool persisting);
    void errorMessage(const QString &text, int clearAfterMs);
    void tracksImported(int beforeCount, bool firstData);
    void importFinished(bool ok, const QString &message);
    void persistFinished(bool ok, const QString &message);
    void dbStale();
    void radarPointModeSelected(RadarPointMode mode);
    void radarSiteDiscovered(const RadarSite &site);

private slots:
    void onImportDone();
    void startNextSave();

private:
    TrackStore *m_store = nullptr;
    GlobeWidget *m_globe = nullptr;
    QWidget *m_dialogParent = nullptr;
    TrackImporter *m_importer = nullptr;
    QColor m_importColor;
    RadarPointMode m_defaultPointMode = RadarPointMode::Smooth;
    bool m_hasImportPointMode = false;
    RadarPointMode m_importPointMode = RadarPointMode::Smooth;
    QHash<QString, QColor> m_customFileColors;
    QList<QAction *> m_importActions;
    DbSaveJob *m_saveJob = nullptr;
    QList<DbSaveJob *> m_saveQueue;
};

#endif // TRACKIMPORTCOORDINATOR_H
