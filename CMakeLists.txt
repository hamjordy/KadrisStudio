cmake_minimum_required(VERSION 3.24)
project(KadrisStudio VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Svg)
qt_standard_project_setup()

qt_add_executable(kadris-studio WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/timelinewidget.cpp
    src/timelinewidget.h
    src/frame_timebase.h
    src/programmonitorwidget.cpp
    src/programmonitorwidget.h
)
qt_add_resources(kadris-studio kadris_resources
    PREFIX "/"
    FILES assets/kadris-studio.svg resources/kadris.qss
)
target_link_libraries(kadris-studio PRIVATE
    Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Svg
)
target_include_directories(kadris-studio PRIVATE src)

if(BUILD_TESTING)
    add_executable(frame-timebase-tests tests/frame_timebase_tests.cpp)
    target_include_directories(frame-timebase-tests PRIVATE src)
    add_test(NAME frame-timebase-tests COMMAND frame-timebase-tests)
endif()

if(MSVC)
    target_compile_options(kadris-studio PRIVATE /W4 /permissive-)
else()
    target_compile_options(kadris-studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS kadris-studio RUNTIME DESTINATION . BUNDLE DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets)
