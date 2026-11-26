#include "trackimportcoordinator.h"

#include <QAction>
#include <QDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSet>

#include "apppaths.h"
#include "globewidget.h"
#include "trackdb.h"
#include "trackimporter.h"
#include "tracklayer.h"
#include "uiwidgets.h"

TrackImportCoordinator::TrackImportCoordinator(TrackStore *store, GlobeWidget *globe,
                                               QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_globe(globe)
    , m_dialogParent(dialogParent)
{
}

void TrackImportCoordinator::setImportActions(const QList<QAction *> &actions)
{
    m_importActions = actions;
}

void TrackImportCoordinator::loadFileColors()
{
    app::Settings &s = app::settings();
    // 文件颜色先恢复，加载后同一文件颜色不变，新文件继续轮换
    QHash<QString, QColor> colors;
    const QJsonObject co = QJsonDocument::fromJson(
        s.value(QStringLiteral("display.file_line_color")).toString().toUtf8()).object();
    for (auto it = co.begin(); it != co.end(); ++it)
        colors.insert(it.key(), QColor(it.value().toString()));
    m_store->setFileColors(colors);

    const QJsonObject custom = QJsonDocument::fromJson(
        s.value(QStringLiteral("display.custom_file_color")).toString().toUtf8()).object();
    for (auto it = custom.begin(); it != custom.end(); ++it) {
        const QColor color(it.value().toString());
        if (color.isValid()) {
            m_customFileColors.insert(it.key(), color);
            m_globe->trackLayer()->setFileColorOverride(it.key(), color);
        }
    }
}

void TrackImportCoordinator::saveFileColors()
{
    QJsonObject o;
    const QHash<QString, QColor> colors = m_store->fileColors();
    for (auto it = colors.constBegin(); it != colors.constEnd(); ++it)
        o.insert(it.key(), it.value().name());
    app::setValue(QStringLiteral("display.file_line_color"),
                  QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    QJsonObject custom;
    for (auto it = m_customFileColors.constBegin(); it != m_customFileColors.constEnd(); ++it)
        custom.insert(it.key(), it.value().name());
    app::setValue(QStringLiteral("display.custom_file_color"),
                  QString::fromUtf8(QJsonDocument(custom).toJson(QJsonDocument::Compact)));
}

void TrackImportCoordinator::setCustomFileColor(const QString &groupKey, const QColor &color)
{
    if (!color.isValid())
        return;
    QHash<QString, QColor> colors = m_store->fileColors();
    colors.insert(groupKey, color);
    m_store->setFileColors(colors);          // 版本号 +1 → 图层按新颜色重建
    m_customFileColors.insert(groupKey, color);
    m_globe->trackLayer()->setFileColorOverride(groupKey, color);
    saveFileColors();
}

void TrackImportCoordinator::waitAndShutdown()
{
    // 等后台入库结束，避免进程退出时写到一半
    if (m_saveJob)
        m_saveJob->wait();
    qDeleteAll(m_saveQueue);
    m_saveQueue.clear();
}

void TrackImportCoordinator::importFile(TrackSource kind, const QString &path,
                                        const QString &displayName, const QColor &color)
{
    if (m_importer)
        return;
    QString name = displayName;
    QColor selectedColor = color;
    m_hasImportPointMode = false;
    if (name.isEmpty() && (kind != TrackSource::Adsb || !app::testMode())) {
        QSet<QString> used;   // 数据库里已有的同类批次名也要避开
        for (const BatchInfo &b : trackdb::batches())
            if (b.source == kind)
                used.insert(b.fileName);
        for (const Track &t : m_store->tracks())
            if (t.source == kind)
                used.insert(t.fileName);
        const bool adsb = kind == TrackSource::Adsb;
        name = adsb ? QFileInfo(path).completeBaseName() : m_store->uniqueFileName(kind, used);
        if (adsb) {
            const QString base = name;
            for (int n = 2; used.contains(name); ++n)
                name = base + QStringLiteral(" (%1)").arg(n);
        }
        if (!app::testMode()) {
            ui::RadarImportDialog dlg(path, name,
                                      QColor(kind == TrackSource::Radar ? "#FF0000" : "#FFC000"),
                                      used.values(), m_dialogParent, adsb,
                                      kind == TrackSource::RadarRaw ? RadarPointMode::Associated : m_defaultPointMode);
            if (dlg.exec() != QDialog::Accepted)
                return;
            name = dlg.trackName();
            if (!adsb) {
                selectedColor = dlg.trackColor();
                m_hasImportPointMode = true;
                m_importPointMode = dlg.pointMode();
            }
        }
    }
    m_importColor = selectedColor;
    m_importer = new TrackImporter(kind, path, name, this);
    connect(m_importer, &TrackImporter::progressChanged, this,
            [this](int percent) { emit loadingStateChanged(true, percent); });
    connect(m_importer, &TrackImporter::importDone, this, &TrackImportCoordinator::onImportDone);
    for (QAction *a : qAsConst(m_importActions))
        a->setEnabled(false);
    emit errorMessage(QString(), 0);        // 与 RadarView 一致：开始导入时清除上次的错误
    emit loadingStateChanged(true, 0);
    m_importer->start();
}

void TrackImportCoordinator::onImportDone()
{
    TrackImporter *imp = m_importer;
    m_importer = nullptr;
    imp->wait();
    emit loadingStateChanged(false, 0);
    for (QAction *a : qAsConst(m_importActions))
        a->setEnabled(true);

    QString msg;
    const bool ok = imp->ok();
    if (ok) {
        const bool firstData = m_store->size() == 0;
        const int before = m_store->size();
        QVector<Track> &tracks = imp->tracks();
        // 后台入库（RadarView：batch_exists 时 append_missing_tracks，否则 save_batch），状态栏显示「保存中」
        if (!tracks.isEmpty() && trackdb::isOpen()) {
            m_saveQueue << new DbSaveJob(tracks.first().fileName, imp->kind(), tracks, this);
            startNextSave();
        }
        // 重新导入：清掉这些航迹的软删除，撤销栈里对应的条目作废
        m_globe->resetInteraction();
        const int added = m_store->addTracks(tracks);
        if (m_importColor.isValid() && !tracks.isEmpty()) {
            const QString key = trackSourceName(imp->kind()) + QStringLiteral("::") + tracks.first().fileName;
            setCustomFileColor(key, m_importColor);
        } else {
            saveFileColors();
        }
        msg = tr("%1：新增 %2 条航迹（共 %3 条，%4 个点），解析 %5 ms")
                  .arg(QFileInfo(imp->path()).fileName()).arg(added)
                  .arg(m_store->size()).arg(m_store->pointCount()).arg(imp->elapsedMs());
        if (m_hasImportPointMode) {
            emit radarPointModeSelected(m_importPointMode);
            m_hasImportPointMode = false;
        }
        if (imp->hasRadarSite())
            emit radarSiteDiscovered(imp->radarSite());
        emit tracksImported(before, firstData);
    } else {
        msg = imp->errorString();
        emit errorMessage(msg, 0);
        if (!m_dialogParent->property("noDialogs").toBool())
            QMessageBox::warning(m_dialogParent, tr("导入失败"), msg);
    }
    imp->deleteLater();
    m_importColor = QColor();
    emit importFinished(ok, msg);
}

void TrackImportCoordinator::startNextSave()
{
    if (m_saveJob || m_saveQueue.isEmpty()) {
        emit persistingStateChanged(isPersisting());
        return;
    }
    m_saveJob = m_saveQueue.takeFirst();
    emit persistingStateChanged(true);
    connect(m_saveJob, &QThread::finished, this, [this]() {
        DbSaveJob *job = m_saveJob;
        m_saveJob = nullptr;
        const bool ok = job->ok();
        QString msg;
        if (ok) {
            msg = QStringLiteral("%1：入库 %2 条").arg(job->fileName()).arg(job->savedCount());
        } else {
            msg = QStringLiteral("数据保存失败: %1").arg(job->errorString());
            emit errorMessage(msg, 8000);
        }
        job->deleteLater();
        emit dbStale();
        startNextSave();
        emit persistFinished(ok, msg);
    });
    m_saveJob->start();
}
