# 离线高德瓦片工具

中国东部 + 东南沿海近岸走廊的高德 z1–z16 离线瓦片，供 RadarView 离线模式加载。

## 文件

| 文件 | 说明 |
|---|---|
| `download_tiles.py` | 按清单多线程下载 → 合并成 `amap_coast_z1_z16.mbtiles`（断点续传） |
| `make_corridor_tiles.py` | 用 Natural Earth 1:10m 海岸线算"应有的走廊"，可与现有库做差集得到缺口清单 |
| `migrate_tile_rows.py` | 修复 `tile_row` 的行号方案（XYZ ⇄ TMS），带 `--check` 诊断模式 |
| `tilelist_z1_z16.json` | 当前清单（z1–z16，XYZ 编号） |

## ⚠️ 行号方案（踩过的坑）

MBTiles 规范规定 `tile_row` 用 **TMS**：行号**从南往北**数。
而下载 URL 用的 y 是 **XYZ**（从北往南）。两者满足 `row = 2^z - 1 - y`。

早期版本的 `download_tiles.py` 直接把 XYZ 的 y 写进了 `tile_row`，结果是整包在
南北方向镜像——球面上表现为**中国沿海的瓦片跑到了南半球**（在 28°S 能看到中国海岸线），
同时本地坐标的位置全是空白。

现在：

- `download_tiles.py` 合并时已翻成 TMS，并写 `metadata.scheme=tms`
- `TileSource` 读取时按 `metadata.scheme` 决定翻不翻（缺省按规范的 TMS 处理）
- 已有的坏包用 `migrate_tile_rows.py` 修：

```
python tools/migrate_tile_rows.py --check  build/win7/release/tiles/amap_coast_z1_z16.mbtiles
python tools/migrate_tile_rows.py          build/win7/release/tiles/amap_coast_z1_z16.mbtiles
```

迁移是同一个事务里两步换位（先整体 `+= 2^z` 再镜像），不会撞主键；完成后校验张数不变。

## 下载

```
python tools/download_tiles.py [workers] [limit]
```
- 默认 16 并发；断点续传，重跑自动跳过已下载
- 完成后合并到 `tools/amap_coast_z1_z16.mbtiles`（`crs=gcj` + `scheme=tms`）

## 检查 / 补齐覆盖

```
# 只看缺口（不下载）
python tools/make_corridor_tiles.py --bbox 105,16,125,42 \
    --diff tools/amap_coast_z1_z16.mbtiles

# 生成缺口清单
python tools/make_corridor_tiles.py --bbox 105,16,125,42 \
    --diff tools/amap_coast_z1_z16.mbtiles --out tools/tilelist_gap.json
```

默认读 `D:\Desktop\RadarView_BiuldByTauri\dist\boundaries\coastline.geojson`
（Natural Earth 1:10m），也可用 `--coast` 指定别的 GeoJSON。

**实测规模参考**（z16、±12 km 走廊、105–125°E / 16–42°N，海岸线总长 23,418 km）：

- 应有 ≈ **154 万张**（约 4–5 GB）
- 现有 47.7 万张里只有约 13 万张落在正确走廊内 —— 早期清单是用一条很粗的海岸
  折线生成的，渤海湾、厦门—台湾海峡、三亚一带整段缺失
- 想压体积就收窄走廊：±6 km ≈ 80 万张

## 部署

把 `amap_coast_z1_z16.mbtiles` 拷到 `build/win7/release/tiles/` 或 exe 同目录，
启动时 `loadDefaultTiles()` 会优先发现它。`tiletmp_all/` 是临时目录，合并后可删。
