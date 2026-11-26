# 架构与模块说明

`RadarView_CPP` 为纯 C++11 + Qt 5.14.2 单体桌面客户端，表现层与数据/业务层运行在同一进程内，不依赖浏览器内核或本地后台服务。

## 模块分层

| 分层 | 核心模块 | 职责 |
|---|---|---|
| 应用外壳 | `main`、`mainwindow`、`appshellwidgets`、`drawingcontroller`、`mapsourcecontroller`、`point3dcoordinator`、`trackimportcoordinator`、`apppaths`、`splashscreen`、`theme`、`uiwidgets`、`lucide` | 启动入口、VSCode 风格活动栏/侧栏外壳、绘图/底图/三维/导入协调器、路径迁移与设置持久化、主题与矢量图标 |
| 地理与航迹核心 | `geo`、`crs`、`geocalc`、`track`、`replaycontroller` | WGS84 ⇄ GCJ-02 纠偏、Vincenty 正反算与大圆加密、内存航迹存储与回放状态机 |
| 解析与持久化 | `matfile`、`trackparsers`、`trackimporter`、`trackdb` | 自研 MATLAB MAT v5 读取器、雷达/ADS-B 解析、异步导入与 SQLite 航迹库 |
| 3D 地球与标绘 | `globewidget`、`globecamera`、`globesurface`、`globedrawcontroller`、`globeoverlay`、`tracklayer`、`tilesource`、`netmap`、`maptools`、`spatiallasso`、`drawingmanager`、`drawcommands` | OpenGL ES 2.0 椭球与宽线渲染、相机/投影、地表着色器、2D 叠加层、离线/在线瓦片源、旗标/标尺、套索与贴地绘图撤销栈 |
| 业务面板与弹窗 | `appstatusbar`、`sidepanels`、`managepanel`、`settingspanel`、`trackpointdialog`、`trackpicker` | 状态栏数据源条、筛选/管理/设置侧栏、点迹明细表、航迹选择器 |
| 点迹三维可视化 | `point3dtypes`、`point3dparsers`、`point3dimportjob`、`point3dcanvas`、`point3dsidepanel`、`point3dadvancedpanel`、`point3dwindow` | 可多开独立点云窗口（`QPainter` 正交投影、侧栏设置/高级参数页、SNR 色标、区域裁剪、独立逐帧回放） |

## 三方联动机制（地图 ↔ 点迹三维窗口 ↔ 数据库管理）

- **统一身份识别**：按 `BatchNo + 时间范围`（`LinkedTrackRef{batchNo, t0, t1}`）匹配，而不是字符串 ID（`RADAR-####` 与 `RAW-####` 编号和时间一致时判定为同一航迹）。
- **单选与筛选广播**：地图单击隔离或管理面板可见集合变更时，通过 `setLinkedSelection` 广播至所有已打开的点迹窗口；若窗口内无匹配航迹则保持原样。
- **悬停双向高亮**：地图悬停 → `MainWindow::onMapHoverTrack` → 各点迹窗口 `setLinkedHighlight`；点迹窗口悬停（航迹命中优先于散点）→ `trackFocusChanged` → 地图 `setHoveredTrack`。
- **航迹配色联动**：颜色粒度为数据源文件（`来源::文件名`），写入 `TrackStore::m_fileColors` 后同步更新地图航迹、点迹、状态栏 chip、管理面板圆点以及所有点迹窗口叠加航迹。
- **回放解耦**：主地图时间轴由 `ReplayController` 驱动；每个点迹三维窗口自带独立逐帧回放（不做时间插值，按帧间时间差推进），互不干扰。
