#ifndef NETMAP_H
#define NETMAP_H

#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>

class QNetworkReply;

// 在线底图瓦片源（统一走 HTTP，不依赖 OpenSSL，Qt5.14 + Win7/RHEL7.6 都能用）
//
// Tianditu 天地图 WMTS：vec_w 底图 + cva_w 注记，两层并行取后合成一张图。
//           原生 z18（1:2257 街道级），坐标 CGCS2000 ≈ WGS84，航迹不用纠偏。
// Amap     高德矢量路网：单层自带注记，原生 z17。
//           坐标 GCJ-02，和 WGS84 航迹有 50–500 m 系统偏移（只适合看底图）。
//
// 天地图对浏览器 UA（Mozilla/...）一律 403，而 Qt 默认就发 "Mozilla/5.0"，必须换掉。
class NetTileSource : public QObject
{
    Q_OBJECT
public:
    enum class Source { Tianditu, Amap };

    explicit NetTileSource(QObject *parent = nullptr);
    ~NetTileSource();

    void setSource(Source s);
    Source source() const { return m_source; }
    void setKey(const QString &tk) { m_key = tk.trimmed(); }
    QString key() const { return m_key; }
    bool hasKey() const { return m_source == Source::Amap || !m_key.isEmpty(); }

    int minZoom() const { return 3; }
    int maxZoom() const { return m_source == Source::Amap ? 17 : 18; }
    QString name() const;

    // 取一张瓦片（非阻塞）。磁盘已有就交线程池解码，否则走网络下载；
    // 两种途径最终都发 tileReady，由 GlobeWidget 收下后在 GL 线程上传。
    void startLoad(int z, int x, int y);
    void request(int z, int x, int y);
    bool isBusy() const { return m_inflightCount > 0 || !m_queue.isEmpty() || !m_decoding.isEmpty(); }

signals:
    void tileReady(int z, int x, int y, const QImage &image);
    void tileFailed(int z, int x, int y);

private slots:
    // 线程池解码完成，被 QMetaObject::invokeMethod 投递回 GUI 线程
    void onDecoded(int z, int x, int y, const QImage &img);

private:
    struct Job {
        QImage part[2];
        bool done[2] = { false, false };
        bool inflight[2] = { false, false };
        int retries[2] = { 0, 0 };
    };

    int layerCount() const { return m_source == Source::Amap ? 1 : 2; }
    int maxInflight() const { return m_source == Source::Amap ? 4 : 6; }
    int pumpMs() const { return m_source == Source::Amap ? 70 : 35; }
    int watchdogMs() const { return 6000; }

    static quint64 key3(int z, int x, int y)
    {
        return (quint64(z) << 40) | (quint64(x) << 20) | quint64(y);
    }
    QString url(int z, int x, int y, int layer) const;
    QString cachePath(int z, int x, int y) const;
    void pump();
    void fetch(int z, int x, int y, int layer);
    void onFinished(QNetworkReply *reply, int z, int x, int y, int layer);
    void tryCompose(int z, int x, int y, quint64 k);

    QNetworkAccessManager m_net;
    QHash<quint64, Job> m_jobs;
    int m_inflightCount = 0;               // 按「请求」计数（一块瓦片可能两个）
    QSet<quint64> m_failed;
    QQueue<quint64> m_queue;
    QSet<quint64> m_decoding;              // 正在线程池里解码的瓦片，避免重复提交
    QThreadPool m_pool;                    // PNG 解码专用，避免占着 GUI 线程
    QTimer m_pumpTimer;
    Source m_source = Source::Tianditu;
    QString m_key;
};

#endif // NETMAP_H
