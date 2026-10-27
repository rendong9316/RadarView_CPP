# -*- coding: utf-8 -*-
"""
高德 近岸离线瓦片下载工具 (z1-z16)
- 读取 tools/tilelist_z1_z16.json (多 zoom 清单，XYZ 编号)
- 多线程下载到 tools/tiletmp_all/{z}_{x}_{y}.png (y 为 XYZ，与 URL 一致)，断点续传
- 完成后单进程合并到 tools/amap_coast_z1_z16.mbtiles

注意：MBTiles 规范里 tile_row 是 **TMS**（从南往北数），而下载用的 y 是
XYZ（从北往南）。合并时必须翻转成 2^z-1-y，否则整包会在南北方向镜像，
在球面上表现为"瓦片全部错位"（本工具早期版本就踩过这个坑，
可用 tools/migrate_tile_rows.py 修复已生成的库）。

用法:
    python tools/download_tiles.py [workers] [limit]
    limit: 测试用, 最多处理多少张
"""
import json, os, sqlite3, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.request import urlopen, Request

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, 'tilelist_z1_z16.json')
DB = os.path.join(HERE, 'amap_coast_z1_z16.mbtiles')
TMP_DIR = os.path.join(HERE, 'tiletmp_all')
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) RadarView/1.0'
REFERER = 'https://www.amap.com/'


def tmp_path(z, x, y):
    return os.path.join(TMP_DIR, '%d_%d_%d.png' % (z, x, y))


def fetch_tile(z, x, y, retries=2, timeout=20):
    sub = str((x + y) % 4 + 1)
    url = ("http://webrd0%s.is.autonavi.com/appmaptile?"
           "lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d") % (sub, x, y, z)
    for attempt in range(retries + 1):
        try:
            req = Request(url, headers={'User-Agent': UA, 'Referer': REFERER})
            data = urlopen(req, timeout=timeout).read()
            return data if len(data) >= 50 else b''
        except Exception:
            if attempt < retries:
                time.sleep(0.5 * (attempt + 1))
    return None


def worker(item):
    z, x, y = item
    p = tmp_path(z, x, y)
    if os.path.exists(p) and os.path.getsize(p) > 0:
        return 'cached'
    data = fetch_tile(z, x, y)
    if data is None:
        return 'fail'
    with open(p, 'wb') as f:
        f.write(data)
    return 'empty' if not data else 'ok'


def merge_to_mbtiles():
    con = sqlite3.connect(DB)
    con.executescript("""
    CREATE TABLE IF NOT EXISTS metadata (name TEXT, value TEXT);
    CREATE TABLE IF NOT EXISTS tiles (
        zoom_level INTEGER,
        tile_column INTEGER,
        tile_row INTEGER,
        tile_data BLOB,
        PRIMARY KEY (zoom_level, tile_column, tile_row));
    CREATE INDEX IF NOT EXISTS idx_tiles_tile_coords ON tiles
        (zoom_level, tile_column, tile_row);
    """)
    con.execute("DELETE FROM metadata WHERE name IN "
                "('name','type','format','crs','scheme','minzoom','maxzoom')")
    con.execute("INSERT INTO metadata VALUES('name','amap_coast_z1_z16')")
    con.execute("INSERT INTO metadata VALUES('type','baselayer')")
    con.execute("INSERT INTO metadata VALUES('crs','gcj')")      # 高德瓦片是 GCJ-02
    con.execute("INSERT INTO metadata VALUES('format','png')")
    con.execute("INSERT INTO metadata VALUES('scheme','tms')")   # 规范：行号从南往北
    con.commit()

    files = [f for f in os.listdir(TMP_DIR) if f.endswith('.png')]
    n, nempty, batch = 0, 0, []
    for fname in files:
        z, x, y = map(int, fname[:-4].split('_'))
        data = open(os.path.join(TMP_DIR, fname), 'rb').read()
        if not data:
            nempty += 1
            continue
        row = (1 << z) - 1 - y          # XYZ -> TMS
        batch.append((z, x, row, sqlite3.Binary(data)))
        n += 1
        if len(batch) >= 2000:
            con.executemany("INSERT OR REPLACE INTO tiles VALUES(?,?,?,?)", batch)
            con.commit()
            batch.clear()
    if batch:
        con.executemany("INSERT OR REPLACE INTO tiles VALUES(?,?,?,?)", batch)
        con.commit()

    zmin, zmax = con.execute("SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles").fetchone()
    for k, v in (('minzoom', zmin), ('maxzoom', zmax)):
        if v is not None:
            con.execute("INSERT OR REPLACE INTO metadata VALUES(?,?)", (k, str(v)))
    con.commit()
    total = con.execute("SELECT COUNT(*) FROM tiles").fetchone()[0]
    zooms = [r[0] for r in con.execute("SELECT DISTINCT zoom_level FROM tiles ORDER BY zoom_level")]
    con.close()
    return n, nempty, total, zooms


def main():
    workers = int(sys.argv[1]) if len(sys.argv) > 1 else 16
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    with open(LIST, 'r') as f:
        d = json.load(f)
    # 展开成 (z,x,y) 列表, z1 在前(低层先下, 大区域少而快)
    items = []
    for z in sorted(d['zooms'].keys(), key=int):
        for (x, y) in d['zooms'][z]:
            items.append((int(z), x, y))
    if limit:
        items = items[:limit]

    os.makedirs(TMP_DIR, exist_ok=True)
    print('清单合计 %d 张 (z1-z%d), 本次处理 %d 张, 并发 %d'
          % (len(items), max(int(z) for z, x, y in items), len(items), workers))

    todo = [it for it in items
            if not (os.path.exists(tmp_path(*it)) and os.path.getsize(tmp_path(*it)) > 0)]
    print('已缓存 %d, 待下载 %d' % (len(items) - len(todo), len(todo)))

    if todo:
        t0 = time.time()
        ok = empty = fail = 0
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = [pool.submit(worker, it) for it in todo]
            for i, fut in enumerate(as_completed(futs), 1):
                r = fut.result()
                if r == 'ok':
                    ok += 1
                elif r in ('empty', 'cached'):
                    empty += 1
                else:
                    fail += 1
                if i % 5000 == 0 or i == len(todo):
                    el = time.time() - t0
                    print('  %d/%d  ok=%d empty=%d fail=%d  %.1f 张/s'
                          % (i, len(todo), ok, empty, fail, i / max(el, 1)))
        print('下载完成: ok=%d empty=%d fail=%d, 耗时 %.0f min'
              % (ok, empty, fail, (time.time() - t0) / 60))
        if fail:
            print('有 %d 张失败, 重跑本脚本可续传' % fail)

    added, nempty, total, zooms = merge_to_mbtiles()
    print('mbtiles: 库内共 %d 张, 层级 %s' % (total, zooms))


if __name__ == '__main__':
    main()
