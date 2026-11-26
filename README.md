# Kadris Studio

Independent open-source desktop video-editor project for Windows 11.

**Status: early prototype (0.1.0).** This first increment provides a native Qt application shell, media import, source playback, project save/open, and a basic timeline visualization. It is not yet a full professional nonlinear editor. Timeline editing, sequence playback/rendering, effects, color grading, audio mixing, captions, and sequence export remain planned work.

## Current functionality
- Native C++20 / Qt 6 Windows desktop application.
- Original SVG brand identity and dark graphite interface.
- Dockable Media, Inspector, and Timeline panels.
- Import local video, audio, and image files.
- Preview a source file using Qt Multimedia (supported formats depend on the Windows multimedia backend).
- Add imported media to the initial timeline model.
- Create, save, and reopen .kadris project files.
- GitHub Actions Windows build workflow.

## Build on Windows 11
Requirements: Visual Studio 2022 C++ tools, CMake 3.24+, Ninja, and Qt 6.8+ with Widgets, Multimedia, MultimediaWidgets, and Svg.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
    cmake --build build --parallel
    .\build\kadris-studio.exe

Replace the Qt path with your local Qt installation. The Windows GitHub Actions workflow installs the required modules and builds the application.

## Repository layout
- src/ — application source
- assets/ — brand assets
- resources/ — Qt stylesheet
- docs/ — roadmap and technical notes
- .github/workflows/ — Windows CI

## Roadmap and limitations
See docs/ROADMAP.md. The current timeline is a model/visualization only; it does not yet render a multi-clip sequence or provide frame-accurate editing.

## License
GNU GPL-3.0-or-later. Review THIRD_PARTY_LICENSES.md before adding or redistributing external codecs, plugins, models, or libraries.

“Kadris Studio” is a working project name; trademark and domain clearance have not yet been completed. This is an independent project and is not affiliated with Adobe.
