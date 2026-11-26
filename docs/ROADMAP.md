# Product roadmap

This roadmap describes planned work. A feature is complete only after implementation and testing.

## Milestone 0 — Application foundation
- [x] C++20 / Qt 6 desktop shell
- [x] Original SVG brand mark
- [x] Dark interface theme
- [x] Media, Inspector, and Timeline panels
- [x] Local media import
- [x] Source preview through Qt Multimedia
- [x] Basic .kadris project save/open
- [x] Windows CI build workflow

## Milestone 1 — Media and project reliability
- [ ] Relative media paths and relinking
- [ ] Autosave and recovery
- [ ] Media metadata and thumbnails
- [ ] Drag-and-drop import
- [ ] Collect/archive project media
- [ ] Serialization tests

## Milestone 2 — Timeline editing engine
- [x] Read media duration where Qt Multimedia exposes it; mark estimates when unavailable
- [x] Time-based multi-track clip model and track assignment
- [x] Select, move, trim, split, and delete
- [x] Basic timeline snapping (100 ms)
- [ ] Frame-rate-aware timebase and sub-frame precision
- [ ] Ripple and roll editing
- [ ] Markers and advanced track controls
- [ ] Undo/redo command stack
- [x] Basic video sequence playback across timeline clips
- [x] Preview compositing of active clips across three video tracks
- [ ] Frame-accurate compositing clock and rendered sequence pipeline
- [x] Basic synchronized playback on AUDIO 1 and AUDIO 2 with master volume
- [x] Audio Mixer panel with independent track faders, Mute, Solo, and master fader
- [ ] Audio peak/RMS meters and clipping indicators
- [ ] Stereo pan/balance, buses, sends, and routing
- [ ] Track automation, keyframes, and audio effects
- [ ] Multiple overlapping audio clips per track in the playback engine

## Milestone 3 — Rendering and media engine
- [ ] FFmpeg integration with documented license/build choices
- [ ] Decode and render pipeline
- [ ] Audio/video synchronization
- [ ] Proxy and cache management
- [ ] Sequence compositing
- [ ] Export queue and output verification

## Milestone 4 — Creative tools
- [ ] Transitions and effects
- [ ] Keyframes and motion controls
- [ ] Color correction and scopes
- [x] Basic dockable audio mixer with per-track gain, Mute/Solo, and master gain
- [ ] Professional audio engine: meters, pan, buses, automation, VST/plugin effects, and sample-accurate mixing
- [ ] Titles and motion graphics
- [ ] Captions and subtitle editing

## Milestone 5 — Professional workflows
- [ ] Multicam editing
- [ ] Stabilization and motion tracking
- [ ] LUT and color management
- [ ] Project interchange formats
- [ ] Plugin API
- [ ] Optional local/provider-based AI assistance


## Long-term professional editing target

The goal is a complete, reliable Windows editing workstation with its own implementation and identity. These are product targets, not claims that the current prototype already supports them.

### Ingest and project management
- [ ] Broad camera, phone, screen-recording, image, audio, and professional media ingest
- [ ] Metadata, searchable bins, labels, ratings, tags, and smart collections
- [ ] Thumbnail, waveform, and filmstrip generation
- [ ] Relink, consolidate, copy, archive, and verify project media
- [ ] Autosave, recovery, backups, project versions, and team handoff

### Editing and timeline
- [ ] Frame-rate-aware timebase, snapping, markers, and sequence settings
- [ ] Insert, overwrite, ripple, roll, slip, slide, rate-stretch, and replace edits
- [ ] Track targeting, sync locks, track locks, patching, nesting, and track groups
- [ ] Multicam source sequences, angle switching, and synchronized camera/audio
- [ ] Transcript-based editing, scene detection, and searchable dialogue
- [ ] Full undo/redo history and keyboard shortcut customization

### Image, motion, and effects
- [ ] Transform, crop, anchor point, opacity, masks, and motion keyframes
- [ ] Transition and effect stacks with parameters, presets, and keyframes
- [ ] Compositing, blending modes, chroma key, and tracked masks
- [ ] Color wheels, curves, scopes, LUTs, HDR tools, and color management
- [ ] Stabilization, tracking, denoise, sharpen, and image restoration
- [ ] Titles, lower thirds, motion graphics, templates, and vector shapes

### Audio and accessibility
- [ ] Sample-accurate multitrack audio mixing and synchronization
- [ ] Per-track meters, pan, buses, sends, automation, and loudness tools
- [ ] Equalizer, dynamics, noise reduction, reverb, and plugin hosting
- [ ] Captions, subtitle editing, transcription, translation, and speaker labels
- [ ] Accessibility checks and caption export formats

### Rendering and delivery
- [ ] Dedicated decode, cache, proxy, and GPU-accelerated render pipeline
- [ ] Reliable sequence rendering independent of real-time preview playback
- [ ] Export presets, codec/container controls, batch queue, and render retry
- [ ] Color-space and HDR-aware delivery, audio loudness checks, and validation
- [ ] Export to common web, broadcast, and archive delivery formats

### Professional extensibility
- [ ] Workspace layouts, panels, scopes, keyboard maps, and user preferences
- [ ] Plugin SDK and documented extension API
- [ ] Interchange with common edit, audio, subtitle, and motion-graphics formats
- [ ] Optional local AI tools for transcription, search, cleanup, and assisted editing
- [ ] Performance, accessibility, localization, crash reporting, and regression test suites
