# KadrisStudio

KadrisStudio is an independent, open-source Windows desktop video-editor project built with C++20 and Qt 6. It uses an original dark graphite interface with purple accents and its own branding.

**Development status:** advanced editing prototype. Core timeline editing, clip-level image/audio controls, keyframes, markers, and multi-selection workflows are implemented. The project is still under active development and is not yet a complete nonlinear editing or export system.

## Current editing capabilities

### Media and projects
- Import local video, audio, and image files.
- Preview source media through Qt Multimedia (supported formats depend on the Windows multimedia backend).
- Create, save, and reopen `.kadris` projects.
- Store imported media references relative to the project file when possible, while retaining support for older absolute-path projects.
- Persist timeline clip positions, source in-points, durations, track assignments, clip adjustments, keyframes, markers, and track states.
- Read media duration where available; use a clearly marked five-second estimate when duration metadata is unavailable.

### Timeline
- Multi-track clip placement, selection, movement, trimming, splitting, deletion, duplication, and ripple deletion.
- Multi-select clips with Ctrl-click; move, delete, duplicate, ripple-delete, and nudge selected groups together.
- Move selected clip groups between tracks while preserving relative track offsets.
- Frame-based navigation and snapping on the current 25 fps timeline.
- Snap clip edges to other clip edges and timeline markers.
- Add, navigate to, and remove timeline markers.
- Lock and mute individual tracks.
- Display WAV audio waveforms, fade overlays, and audio automation curves/keyframe markers.

### Video and image adjustments
- Per-clip brightness, contrast, saturation, color wheels, and RGB curve controls.
- Per-clip transform (scale and X/Y position), opacity, and blend modes.
- Brightness, contrast, transform, and opacity keyframes with interpolation.
- Persist clip adjustments and keyframes in project files.
- Autosave dirty projects and recover unsaved sessions; browse, restore, or discard available recovery snapshots in the Recovery Center.

### Audio
- Per-clip volume, mute, fade-in, and fade-out.
- Audio volume automation keyframes with interpolation.
- Audio Mixer with track faders, Mute/Solo, and master gain.
- Basic synchronized timeline playback on the supported audio tracks.

## Important limitations

- The current preview/playback pipeline is not a frame-accurate final render engine.
- Sequence export, an export queue, and verified output rendering are not yet implemented.
- WAV waveform visualization is currently limited to supported WAV PCM/float formats.
- Multiple overlapping audio clips per track, advanced routing, sample-accurate mixing, audio meters, and professional audio effects remain future work.
- Proxy media and project media collection remain future work. Timeline undo/redo (up to 100 states), autosave, and the Recovery Center are implemented, but still need broader runtime and crash/recovery testing.
- Supported media formats depend on the installed Qt/Windows multimedia backend.

## Build on Windows 11

Requirements: Visual Studio 2022 C++ tools, CMake 3.24+, Ninja, and Qt 6.8+ with Widgets, Multimedia, MultimediaWidgets, and Svg.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build --parallel
.\build\kadris-studio.exe
```

Replace the Qt path with your local Qt installation. The GitHub Actions Windows workflow checks that the project configures and builds.

## Repository layout
- `src/` — application source
- `assets/` — original brand assets
- `resources/` — Qt stylesheet and resources
- `docs/` — roadmap and technical notes
- `.github/workflows/` — Windows CI

See [docs/ROADMAP.md](docs/ROADMAP.md) for completed work and the next development milestones.

## License and project identity

Licensed under GNU GPL-3.0-or-later. Review `THIRD_PARTY_LICENSES.md` before adding or redistributing external codecs, plugins, models, or libraries.

“KadrisStudio” is a working project name; trademark and domain clearance have not yet been completed. This is an independent project and is not affiliated with Adobe or other video-editing brands.
