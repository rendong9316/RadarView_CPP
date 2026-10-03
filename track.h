#ifndef TRACK_H
#define TRACK_H

#include <QString>
#include <QVector>
#include <QHash>
#include <QColor>

// 航迹数据源，对应 RadarView 的 adsb / radar / radar_raw
enum class TrackSource { Adsb = 0, Radar = 1, RadarRaw = 2 };

QString trackSourceName(TrackSource s);      // "ADS-B" / "Radar" / "RadarRaw"
QColor trackSourceColor(TrackSource s);      // 默认配色

// 单个航迹点；时间为 Unix 毫秒（数据源时间按北京时间 UTC+8 解析）
struct TrackPoint {
    qint64 t = 0;
    double lat = 0.0;        // 度
    double lon = 0.0;        // 度
    float alt = 0.0f;        // 米
    float heading = 0.0f;    // 度
    float speed = 0.0f;      // 节
    float vrate = 0.0f;      // 英尺/分
};

struct Track {
    QString id;              // ADS-B: ICAO 地址；雷达: RADAR-0001 / RAW-0001
    TrackSource source = TrackSource::Adsb;
    QString fileName;        // 导入文件名（雷达为显示名称）
    QString flightNo, icaoFlightNo, aircraftType, registration, airline, origin, destination;
    QVector<TrackPoint> points;   // 按时间升序，时间戳不重复

    QString key() const;     // id::source::fileName，合并去重用
    qint64 minTime() const { return points.isEmpty() ? 0 : points.first().t; }
    qint64 maxTime() const { return points.isEmpty() ? 0 : points.last().t; }

    // 按时间稳定排序并去掉时间戳重复的点（保留先出现的）
    void normalize();
};

// "YYYY-MM-DD HH:MM:SS[.fff]" 按 UTC+8 解析为 Unix 毫秒；格式非法返回 false
bool parseBeijingTime(const char *s, int len, qint64 *ms);
// Unix 毫秒 -> "YYYY MM DD HH:mm:ss"（UTC+8，与 RadarView 显示一致）
QString formatBeijingTime(qint64 ms);

// 已加载航迹集合：按 key 合并，同 key 只追加新时间点
class TrackStore
{
public:
    // 返回新增的航迹条数
    int addTracks(QVector<Track> tracks);
    void clear();

    const QVector<Track> &tracks() const { return m_tracks; }
    int size() const { return m_tracks.size(); }
    qint64 pointCount() const;
    quint64 version() const { return m_version; }   // 内容每变一次加 1，供渲染层判断是否重建
    qint64 minTime() const;
    qint64 maxTime() const;

    // 每个导入文件一种颜色（雷达按导入顺序轮换，ADS-B 固定）
    QColor fileColor(TrackSource s, const QString &fileName) const;
    // 为新导入的雷达文件生成不重名的显示名称：Radar、Radar2...
    QString uniqueFileName(TrackSource s) const;

private:
    QVector<Track> m_tracks;
    QHash<QString, int> m_index;
    quint64 m_version = 0;
    QHash<QString, QColor> m_fileColors;   // key: source::fileName
};

#endif // TRACK_H
