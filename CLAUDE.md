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
4. `powershell -NoProfile -ExecutionPolicy Bypass -File check_win7_compat.ps1`：检查所有 exe/dll 的导入表，必须全部 `[OK]`。
5. Linux（有 CentOS/RHEL 7 环境时）：`bash build_linux.sh`，会编译、部署、跑 `--selftest`，并检查产物不依赖高于 GLIBC_2.17 / GLIBCXX_3.4.19 的符号。软件渲染用 `LIBGL_ALWAYS_SOFTWARE=1` 再跑一遍。
6. 最终确认需在 Win7 SP1 x64 虚拟机和 CentOS/RHEL 7.6 虚拟机里分别运行整包。

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
