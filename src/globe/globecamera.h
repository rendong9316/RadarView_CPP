#ifndef GLOBECAMERA_H
#define GLOBECAMERA_H

#include <QMatrix4x4>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QVector3D>
#include <QVector4D>

// 3D 地球相机、坐标系（WGS84 / GCJ-02）、椭球射线拾取、屏幕投影、测地线采样与瓦片四叉树剔除。
// 纯 CPU 数学模块，不依赖 OpenGL 上下文或 QWidget。
class GlobeCamera
{
public:
    struct TileId {
        int z;
        int x;
        int y;
    };

    static const double kMercMaxLat;
    static const float kFovDeg;
    static const double kMinAlt;
    static const double kMaxAlt;
    static const float kRefinePx;

    GlobeCamera();

    // ---- 坐标系（WGS84 数据坐标 <-> 当前底图场景坐标）----
    bool sceneIsGcj() const { return m_sceneCrs; }
    QString coordSystemName() const { return m_sceneCrs ? QStringLiteral("GCJ-02") : QStringLiteral("WGS84"); }
    void toScene(double lonWgs, double latWgs, double *lonScene, double *latScene) const;
    void fromScene(double lonScene, double latScene, double *lonWgs, double *latWgs) const;
    // 切换场景坐标系并把相机中心换算到新基准，保证切换底图时画面仍对准同一真实地点；返回是否发生变化
    bool setSceneGcj(bool gcj);

    // ---- 相机状态与控制 ----
    void reset();
    void lookAt(double lonDeg, double latDeg, double altKm);
    void view(double *lonDeg, double *latDeg, double *altKm) const;
    void zoomBy(double steps);
    void panPixels(const QPoint &from, const QPoint &to, const QSize &viewport);

    double camLonRad() const { return m_camLon; }
    double camLatRad() const { return m_camLat; }
    double altRadius() const { return m_alt; }
    double altitudeKm() const;

    // ---- 矩阵与视锥 ----
    void updateMatrices(const QSize &viewport) const;
    const QMatrix4x4 &mvp() const { return m_mvp; }
    const QVector3D &eye() const { return m_eye; }

    // ---- 拾取与投影 ----
    // 鼠标射线与 WGS84 椭球求交，返回场景坐标（弧度）
    bool pick(const QPointF &pos, const QSize &viewport, double *lonRad, double *latRad) const;
    // 屏幕坐标 -> WGS84 经纬度（度）
    bool geoAt(const QPointF &pos, const QSize &viewport, double *lonDeg, double *latDeg) const;
    // 场景经纬度（度）+ 高度（米）-> 屏幕坐标，不做坐标转换
    bool sceneScreenPos(double lonDeg, double latDeg, double altM, const QSize &viewport, QPointF *pos) const;
    bool sceneVisiblePos(double lonDeg, double latDeg, double altM, const QSize &viewport, QPointF *pos) const;
    // WGS84 经纬度（度）+ 高度（米）-> 屏幕坐标
    bool screenPos(double lonDeg, double latDeg, double altM, const QSize &viewport, QPointF *pos) const;
    bool visiblePos(double lonDeg, double latDeg, double altM, const QSize &viewport, QPointF *pos) const;

    // ---- 曲线采样 ----
    // 两点间沿大圆插值的屏幕折线（背面部分断开）
    QVector<QPolygonF> geodesicLines(double lat1, double lon1, double lat2, double lon2,
                                     const QSize &viewport) const;
    // 套索边界采样：两点间按经纬度平面线性插值再投影（与 pointInPolygon 口径一致）
    void sampleLonLatLine(double lat1, double lon1, double lat2, double lon2,
                          const QSize &viewport, QVector<QPointF> *pts, QVector<bool> *vis) const;

    // ---- 瓦片几何与四叉树剔除 ----
    static quint64 tileKey(int z, int x, int y);
    static QVector4D tileRange(int z, int x, int y);
    static void tileBounds(int z, int x, int y, QVector3D *center, float *radius);
    void collectTiles(int z, int x, int y, int mapMaxZoom, QVector<TileId> &out) const;

private:
    bool m_sceneCrs = false;
    // 星下点经纬度（场景坐标，弧度）+ 离地高度（单位：赤道半径）
    double m_camLon = 0.0;
    double m_camLat = 0.0;
    double m_alt = 2.5;

    mutable QMatrix4x4 m_view;
    mutable QMatrix4x4 m_proj;
    mutable QMatrix4x4 m_mvp;
    mutable QVector3D m_eye;
    mutable QVector4D m_planes[6];
    mutable float m_focalPx = 1.0f;
    mutable QSize m_viewport;
    mutable bool m_dirty = true;
};

#endif // GLOBECAMERA_H
