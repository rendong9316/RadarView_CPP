#include "geocalc.h"

#include <QtMath>
#include <cmath>

namespace geocalc {

namespace {

const double kA = 6378137.0;            // WGS-84 长半轴（m）
const double kB = 6356752.314245;       // 短半轴（m）
const double kF = 1.0 / 298.257223563;  // 扁率

double toRad(double d) { return d * M_PI / 180.0; }
double toDeg(double r) { return r * 180.0 / M_PI; }

} // namespace

double haversineKm(double lat1, double lng1, double lat2, double lng2)
{
    const double R = 6371.0;
    const double dLat = toRad(lat2 - lat1), dLng = toRad(lng2 - lng1);
    const double a = std::pow(std::sin(dLat / 2), 2)
                   + std::cos(toRad(lat1)) * std::cos(toRad(lat2)) * std::pow(std::sin(dLng / 2), 2);
    return R * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

double vincentyKm(double lat1, double lng1, double lat2, double lng2)
{
    if (std::fabs(lat1 - lat2) < 1e-12 && std::fabs(lng1 - lng2) < 1e-12)
        return 0.0;
    const double phi1 = toRad(lat1), phi2 = toRad(lat2);
    const double L = toRad(lng2 - lng1);
    const double U1 = std::atan((1 - kF) * std::tan(phi1));
    const double U2 = std::atan((1 - kF) * std::tan(phi2));
    double lambda = L;
    double sinSigma = 0, cosSigma = 0, sigma = 0, sinAlpha = 0, cos2Alpha = 0, cos2SigmaM = 0;
    const int maxIter = 100;
    for (int i = 0; i < maxIter; ++i) {
        const double sinLambda = std::sin(lambda), cosLambda = std::cos(lambda);
        sinSigma = std::sqrt(std::pow(std::cos(U2) * sinLambda, 2)
                             + std::pow(std::cos(U1) * std::sin(U2) - std::sin(U1) * std::cos(U2) * cosLambda, 2));
        if (sinSigma < 1e-12)
            return 0.0;
        cosSigma = std::sin(U1) * std::sin(U2) + std::cos(U1) * std::cos(U2) * cosLambda;
        sigma = std::atan2(sinSigma, cosSigma);
        sinAlpha = std::cos(U1) * std::cos(U2) * sinLambda / sinSigma;
        cos2Alpha = 1 - sinAlpha * sinAlpha;
        cos2SigmaM = cosSigma - 2 * std::sin(U1) * std::sin(U2) / cos2Alpha;
        if (!std::isfinite(cos2SigmaM))
            cos2SigmaM = 0;   // 赤道线
        const double C = kF / 16 * cos2Alpha * (4 + kF * (4 - 3 * cos2Alpha));
        const double lambdaP = lambda;
        lambda = L + (1 - C) * kF * sinAlpha
                 * (sigma + C * sinSigma * (cos2SigmaM + C * cosSigma * (-1 + 2 * cos2SigmaM * cos2SigmaM)));
        if (std::fabs(lambda - lambdaP) < 1e-12)
            break;
        if (i == maxIter - 1)
            return haversineKm(lat1, lng1, lat2, lng2);   // 近对跖点不收敛
    }
    const double u2 = cos2Alpha * ((kA * kA - kB * kB) / (kB * kB));
    const double k1 = (std::sqrt(1 + u2) - 1) / (std::sqrt(1 + u2) + 1);
    const double aCoeff = (1 + 0.25 * k1 * k1) / (1 - k1);
    const double bCoeff = k1 * (1 - 0.375 * k1 * k1);
    const double deltaSigma = bCoeff * sinSigma
        * (cos2SigmaM + bCoeff / 4
           * (cosSigma * (-1 + 2 * cos2SigmaM * cos2SigmaM)
              - bCoeff / 6 * cos2SigmaM * (-3 + 4 * sinSigma * sinSigma) * (-3 + 4 * cos2SigmaM * cos2SigmaM)));
    return kB * aCoeff * (sigma - deltaSigma) / 1000.0;
}

double initialBearing(double lat1, double lng1, double lat2, double lng2)
{
    const double phi1 = toRad(lat1), phi2 = toRad(lat2), dL = toRad(lng2 - lng1);
    const double y = std::sin(dL) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2) - std::sin(phi1) * std::cos(phi2) * std::cos(dL);
    return std::fmod(toDeg(std::atan2(y, x)) + 360.0, 360.0);
}

QString bearingToCardinal(double deg)
{
    static const char *const kCardinals[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    // JS Math.round：.5 向正无穷取整
    const int idx = int(std::floor(deg / 45.0 + 0.5)) % 8;
    return QLatin1String(kCardinals[(idx + 8) % 8]);
}

} // namespace geocalc
