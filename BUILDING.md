# Building Kadris Studio

## Windows 11 prerequisites
1. Visual Studio 2022 with the Desktop development with C++ workload.
2. CMake 3.24 or newer and Ninja.
3. Qt 6.8 or newer with the MSVC 2022 64-bit kit and Widgets, Multimedia, Multimedia Widgets, and SVG modules.

## Configure and build
Run from a Developer PowerShell for Visual Studio 2022. Adjust the Qt path to match your installation.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
    cmake --build build --parallel
    .\build\kadris-studio.exe

Qt Multimedia uses the Windows media backend. Playback compatibility depends on Windows and the available codecs. This prototype provides source-file playback; it does not yet render a multi-clip timeline sequence.
