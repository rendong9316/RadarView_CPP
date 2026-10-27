#ifndef GEOCALC_H
#define GEOCALC_H

#include <QPointF>
#include <QString>
#include <QVector>

// 测距和方位，逐条对应 RadarView src/composables/useGeoCalc.ts
namespace geocalc {

// 球面近似（R = 6371 km），Vincenty 不收敛时的后备
double haversineKm(double lat1, double lng1, double lat2, double lng2);
// WGS-84 椭球 Vincenty 反算，km
double vincentyKm(double lat1, double lng1, double lat2, double lng2);
// 初始方位角，0~360，正北为 0、顺时针
double initialBearing(double lat1, double lng1, double lat2, double lng2);
// N / NE / E / SE / S / SW / W / NW
QString bearingToCardinal(double deg);

// 正算（Vincenty direct）：从 (lat1,lng1) 沿方位角 bearingDeg 走 distKm 到达的点。
// 与 vincentyKm 同口径（WGS-84 椭球），两者互为反算；不收敛时退回球面公式。
bool destination(double lat1, double lng1, double bearingDeg, double distKm,
                 double *lat2, double *lng2);

// 测地线圆：以 (lat,lon) 为圆心、radiusKm 为测地距离的一圈点，x=经度 y=纬度（WGS84 度）。
// 每个点都是「从圆心沿该方位角走 radiusKm」的正算结果，所以圆在椭球上是等距圆，
// 不是经纬度平面上的椭圆。首尾不重复；segments 建议 180（每 2° 一个点）。
QVector<QPointF> geodesicCircle(double lat, double lon, double radiusKm, int segments = 180);

// 大圆加密：把两点之间的大圆按不超过 maxStepDeg 的步长采样，返回**含起点和终点**的点列
// （x = 经度, y = 纬度）。贴地折线用它把「两点直连」变成一串球面顶点，
// 投影到屏幕上才是曲线；纬度用大地纬度换算，和 GlobeWidget::geodesicLines 同一口径。
QVector<QPointF> geodesicPath(double lat1, double lon1, double lat2, double lon2,
                              double maxStepDeg = 0.25);

} // namespace geocalc

#endif // GEOCALC_H
