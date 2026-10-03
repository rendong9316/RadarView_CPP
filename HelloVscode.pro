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
    replaybar.cpp

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
    replaybar.h \
    geo.h

# Windows 用 Qt5Core 内置并导出的 zlib；其他平台链接系统 zlib
unix: LIBS += -lz
