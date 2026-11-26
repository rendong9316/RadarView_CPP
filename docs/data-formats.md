# 数据格式、时间基准与数据库 Schema

## 1. 强制统一的时间基准

所有外部数据源的原始时间均为**北京时间（UTC+8）墙钟**，程序内部统一转换为 **Unix epoch 毫秒（UTC 数轴）**，全工程仅保留此单一时间表示：

| 数据源 | 原始时间字段 | 换算入口 | 换算规则 |
|---|---|---|---|
| 雷达航迹 `.mat` | `time` / `sysPara.datenum`（MATLAB datenum） | `datenumToMs` (`trackparsers.cpp`) | `(dn - 719529) * 86400 * 1000 - 8h` |
| 点迹 `.mat` (`DetPoint`) | `DetPoint/sysPara.datenum` | `datenumToMs` | 与航迹 `time` 同口径，可逐帧对齐 |
| ADS-B 批量历史 `.csv` | 第 10 列文本时间（UTC+8） | `parseBeijingTime` (`track.cpp`) | 按 UTC+8 解析后减 8 小时 |
| 界面与表格显示 | 内部 epoch ms | `formatBeijingTime` (`track.cpp`) | 加 8 小时还原北京时间 |

> 注意：`frameID` 在不同源间位数与起始编号不一致，**严禁作为跨源对齐键**；跨源对齐与排序一律以 epoch ms 为准。

## 2. 输入文件格式

- **MATLAB `.mat` v5**：由自研只读解析器 `MatFile` 读取，支持小端、`miCOMPRESSED`（zlib）、数值矩阵、结构体数组、元胞与字符数组。雷达航迹导入时同步提取 `smoothPointList`/`outputPointList`（主点列）与 `asscPointList`（关联点列，含 `snr`）。
- **ADS-B CSV**：批量历史位置格式，包含航班元数据与位置/高度/航向/地速/垂直率。
- **离线瓦片 `.mbtiles`**：SQLite 瓦片库，对外统一 XYZ 编号，内部按 `metadata.scheme`（默认 `tms`）与 `metadata.crs`（`gcj` 或默认 `wgs84`）自适应处理。

## 3. SQLite 航迹库 Schema 演进（`trackdb`）

核心表：`batches`（导入批次）与 `saved_tracks`（航迹元数据列 + 小端二进制点列 BLOB）。

| 版本 | 主要变更 |
|---|---|
| v2 | 基础航迹元数据与经纬高点列 |
| v3 | 点列扩展至 52 字节/点（含 `prange`、`pvr`、`paz`）与 `has_measure` 标记 |
| v4 | 同行新增关联点列 `raw_points`（`asscPointList`）及 `has_raw` / `raw_point_count` |
| v5 | `batches` 新增 `display_name TEXT`（重命名仅改显示名，`file_name` 保持唯一键） |
| v6 | 点列扩展至 56 字节/点（追加 `snr` dB）及 `has_snr` 列；老库迁移补 `NaN`，重导同文件原地补列 |

## 4. 本地数据目录与旧版迁移

- 默认用户数据目录位于 `QStandardPaths::GenericDataLocation` 下的 `RadarView/RadarView/`，存放 `radarview.db`、`drawings.json` 及在线底图缓存 `tilecache/`。
- 首次启动时若检测到历史目录 `RadarView/HelloVscode/` 存在且新目录缺失对应数据，程序会自动迁移数据库（含 `-wal`/`-shm`）、绘图文件与瓦片缓存（数据库与绘图按复制迁入、保留旧文件，且不覆盖新目录已有文件）。自检模式（`app::setTestMode(true)`）完全隔离在可执行文件同目录的临时文件内。
