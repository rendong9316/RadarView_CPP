# -*- coding: utf-8 -*-
"""
近岸走廊瓦片清单生成器 + 与现有 mbtiles 的差集（缺口）统计

为什么需要它
------------
之前 tools/tilelist_z1_z16.json 是临时脚本用一条极粗的海岸折线生成的，
结果整个渤海湾、厦门—台湾海峡、三亚一带根本没有瓦片（离线时整片空白）。
这个脚本用 Natural Earth 1:10m 海岸线重新算一遍走廊，并且支持和已有
mbtiles 做差集，只输出"缺的那些瓦片"，方便增量补齐。

用法
----
    # 1) 只统计缺口（不改任何东西）
    python tools/make_corridor_tiles.py --diff tools/amap_coast_z1_z16.mbtiles

    # 2) 生成缺口清单，供 download_tiles.py 增量下载
    python tools/make_corridor_tiles.py --diff tools/amap_coast_z1_z16.mbtiles \
        --out tools/tilelist_gap.json

    # 3) 生成全新的走廊清单（不含差集）
    python tools/make_corridor_tiles.py --out tools/tilelist_new.json

说明
----
* 瓦片编号是 XYZ（y 从北往南），和下载 URL 一致；写入 mbtiles 时由
  download_tiles.py 翻转成 TMS。
* 高德瓦片是 GCJ-02 网格，所以海岸线点先转 GCJ 再算瓦片号。
* 走廊半宽默认 12 km（即陆侧 12 km + 海侧 12 km）。
"""
import argparse
import json
import math
import os
import sqlite3
import sys

# ---- GCJ-02 转换（与 crs.cpp 同一套算法） ----
_A = 6378245.0
_EE = 0.00669342162296594323


def _out_of_china(lon, lat):
    return not (72.004 <= lon <= 137.8347 and 0.8293 <= lat <= 55.8271)


def _transform_lat(x, y):
    ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * math.sqrt(abs(x))
    ret += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    ret += (20.0 * math.sin(y * math.pi) + 40.0 * math.sin(y / 3.0 * math.pi)) * 2.0 / 3.0
    ret += (160.0 * math.sin(y / 12.0 * math.pi) + 320.0 * math.sin(y * math.pi / 30.0)) * 2.0 / 3.0
    return ret


def _transform_lon(x, y):
    ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * math.sqrt(abs(x))
    ret += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    ret += (20.0 * math.sin(x * math.pi) + 40.0 * math.sin(x / 3.0 * math.pi)) * 2.0 / 3.0
    ret += (150.0 * math.sin(x / 12.0 * math.pi) + 300.0 * math.sin(x / 30.0 * math.pi)) * 2.0 / 3.0
    return ret


def wgs2gcj(lon, lat):
    if _out_of_china(lon, lat):
        return lon, lat
    dlat = _transform_lat(lon - 105.0, lat - 35.0)
    dlon = _transform_lon(lon - 105.0, lat - 35.0)
    rad = math.radians(lat)
    magic = 1 - _EE * math.sin(rad) ** 2
    sqrt_magic = math.sqrt(magic)
    dlat = (dlat * 180.0) / ((_A * (1 - _EE)) / (magic * sqrt_magic) * math.pi)
    dlon = (dlon * 180.0) / (_A / sqrt_magic * math.cos(rad) * math.pi)
    return lon + dlon, lat + dlat


# ---- 瓦片编号 ----
def lonlat_to_xyz(lon, lat, z):
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    r = math.radians(lat)
    y = int((1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def load_coastlines(path, bbox):
    """读 GeoJSON，只保留落在 bbox 附近的线；返回 [[(lon,lat), ...], ...]"""
    with open(path, encoding='utf-8') as f:
        gj = json.load(f)
    x0, y0, x1, y1 = bbox
    lines = []

    def push(ring):
        # 注意：不能"过滤掉外面的点再连起来"——那样会把断开的点连成跨海的
        # 假线段（例如从山东直接连到朝鲜半岛），走廊会被算大好几倍。
        # 这里按连续段切开，只在 bbox 内的连续点之间连线。
        run = []
        for p in ring:
            if x0 - 1 <= p[0] <= x1 + 1 and y0 - 1 <= p[1] <= y1 + 1:
                run.append(p)
            else:
                if len(run) >= 2:
                    lines.append(run)
                run = []
        if len(run) >= 2:
            lines.append(run)

    for feat in gj.get('features', []):
        geom = feat.get('geometry') or {}
        t = geom.get('type')
        c = geom.get('coordinates') or []
        if t == 'LineString':
            push([(p[0], p[1]) for p in c])
        elif t == 'MultiLineString':
            for ring in c:
                push([(p[0], p[1]) for p in ring])
    return lines


def corridor_tiles(lines, z, half_km, step_km=0.6):
    """沿每条线按 step_km 采样，把每个采样点周围 ±half_km 的瓦片都收进来"""
    n = 1 << z
    tiles = set()
    samples = 0
    for line in lines:
        for i in range(len(line) - 1):
            lo0, la0 = line[i]
            lo1, la1 = line[i + 1]
            # 段长（km），用等距圆柱近似，量级足够
            dx = (lo1 - lo0) * 111.32 * math.cos(math.radians((la0 + la1) / 2))
            dy = (la1 - la0) * 111.32
            steps = max(1, int(math.ceil(math.hypot(dx, dy) / step_km)))
            for k in range(steps + 1):
                t = k / float(steps)
                lo = lo0 + (lo1 - lo0) * t
                la = la0 + (la1 - la0) * t
                glo, gla = wgs2gcj(lo, la)
                dlat = half_km / 111.32
                dlon = half_km / max(0.2, 111.32 * math.cos(math.radians(gla)))
                xa, yb = lonlat_to_xyz(glo - dlon, gla + dlat, z)   # 西北角（y 小）
                xb, ya = lonlat_to_xyz(glo + dlon, gla - dlat, z)   # 东南角（y 大）
                for x in range(min(xa, xb), max(xa, xb) + 1):
                    for y in range(min(ya, yb), max(ya, yb) + 1):
                        tiles.add((x, y))
                samples += 1
    return tiles, samples


def load_existing(db):
    """读现有 mbtiles 的 (z, x, y)，行号统一换回 XYZ"""
    con = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    scheme = 'tms'
    try:
        row = con.execute("SELECT value FROM metadata WHERE name='scheme'").fetchone()
        if row and str(row[0]).strip().lower() == 'xyz':
            scheme = 'xyz'
    except sqlite3.Error:
        pass
    out = {}
    for z, x, row in con.execute("SELECT zoom_level, tile_column, tile_row FROM tiles"):
        y = row if scheme == 'xyz' else ((1 << z) - 1 - row)
        out.setdefault(z, set()).add((x, y))
    con.close()
    return out, scheme


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--coast', default=None, help='海岸线 GeoJSON（Natural Earth 1:10m）')
    ap.add_argument('--z', type=int, default=16)
    ap.add_argument('--half-km', type=float, default=12.0, help='走廊半宽（km）')
    ap.add_argument('--bbox', default='104,14,136,46', help='经度0,纬度0,经度1,纬度1')
    ap.add_argument('--diff', default=None, help='已有 mbtiles，只输出缺失的瓦片')
    ap.add_argument('--out', default=None, help='清单输出 JSON')
    args = ap.parse_args()

    coast = args.coast
    if not coast:
        for cand in (r'D:\Desktop\RadarView_BiuldByTauri\dist\boundaries\coastline.geojson',
                     r'D:\Desktop\RadarView_BiuldByTauri\public\boundaries\coastline.geojson',
                     'tools/data/ne_10m_coastline.geojson'):
            if os.path.isfile(cand):
                coast = cand
                break
    if not coast or not os.path.isfile(coast):
        print('找不到海岸线数据，请用 --coast 指定 Natural Earth 1:10m coastline.geojson')
        return 2

    bbox = tuple(float(v) for v in args.bbox.split(','))
    lines = load_coastlines(coast, bbox)
    print('海岸线: %s  段数 %d' % (coast, len(lines)))

    want, samples = corridor_tiles(lines, args.z, args.half_km)
    print('z%d 走廊（半宽 %.0f km）应有瓦片 %d 张（采样点 %d）' %
          (args.z, args.half_km, len(want), samples))

    if args.diff:
        have, scheme = load_existing(args.diff)
        got = have.get(args.z, set())
        missing = want - got
        extra = got - want
        print('现有库: scheme=%s  z%d 共 %d 张' % (scheme, args.z, len(got)))
        print('缺口  : %d 张（占应有 %.1f%%）' % (len(missing), 100.0 * len(missing) / max(1, len(want))))
        print('超出走廊: %d 张（多余，不影响显示）' % len(extra))
        # 按 2°×2° 格子给出缺口最大的区域
        bucket = {}
        for (x, y) in missing:
            lon = (x + 0.5) / (1 << args.z) * 360 - 180
            lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + 0.5) / (1 << args.z)))))
            key = (int(math.floor(lon / 2) * 2), int(math.floor(lat / 2) * 2))
            bucket[key] = bucket.get(key, 0) + 1
        print('缺口最大的 8 个 2°x2° 区域:')
        for (lo, la), c in sorted(bucket.items(), key=lambda kv: -kv[1])[:8]:
            print('   %3d..%3dE  %3d..%3dN   %d 张' % (lo, lo + 2, la, la + 2, c))
        result = sorted(missing)
    else:
        result = sorted(want)

    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump({'zooms': {str(args.z): [list(t) for t in result]}}, f)
        print('清单已写入 %s（%d 张）' % (args.out, len(result)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
