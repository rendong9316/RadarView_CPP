#ifndef TRACKDB_H
#define TRACKDB_H

#include <QHash>
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
    QString displayName;         // 用户在「批量数据管理」里改的显示名（v5）；空 = 用 fileName
    TrackSource source = TrackSource::Adsb;
    int trackCount = 0;
    QString importedAt;
    QString label() const { return displayName.isEmpty() ? fileName : displayName; }
};

// 点云窗口「从数据库选择航迹」用的摘要（不含点序列）
struct DbTrackSummary {
    QString icao;
    qint64 batchId = 0;
    TrackSource source = TrackSource::Radar;
    QString fileName;
    QString displayName;        // 批次的显示名（空 = 用 fileName）
    QString flightNo;
    qint64 minMs = 0, maxMs = 0;
    int pointCount = 0;
    bool hasMeasure = false;    // 主点列是否带 prange/pvr/paz（v2 旧数据为 false，需重新导入）
    bool hasRaw = false;        // 是否带关联点列（v4 起，同一 MAT 的 asscPointList）
    int rawCount = 0;           // 关联点列的原始点数
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

    // 空间套索查询范围（Track::key()）：spatialActive 时 query() 只返回 spatialKeys 内的航迹。
    // 这是查询条件，不是地图可见集合；由管理系统按套索多边形算好传进来，
    // 不写进 filterJson（多边形本身另行持久化，启动时重新求交即可）。
    bool spatialActive = false;
    QStringList spatialKeys;

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
QVector<DbTrackSummary> trackSummaries(const QString &sourceFilter = QString(),
                                       QString *error = nullptr);  // 点云航迹选择器用
ManageStats stats();
QStringList distinctValues(const QString &column, const QString &source);   // airline / aircraft_type
QVector<ManageRow> query(const ManageFilter &f, int limit, int offset, int *total);
bool setDeleted(const QStringList &trackKeys, bool deleted);
QSet<QString> deletedKeys();

// 批次级硬删除（RadarView db.rs::delete_batch）：从数据库彻底移除某批次的全部航迹。不可撤销。
bool deleteBatch(qint64 batchId, QString *error = nullptr);
// 清空数据库里的全部航迹与批次（「删除全部数据」），不可撤销
bool deleteAllBatches(QString *error = nullptr);
// 批次显示名（v5，「批量数据管理」里重命名）：空名恢复成文件名
bool setBatchDisplayName(qint64 batchId, const QString &name, QString *error = nullptr);
// key = 来源::文件名 → 显示名（只含改过名的批次）
QHash<QString, QString> batchDisplayNames();

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
