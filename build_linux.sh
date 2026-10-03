#!/bin/bash
# 在 CentOS 7 / RHEL 7.x x86_64 上编译、整包部署并自检
# 用法: cd HelloVscode && bash build_linux.sh
# 可选环境变量:
#   QTDIR     Qt 5.14.2 gcc_64 目录（默认 ~/Qt5.14.2/5.14.2/gcc_64）
#   TILE_SRC  含 .mbtiles 的目录（默认 ./tiles）
#   FONT_SRC  中文字体文件（默认依次找文泉驿微米黑 / Noto CJK）
# 产物: build/linux/HelloVscode/（整个目录拷到目标机，运行 ./HelloVscode.sh）
set -e
cd "$(dirname "$0")"
SRC="$(pwd)"

QTDIR="${QTDIR:-$HOME/Qt5.14.2/5.14.2/gcc_64}"
TILE_SRC="${TILE_SRC:-$SRC/tiles}"

if [ ! -x "$QTDIR/bin/qmake" ]; then
    echo "找不到 Qt 5.14.2 gcc_64：$QTDIR（用 QTDIR=... 指定）" >&2
    exit 2
fi

# RHEL 7 自带 GCC 4.8.5 不在 Qt 5.14 支持列表内，用 devtoolset（与 Windows 端 MinGW 7.3 对齐）
if [ -z "$NO_DEVTOOLSET" ]; then
    for v in 7 8 9; do
        if [ -f "/opt/rh/devtoolset-$v/enable" ]; then
            # shellcheck disable=SC1090
            . "/opt/rh/devtoolset-$v/enable"
            break
        fi
    done
fi
echo "编译器: $(g++ --version | head -1)"

BUILD="$SRC/build/linux"
OUT="$BUILD/HelloVscode"
rm -rf "$BUILD"
mkdir -p "$BUILD/obj" "$OUT"

# 1) 编译
cd "$BUILD/obj"
"$QTDIR/bin/qmake" "$SRC/HelloVscode.pro" -spec linux-g++ CONFIG+=release -o Makefile
make -j"$(nproc)"
cp HelloVscode "$OUT/"
cd "$OUT"

# 2) Qt 运行库（含 ICU、xcb 平台抽象层）
mkdir -p lib plugins/platforms plugins/xcbglintegrations plugins/sqldrivers plugins/imageformats fonts tiles
for m in Core Gui Widgets Sql DBus XcbQpa; do
    cp -P "$QTDIR"/lib/libQt5$m.so.5* lib/
done
cp -P "$QTDIR"/lib/libicu*.so.* lib/ 2>/dev/null || true

# 3) 插件 + qt.conf（让 Qt 在 exe 旁的 plugins/ 里找插件）
cp "$QTDIR/plugins/platforms/libqxcb.so" plugins/platforms/
cp "$QTDIR"/plugins/xcbglintegrations/*.so plugins/xcbglintegrations/
cp "$QTDIR/plugins/sqldrivers/libqsqlite.so" plugins/sqldrivers/
for f in libqjpeg.so libqgif.so libqico.so; do
    [ -f "$QTDIR/plugins/imageformats/$f" ] && cp "$QTDIR/plugins/imageformats/$f" plugins/imageformats/
done
printf '[Paths]\nPrefix=.\nPlugins=plugins\n' > qt.conf

# 4) 中文字体（RHEL 7 最小/涉密环境可能没有）
if [ -z "$FONT_SRC" ]; then
    for f in /usr/share/fonts/wqy-microhei/wqy-microhei.ttc \
             /usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc \
             /usr/share/fonts/google-noto-cjk/NotoSansCJKsc-Regular.otf; do
        if [ -f "$f" ]; then FONT_SRC="$f"; break; fi
    done
fi
if [ -n "$FONT_SRC" ] && [ -f "$FONT_SRC" ]; then
    cp "$FONT_SRC" fonts/
else
    echo "警告：未找到中文字体，目标机若没有中文字体，界面中文会显示为方块（用 FONT_SRC=... 指定）" >&2
fi

# 5) 瓦片
cp "$TILE_SRC"/*.mbtiles tiles/ 2>/dev/null || echo "警告：$TILE_SRC 下没有 .mbtiles" >&2

# 6) 启动脚本：库路径兜底（rpath 已指向 lib/），并允许强制软件渲染
cat > HelloVscode.sh <<'EOF'
#!/bin/bash
# 用法: ./HelloVscode.sh            正常启动
#       SOFTWARE_GL=1 ./HelloVscode.sh   强制 Mesa 软件渲染（无显卡驱动 / 远程桌面时）
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
[ -n "$SOFTWARE_GL" ] && export LIBGL_ALWAYS_SOFTWARE=1
exec "$HERE/HelloVscode" "$@"
EOF
chmod +x HelloVscode.sh HelloVscode

# 7) ABI 检查：不能依赖高于 RHEL 7 的 glibc / libstdc++ 符号版本
echo ""
echo "===== ABI 检查（RHEL 7: GLIBC <= 2.17, GLIBCXX <= 3.4.19, CXXABI <= 1.3.7）====="
ABI_FAIL=0
for f in HelloVscode lib/*.so* plugins/*/*.so; do
    [ -L "$f" ] && continue
    bad=$(objdump -T "$f" 2>/dev/null | grep -oE '(GLIBC_2\.[0-9]+|GLIBCXX_3\.4\.[0-9]+|CXXABI_1\.3\.[0-9]+)' | sort -u | \
        awk -F'[_.]' '
            /^GLIBC_/   { if ($3 > 17) print }
            /^GLIBCXX_/ { if ($4 > 19) print }
            /^CXXABI_/  { if ($4 > 7)  print }')
    if [ -n "$bad" ]; then
        echo "  [FAIL] $f: $(echo $bad)"
        ABI_FAIL=1
    fi
done
[ $ABI_FAIL -eq 0 ] && echo "  [OK] 所有文件均兼容 RHEL 7"
echo "缺失的系统库（应为空）:"
LD_LIBRARY_PATH="$OUT/lib" ldd HelloVscode plugins/platforms/libqxcb.so | grep "not found" || echo "  无"

# 8) 自检（需要图形环境；无显示时用 xvfb-run）
echo ""
RUN=""
if [ -z "$DISPLAY" ]; then
    if command -v xvfb-run >/dev/null 2>&1; then
        RUN="xvfb-run -a -s '-screen 0 1280x800x24'"
    else
        echo "没有 DISPLAY 也没有 xvfb-run，跳过运行自检"
        exit $ABI_FAIL
    fi
fi
RC=0
for mode in "" "1"; do
    rm -f selftest.log
    if [ -n "$mode" ]; then label="软件渲染"; else label="默认渲染"; fi
    set +e
    eval SOFTWARE_GL="$mode" $RUN ./HelloVscode.sh --selftest
    r=$?
    set -e
    echo "===== selftest（$label）exit=$r ====="
    cat selftest.log 2>/dev/null || echo "（没有生成 selftest.log）"
    [ $r -ne 0 ] && RC=$r
done
echo ""
echo "产物目录: $OUT（整个目录拷到目标机，运行 ./HelloVscode.sh）"
[ $ABI_FAIL -ne 0 ] && exit 3
exit $RC
