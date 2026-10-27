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

bool destination(double lat1, double lng1, double bearingDeg, double distKm,
                 double *lat2, double *lng2)
{
    if (!lat2 || !lng2 || !std::isfinite(lat1) || !std::isfinite(lng1)
        || !std::isfinite(bearingDeg) || !std::isfinite(distKm))
        return false;
    if (distKm <= 0.0) {          // 半径为 0：圆退化成点
        *lat2 = lat1;
        *lng2 = lng1;
        return true;
    }

    const double alpha1 = toRad(bearingDeg);
    const double sinAlpha1 = std::sin(alpha1), cosAlpha1 = std::cos(alpha1);
    const double s = distKm * 1000.0;

    // Vincenty 正算（与上面反算同一套公式）
    const double tanU1 = (1 - kF) * std::tan(toRad(lat1));
    const double cosU1 = 1.0 / std::sqrt(1 + tanU1 * tanU1);
    const double sinU1 = tanU1 * cosU1;
    const double sigma1 = std::atan2(tanU1, cosAlpha1);
    const double sinAlpha = cosU1 * sinAlpha1;
    const double cosSqAlpha = 1 - sinAlpha * sinAlpha;
    const double uSq = cosSqAlpha * (kA * kA - kB * kB) / (kB * kB);
    const double A = 1 + uSq / 16384.0 * (4096.0 + uSq * (-768.0 + uSq * (320.0 - 175.0 * uSq)));
    const double B = uSq / 1024.0 * (256.0 + uSq * (-128.0 + uSq * (74.0 - 47.0 * uSq)));

    double sigma = s / (kB * A);
    double sinSigma = 0, cosSigma = 0, cos2SigmaM = 0;
    for (int i = 0; i < 100; ++i) {
        cos2SigmaM = std::cos(2 * sigma1 + sigma);
        sinSigma = std::sin(sigma);
        cosSigma = std::cos(sigma);
        const double deltaSigma = B * sinSigma
            * (cos2SigmaM + B / 4.0
               * (cosSigma * (-1 + 2 * cos2SigmaM * cos2SigmaM)
                  - B / 6.0 * cos2SigmaM * (-3 + 4 * sinSigma * sinSigma)
                        * (-3 + 4 * cos2SigmaM * cos2SigmaM)));
        const double next = s / (kB * A) + deltaSigma;
        const bool done = std::fabs(next - sigma) < 1e-12;
        sigma = next;
        if (done)
            break;
    }

    const double tmp = sinU1 * sinSigma - cosU1 * cosSigma * cosAlpha1;
    const double phi2 = std::atan2(sinU1 * cosSigma + cosU1 * sinSigma * cosAlpha1,
                                   (1 - kF) * std::sqrt(sinAlpha * sinAlpha + tmp * tmp));
    const double lambda = std::atan2(sinSigma * sinAlpha1, cosU1 * cosSigma - sinU1 * sinSigma * cosAlpha1);
    const double C = kF / 16.0 * cosSqAlpha * (4 + kF * (4 - 3 * cosSqAlpha));
    const double L = lambda - (1 - C) * kF * sinAlpha
        * (sigma + C * sinSigma * (cos2SigmaM + C * cosSigma * (-1 + 2 * cos2SigmaM * cos2SigmaM)));

    double lat = toDeg(phi2);
    double lon = lng1 + toDeg(L);
    if (!std::isfinite(lat) || !std::isfinite(lon)) {
        // 球面后备（半径接近半周长时不收敛）：误差 < 0.5%，总比画出 NaN 好
        const double d = distKm / 6371.0;
        const double phi1 = toRad(lat1);
        const double p2 = std::asin(qBound(-1.0, std::sin(phi1) * std::cos(d)
                                                    + std::cos(phi1) * std::sin(d) * cosAlpha1, 1.0));
        lat = toDeg(p2);
        lon = lng1 + toDeg(std::atan2(std::sin(alpha1) * std::sin(d) * std::cos(phi1),
                                      std::cos(d) - std::sin(phi1) * std::sin(p2)));
    }
    while (lon > 180.0)  lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    *lat2 = lat;
    *lng2 = lon;
    return true;
}

QVector<QPointF> geodesicCircle(double lat, double lon, double radiusKm, int segments)
{
    QVector<QPointF> out;
    if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(radiusKm) || radiusKm <= 0.0)
        return out;
    if (std::fabs(lat) >= 90.0)          // 极点上「方位角」无定义，圆退化成纬圈
        lat = lat > 0 ? 89.9999 : -89.9999;
    segments = qBound(8, segments, 1024);
    const double r = qMin(radiusKm, 20000.0);   // 超过半个周长会自交，钳一下
    out.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        double plat = lat, plon = lon;
        if (!destination(lat, lon, 360.0 * i / segments, r, &plat, &plon))
            return QVector<QPointF>();
        out.append(QPointF(plon, plat));       // x = 经度, y = 纬度
    }
    return out;
}

QVector<QPointF> geodesicPath(double lat1, double lon1, double lat2, double lon2, double maxStepDeg)
{
    QVector<QPointF> out;
    if (!std::isfinite(lat1) || !std::isfinite(lon1) || !std::isfinite(lat2) || !std::isfinite(lon2))
        return out;
    const double d2r = M_PI / 180.0;
    const double p1 = lat1 * d2r, l1 = lon1 * d2r, p2 = lat2 * d2r, l2 = lon2 * d2r;
    const double e2 = 2 * kF - kF * kF;      // WGS-84 第一偏心率平方
    // 用椭球面上的点（不是法线方向）做单位向量：反算时才能用 atan2(z, (1-e²)p) 还原大地纬度，
    // 直接拿 (cosφcosλ, cosφsinλ, sinφ) 会多算一次扁率修正，纬度偏 0.19°（约 21 km）
    const double n1 = 1.0 / std::sqrt(1 - e2 * std::sin(p1) * std::sin(p1));
    const double n2 = 1.0 / std::sqrt(1 - e2 * std::sin(p2) * std::sin(p2));
    double ax = n1 * std::cos(p1) * std::cos(l1);
    double ay = n1 * std::cos(p1) * std::sin(l1);
    double az = n1 * (1 - e2) * std::sin(p1);
    double bx = n2 * std::cos(p2) * std::cos(l2);
    double by = n2 * std::cos(p2) * std::sin(l2);
    double bz = n2 * (1 - e2) * std::sin(p2);
    const double na = std::sqrt(ax * ax + ay * ay + az * az);
    const double nb = std::sqrt(bx * bx + by * by + bz * bz);
    ax /= na; ay /= na; az /= na;
    bx /= nb; by /= nb; bz /= nb;
    const double ang = std::acos(qBound(-1.0, ax * bx + ay * by + az * bz, 1.0));
    if (maxStepDeg <= 0.0)
        maxStepDeg = 0.25;
    int n = int(std::ceil(ang / (maxStepDeg * d2r)));
    n = qBound(1, n, 2048);
    out.reserve(n + 1);
    for (int i = 0; i <= n; ++i) {
        const double f = double(i) / n;
        double x = ax, y = ay, z = az;
        if (ang > 1e-12) {
            const double s = std::sin(ang);
            const double w1 = std::sin((1 - f) * ang) / s, w2 = std::sin(f * ang) / s;
            x = ax * w1 + bx * w2;
            y = ay * w1 + by * w2;
            z = az * w1 + bz * w2;
        }
        const double norm = std::sqrt(x * x + y * y + z * z);
        if (norm < 1e-12)
            continue;
        x /= norm; y /= norm; z /= norm;
        // 方向向量 -> 大地纬度（直接 asin(z) 是地心纬度，最多差 0.19°）
        const double lat = std::atan2(z, (1 - e2) * std::sqrt(x * x + y * y));
        const double lon = std::atan2(y, x);
        out.append(QPointF(lon / d2r, lat / d2r));
    }
    return out;
}

} // namespace geocalc
