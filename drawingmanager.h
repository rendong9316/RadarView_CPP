#ifndef DRAWINGMANAGER_H
#define DRAWINGMANAGER_H

#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

// 球面贴地绘图的数据层：旗标距离圆、自由曲线、多边形、矩形统一成一个 DrawingFeature。
//
// 几何一律保存成 WGS84 经纬度顶点（x = 经度、y = 纬度，单位度），并且**已经按大圆加密**：
//   - 折线每一段用球面 slerp 插值，投影到球面才是贴地曲线，不是两点间的弦；
//   - 圆用 Vincenty 正算逐方位角求点，是椭球上的等距圆，不是经纬度平面上的椭圆；
//   - 闭合图形（圆 / 多边形 / 矩形）首尾重复，渲染时一律当一条折线画。
// 数据永远是 WGS84；高德底图的 GCJ-02 只在 GlobeWidget 投影那一刻转换。
// 不依赖 GeographicLib：本工程禁止第三方库（见 CODEBUDDY.md）。

enum class DrawKind { Circle, Polyline, Polygon, Rect };

struct DrawingFeature {
    int id = 0;
    DrawKind kind = DrawKind::Polyline;
    QColor color = QColor(0xf5, 0x9e, 0x0b);   // #f59e0b，与标尺同色
    double widthPx = 3.0;                      // 需求：线宽固定 3px
    QVector<QPointF> pts;                      // 已加密的 WGS84 顶点（度）
    // 圆专用（其余图形为 0），序列化后据此还原参数
    double centerLon = 0.0, centerLat = 0.0, radiusKm = 0.0;
    QString label;

    bool isClosed() const { return kind != DrawKind::Polyline; }
};

// 圆/折线/多边形/矩形的名字，用于 GeoJSON properties.kind
QString drawKindName(DrawKind k);
bool drawKindFromName(const QString &name, DrawKind *out);

class DrawingManager : public QObject
{
    Q_OBJECT
public:
    explicit DrawingManager(QObject *parent = nullptr);

    const QVector<DrawingFeature> &features() const { return m_features; }
    int count() const { return m_features.size(); }
    const DrawingFeature *feature(int id) const;
    int indexOf(int id) const;

    int addFeature(const DrawingFeature &f);      // 返回分配到的 id（0 表示无效）
    int addCircle(double lat, double lon, double radiusKm, const QColor &color,
                  const QString &label = QString());
    bool removeFeature(int id);
    void clear();

    quint64 version() const { return m_version; }

    // GeoJSON FeatureCollection：圆/多边形/矩形 -> Polygon，自由曲线 -> LineString，
    // 圆心半径、颜色、线宽、名称放在 properties 里，其他 GIS 软件能直接打开。
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &root, QString *error = nullptr);
    bool saveToFile(const QString &path, QString *error = nullptr) const;
    bool loadFromFile(const QString &path, QString *error = nullptr);

signals:
    void changed();

private:
    QVector<DrawingFeature> m_features;
    int m_nextId = 1;
    quint64 m_version = 0;
};

#endif // DRAWINGMANAGER_H
