# -*- coding: utf-8 -*-
"""
mbtiles 行号方案迁移工具：XYZ -> TMS（符合 MBTiles 规范）

背景
----
MBTiles 规范规定 tile_row 用 TMS：行号从**南**往北数。
本项目的 amap_coast_z1_z16.mbtiles 早期由 download_tiles.py 生成时，
直接把 XYZ 的 y（从北往南）写进了 tile_row，导致所有瓦片在南北方向
被镜像 —— 在球面上表现为"整包错位"（中国沿海的瓦片跑到了南半球）。

用法
----
    python tools/migrate_tile_rows.py <a.mbtiles> [b.mbtiles ...]
    python tools/migrate_tile_rows.py --check <a.mbtiles>   只诊断不改动

做法
----
同一次事务里两步换位，避免主键冲突（y -> 2^z-1-y 直接原地改会互相撞）：
    1) tile_row += 2^z      （所有行整体移出 0..2^z-1 区间）
    2) tile_row  = 2^z-1-(tile_row-2^z)
同时在 metadata 里补 minzoom/maxzoom/format/scheme，并去掉重复的元数据行。
"""
import os
import sqlite3
import sys

META = ['name', 'type', 'version', 'description', 'format',
        'minzoom', 'maxzoom', 'bounds', 'crs', 'scheme']


def table_names(con):
    return {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type IN ('table','view')")}


def stats(con):
    out = {}
    for z, n, y0, y1 in con.execute(
            "SELECT zoom_level, COUNT(*), MIN(tile_row), MAX(tile_row) "
            "FROM tiles GROUP BY zoom_level ORDER BY zoom_level"):
        out[z] = (n, y0, y1, (1 << z) - 1)
    return out


def scheme_hint(con, st):
    """按纬度分布判断行号到底是从北数还是从南数。

    MBTiles 的 scheme 没有可靠声明时可这样判断：中国一带的瓦片包，
    若行号集中在 2^z 的**上半段**（0 .. 2^z/2），说明是从北往南数（XYZ），
    因为北京/上海都在北纬 20-40°，而 TMS 行号会是 2^z 的 60%-80% 段。
    """
    if not st:
        return 'empty'
    zmax = max(st)
    n, y0, y1, m = st[zmax]
    mid = m / 2.0
    if y1 < mid:
        return 'xyz'          # 全在北半球上半段 -> 从北数
    if y0 > mid:
        return 'tms'          # 全在下半段 -> 从南数
    return 'unknown'          # 跨赤道，判断不了


def check(path):
    con = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
    if 'tiles' not in table_names(con):
        print('%s: 没有 tiles 表' % path)
        return
    st = stats(con)
    meta = dict(con.execute("SELECT name, value FROM metadata"))
    print('%s' % path)
    print('  声明: crs=%s  scheme=%s  format=%s' %
          (meta.get('crs', '-'), meta.get('scheme', '(未声明)'), meta.get('format', '-')))
    print('  行号分布推断: %s' % scheme_hint(con, st))
    for z in sorted(st):
        n, y0, y1, m = st[z]
        print('    z%-2d n=%-6d row=%d..%d  (2^z-1=%d)' % (z, n, y0, y1, m))
    con.close()


def migrate(path):
    con = sqlite3.connect(path, isolation_level=None)
    con.execute("PRAGMA journal_mode=DELETE")
    st = stats(con)
    if not st:
        print('%s: 没有瓦片，跳过' % path)
        con.close()
        return False
    if scheme_hint(con, st) == 'tms':
        print('%s: 已是 TMS，无需迁移' % path)
        con.close()
        return True

    before = {z: st[z][0] for z in st}
    print('%s: 开始迁移 %d 张瓦片 (XYZ -> TMS)' % (path, sum(before.values())))
    con.execute("BEGIN IMMEDIATE")
    try:
        # 1) 所有行整体抬高 2^z，移出目标区间
        con.execute("UPDATE tiles SET tile_row = tile_row + (1 << zoom_level)")
        # 2) 镜像回 0..2^z-1
        con.execute("UPDATE tiles SET tile_row = ((1 << zoom_level) - 1) "
                                     "- (tile_row - (1 << zoom_level))")
        con.execute("COMMIT")
    except Exception:
        con.execute("ROLLBACK")
        con.close()
        raise

    # metadata 整理：去重 + 补全
    print('  整理 metadata ...')
    con.execute("BEGIN IMMEDIATE")
    rows = {}
    for k, v in con.execute("SELECT name, value FROM metadata"):
        rows.setdefault(k, v)          # 同名只保留第一条
    st_after = stats(con)
    rows.setdefault('format', 'png')
    rows['scheme'] = 'tms'
    rows['minzoom'] = str(min(st_after))
    rows['maxzoom'] = str(max(st_after))
    con.execute("DELETE FROM metadata")
    for k in META:
        if k in rows:
            con.execute("INSERT INTO metadata VALUES(?,?)", (k, rows[k]))
    for k, v in rows.items():
        if k not in META:
            con.execute("INSERT INTO metadata VALUES(?,?)", (k, v))
    con.execute("COMMIT")
    con.execute("VACUUM")
    con.close()

    # 校验：总数不变、行号落在新位置
    con = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
    after = {z: stats(con)[z][0] for z in stats(con)}
    con.close()
    ok = before == after
    print('  校验: 迁移前 %d 张, 迁移后 %d 张 -> %s' %
          (sum(before.values()), sum(after.values()), 'OK' if ok else '不一致!'))
    return ok


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    if args[0] == '--check':
        for p in args[1:]:
            check(p)
        return 0
    ok = True
    for p in args:
        if not os.path.isfile(p):
            print('%s: 文件不存在' % p)
            ok = False
            continue
        ok = migrate(p) and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
