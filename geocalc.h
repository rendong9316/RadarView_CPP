#ifndef GEOCALC_H
#define GEOCALC_H

#include <QString>

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

} // namespace geocalc

#endif // GEOCALC_H
