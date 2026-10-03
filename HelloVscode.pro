QT       += core gui widgets sql

TARGET = HelloVscode
TEMPLATE = app

# ---- 兼容 Windows 7 ----
# Qt 5.14 默认 C++11，安全；不启用更严的 C++14/17，避免老 MinGW 头文件冲突
CONFIG += c++11

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    globewidget.cpp \
    tilesource.cpp \
    track.cpp \
    matfile.cpp \
    trackparsers.cpp \
    tracklayer.cpp \
    trackimporter.cpp \
    replaycontroller.cpp \
    appstatusbar.cpp \
    trackpointdialog.cpp \
    theme.cpp \
    lucide.cpp \
    geocalc.cpp \
    apppaths.cpp \
    trackdb.cpp \
    maptools.cpp \
    uiwidgets.cpp \
    managepanel.cpp \
    sidepanels.cpp

HEADERS += \
    mainwindow.h \
    globewidget.h \
    tilesource.h \
    track.h \
    matfile.h \
    trackparsers.h \
    tracklayer.h \
    trackimporter.h \
    replaycontroller.h \
    appstatusbar.h \
    trackpointdialog.h \
    geo.h \
    theme.h \
    lucide.h \
    geocalc.h \
    apppaths.h \
    trackdb.h \
    maptools.h \
    uiwidgets.h \
    managepanel.h \
    sidepanels.h

# Windows 用 Qt5Core 内置并导出的 zlib；其他平台链接系统 zlib
unix: LIBS += -lz

# ---- 兼容 RHEL 7.6 ----
# 整包部署：程序优先从自身目录下的 lib/ 加载 Qt 库（配合 build_linux.sh 的打包布局）
linux {
    QMAKE_LFLAGS += -Wl,-rpath,"'\$$ORIGIN/lib'"
}
