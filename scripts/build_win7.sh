#!/bin/bash
# 一键重建 Win7 兼容版并自检
# 用法: bash scripts/build_win7.sh
# 可选环境变量（默认自动探测 /d/software 与 /h/software）：
#   MINGW     干净的 MinGW 7.3.0 bin 目录
#   QTPATH    Qt 5.14.2 mingw73_64 目录（也兼容 QTDIR）
#   TILE_SRC  含 .mbtiles 的目录
# 产物: build/win7/release/（整包拷到 Win7 64位机器即可运行）
set -e
SRC="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SRC"

# 自动探测干净的 Qt 官方 MinGW 7.3.0（x86_64-posix-seh-rt_v5-rev0），运行库不依赖 Win8+ API
# 注意：Qt 自带的 Tools/mingw730_64 可能已被 MSYS2 gcc16 污染，不要用
if [ -z "$MINGW" ]; then
    for cand in \
        "/d/software/mingw730_clean/Tools/mingw730_64/bin" \
        "/h/software/mingw730_clean/Tools/mingw730_64/bin"; do
        if [ -x "$cand/g++.exe" ]; then
            MINGW="$cand"
            break
        fi
    done
fi

# Qt 5.14.2 本体（moc/uic/Qt 库/平台插件）
QTPATH="${QTPATH:-$QTDIR}"
if [ -z "$QTPATH" ]; then
    for cand in \
        "/d/software/Qt/Qt5.14.2/5.14.2/mingw73_64" \
        "/h/software/Qt/Qt5.14.2/5.14.2/mingw73_64"; do
        if [ -x "$cand/bin/qmake.exe" ]; then
            QTPATH="$cand"
            break
        fi
    done
fi

if [ -z "$MINGW" ] || [ ! -x "$MINGW/g++.exe" ]; then
    echo "找不到干净的 MinGW 7.3.0：请通过 MINGW=/path/to/mingw730_64/bin 指定" >&2
    exit 2
fi
if [ -z "$QTPATH" ] || [ ! -x "$QTPATH/bin/qmake.exe" ]; then
    echo "找不到 Qt 5.14.2 mingw73_64：请通过 QTPATH=/path/to/5.14.2/mingw73_64 指定" >&2
    exit 2
fi

QTBIN="$QTPATH/bin"
if [ -z "$TILE_SRC" ]; then
    for cand in \
        "$SRC/tiles" \
        "/d/Desktop/RadarView_BiuldByTauri/src-tauri" \
        "/h/Desktop/RadarView_BiuldByTauri/src-tauri"; do
        if [ -d "$cand" ]; then
            TILE_SRC="$cand"
            break
        fi
    done
fi

echo "MINGW:  $MINGW"
echo "QTPATH: $QTPATH"
echo "TILE:   ${TILE_SRC:-<未找到，跳过拷贝>}"

# PATH 必须用 /d/... 或 /h/... 形式；盘符中的冒号会把 PATH 切碎
export PATH="$MINGW:$QTBIN:/usr/bin:/bin"

taskkill //IM RadarView.exe //F >/dev/null 2>&1 || true

BUILD="build/win7"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# 1) qmake（全路径调用 Qt5 的 qmake，.pro 用绝对路径）
"$QTBIN/qmake.exe" "$SRC/RadarView.pro" -spec win32-g++ -o Makefile

# 2) 编译
"$MINGW/mingw32-make.exe" -j8

# 3) 部署
cd release
rm -f *.o moc_*.cpp moc_predefs.h
rm -rf platforms sqldrivers imageformats tiles
cp "$QTBIN/Qt5Core.dll" "$QTBIN/Qt5Gui.dll" "$QTBIN/Qt5Widgets.dll" "$QTBIN/Qt5Sql.dll" "$QTBIN/Qt5Network.dll" .
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
if [ -n "$TILE_SRC" ]; then
    [ -f "$TILE_SRC/target/release/GRAY_HR_SR6.mbtiles" ] && cp "$TILE_SRC/target/release/GRAY_HR_SR6.mbtiles" tiles/
    [ -f "$TILE_SRC/natural_earth4.mbtiles" ] && cp "$TILE_SRC/natural_earth4.mbtiles" tiles/
    for f in "$TILE_SRC"/*.mbtiles; do
        [ -f "$f" ] && cp -n "$f" tiles/
    done
fi
# offline amap z1-z16 coastal (tools/download_tiles.py output)
[ -f "$SRC/tools/amap_coast_z1_z16.mbtiles" ] && cp "$SRC/tools/amap_coast_z1_z16.mbtiles" tiles/

# 5) 自检（写 selftest.log）
rm -f selftest.log
set +e
./RadarView.exe --selftest
RC=$?
set -e
echo ""
echo "===== selftest.log ====="
cat selftest.log
echo ""
echo "产物目录: $BUILD/release（整个文件夹拷到 Win7 64 位机器即可双击运行）"
exit $RC
