#ifndef GEO_H
#define GEO_H

#include <QVector3D>
#include <cmath>

// WGS84 椭球，长半轴归一化为 1（场景长度单位 = 赤道半径）
namespace geo {

const double kEarthRadiusKm = 6378.137;
const double kE2 = 6.69437999014e-3;               // 第一偏心率平方

// 大地坐标 -> 地心直角坐标（Z 轴指向北极）；lon/lat 为弧度，h 为赤道半径单位
inline QVector3D ecef(double lon, double lat, double h)
{
    const double sl = std::sin(lat), cl = std::cos(lat);
    const double n = 1.0 / std::sqrt(1.0 - kE2 * sl * sl);
    return QVector3D(float((n + h) * cl * std::cos(lon)),
                     float((n + h) * cl * std::sin(lon)),
                     float((n * (1.0 - kE2) + h) * sl));
}

inline QVector3D ecefDeg(double lonDeg, double latDeg, double hMeters)
{
    const double d2r = 3.14159265358979323846 / 180.0;
    return ecef(lonDeg * d2r, latDeg * d2r, hMeters / (kEarthRadiusKm * 1000.0));
}

} // namespace geo

#endif // GEO_H
