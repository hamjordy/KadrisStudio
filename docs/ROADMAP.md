# Product roadmap

This roadmap distinguishes implemented prototype functionality from work still required for a dependable editing workstation. A feature is marked complete only when it is implemented; a successful CI build confirms compilation, not full end-user runtime behavior.

## Milestone 0 — Application foundation
- [x] C++20 / Qt 6 Windows desktop shell
- [x] Original SVG brand identity and dark graphite interface
- [x] Dockable Media, Inspector, Timeline, and Audio Mixer panels
- [x] Local media import and source preview through Qt Multimedia
- [x] Basic `.kadris` project save/open
- [x] Windows CI build workflow

## Milestone 1 — Media and project reliability
- [x] Relative media paths for media-bin items and timeline clips, with legacy absolute-path compatibility
- [x] Relink a missing media-bin item and update every matching timeline clip without losing edit settings
- [x] Timed autosave of dirty projects and recovery of unsaved sessions
- [x] Recovery Center to inspect, restore, and discard available autosave snapshots
- [ ] Crash/restart recovery regression tests and rotating project backups
- [ ] Media metadata, searchable bins, and thumbnail generation
- [ ] Drag-and-drop import
- [ ] Collect/archive project media
- [x] Dedicated timeline serialization regression tests
- [x] Atomic project-file saving to protect existing work during interrupted saves

## Milestone 2 — Timeline editing
- [x] Time-based multi-track clip model and track assignment
- [x] Select, move, trim, split, delete, duplicate, and ripple-delete clips
- [x] Multi-select and group move/delete/duplicate/ripple-delete/nudge
- [x] Move selected clip groups between tracks while preserving offsets
- [x] 25 fps frame snapping and frame-based playhead navigation
- [x] Snap clip edges to other clip edges and timeline markers
- [x] Add, navigate, and remove timeline markers
- [x] Track lock and mute states
- [ ] User-selectable sequence frame rate
- [ ] Ripple, roll, slip, slide, and rate-stretch editing modes
- [x] Timeline undo/redo history (up to 100 snapshots; keyboard shortcuts)
- [ ] Track targeting, sync locks, nesting, and track groups

## Milestone 3 — Preview, compositing, and rendering
- [x] Basic timeline playback across supported video clips
- [x] Preview compositing of active clips across video tracks
- [x] Basic synchronized playback on supported audio tracks
- [ ] Frame-accurate compositing clock and robust A/V synchronization
- [ ] Dedicated decode, cache, and render pipeline
- [ ] FFmpeg integration with documented licensing/build choices
- [ ] Sequence export, export presets, and batch queue
- [ ] Output verification and render retry/recovery

## Milestone 4 — Creative tools
- [x] Per-clip brightness, contrast, saturation, color wheels, and RGB curves
- [x] Per-clip transform, opacity, and blend modes
- [x] Brightness, contrast, transform, and opacity keyframes
- [x] Per-clip audio volume, mute, fades, and volume automation keyframes
- [x] WAV waveform display and timeline audio automation overlays
- [ ] Transitions and effect stacks with keyframes and presets
- [ ] Masks, chroma key, and tracked masks
- [ ] Color scopes, LUTs, and color management
- [ ] Titles, lower thirds, and motion graphics
- [ ] Captions and subtitle editing
- [ ] Audio meters, pan/balance, buses, sends, and advanced effects

## Milestone 5 — Professional workflows
- [ ] Multicam editing and synchronized camera/audio sources
- [ ] Proxy workflows and cache management
- [ ] Project interchange formats
- [ ] Customizable workspaces and keyboard shortcuts
- [ ] Plugin API and documented extension SDK
- [ ] Optional local/provider-based AI assistance
- [ ] Performance, accessibility, localization, and regression testing

## Immediate priorities

1. Build a reliable undo/redo history for timeline and Inspector edits.
2. Add relative project paths so projects survive folder moves.
3. Add automated crash/restart recovery tests and rotating project backups.
4. Establish a real render/export pipeline with output validation.
5. Expand automated tests beyond compilation, including serialization, timeline behavior, and project recovery.

## Long-term target

The goal is a complete, reliable Windows editing workstation with its own implementation and identity. The roadmap items above are product targets, not claims that the current prototype already supports them.
