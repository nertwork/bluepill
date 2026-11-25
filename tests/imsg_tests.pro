QT += core gui widgets qml quick quickcontrols2 network testlib
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
CONFIG += c++20 testcase

TARGET = imsg_tests
TEMPLATE = app

DESTDIR = .
OBJECTS_DIR = obj
MOC_DIR = moc
RCC_DIR = rcc

INCLUDEPATH += $$PWD/../src $$OUT_PWD/moc moc

SOURCES += \
    imsg_tests.cpp \
    $$PWD/../src/theme.cpp \
    $$PWD/../src/translator.cpp \
    $$PWD/../src/spellchecker.cpp \
    $$PWD/../src/backend.cpp

HEADERS += \
    $$PWD/../src/theme.h \
    $$PWD/../src/translator.h \
    $$PWD/../src/spellchecker.h \
    $$PWD/../src/backend.h \
    $$PWD/../src/emoji_ranges.h

RESOURCES += \
    $$PWD/../src/resources.qrc
