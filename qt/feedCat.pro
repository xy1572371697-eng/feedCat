QT       += core gui widgets

TARGET   = feedCat
TEMPLATE = app

CONFIG   += c++11

# 交叉编译时取消注释并设置路径
# QMAKE_CC  = arm-linux-gnueabihf-gcc
# QMAKE_CXX = arm-linux-gnueabihf-g++

# 头文件搜索路径（指向项目 include/ 和 src/）
INCLUDEPATH += ../include ../src

# Qt 界面源文件
SOURCES += \
    main.cpp \
    mainwindow.cpp \
    feedpage.cpp \
    timerpage.cpp \
    historypage.cpp \
    settingpage.cpp \
    camerapage.cpp \
    feedcontroller.cpp

HEADERS += \
    mainwindow.h \
    feedpage.h \
    timerpage.h \
    historypage.h \
    settingpage.h \
    camerapage.h \
    feedcontroller.h

FORMS += \
    ../mainwindow.ui

# 后端 C 源文件
SOURCES += \
    ../src/feed_core.c \
    ../src/ir_sensor.c \
    ../src/motor.c \
    ../src/weight.c \
    ../src/camera.c \
    ../src/video_stream.c \
    ../src/rtc_timer.c \
    ../src/aht30.c

# 链接库
LIBS += -lpthread -ljpeg

# 输出目录（全部放在项目根目录下）
DESTDIR     = .
OBJECTS_DIR = ./obj
MOC_DIR     = ./moc
UI_DIR      = ./ui
RCC_DIR     = ./rcc
