# -*- coding: utf-8 -*-
"""
高德 z16 近岸离线瓦片下载工具
- 读取 tools/tilelist_z16.json 清单
- 多线程下载到临时文件(每线程独立, 无共享状态), 完成后单进程合并进 mbtiles
- 断点续传: 临时文件已存在且有效则跳过; 重新运行自动继续
- 输出 mbtiles 与 TileSource 直接兼容(metadata crs=gcj)

用法:
    python tools/download_tiles.py [workers] [limit]
"""
import json, os, sqlite3, sys, time, threading, hashlib
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.request import urlopen, Request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIST = os.path.join(HERE, 'tilelist_z16.json')
DB = os.path.join(HERE, 'amap_z16_coast.mbtiles')
TMP_DIR = os.path.join(HERE, 'tiletmp_z16')
Z = 16
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) RadarView/1.0'
REFERER = 'https://www.amap.com/'


def tmp_path(x, y):
    return os.path.join(TMP_DIR, '%d_%d_%d.png' % (Z, x, y))


def fetch_tile(x, y, retries=2, timeout=20):
    sub = str((x + y) % 4 + 1)
    url = ("http://webrd0%s.is.autonavi.com/appmaptile?"
           "lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d") % (sub, x, y, Z)
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
    x, y = item
    p = tmp_path(x, y)
    # 已有有效缓存
    if os.path.exists(p) and os.path.getsize(p) > 0:
        return 'cached'
    data = fetch_tile(x, y)
    if data is None:
        return 'fail'
    # 空瓦片(纯海) 也记一个 0 字节占位, 避免下次重下
    with open(p, 'wb') as f:
        f.write(data)
    return 'empty' if not data else 'ok'


def merge_to_mbtiles():
    """把所有临时文件灌进 mbtiles (单进程, 无锁)."""
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
    con.execute("INSERT OR REPLACE INTO metadata VALUES('name','amap_z16_coast')")
    con.execute("INSERT OR REPLACE INTO metadata VALUES('crs','gcj')")
    con.execute("INSERT OR REPLACE INTO metadata VALUES('format','png')")
    con.commit()

    files = [f for f in os.listdir(TMP_DIR) if f.endswith('.png')]
    n, nempty, nfail = 0, 0, 0
    for fname in files:
        z_, x, y = map(int, fname[:-4].split('_'))
        data = open(os.path.join(TMP_DIR, fname), 'rb').read()
        if not data:
            nempty += 1
            continue
        con.execute("INSERT OR REPLACE INTO tiles VALUES(?,?,?,?)",
                    (z_, x, y, sqlite3.Binary(data)))
        n += 1
    con.commit()
    total = con.execute("SELECT COUNT(*) FROM tiles").fetchone()[0]
    con.close()
    return n, nempty, total


def main():
    workers = int(sys.argv[1]) if len(sys.argv) > 1 else 16
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    with open(LIST, 'r') as f:
        tiles = json.load(f)['tiles']
    if limit:
        tiles = tiles[:limit]

    os.makedirs(TMP_DIR, exist_ok=True)
    print('清单 %d 张, 并发 %d' % (len(tiles), workers))

    todo = [(x, y) for (x, y) in tiles
            if not (os.path.exists(tmp_path(x, y)))
            or (os.path.exists(tmp_path(x, y)) and os.path.getsize(tmp_path(x, y)) == 0)]
    cached = len(tiles) - len(todo)
    print('已有缓存 %d, 待下载 %d' % (cached, len(todo)))

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
            print('有 %d 张失败(未写文件), 重跑本脚本可续传' % fail)

    added, nempty, total = merge_to_mbtiles()
    print('mbtiles: 本次写入 %d 张, 库内共 %d 张 (空瓦片 %d 未入库)'
          % (added, total, nempty))


if __name__ == '__main__':
    main()
