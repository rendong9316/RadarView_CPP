#!/bin/bash
# 一键重建 Win7 兼容版并自检
# 用法: cd D:/software/Qt/Qt5.14.2/HelloVscode && bash build_win7.sh
# 产物: build/win7/release/（整包拷到 Win7 64位机器即可运行）
set -e
cd "$(dirname "$0")"
SRC="$(pwd)"

# 干净的 Qt 官方 MinGW 7.3.0（x86_64-posix-seh-rt_v5-rev0），运行库不依赖 Win8+ API
# 注意：Qt 自带的 Tools/mingw730_64 已被 gcc16 污染，不要用
MINGW="/d/software/mingw730_clean/Tools/mingw730_64/bin"
# Qt 5.14.2 本体（moc/uic/Qt 库/平台插件）
QTPATH="/d/software/Qt/Qt5.14.2/5.14.2/mingw73_64"
QTBIN="$QTPATH/bin"
TILE_SRC="/d/Desktop/RadarView_BiuldByTauri/src-tauri"

# PATH 必须用 /d/... 形式；D:/... 里的冒号会把 PATH 切碎
export PATH="$MINGW:$QTBIN:/usr/bin:/bin"

taskkill //IM HelloVscode.exe //F >/dev/null 2>&1 || true

BUILD="build/win7"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# 1) qmake（全路径调用 Qt5 的 qmake，.pro 用绝对路径）
"$QTBIN/qmake.exe" "$SRC/HelloVscode.pro" -spec win32-g++ -o Makefile

# 2) 编译
"$MINGW/mingw32-make.exe" -j8

# 3) 部署
cd release
rm -f *.o moc_*.cpp moc_predefs.h
rm -rf platforms sqldrivers imageformats tiles
cp "$QTBIN/Qt5Core.dll" "$QTBIN/Qt5Gui.dll" "$QTBIN/Qt5Widgets.dll" "$QTBIN/Qt5Sql.dll" .
# OpenGL 回退：显卡驱动不支持 GL2 时走 ANGLE(D3D) 或软件渲染
cp "$QTBIN/libEGL.dll" "$QTBIN/libGLESv2.dll" "$QTBIN/d3dcompiler_47.dll" "$QTBIN/opengl32sw.dll" .
# MinGW 7.3 运行库
cp "$MINGW/libgcc_s_seh-1.dll" "$MINGW/libstdc++-6.dll" "$MINGW/libwinpthread-1.dll" .
mkdir platforms sqldrivers imageformats tiles
cp "$QTPATH/plugins/platforms/qwindows.dll" platforms/
cp "$QTPATH/plugins/sqldrivers/qsqlite.dll" sqldrivers/
cp "$QTPATH/plugins/imageformats/qjpeg.dll" "$QTPATH/plugins/imageformats/qgif.dll" \
   "$QTPATH/plugins/imageformats/qico.dll" imageformats/

# 4) 瓦片（默认加载层级最高的那个）
[ -f "$TILE_SRC/target/release/GRAY_HR_SR6.mbtiles" ] && cp "$TILE_SRC/target/release/GRAY_HR_SR6.mbtiles" tiles/
[ -f "$TILE_SRC/natural_earth4.mbtiles" ] && cp "$TILE_SRC/natural_earth4.mbtiles" tiles/

# 5) 自检（写 selftest.log）
rm -f selftest.log
set +e
./HelloVscode.exe --selftest
RC=$?
set -e
echo ""
echo "===== selftest.log ====="
cat selftest.log
echo ""
echo "产物目录: $BUILD/release（整个文件夹拷到 Win7 64 位机器即可双击运行）"
exit $RC
