# 双平台兼容性与 OpenGL 约束

项目所有改动必须同时满足 **Windows 7 SP1 x64** 与 **Red Hat Enterprise Linux 7.6 x86_64** 离线整包运行要求。

## 1. 工具链要求

| 项目 | Windows 7 | RHEL 7.6 |
|---|---|---|
| Qt | 5.14.2 `mingw73_64` | 5.14.2 `gcc_64` |
| 编译器 | 干净的 MinGW 7.3.0 (`x86_64-posix-seh-rt_v5-rev0`) | `devtoolset-7` 及以上（禁止系统 GCC 4.8.5） |
| 语言标准 | `CONFIG += c++11` | `CONFIG += c++11` |
| ABI 上限 | 禁止静态导入 Win8+ API / API Set DLL | `GLIBC <= 2.17`、`GLIBCXX <= 3.4.19`、`CXXABI <= 1.3.7` |

## 2. 依赖边界

- **允许的 Qt 模块**：`QtCore`、`QtGui`、`QtWidgets`、`QtSql`（`qsqlite`）、`QtNetwork`（仅用于可选在线 HTTP 底图，默认离线运行、不监听端口）。
- **禁止引入**：`QtWebEngine`、`QtMultimedia`、`ActiveQt`、`QtQuick` 及外部第三方库。
- **zlib**：Windows 使用 Qt5Core 内置导出的 `<QtZlib/zlib.h>`；Linux 链接系统 `libz.so.1`。

## 3. 跨平台代码约束

- 平台差异严格收敛在 `#ifdef Q_OS_WIN` / `Q_OS_LINUX` 内，避免直接调用系统 API；Win8+ API 若必须使用，须通过 `GetProcAddress` 动态探测并提供 Win7 回退。
- 路径一律通过 `QDir` / `QFileInfo` / `QStandardPaths` / `QCoreApplication::applicationDirPath()` 拼接。
- Linux 文件系统区分大小写，所有 `#include` 与 `.pro` 路径必须与磁盘精确一致。
- 文本读写统一 UTF-8；源码与 `.sh` 为 LF，`.ps1` 为 UTF-8 带 BOM + CRLF。
- 二进制解析显式按小端 `memcpy`，不依赖结构体对齐；C 标准库函数显式包含对应头文件。

## 4. OpenGL ES 2.0 渲染子集

为兼容 Win7 老驱动（ANGLE D3D11/D3D9、opengl32sw）与 RHEL 7 软件渲染（Mesa llvmpipe）：

- 着色器仅用 `attribute` / `varying` / `texture2D`，不写 `#version`；片元着色器带 `#ifdef GL_ES precision mediump float; #endif`。
- 禁用 VAO、实例化、`glTexStorage`、`GL_TEXTURE_MAX_LEVEL` 和 `QOpenGLTexture`；纹理手动用 `glTexImage2D(GL_RGBA, GL_UNSIGNED_BYTE)` 上传。
- 索引统一用 `GLushort`（单批次顶点数 ≤ 65536），仅对 2 的幂纹理生成 mipmap。
- 顶点与片元共用数据通过 `varying` 传递（避免 ANGLE 精度声明冲突导致链接失败）。
- 宽线在顶点着色器按屏幕像素展开成四边形，不依赖 `glLineWidth`。
- 所有 GL 调用经 `QOpenGLFunctions` 转发。
