# 离线高德瓦片下载工具

## 用途
中国东部 + 东南沿海近岸走廊（双向 12 km）的 高德 z1-z16 离线瓦片，供 RadarView 离线模式加载。

## 清单
`tools/tilelist_z1_z16.json` — 476,978 张 z16 瓦片（GCJ-02 对齐，含青岛、上海验证点）。

## 下载
```
python tools/download_tiles.py [workers] [limit]
```
- 默认 16 并发；断点续传，重跑自动跳过已下载
- 下载后自动合并到 `tools/amap_coast_z1_z16.mbtiles`（metadata 声明 `crs=gcj`，与 `TileSource` 直接兼容）

## 使用
下载完成后，把 `tools/amap_coast_z1_z16.mbtiles` 拷到 `build/win7/release/tiles/` 或 exe 同目录，
启动时 `loadDefaultTiles()` 会自动发现并加载。

## 清理
- `tools/tiletmp_z16/` 是临时目录，合并完成后可以整个删掉
- `tools/amap_coast_z1_z16.mbtiles` 是最终产物
