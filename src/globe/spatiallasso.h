#ifndef SPATIALLASSO_H
#define SPATIALLASSO_H

#include <QObject>
#include <QVector>

#include "track.h"

// 空间套索（对应 RadarView src/composables/useSpatialLasso.ts）
//
// 交互状态与几何判定分开：
//   lassogeo::*  纯函数，不依赖 Qt 事件循环，便于自检
//   SpatialLasso 绘制状态（顶点、模式、鼠标预览），供地图叠加层和筛选面板共用
//
// 多边形顶点一律是 WGS84（度）。数据（航迹/旗标）也永远是 WGS84，
// 所以判定不需要关心当前底图是 WGS84 还是 GCJ-02。

// 套索顶点；id 只用于拖动时定位，不参与几何计算
struct LassoVertex {
    int id = 0;
    double lat = 0.0;
    double lon = 0.0;
    LassoVertex() {}
    LassoVertex(int i, double la, double lo) : id(i), lat(la), lon(lo) {}
};

namespace lassogeo {

// 射线法：点是否在多边形内（经纬度平面，x=lon, y=lat）
bool pointInPolygon(double lat, double lon, const QVector<LassoVertex> &poly);

// 两条线段是否相交（含共线情形），带包围盒快速排除
bool segmentsIntersect(double ax, double ay, double bx, double by,
                       double cx, double cy, double dx, double dy);

// 航迹是否与多边形相交：任一点落在多边形内，或任一航迹线段与多边形边相交。
// 先做多边形包围盒预筛，再逐点判定，最后做线段相交，避免大数据量下的无谓计算。
bool trackIntersectsPolygon(const Track &t, const QVector<LassoVertex> &poly);

// Douglas-Peucker 简化（等距近似），自由手绘顶点过多时调用；结果少于 3 个时返回原多边形
QVector<LassoVertex> simplifyPolygon(const QVector<LassoVertex> &poly, double epsilonMeters = 5.0);

// 球面多边形面积（km²，Green 公式），顶点不足 3 个返回 0
double polygonAreaSqKm(const QVector<LassoVertex> &poly);

// 多边形周长（km）
double polygonPerimeterKm(const QVector<LassoVertex> &poly);

} // namespace lassogeo

class SpatialLasso : public QObject
{
    Q_OBJECT
public:
    enum class Mode {
        Vertex,    // 单击逐点放顶点，双击/右键/Enter 闭合
        Freehand   // 按住左键自由手绘，松开自动闭合
    };

    explicit SpatialLasso(QObject *parent = nullptr);

    bool isActive() const { return m_active; }        // 正在绘制（含自由手绘中）
    bool isClosed() const { return m_closed; }        // 多边形已闭合
    bool isDrawingFreehand() const { return m_freehand; }
    Mode mode() const { return m_mode; }
    const QVector<LassoVertex> &vertices() const { return m_vertices; }

    bool hasMouse() const { return m_hasMouse; }
    double mouseLat() const { return m_mouseLat; }
    double mouseLon() const { return m_mouseLon; }

    double areaSqKm() const;        // closed 时有效，否则 0
    double perimeterKm() const;

    void setMode(Mode m);
    void activate();                // 清空并进入绘制
    void deactivate();              // 退出绘制，保留已画多边形（便于查看）
    void toggle();
    void clearAll();                // 清空多边形并退出

    void addVertex(double lat, double lon);
    void removeVertex(int id);
    void moveVertex(int id, double lat, double lon);
    void undoLastVertex();
    void closePolygon();            // >=3 顶点时闭合并退出绘制，发 closed()

    void beginFreehand(double lat, double lon);
    void addFreehandPoint(double lat, double lon);
    void endFreehand();

    void setMouse(bool valid, double lat, double lon);

signals:
    void changed();
    void mouseMoved();
    void closed();                  // 多边形闭合，主窗口据此自动应用空间筛选

private:
    void emitChanged();

    bool m_active = false;
    bool m_closed = false;
    bool m_freehand = false;
    bool m_hasMouse = false;
    double m_mouseLat = 0.0, m_mouseLon = 0.0;
    Mode m_mode = Mode::Vertex;
    QVector<LassoVertex> m_vertices;
    int m_nextId = 0;
};

#endif // SPATIALLASSO_H
