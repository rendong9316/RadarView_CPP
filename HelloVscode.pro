QT       += core gui widgets sql network

TARGET = HelloVscode
TEMPLATE = app

# ---- 兼容 Windows 7 ----
# Qt 5.14 默认 C++11，安全；不启用更严的 C++14/17，避免老 MinGW 头文件冲突
CONFIG += c++11

INCLUDEPATH += \
    $$PWD/src \
    $$PWD/src/app \
    $$PWD/src/core \
    $$PWD/src/io \
    $$PWD/src/globe \
    $$PWD/src/ui \
    $$PWD/src/point3d \
    $$PWD/src/selftest

include(src/selftest/selftest.pri)

SOURCES += \
    # app
    src/app/main.cpp \
    src/app/mainwindow.cpp \
    src/app/apppaths.cpp \
    src/app/splashscreen.cpp \
    src/app/theme.cpp \
    src/app/uiwidgets.cpp \
    src/app/lucide.cpp \
    # core
    src/core/crs.cpp \
    src/core/geocalc.cpp \
    src/core/track.cpp \
    src/core/replaycontroller.cpp \
    # io
    src/io/matfile.cpp \
    src/io/trackparsers.cpp \
    src/io/trackimporter.cpp \
    src/io/trackdb.cpp \
    # globe
    src/globe/globewidget.cpp \
    src/globe/tracklayer.cpp \
    src/globe/tilesource.cpp \
    src/globe/netmap.cpp \
    src/globe/maptools.cpp \
    src/globe/spatiallasso.cpp \
    src/globe/drawingmanager.cpp \
    src/globe/drawcommands.cpp \
    # ui
    src/ui/appstatusbar.cpp \
    src/ui/sidepanels.cpp \
    src/ui/managepanel.cpp \
    src/ui/settingspanel.cpp \
    src/ui/trackpointdialog.cpp \
    src/ui/trackpicker.cpp \
    # point3d
    src/point3d/point3dparsers.cpp \
    src/point3d/point3dimportjob.cpp \
    src/point3d/point3dcanvas.cpp \
    src/point3d/point3dsidepanel.cpp \
    src/point3d/point3dadvancedpanel.cpp \
    src/point3d/point3dwindow.cpp

HEADERS += \
    # app
    src/app/mainwindow.h \
    src/app/apppaths.h \
    src/app/splashscreen.h \
    src/app/theme.h \
    src/app/uiwidgets.h \
    src/app/lucide.h \
    # core
    src/core/geo.h \
    src/core/crs.h \
    src/core/geocalc.h \
    src/core/track.h \
    src/core/replaycontroller.h \
    # io
    src/io/matfile.h \
    src/io/trackparsers.h \
    src/io/trackimporter.h \
    src/io/trackdb.h \
    # globe
    src/globe/globewidget.h \
    src/globe/tracklayer.h \
    src/globe/tilesource.h \
    src/globe/netmap.h \
    src/globe/maptools.h \
    src/globe/spatiallasso.h \
    src/globe/drawingmanager.h \
    src/globe/drawcommands.h \
    # ui
    src/ui/appstatusbar.h \
    src/ui/sidepanels.h \
    src/ui/managepanel.h \
    src/ui/settingspanel.h \
    src/ui/trackpointdialog.h \
    src/ui/trackpicker.h \
    # point3d
    src/point3d/point3dtypes.h \
    src/point3d/point3dparsers.h \
    src/point3d/point3dimportjob.h \
    src/point3d/point3dcanvas.h \
    src/point3d/point3dsidepanel.h \
    src/point3d/point3dadvancedpanel.h \
    src/point3d/point3dwindow.h

# Windows 用 Qt5Core 内置并导出的 zlib；其他平台链接系统 zlib
unix: LIBS += -lz

# ---- 兼容 RHEL 7.6 ----
# 整包部署：程序优先从自身目录下的 lib/ 加载 Qt 库（配合 scripts/build_linux.sh 的打包布局）
linux {
    QMAKE_LFLAGS += -Wl,-rpath,"'\$$ORIGIN/lib'"
}
