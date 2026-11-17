# HelloVscode

VSCode 风格的 Qt 外壳 + 自研 3D 地球（WGS84 椭球，加载本地 mbtiles 瓦片），目标是用 C++ 复刻 `D:/Desktop/RadarView_BiuldByTauri`（雷达/ADS-B 航迹导入、绘制、回放）。

## 首要目标：同时兼容 Windows 7 和 RHEL 7.6

以后所有改动都必须同时满足：

- **Windows 7 SP1 64 位**
- **Red Hat Enterprise Linux 7.6 x86_64**（军用涉密电脑：离线、不能装软件、可能没有独立显卡或只能软件渲染）

新功能、重构、依赖升级前先确认两个平台都不受影响；做不到时先说明原因再动手。代码只写一份，平台差异用 `#ifdef Q_OS_WIN` / `Q_OS_LINUX` 隔离，并尽量收敛在少数文件里。

### 工具链（不要换）

| | Windows 7 | RHEL 7.6 |
|---|---|---|
| Qt | 5.14.2 MinGW 64：`D:/software/Qt/Qt5.14.2/5.14.2/mingw73_64` | 5.14.2 gcc_64（官方 Linux 安装包） |
| 编译器 | 干净的 MinGW 7.3.0：`D:/software/mingw730_clean/Tools/mingw730_64/bin` | devtoolset-7（GCC 7.3）；不要用系统自带 GCC 4.8.5 |
| 构建脚本 | `build_win7.sh` | `build_linux.sh` |

- Qt 6 不支持 Win7，也不支持 RHEL 7（glibc 2.17），不要升级。
- 不要用 `D:/software/Qt/Qt5.14.2/Tools/mingw730_64`，它已被 MSYS2 gcc 16 污染（运行库依赖 Win8+ API，且带一个 Qt6 的 qmake）。
- qmake 用全路径调用。语言标准保持 `CONFIG += c++11`。
- RHEL 7.6 只有 glibc 2.17、libstdc++ 4.8.5 的 ABI：在更新的 Linux 上编出的程序不能直接拷过去，必须在 CentOS/RHEL 7 上编译。devtoolset 的新特性靠静态补丁链接，产物仍只依赖系统 libstdc++。

### 依赖约束

- 只用 QtCore / QtGui / QtWidgets / QtSql（qsqlite）。不引入 QtWebEngine、QtMultimedia、QtNetwork 服务端、ActiveQt、QtQuick 等，也不引入第三方库；确有需要时先评估两个平台的离线部署。
- zlib：Windows 用 Qt5Core 内置并导出的版本（`<QtZlib/zlib.h>`），Linux 链接系统 `libz.so.1`（RHEL 7 自带）。
- 程序完全离线运行：不联网、不开监听端口、不调用外部进程。

### 代码约束

- 不直接调用 Win32 / POSIX 系统 API。确实需要时放在 `#ifdef` 里，两边都要实现。Windows 侧不能用 Win8+ 才有的 API（如 `GetSystemTimePreciseAsFileTime`、`SetThreadDescription`、`CreateFile2`、`AddDllDirectory`、`WaitOnAddress`），需要时用 `GetProcAddress` 动态获取并提供 Win7 回退。
- 路径一律用 `QDir` / `QFileInfo` / `QStandardPaths` / `QCoreApplication::applicationDirPath()` 拼接，不写死盘符、反斜杠或 `/home/...`。设置用 `QSettings`。
- Linux 文件名区分大小写：`#include` 和 `.pro` 里的文件名必须与磁盘上完全一致（含 Qt 头文件，如 `<QOpenGLWidget>`）。
- 读写文本统一 UTF-8；源文件 UTF-8、LF 换行，`.sh` 脚本必须是 LF。
- 二进制解析按小端显式读取（`memcpy`），不依赖结构体内存布局。
- 用到的 C 标准库函数要显式包含对应头文件（如 `memchr` → `<cstring>`），不要依赖 MinGW 的间接包含。
- 不在代码里用 `emit` 等 Qt 宏名当变量或参数名。
- 字体不写死单个字体名，用带备选的列表，如等宽 `Consolas, "DejaVu Sans Mono", "Liberation Mono", monospace`，界面 `"Microsoft YaHei", "WenQuanYi Micro Hei", "Noto Sans CJK SC", sans-serif`。RHEL 7.6 默认可能没有中文字体，中文字体需随程序部署（见下）。
- 快捷键用 `Qt::CTRL` 等跨平台写法；文件对话框用 Qt 的 `QFileDialog`。

### 时间基准（强制统一）

**三路数据的时间都是北京时间墙钟；内部统一换算成 Unix epoch 毫秒（UTC 数轴），全工程只保留这一种时间表示。**

| 数据源 | 原始时间 | 换算入口 | 规则 |
|---|---|---|---|
| 雷达航迹 MAT | `time` / `sysPara.datenum`（MATLAB datenum） | `datenumToMs`（`trackparsers.cpp`） | `(dn - 719529) * 86400 * 1000 - 8h` |
| 点迹 MAT（DetPoint） | `DetPoint/sysPara.datenum`（同为 datenum） | 同上 | 与航迹 `time` 同值，可直接逐帧对齐 |
| ADS-B CSV（批量历史位置） | 第 10 列文本时间 | `parseBeijingTime`（`track.cpp`） | 按 UTC+8 解析后 -8h |
| 显示 | — | `formatBeijingTime`（`track.cpp`） | +8h 还原北京时间 |

- 已实测：`detPoint_20251210175946.mat` 的 datenum `739961.74983796` = 北京时间 `2025-12-10 17:59:46`（与文件名一致），与 `track_20251210175946.mat` 的 `smoothPointList.time` 完全相等，frameID 221–270 逐帧对应。
- ADS-B 批量历史 CSV 的时间列实测是**本地时间（UTC+8）**，不是 UTC：按各机场地面点（alt≤100）统计，17 点（若为 UTC 即本地凌晨 1 点）仍有大量滑行/起降活动，符合本地白天模式。FR24 官方「单趟导出」是 UTC 口径，但本项目处理的批量文件是本地时间，不要混用。
- 修改任何导入/回放代码时不得引入第二套时间换算；新数据源必须接入上表同一套入口。
- **frameID 不能作为跨数据源的对齐键**：DetPoint 的 frameID 为整型，宽度由 MATLAB 按文件自动选（同一批数据里 uint8 与 uint16 都出现过，取决于该帧数值是否超过 255）；航迹的 frameID 是 int32 且每次运行编号不同（实测一次 221–270、另一次从 1527 起）。跨源对齐、排序一律用时间（datenum / epoch ms）。
- 导入后必须打印/显示数据时间范围（北京时间）；多源时间窗完全不重叠时要提示，用于兜住 8 小时级别的时区错误。

### 点迹三维可视化（point3d）

复刻 MATLAB `track_debuger_suite/tool_Track visualization`，对应文件 `point3dtypes.h` / `point3dparsers.*` / `point3dimportjob.*` / `point3dcanvas.*` / `point3dwindow.*` / `trackpicker.*`。

- 窗口可**多开**（每个窗口一组点迹），从主窗口「视图 → 新建点迹三维窗口」打开（Ctrl+Shift+P）。
- **布局对齐地图窗口：菜单 + 左侧「显示」侧栏 + 底部回放条**（用户确认过的方案，不要再把所有控件平铺成一条工具栏）：
  - 顶部细菜单：文件（导入点迹 Ctrl+O / 选择航迹 / 导出 PNG / 关闭 Ctrl+W）、视图（显示侧栏 / 三个三轴视图 / 重置视角）、设置（高级参数…）；
  - 左侧 `QScrollArea#point3dSide`（`SideScrollArea` 子类把 minimumSizeHint 固定得很小，否则内容最小宽度会顶到窗口最小宽度上）按分区放：视图、显示（点源 + P/T/L）、尺寸（点迹尺寸/航迹线宽/标签大小/提示字号）、色标 SNR、区域筛选；宽度 `ui::sz(300)`，可由「视图 → 显示侧栏」收起（`point3d.side_panel`）；
  - 底部状态栏两行：状态文字 + 导入进度；回放条（播放 / 帧进度 / 时间 / 倍速），与地图窗口的 `AppStatusBar` 同一视觉语言。
  - 自检断言：窗口最小宽度 < 900、左侧存在 `point3dSide` 滚动区、≥5 个 `QGroupBox`、存在菜单栏。
- **色标条紧贴立方体投影包围盒的右侧**（`colorBarRect()`：x = 立方体右沿 + 间距，竖向对齐立方体，右侧留刻度文字空间），不要再按绘图区右边距摆。
- **三方联动（地图 ↔ 点迹窗口 ↔ 数据库航迹）按「BatchNo + 时间范围」认人**，不是按 id 字符串：同一 MAT 分别按「雷达航迹」和「雷达原始测量」导入时 id 前缀不同（`RADAR-####` / `RAW-####`），但编号和时间一致，`point3dRefsMatch()` 据此认定同一条航迹。所有联动广播都带 `LinkedTrackRef{batchNo,t0,t1}`。
  - 地图**单击单独显示**某条航迹 → `MainWindow::applyDisplay` 把所有窗口 `setLinkedSelection({该航迹})`：窗口里有对应叠加航迹就只显示它们，**一条都对不上时窗口保持原样**（这是用户明确要求）；地图回到全部（refs 为空）才恢复显示全部叠加航迹。
  - 航迹管理系统**筛选/勾选**出的可见集合（`ManageState::visibleKeys`）同样按上面规则广播，多开的每个窗口都执行。
  - **悬停双向同时高亮**：地图悬停经 `GlobeWidget::trackHovered` → `MainWindow::onMapHoverTrack` → 各窗口 `setLinkedHighlight(batchNo,t0,t1)`；点迹窗口悬停经 `trackFocusChanged` → `trackIndexForBatch`（同样要求编号相同且时间有重叠）→ 地图 `setHoveredTrack`。
  - 画布侧用 `setIsolatedBatches(QSet<int>)`（空=全部）实现"只显示所选"，`m_tracks` 只装画出来的，悬停/双击自然只命中隔离后的航迹。
- **一致性校验**：叠加航迹的点只有在"离最近检测帧不超过最大帧间隔"时才会被采用（`appendOverlayPoints` 的 `tol`），时间窗完全对不上的航迹整条判为不可叠加（不会被硬贴到首/末帧画一条假航迹），窗口记录 `m_skippedTracks` 并在状态栏/选择航迹后弹窗提示。
- **点迹窗口的回放是窗口自带的，与主窗口时间轴完全独立**（用户明确要求，不要再接 `ReplayController`）：只回放本窗口的点迹/航迹，**不做时间插值**，而是按相邻帧的时间差逐帧切换（每帧停留 = `frames[i+1].timeMs - frames[i].timeMs` / 倍速，倍速存 `point3d.play_speed`，默认 100）。进度条绑定**帧序号**（有级），拖动只能落在某一帧上；拖拽时暂停、松手恢复；播到最后一帧自动停，再点播放从头开始。主窗口的 `resetReplayRange` 只统计地图航迹，不再并入点迹窗口。
- 航迹叠加**只从数据库挑选**（`TrackPicker`），不单独导入 track MAT。数据库点格式 v3 起每点 52 字节（t/lat/lon + 7 个 float），包含 `prange/pvr/paz`；**v4 起同一行还存关联点列 `raw_points`**（同一 MAT 的 `asscPointList`，`has_raw`/`raw_point_count` 标记）。`has_measure=0` 的旧航迹不能叠加，需重新导入雷达 MAT。
- 航迹点源（v4 起是"同一条航迹的两套点列"，不是两个批次）：**平滑**=主点列（`smoothPointList`/`outputPointList`）、**关联**=关联点列（`asscPointList`）。`RadarRaw` 批次（单独"导入雷达原始测量数据"产生）的主点列本身就是关联点列。判定统一走 `point3dOverlayPointsFor()` / `point3dOverlayHasColumn()`（`point3dtypes.h`），画布与窗口共用，不要再各写一套。
- **点源只用一个按钮切平滑/关联（在侧栏「显示」分组里）**（库里就这两套点列，不要下拉框）：`m_sourceBtn` 弹起=平滑、按下=关联，按钮文字同步显示当前点列；取值持久化到 `point3d.track_source`（`smooth`/`associated`，老值 `auto`/`predicted` 一律按平滑处理）。不允许出现"选了却看不见"：用户在「选择航迹…」里选中的航迹若当前点源取不到点列，窗口要自动切到另一列。
- **画布鼠标交互**：左键拖拽=旋转视角（空白处双击 `resetCamera()`），**右键拖拽=平移视图**（拖动中鼠标为闭合手爪；不要再做「平移」按钮/模式开关）。偏移以屏幕像素叠加在投影中心（`m_panX/m_panY`），随相机持久化（`point3d.camera_pan_x/y`、会话 JSON `camera.pan_x/pan_y`）。**左键单击某条航迹**（按下与松开位移 < 3px）= 本窗口只显示这一条并通知主窗口让地图也单独显示它（`trackClicked` → `trackActivated`）。
- **悬停命中优先级：航迹优先于点**。点云很密时若先判点，航迹悬停会被附近的检测点抢走，表现为"在窗口里悬停航迹、地图毫无反应"；`mouseMoveEvent` 必须先 `hitTrack()`，没命中航迹才找最近点。
- **色标（SNR→颜色）映射可调**：`setSnrRange(lo,hi)`，两个 NaN = 按本窗口数据自动取 min/max（默认）。侧栏「色标 SNR」分组（高级参数里也有一份，改完会同步）对应 `point3d.snr_auto` / `point3d.snr_min` / `point3d.snr_max`，同时影响点迹着色与右侧色标条刻度。悬停信息浮窗字号走 `setHoverTextScale()`（侧栏「尺寸 → 提示字号」，`point3d.hover_text_scale`）。
- **区域筛选（三行 = 当前视图的三个轴，在侧栏分组里）**：三行分别对应 X/Y/Z（X/Y 随视图模式变，Z 恒为帧号），**每个视图各存一套**（`point3d.region.rvf/raf/vaf`、`point3d.region_applied.*`），切换视图原样恢复。语义：
  - 未应用：归一化仍按全量，`setRegionBox()` 只画一个橙色候选框，数据不裁剪；
  - 「应用」`setRegionFilter()`：归一化范围改成该区间（框 = 绘图区边界，坐标轴刻度变成子区间，即"放大到只有这个框"），点迹与叠加航迹都按 `x/y/z` 裁到框内；区间先夹到数据量程，`min ≥ max` 直接拒绝；
  - 已应用后再改数字 → **自动退回全量视图**（`onRegionSpinEdited` 清掉 filter），只显示候选框，点「应用」才重新放大+裁剪；
  - 「重置」清筛选回全量；数据重新导入后 `revalidateRegions()` 按新量程夹紧/复位各视图区间；
  - 自检用 `setRegionForCurrentView()` / `regionForView()` / `regionAppliedForView()`。
- 雷达 MAT 导入时 `parseRadarMat(..., raw=false)` 会把 `asscPointList` 一并读进 `Track::rawPoints` 入库，所以同一份数据能直接切换平滑/关联，不用导两遍；老库（v3 及以前）重导同一 MAT 会**原地补列**（`DbSaveJob` 对已存在且缺 `has_measure`/`has_raw` 的行做 `INSERT OR REPLACE`，不算新增，不影响批次计数）。
- **`ReplayController::setRange` 不得随意重置当前时刻**（主窗口地图回放）：单独显示某条航迹、筛选、增删数据都会重算回放范围，范围变了但当前时刻仍在新范围内时必须原地保留（范围完全没变则直接返回，连回放状态都不打断）。历史上每次都跳回起点，表现为"在地图上点一条航迹，时间轴就跳回开头"。
- 航迹选择属于单个窗口：只随窗口会话（`point3d.windows`）持久化，不写全局默认键，否则新开窗口会莫名继承别的窗口选过的航迹。
- 画布绘制必须成对 `save()/restore()`：悬停提示框用了不透明画刷，泄漏到后面的 `drawColorBar()` 会把色标渐变整块盖掉（表现为"鼠标一悬停到点迹上，右侧色标条就消失"）。色标条任何状态下都要完整重画。
- 设置全部走 `point3d.*` 键；多窗口现场存 `point3d.windows`（JSON 数组），`point3d.auto_reload` 控制启动时是否恢复。
- 渲染只用 QPainter（不用 GL）：正交投影、turbo 色标、深度分桶、LOD 抽样；画布是固定深色背景，工具条/状态栏跟随主题。
- **坐标轴刻度**：数字字号跟随「标签大小」（`m_labelScale`）；刻度数量按"轴在屏幕上的实际长度 ÷ 文字占位"自适应（3–16 个：放大时更密，字号调大时自动变稀，不重叠）；刻度与轴标题由 `drawAxisTicks()` 在**数据之后**绘制——放在 `drawGridAndAxes()` 里会被密集点云盖住。
- 自检：`--pointtest <目录|文件> [--track <mat>]`（解析/连续性/画布/多窗口/独立逐帧回放/联动匹配/区域筛选/窗口缩放与设置栏/会话恢复）与 `--dbtest`（v2→v4 迁移与重导补列）都必须 `RESULT OK`。端到端冒烟（走主窗口菜单外的真实创建/导入路径并截图）：`--testmode --pointdemo "<detPoint 目录>" --point-shot <png>`。

### 主窗口数据源条（SourceStrip）

- 数据源 chip 行整体重建（`SourceStrip::setItems`）时必须**保住横向滚动位置**：切换某个数据源的显示/隐藏会走重建，旧实现无条件 `setValue(0)`，用户滚到中间/末尾后一开关显示就跳回开头。重建前后读取/恢复 `horizontalScrollBar()->value()`（按新的 maximum 夹一下）。

### OpenGL 约束

Win7 老驱动会回退到 ANGLE（D3D11/D3D9）或软件渲染（opengl32sw）；RHEL 涉密机常见 Mesa llvmpipe 软件渲染、VNC/远程桌面或国产显卡。所以只用 **OpenGL ES 2.0 / GLSL ES 1.00 子集**：

- 着色器用 `attribute`/`varying`/`texture2D`，不写 `#version`；片元着色器带 `#ifdef GL_ES precision mediump float; #endif`。
- 不用 VAO、实例化、GL3+ 功能、`glTexStorage`、`GL_TEXTURE_MAX_LEVEL` 等 ES2 没有的参数；不用 `QOpenGLTexture`（它会设置 ES2 不支持的参数，在 ANGLE 下产生 GL 错误），纹理用 `glTexImage2D(GL_RGBA, GL_UNSIGNED_BYTE)` 手动上传。
- 索引用 `GLushort`（单次绘制不超过 65536 个顶点）；只对 2 的幂尺寸纹理生成 mipmap。
- 顶点和片元着色器共用的 uniform 在 ANGLE 下精度会不一致导致链接失败：片元要用的值经 varying 传过去。
- 宽线不能用 `glLineWidth`（ANGLE 和很多 Linux 核心模式驱动只支持 1px），在顶点着色器里按屏幕像素展开成四边形（见 `tracklayer.cpp`）。
- 只通过 `QOpenGLFunctions` 调用 GL 函数。

### 部署

- **Windows**：`build_win7.sh` 部署到 `build/win7/release`。新增 Qt 模块或插件时，同步在部署段里拷贝对应 dll。
- **Linux**：`build_linux.sh` 部署到 `build/linux/HelloVscode/`，整包带走，不依赖目标机上的 Qt：
  - `lib/`：Qt `.so`（含 `libicu*`）；
  - `plugins/`：`platforms/libqxcb.so`、`xcbglintegrations/`、`sqldrivers/libqsqlite.so`、`imageformats/`；
  - `fonts/`：中文字体，启动时用 `QFontDatabase::addApplicationFont` 加载；
  - `HelloVscode.sh`：启动脚本，设置 `LD_LIBRARY_PATH` / `QT_PLUGIN_PATH`；exe 同时带 `$ORIGIN/lib` 的 rpath。
  - 目标机需有系统库：glibc 2.17、libstdc++、libGL（Mesa）、libX11/libxcb、fontconfig、freetype、zlib（RHEL 7 默认图形安装都有）。

### 每次改动后的验证

1. `bash build_win7.sh`：用 7.3 重编、部署到 `build/win7/release` 并跑 `--selftest`，必须 `RESULT OK`。
2. 在 `build/win7/release` 下分别用默认、`QT_OPENGL=angle`、`QT_OPENGL=software`、`QT_OPENGL=angle QT_ANGLE_PLATFORM=d3d9` 跑 `./HelloVscode.exe --selftest`，都要 `RESULT OK` 且 GL 错误数为 0。
3. 航迹/回放自检（同样四种渲染模式）：`./HelloVscode.exe --tracktest "D:/Desktop/RadarView_BiuldByTauri/2026-04-27 09-30-00.csv" "D:/Desktop/RadarView_BiuldByTauri/track_20251210175946.mat"`，看 `tracktest.log`。纯解析自检用 `--parsetest <csv> <mat>`，看 `parsetest.log`。
4. 点迹可视化自检：`./HelloVscode.exe --pointtest "<detPoint 目录或文件>" --track "<track_*.mat>"` 与 `./HelloVscode.exe --dbtest`（数据库 v2→v4 迁移与重导补列），均须 `RESULT OK`，分别看 `pointtest.log` / `dbtest.log`。
5. `powershell -NoProfile -ExecutionPolicy Bypass -File check_win7_compat.ps1`：检查所有 exe/dll 的导入表，必须全部 `[OK]`。
6. Linux（有 CentOS/RHEL 7 环境时）：`bash build_linux.sh`，会编译、部署、跑 `--selftest`，并检查产物不依赖高于 GLIBC_2.17 / GLIBCXX_3.4.19 的符号。软件渲染用 `LIBGL_ALWAYS_SOFTWARE=1` 再跑一遍。
7. 最终确认需在 Win7 SP1 x64 虚拟机和 CentOS/RHEL 7.6 虚拟机里分别运行整包。

### Git 提交与推送（已获用户长期授权）

- 每次修改代码并通过上面的验证后，自动提交并推送，不用再询问：`git add <具体文件>` → `git commit` → `git push origin main`。
- 验证不通过不提交；先修好再提交。一次提交只包含一个完整的改动。
- 提交信息用中文，格式 `type: 描述`，type 取 feat / fix / refactor / docs / build / chore；正文写清改了什么、怎么验证的。
- 只暂存具体文件，不用 `git add .` / `git add -A`。
- 不提交构建产物和数据：`build/`、exe/dll/so、`*.mbtiles`、`*.csv`、`*.mat`、日志（`.gitignore` 已覆盖，提交前用 `git status` 确认）。单个文件不得超过 50 MB（GitHub 上限 100 MB）。
- 不提交密钥、账号或任何涉密数据。
- 禁止 `push --force`、改写已推送的历史、`--no-verify`。推送失败时报告原因并停下，不要强推。
- 远端：`origin` = `git@github.com:rendong9316/RadarView_CPP.git`，分支 `main`。

### 环境注意事项

- Git Bash 里 PATH 必须写 `/d/...` 形式，`D:/...` 中的冒号会把 PATH 拆坏。
- 重编前先 `taskkill //IM HelloVscode.exe //F`，否则 exe 被占用无法覆盖。
- `check_win7_compat.ps1` 要保存为 UTF-8 带 BOM，否则 Windows PowerShell 5 会按 GBK 读坏中文。
- 瓦片来自 `D:/Desktop/RadarView_BiuldByTauri/src-tauri`，两个构建脚本都会拷到 `tiles/`。
- 这台 Win11 上目前没有可用的 Linux 环境（Docker 引擎未运行、没有 WSL 发行版），Linux 构建只能在 CentOS 7 虚拟机或目标机上做。
