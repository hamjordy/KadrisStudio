# Kadris Studio

Independent open-source desktop video-editor project for Windows 11.

**Status: early prototype (0.3.0).** The application provides a native Qt shell, media import, source playback, project save/open, and an interactive timeline foundation. Clips can be moved between tracks, trimmed from either edge, split at the playhead, selected, and deleted. The timeline stores clip timing and track placement in the project. This is not yet a full nonlinear editor: timeline playback switches between video clips but does not composite overlapping video tracks. Separate audio-track mixing, effects, color grading, captions, undo/redo, frame-accurate timebase, and sequence export remain planned work.

## Current functionality
- Native C++20 / Qt 6 Windows desktop application.
- Original SVG brand identity and dark graphite interface.
- Dockable Media, Inspector, and Timeline panels.
- Import local video, audio, and image files.
- Preview a source file using Qt Multimedia (supported formats depend on the Windows multimedia backend).
- Add imported media to the timeline; supported media duration is read through Qt Multimedia, with a clearly marked five-second estimate when duration metadata is unavailable.
- Move clips horizontally and between video/audio tracks.
- Trim clip in/out points by dragging timeline edges.
- Position the playhead and split a selected clip at that point.
- Delete selected clips and use the Delete/Backspace keys.
- Play video clips across the video tracks in timeline order in the program monitor, honoring clip start positions and source in-points.
- Save timeline positions, source in-points, durations, and track assignments in project format version 2 (version 1 projects remain readable).
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
See docs/ROADMAP.md. Timeline edits are now stored in a time-based clip model, but the timeline does not yet render a multi-clip sequence, provide frame-accurate timebase editing, or include undo/redo.

## License
GNU GPL-3.0-or-later. Review THIRD_PARTY_LICENSES.md before adding or redistributing external codecs, plugins, models, or libraries.

“Kadris Studio” is a working project name; trademark and domain clearance have not yet been completed. This is an independent project and is not affiliated with Adobe.
