QT += core gui widgets qml quick quickcontrols2 network svg concurrent
qtHaveModule(multimedia) {
    QT += multimedia
    DEFINES += BLUEPILL_HAS_MULTIMEDIA=1
}
qtHaveModule(dbus) {
    QT += dbus
    DEFINES += BLUEPILL_HAS_DBUS=1
}
win32 {
    LIBS += -lole32
}
CONFIG += c++20

TARGET = bluepill
VERSION = 1.2.13
TEMPLATE = app

DESTDIR = .
OBJECTS_DIR = obj
MOC_DIR = moc
RCC_DIR = rcc

SOURCES += \
    src/main.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/spellchecker.cpp \
    src/backend.cpp

HEADERS += \
    src/theme.h \
    src/translator.h \
    src/spellchecker.h \
    src/backend.h \
    src/emoji_ranges.h

RESOURCES += \
    src/resources.qrc
