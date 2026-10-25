#ifndef CRS_H
#define CRS_H

#include <QString>

// 坐标系转换：WGS84（GPS 原始坐标） ⇄ GCJ-02（高德/腾讯等国内地图的加密坐标）
//
// GCJ-02 是在 WGS84 上加了一个非线性的偏移（公开算法，国内 50–500 m），
// 偏移量只和经纬度有关，所以：给定一个坐标，你无法从数值上判断它是哪套——
// 只能靠来源声明。本程序内部一律用 WGS84 存数据，只在「画到某种底图上」时转换。
namespace crs {

// 国内范围矩形（标准 GCJ-02 判定；境外 Amap 也不加偏，所以这里返回 false 就按恒等处理）
bool inChina(double lonDeg, double latDeg);

// WGS84 → GCJ-02。境外坐标原样返回。返回 true 表示确实做了偏移
bool wgs84ToGcj02(double lonWgs, double latWgs, double *lonGcj, double *latGcj);

// GCJ-02 → WGS84。GCJ-02 没有解析逆，这里用不动点迭代（8 次可收敛到 1e-9°，约 0.1 mm）
bool gcj02ToWgs84(double lonGcj, double latGcj, double *lonWgs, double *latWgs);

} // namespace crs

#endif // CRS_H
