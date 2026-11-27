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
    src/project_media_paths.h
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

    add_executable(project-media-paths-tests tests/project_media_paths_tests.cpp)
    target_include_directories(project-media-paths-tests PRIVATE src)
    target_link_libraries(project-media-paths-tests PRIVATE Qt6::Widgets)
    add_test(NAME project-media-paths-tests COMMAND project-media-paths-tests)

    add_executable(timeline-serialization-tests
        tests/timeline_serialization_tests.cpp
        src/timelinewidget.cpp
        src/timelinewidget.h
    )
    target_include_directories(timeline-serialization-tests PRIVATE src)
    target_link_libraries(timeline-serialization-tests PRIVATE Qt6::Widgets Qt6::Multimedia)
    add_test(NAME timeline-serialization-tests COMMAND timeline-serialization-tests)
    set_tests_properties(timeline-serialization-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Keep assert-based regression checks active even in Release CI builds.
    target_compile_options(project-media-paths-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    target_compile_options(frame-timebase-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    target_compile_options(timeline-serialization-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
endif()

if(MSVC)
    target_compile_options(kadris-studio PRIVATE /W4 /permissive-)
else()
    target_compile_options(kadris-studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS kadris-studio RUNTIME DESTINATION . BUNDLE DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets)
