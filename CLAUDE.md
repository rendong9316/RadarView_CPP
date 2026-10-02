# HelloVscode

VSCode 风格的 Qt 外壳 + 自研 3D 地球（WGS84 椭球，加载本地 mbtiles 瓦片）。

## 首要目标：兼容 Windows 7

以后所有改动都必须以「能在 Windows 7 SP1 64 位上运行」为前提。新功能、重构、依赖升级前先确认不破坏 Win7 兼容性；做不到时先说明原因再动手。

### 工具链（不要换）

- Qt 5.14.2 MinGW 64 位：`D:/software/Qt/Qt5.14.2/5.14.2/mingw73_64`。Qt 6 不支持 Win7，不要升级。
- 编译器只用干净的 MinGW 7.3.0：`D:/software/mingw730_clean/Tools/mingw730_64/bin`。
- 不要用 `D:/software/Qt/Qt5.14.2/Tools/mingw730_64`，它已被 MSYS2 gcc 16 污染（运行库依赖 Win8+ API，且带一个 Qt6 的 qmake）。
- qmake 用全路径调用：`/d/software/Qt/Qt5.14.2/5.14.2/mingw73_64/bin/qmake.exe`。
- 语言标准保持 `CONFIG += c++11`。

### 代码约束

- 不直接调用 Win8+ 才有的 Win32 API（如 `GetSystemTimePreciseAsFileTime`、`SetThreadDescription`、`CreateFile2`、`AddDllDirectory`、`WaitOnAddress`）。确实需要时用 `GetProcAddress` 动态获取并提供 Win7 回退。
- OpenGL 只用 OpenGL ES 2.0 / GLSL ES 1.00 子集。Win7 老驱动会回退到 ANGLE（D3D11/D3D9）或软件渲染（opengl32sw）：
  - 着色器用 `attribute`/`varying`/`texture2D`，片元着色器带 `#ifdef GL_ES precision mediump float; #endif`。
  - 不用 VAO、GL3+ 功能、`glTexStorage`、`GL_TEXTURE_MAX_LEVEL` 等 ES2 没有的参数；不用 `QOpenGLTexture`（它会设置 ES2 不支持的参数，在 ANGLE 下产生 GL 错误），纹理用 `glTexImage2D(GL_RGBA, GL_UNSIGNED_BYTE)` 手动上传。
  - 索引用 `GLushort`；只对 2 的幂尺寸纹理生成 mipmap。
- 新增 Qt 模块或插件时，同步在 `build_win7.sh` 的部署段里拷贝对应 dll。
- 代码保持可移植（Windows 专有代码放在 `#ifdef Q_OS_WIN` 里），后续还要在 RHEL 7.6 上编译。

### 每次改动后的验证

1. `bash build_win7.sh`：用 7.3 重编、部署到 `build/win7/release` 并跑 `--selftest`，必须 `RESULT OK`。
2. 在 `build/win7/release` 下分别用 `QT_OPENGL=angle`、`QT_OPENGL=software`、`QT_OPENGL=angle QT_ANGLE_PLATFORM=d3d9` 跑 `./HelloVscode.exe --selftest`，都要 `RESULT OK` 且 GL 错误数为 0。
3. `powershell -NoProfile -ExecutionPolicy Bypass -File check_win7_compat.ps1`：检查所有 exe/dll 的导入表，必须全部 `[OK]`。
4. 最终确认需在 Win7 SP1 x64 虚拟机里双击运行整个 `build/win7/release` 目录。

### 环境注意事项

- Git Bash 里 PATH 必须写 `/d/...` 形式，`D:/...` 中的冒号会把 PATH 拆坏。
- 重编前先 `taskkill //IM HelloVscode.exe //F`，否则 exe 被占用无法覆盖。
- `check_win7_compat.ps1` 要保存为 UTF-8 带 BOM，否则 Windows PowerShell 5 会按 GBK 读坏中文。
- 瓦片来自 `D:/Desktop/RadarView_BiuldByTauri/src-tauri`，`build_win7.sh` 会自动拷到 `tiles/`。
