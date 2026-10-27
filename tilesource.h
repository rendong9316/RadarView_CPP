#ifndef TILESOURCE_H
#define TILESOURCE_H

#include <QString>
#include <QImage>

class QSqlQuery;

// 本地 .mbtiles 瓦片源（SQLite）。对外一律用 XYZ 编号（y 从北往南），
// 内部按 metadata 里的 scheme 决定要不要翻转成 TMS 行号：
//   · 规范 MBTiles（scheme 缺省 / tms）用 tile_row = 2^z-1-y
//   · 有些工具（以及本项目早期版下载器）把 XYZ 的 y 直接写进 tile_row，
//     这类包必须声明 scheme=xyz，否则整包会在南北方向镜像
class TileSource
{
public:
    TileSource();
    ~TileSource();

    bool open(const QString &path, QString *error = nullptr);
    void close();

    bool isOpen() const { return m_open; }
    int minZoom() const { return m_minZoom; }
    int maxZoom() const { return m_maxZoom; }
    QString name() const { return m_name; }
    QString path() const { return m_path; }
    // 瓦片网格用的是 GCJ-02（高德瓦片抓的包）。由 metadata 里的 crs 声明，
    // 不声明就是 WGS84 —— 坐标系只能靠声明，不能从数值上猜。
    bool isGcj() const { return m_gcj; }
    // 库里是不是按 XYZ 存的行号（scheme=xyz）；规范包为 false
    bool isXyzScheme() const { return m_xyzScheme; }

    // 读取一块瓦片，不存在时返回空 QImage
    QImage tile(int z, int x, int y);

private:
    Q_DISABLE_COPY(TileSource)

    QString m_conn;
    QString m_path;
    QString m_name;
    int m_minZoom = 0;
    int m_maxZoom = 0;
    bool m_gcj = false;
    bool m_xyzScheme = false;   // metadata scheme=xyz：tile_row 是从北往南数的
    bool m_open = false;
    QSqlQuery *m_query = nullptr;
};

#endif // TILESOURCE_H
