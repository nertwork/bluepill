QT += core gui qml quick quickcontrols2 network testlib
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
    $$PWD/../src/backend.cpp

HEADERS += \
    $$PWD/../src/theme.h \
    $$PWD/../src/translator.h \
    $$PWD/../src/backend.h

RESOURCES += \
    $$PWD/../src/resources.qrc
