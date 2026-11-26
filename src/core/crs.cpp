#include "crs.h"

#include <cmath>

namespace crs {
namespace {

// 克拉索夫斯基椭球（GCJ-02 算法里固定的参数，不是 WGS84 的）
const double kA = 6378245.0;
const double kEe = 0.00669342162296594323;

double transformLat(double x, double y)
{
    double r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * std::sqrt(std::fabs(x));
    r += (20.0 * std::sin(6.0 * x * M_PI) + 20.0 * std::sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * std::sin(y * M_PI) + 40.0 * std::sin(y / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (160.0 * std::sin(y / 12.0 * M_PI) + 320.0 * std::sin(y * M_PI / 30.0)) * 2.0 / 3.0;
    return r;
}

double transformLon(double x, double y)
{
    double r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * std::sqrt(std::fabs(x));
    r += (20.0 * std::sin(6.0 * x * M_PI) + 20.0 * std::sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * std::sin(x * M_PI) + 40.0 * std::sin(x / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (150.0 * std::sin(x / 12.0 * M_PI) + 300.0 * std::sin(x / 30.0 * M_PI)) * 2.0 / 3.0;
    return r;
}

} // namespace

bool inChina(double lonDeg, double latDeg)
{
    return lonDeg > 72.004 && lonDeg < 137.8347 && latDeg > 0.8293 && latDeg < 55.8271;
}

bool wgs84ToGcj02(double lonWgs, double latWgs, double *lonGcj, double *latGcj)
{
    if (!inChina(lonWgs, latWgs)) {
        *lonGcj = lonWgs;
        *latGcj = latWgs;
        return false;
    }

    const double x = lonWgs - 105.0;
    const double y = latWgs - 35.0;
    double dLat = transformLat(x, y);
    double dLon = transformLon(x, y);

    const double radLat = latWgs / 180.0 * M_PI;
    const double sinLat = std::sin(radLat);
    double magic = 1.0 - kEe * sinLat * sinLat;      // 1 - e²·sin²φ
    const double sqrtMagic = std::sqrt(magic);

    // 米 → 度：纬向和经向的弧长不同，必须分别除
    dLat = (dLat * 180.0) / ((kA * (1.0 - kEe)) / (magic * sqrtMagic) * M_PI);
    dLon = (dLon * 180.0) / (kA / sqrtMagic * std::cos(radLat) * M_PI);

    *latGcj = latWgs + dLat;
    *lonGcj = lonWgs + dLon;
    return true;
}

bool gcj02ToWgs84(double lonGcj, double latGcj, double *lonWgs, double *latWgs)
{
    if (!inChina(lonGcj, latGcj)) {
        *lonWgs = lonGcj;
        *latWgs = latGcj;
        return false;
    }

    // 不动点迭代：w ← w + (gcj - f(w))，f 为正向转换。
    // 正向偏移量小于 0.006°，迭代 8 次残差已降到 1e-9° 量级（约 0.1 mm）。
    double lon = lonGcj;
    double lat = latGcj;
    for (int i = 0; i < 8; ++i) {
        double glon = 0.0, glat = 0.0;
        wgs84ToGcj02(lon, lat, &glon, &glat);
        const double dLon = lonGcj - glon;
        const double dLat = latGcj - glat;
        lon += dLon;
        lat += dLat;
        if (std::fabs(dLon) < 1e-10 && std::fabs(dLat) < 1e-10)
            break;
    }
    *lonWgs = lon;
    *latWgs = lat;
    return true;
}

} // namespace crs
