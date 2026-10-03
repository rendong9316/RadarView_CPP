#ifndef TRACKDB_H
#define TRACKDB_H

#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include "track.h"

// 航迹数据库（SQLite），对应 RadarView src-tauri/src/db.rs：
//   batches      每次导入一个批次（file_name 唯一识别）
//   saved_tracks 每条航迹一行，元数据单独成列便于管理面板筛选排序，点序列按小端二进制存成 BLOB
// 软删除用 deleted 列（RadarView 存在设置里，效果相同：数据保留，可撤销，重新导入同一文件时恢复）

struct BatchInfo {
    qint64 id = 0;
    QString fileName;
    TrackSource source = TrackSource::Adsb;
    int trackCount = 0;
    QString importedAt;
};

// 管理面板筛选条件（RadarView types/manage.ts TrackMetaFilter + SortConfig）
struct ManageFilter {
    QString source;              // "" / adsb / radar / radar_raw
    qint64 batchId = -1;         // -1 不限
    QString searchText, airline, aircraftType;
    int minPoints = -1, maxPoints = -1;
    qint64 minTimeMs = 0, maxTimeMs = 0;   // 0 不限
    QString sortBy = QStringLiteral("batch_imported_at");
    bool sortDesc = true;

    QJsonObject filterJson() const;
    QJsonObject sortJson() const;
    void loadJson(const QJsonObject &filter, const QJsonObject &sort);
    void resetAll() { *this = ManageFilter(); }
};

struct ManageRow {
    QString icao;
    qint64 batchId = 0;
    TrackSource source = TrackSource::Adsb;
    QString flightNo, icaoFlightNo, registration, aircraftType, airline, origin, destination;
    QString minTs, maxTs;        // 北京时间 "YYYY-MM-DD HH:MM:SS"
    int pointCount = 0;
    QString fileName, importedAt;
    QString trackKey() const;    // 与 Track::key() 相同
};

struct ManageStats {
    int totalTracks = 0, totalBatches = 0, uniqueIcao = 0;
    QMap<QString, int> bySource;   // "ADS-B" / "Radar" / "RadarRaw"
    qint64 timeMin = 0, timeMax = 0;
};

namespace trackdb {

QString beijingText(qint64 ms);       // "YYYY-MM-DD HH:MM:SS"

// 主线程连接；打开时建表
bool open(QString *error = nullptr);
void close();
bool isOpen();

QVector<Track> loadAll(QString *error = nullptr);            // 启动加载（不含软删除的）
QVector<Track> loadTracks(const QStringList &trackKeys);      // 按 Track::key() 读取（含软删除的）
QVector<BatchInfo> batches();                                 // id 倒序
QVector<Track> loadBatchTracks(qint64 batchId);               // 该批次全部航迹（含软删除的）
QStringList batchKeys(qint64 batchId);                        // 该批次航迹的 Track::key()
ManageStats stats();
QStringList distinctValues(const QString &column, const QString &source);   // airline / aircraft_type
QVector<ManageRow> query(const ManageFilter &f, int limit, int offset, int *total);
bool setDeleted(const QStringList &trackKeys, bool deleted);
QSet<QString> deletedKeys();

// 批次级硬删除（RadarView db.rs::delete_batch）：从数据库彻底移除某批次的全部航迹。不可撤销。
bool deleteBatch(qint64 batchId, QString *error = nullptr);

} // namespace trackdb

// 后台入库：批次不存在则新建，存在则只补上缺失的航迹，并恢复该批次被软删除的航迹
class DbSaveJob : public QThread
{
    Q_OBJECT
public:
    DbSaveJob(const QString &fileName, TrackSource source, const QVector<Track> &tracks, QObject *parent = nullptr);
    bool ok() const { return m_ok; }
    QString errorString() const { return m_error; }
    int savedCount() const { return m_saved; }
    bool appended() const { return m_appended; }
    QString fileName() const { return m_fileName; }

protected:
    void run() override;

private:
    QString m_path, m_fileName, m_error;
    TrackSource m_source;
    QVector<Track> m_tracks;
    bool m_ok = false, m_appended = false;
    int m_saved = 0;
};

#endif // TRACKDB_H
