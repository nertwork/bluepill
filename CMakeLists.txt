cmake_minimum_required(VERSION 3.16)
project(bluepill VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Svg Concurrent)
find_package(Qt6 OPTIONAL_COMPONENTS Test)

add_executable(bluepill
    src/main.cpp
    src/theme.h
    src/theme.cpp
    src/translator.h
    src/translator.cpp
    src/backend.h
    src/backend.cpp
    src/resources.qrc
)

target_link_libraries(bluepill PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Svg
    Qt6::Concurrent
)

if (TARGET Qt6::Test)
    enable_testing()
    add_executable(imsg_tests
        tests/imsg_tests.cpp
        src/theme.h
        src/theme.cpp
        src/translator.h
        src/translator.cpp
        src/backend.h
        src/backend.cpp
        src/resources.qrc
    )
    target_link_libraries(imsg_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Network
        Qt6::Svg
        Qt6::Concurrent
        Qt6::Test
    )
    target_include_directories(imsg_tests PRIVATE src)
    target_compile_definitions(imsg_tests PRIVATE IMSG_UNIT_TEST=1)
    add_test(NAME imsg_tests COMMAND imsg_tests)
    set_tests_properties(imsg_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
