# RadarView_CPP

基于 **C++11 + Qt 5.14.2** 的离线雷达 / ADS-B 航迹与点迹三维可视化桌面客户端，采用 VSCode 风格界面外壳与自研 WGS84 3D 地球渲染，面向 **Windows 7 SP1 x64** 与 **Red Hat Enterprise Linux 7.6 x86_64** 双平台整包离线部署。

## 核心特性

- **自研 3D 地球渲染**：基于 OpenGL ES 2.0 子集（兼容 Win7 ANGLE / 软件渲染及 RHEL 7 Mesa llvmpipe），支持本地 `.mbtiles` 离线瓦片（WGS84 / GCJ-02 自动纠偏）与可选在线 HTTP 底图（天地图 / 高德）。
- **多源航迹导入与持久化**：内置只读 MATLAB `.mat` v5 解析器与 ADS-B 批量 CSV 解析器；SQLite 本地入库、多维检索、分页管理、软删除撤销与批次管理。
- **时间轴回放与地图标绘**：多源统一 UTC 毫秒时间基准（北京时间输入/显示）、空间套索筛选、球面贴地绘图（Vincenty 测地圆/大圆加密/GeoJSON 导出/撤销栈）、旗标与航线标尺。
- **点迹三维可视化（point3d）**：可多开的独立三维点云窗口（纯 `QPainter` 正交投影、SNR 色标、区域筛选、独立逐帧回放），与主地图和数据库按 `BatchNo + 时间窗` 三方双向联动。

## 目录结构

```text
RadarView_CPP/
├── RadarView.pro            # Qt qmake 工程文件
├── README.md                # 项目总览与构建指南
├── CLAUDE.md                # 开发约束与 AI 协作规范
├── docs/                    # 架构、双平台兼容性与数据格式文档
├── scripts/                 # Win7 / RHEL 7.6 构建、打包与导入表检查脚本
├── src/
│   ├── app/                 # 启动入口、主窗口壳层、路径、主题与通用控件
│   ├── core/                # 地理算法、坐标系、航迹数据模型与回放控制
│   ├── io/                  # MAT v5 / CSV 解析、异步导入与 SQLite 持久化
│   ├── globe/               # 3D 地球渲染、航迹图层、瓦片源与地图标绘工具
│   ├── ui/                  # 状态栏、筛选/管理/设置面板与对话框
│   ├── point3d/             # 点迹三维可视化独立子系统
│   └── selftest/            # 内置离线自检套件（--selftest / --dbtest 等）
└── tools/                   # Python 离线高德瓦片制作与维护脚本
```

## 环境要求

| 平台 | Qt 版本 | 编译器 | 构建脚本 |
|---|---|---|---|
| Windows 7 SP1 x64 | Qt 5.14.2 (`mingw73_64`) | 干净的 MinGW 7.3.0 (`x86_64-posix-seh`) | [scripts/build_win7.sh](scripts/build_win7.sh) |
| RHEL / CentOS 7.6 x64 | Qt 5.14.2 (`gcc_64`) | devtoolset-7+ (GCC 7.3+) | [scripts/build_linux.sh](scripts/build_linux.sh) |

> 不要升级至 Qt 6，也不要使用被 MSYS2 GCC 16 污染的 MinGW 工具链（会导致 Win7 缺失 Win8+ API 或 RHEL 7 超过 `GLIBC_2.17`）。详见 [docs/platform-compat.md](docs/platform-compat.md)。

## 构建与验证

### Windows 7 兼容包（Git Bash）

```bash
# 默认自动探测 /d/software 与 /h/software，也可显式指定环境变量
MINGW=/h/software/mingw730_clean/Tools/mingw730_64/bin \
QTPATH=/h/software/Qt/Qt5.14.2/5.14.2/mingw73_64 \
bash scripts/build_win7.sh

# 检查产物目录中所有 exe/dll 导入表是否兼容 Win7
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check_win7_compat.ps1
```

### RHEL / CentOS 7.6 整包

```bash
QTDIR=~/Qt5.14.2/5.14.2/gcc_64 bash scripts/build_linux.sh
```

### 内置自检模式

构建产物默认包含内置自检套件，支持在目标机直接执行以下命令行自检（正式发布如需裁掉自检代码，可在 `qmake` 时传入 `CONFIG+=no_selftest`）：

```bash
./RadarView.exe --selftest                                    # 3D 地球与 UI 自检（selftest.log）
./RadarView.exe --dbtest                                      # SQLite 迁移与批次管理自检（dbtest.log）
./RadarView.exe --statecheck                                  # 状态持久化与旧目录迁移自检（statecheck.log）
./RadarView.exe --parsetest <adsb.csv> <radar.mat>            # 纯解析自检（parsetest.log）
./RadarView.exe --tracktest <adsb.csv> <radar.mat>            # 航迹与回放自检（tracktest.log）
./RadarView.exe --pointtest <detPoint目录> --track <track.mat> # 点迹三维自检（pointtest.log）
```

## 文档索引

- [架构与模块说明](docs/architecture.md)
- [双平台兼容性与 OpenGL 约束](docs/platform-compat.md)
- [数据格式、时间基准与数据库 Schema](docs/data-formats.md)
- [离线瓦片制作工具](tools/README.md)
- [开发规范与 AI 协作指南](CLAUDE.md)
