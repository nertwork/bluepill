cmake_minimum_required(VERSION 3.16)
project(bluepill VERSION 1.2.16 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 Network Svg Concurrent)
find_package(Qt6 OPTIONAL_COMPONENTS Test Multimedia DBus)

set(BLUEPILL_COMMON_SOURCES
    src/theme.h
    src/theme.cpp
    src/translator.h
    src/translator.cpp
    src/spellchecker.h
    src/spellchecker.cpp
    src/backend.h
    src/backend.cpp
    src/resources.qrc
)

if (WIN32)
    add_executable(bluepill WIN32
        src/main.cpp
        ${BLUEPILL_COMMON_SOURCES}
        resources/bluepill.rc
    )
else()
    add_executable(bluepill
        src/main.cpp
        ${BLUEPILL_COMMON_SOURCES}
    )
endif()

target_link_libraries(bluepill PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Svg
    Qt6::Concurrent
)

if (TARGET Qt6::Multimedia)
    target_link_libraries(bluepill PRIVATE Qt6::Multimedia)
    target_compile_definitions(bluepill PRIVATE BLUEPILL_HAS_MULTIMEDIA=1)
endif()

if (TARGET Qt6::DBus)
    target_link_libraries(bluepill PRIVATE Qt6::DBus)
    target_compile_definitions(bluepill PRIVATE BLUEPILL_HAS_DBUS=1)
endif()

if (WIN32)
    target_link_libraries(bluepill PRIVATE Advapi32 Crypt32 Winmm Ole32)
endif()

install(TARGETS bluepill RUNTIME DESTINATION bin)

if (TARGET Qt6::Test)
    enable_testing()
    add_executable(imsg_tests
        tests/imsg_tests.cpp
        ${BLUEPILL_COMMON_SOURCES}
    )
    target_link_libraries(imsg_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Network
        Qt6::Svg
        Qt6::Concurrent
        Qt6::Test
    )
    if (TARGET Qt6::Multimedia)
        target_link_libraries(imsg_tests PRIVATE Qt6::Multimedia)
        target_compile_definitions(imsg_tests PRIVATE BLUEPILL_HAS_MULTIMEDIA=1)
    endif()
    if (TARGET Qt6::DBus)
        target_link_libraries(imsg_tests PRIVATE Qt6::DBus)
        target_compile_definitions(imsg_tests PRIVATE BLUEPILL_HAS_DBUS=1)
    endif()
    if (WIN32)
        target_link_libraries(imsg_tests PRIVATE Advapi32 Crypt32 Winmm Ole32)
    endif()
    target_include_directories(imsg_tests PRIVATE src)
    target_compile_definitions(imsg_tests PRIVATE IMSG_UNIT_TEST=1)
    add_test(NAME imsg_tests COMMAND imsg_tests)
    set_tests_properties(imsg_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
