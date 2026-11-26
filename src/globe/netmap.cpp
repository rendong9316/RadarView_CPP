#include "netmap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QUrl>

namespace {

const int kMaxQueued = 400;


// 天地图 WAF 只拦浏览器 UA，这里发自己的标识；高德反过来要有 Referer
const char *const kUserAgent = "RadarView/1.0 (Qt)";
const char *const kAmapReferer = "https://www.amap.com/";

// 磁盘 PNG 的解码放到线程池里做：实测单张 256×256 要 5ms 左右，
// 一屏 ~300 张就是在 GUI 线程上堆 1.7 秒的卡顿。
class DecodeTask : public QRunnable
{
public:
    DecodeTask(const QString &path, int z, int x, int y, QPointer<NetTileSource> dst)
        : m_path(path), m_z(z), m_x(x), m_y(y), m_dst(dst)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        const QImage img(m_path);
        if (img.isNull())
            return;
        // 队列投递回 GUI 线程；QPointer 保证对象已析构时不会踩野指针
        QMetaObject::invokeMethod(m_dst, "onDecoded", Qt::QueuedConnection,
                                  Q_ARG(int, m_z), Q_ARG(int, m_x), Q_ARG(int, m_y),
                                  Q_ARG(QImage, img));
    }

private:
    const QString m_path;
    const int m_z, m_x, m_y;
    const QPointer<NetTileSource> m_dst;
};

} // namespace

NetTileSource::NetTileSource(QObject *parent)
    : QObject(parent)
{
    connect(&m_pumpTimer, &QTimer::timeout, this, &NetTileSource::pump);
    m_pumpTimer.setInterval(pumpMs());
    // 解码是 CPU 密集的，给个独立小池子，不要和全局池抢线程
    m_pool.setMaxThreadCount(4);
    m_pool.setExpiryTimeout(30000);
}

NetTileSource::~NetTileSource()
{
    m_pool.waitForDone();
}

void NetTileSource::setSource(Source s)
{
    if (m_source == s)
        return;
    m_source = s;
    // 换源等于换了一套瓦片，缓存与未完成的请求全部作废
    m_jobs.clear();
    m_failed.clear();
    m_queue.clear();
    m_decoding.clear();
    m_inflightCount = 0;
    m_pumpTimer.stop();
    m_pumpTimer.setInterval(pumpMs());
}

QString NetTileSource::name() const
{
    return m_source == Source::Amap ? QStringLiteral("高德矢量（在线）")
                                    : QStringLiteral("天地图矢量（在线）");
}

QString NetTileSource::url(int z, int x, int y, int layer) const
{
    if (m_source == Source::Amap) {
        const int s = (x + y) % 4 + 1;
        return QStringLiteral("http://webrd0%1.is.autonavi.com/appmaptile"
                              "?lang=zh_cn&size=1&scale=1&style=8&x=%2&y=%3&z=%4")
            .arg(s).arg(x).arg(y).arg(z);
    }
    const QString lname = layer == 0 ? QStringLiteral("vec") : QStringLiteral("cva");
    const int s = (x + y) % 8;
    return QStringLiteral("http://t%1.tianditu.gov.cn/%2_w/wmts"
                          "?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0"
                          "&LAYER=%2&STYLE=default&TILEMATRIXSET=w&FORMAT=tiles"
                          "&TILEMATRIX=%3&TILEROW=%4&TILECOL=%5&tk=%6")
        .arg(s).arg(lname).arg(z).arg(y).arg(x).arg(m_key);
}

QString NetTileSource::cachePath(int z, int x, int y) const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                         + QStringLiteral("/RadarView/HelloVscode/tilecache/")
                         + (m_source == Source::Amap ? QStringLiteral("amap") : QStringLiteral("tdt"));
    return QStringLiteral("%1/%2/%3/%4.png").arg(base).arg(z).arg(x).arg(y);
}

// 取一张瓦片。不再同步返回 QImage —— 磁盘 PNG 的解码要 5ms 上下，
// 放在 paintGL 里会让一屏 300 张瓦片堆出 1.7 秒卡顿。
// 这里只负责「把瓦片搞到手」：磁盘已有就丢给线程池解码，否则走网络下载，
// 两条路最终都走 tileReady 信号，由 GlobeWidget 收进 m_pendingTiles 再上传。
void NetTileSource::startLoad(int z, int x, int y)
{
    const quint64 k = key3(z, x, y);
    if (m_failed.contains(k) || m_decoding.contains(k))
        return;

    const QString path = cachePath(z, x, y);
    if (QFileInfo::exists(path)) {
        m_decoding.insert(k);
        m_pool.start(new DecodeTask(path, z, x, y, this));
        return;
    }
    request(z, x, y);
}

// 线程池解码完成，回到 GUI 线程
void NetTileSource::onDecoded(int z, int x, int y, const QImage &img)
{
    m_decoding.remove(key3(z, x, y));
    if (img.isNull()) {
        m_failed.insert(key3(z, x, y));
        emit tileFailed(z, x, y);
        return;
    }
    emit tileReady(z, x, y, img);
}

void NetTileSource::request(int z, int x, int y)
{
    if (!hasKey())
        return;
    const quint64 k = key3(z, x, y);
    if (m_failed.contains(k) || m_jobs.contains(k) || m_decoding.contains(k))
        return;
    if (m_queue.size() >= kMaxQueued || m_queue.contains(k))
        return;

    m_jobs.insert(k, Job());
    m_queue.enqueue(k);
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
}

void NetTileSource::pump()
{
    while (!m_queue.isEmpty() && m_inflightCount < maxInflight()) {
        const quint64 k = m_queue.dequeue();
        if (!m_jobs.contains(k))
            continue;
        const int z = int((k >> 40) & 0xFF);
        const int x = int((k >> 20) & 0xFFFFF);
        const int y = int(k & 0xFFFFF);

        Job &job = m_jobs[k];
        bool started = false;
        // 两层同时发，别串行等：串行等于每块瓦片两个往返
        for (int layer = 0; layer < layerCount(); ++layer) {
            if (!job.done[layer] && !job.inflight[layer]) {
                fetch(z, x, y, layer);
                started = true;
            }
        }
        if (!started && m_inflightCount == 0)
            break;
    }
    if (m_queue.isEmpty() && m_inflightCount == 0)
        m_pumpTimer.stop();
}

void NetTileSource::fetch(int z, int x, int y, int layer)
{
    const quint64 k = key3(z, x, y);
    m_jobs[k].inflight[layer] = true;
    ++m_inflightCount;

    QNetworkRequest req{QUrl(url(z, x, y, layer))};
    if (m_source == Source::Amap) {
        req.setRawHeader("Referer", kAmapReferer);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64)"));
    } else {
        req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    }
    QNetworkReply *reply = m_net.get(req);

    // Qt 5.14 没有 setTransferTimeout，用一个短看门狗兜底：
    // 超时太长会让卡住的连接长期占着并发位，整屏都变慢
    QTimer *watchdog = new QTimer(reply);
    watchdog->setSingleShot(true);
    watchdog->setInterval(watchdogMs());
    connect(watchdog, &QTimer::timeout, reply, &QNetworkReply::abort);
    watchdog->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, z, x, y, layer]() {
        onFinished(reply, z, x, y, layer);
    });
}

void NetTileSource::onFinished(QNetworkReply *reply, int z, int x, int y, int layer)
{
    const quint64 k = key3(z, x, y);
    reply->deleteLater();
    if (m_inflightCount > 0)
        --m_inflightCount;

    auto it = m_jobs.find(k);
    if (it == m_jobs.end())
        return;                       // 换源时已被清掉
    Job &job = it.value();
    job.inflight[layer] = false;

    const QByteArray data = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QImage img;
    const bool ok = reply->error() == QNetworkReply::NoError && status == 200 && data.size() > 100
                    && img.loadFromData(data);

    if (!ok) {
        if (job.retries[layer] < 1) {          // 瞬态失败重试一次
            ++job.retries[layer];
            QTimer::singleShot(900, this, [this, z, x, y, layer]() {
                const quint64 kk = key3(z, x, y);
                if (m_jobs.contains(kk))
                    fetch(z, x, y, layer);
            });
            return;
        }
        m_failed.insert(k);
        m_jobs.erase(it);
        emit tileFailed(z, x, y);
        pump();
        return;
    }

    job.part[layer] = img;
    job.done[layer] = true;
    tryCompose(z, x, y, k);
    pump();
}

void NetTileSource::tryCompose(int z, int x, int y, quint64 k)
{
    auto it = m_jobs.find(k);
    if (it == m_jobs.end())
        return;
    Job &job = it.value();
    for (int layer = 0; layer < layerCount(); ++layer)
        if (!job.done[layer])
            return;                    // 还有层没回来

    QImage composed = job.part[0].convertToFormat(QImage::Format_RGB32);
    if (layerCount() > 1) {
        QPainter p(&composed);
        p.drawImage(0, 0, job.part[1]);   // 注记层带透明通道，直接叠
    }
    m_jobs.erase(it);

    const QString path = cachePath(z, x, y);
    QDir().mkpath(QFileInfo(path).absolutePath());
    composed.save(path, "PNG");

    emit tileReady(z, x, y, composed);
}
