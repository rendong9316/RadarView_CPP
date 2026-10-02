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
    tilesource.cpp

HEADERS += \
    mainwindow.h \
    globewidget.h \
    tilesource.h
