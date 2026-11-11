QT += core gui qml quick quickcontrols2 network svg concurrent
CONFIG += c++20

TARGET = bluepill
VERSION = 1.1.0
TEMPLATE = app

DESTDIR = .
OBJECTS_DIR = obj
MOC_DIR = moc
RCC_DIR = rcc

SOURCES += \
    src/main.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/backend.cpp

HEADERS += \
    src/theme.h \
    src/translator.h \
    src/backend.h

RESOURCES += \
    src/resources.qrc
