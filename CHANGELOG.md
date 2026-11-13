# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/), versions follow [SemVer](https://semver.org/).

## [1.2.0] - 2026-11-13

### Added - Aether Bar (new, separate `AetherBar.exe`)
- Standalone **taskbar replacement** built on the same Direct2D + DirectComposition stack as the island.
- Floating, centered dock with squircle corners, plus a separate system pill (battery, clock, date).
- Windows are grouped per application (UWP frames get their own button); icons are pulled from the windows themselves.
- **Pinned apps** stored in `%APPDATA%\Aether\bar_pins.txt`; pin / unpin from the right-click menu.
- Click to activate or minimize, click again on multi-window apps to cycle, middle-click opens a new instance.
- Animated hover magnification, active-app pill indicator, tooltip with the window title, spring-based add / remove reflow.
- Start button opens the real Windows Start menu.
- Registers as a proper **AppBar**: maximized windows stop above the bar; hides itself behind full-screen apps.
- Hides the stock Windows taskbar (auto-hide + hidden) and re-hides it if Explorer restarts.
- **Safety:** the stock taskbar is restored on exit, on crash, on logoff, and via `AetherBar.exe --restore`.
- New CMake target `AetherBar`.

### Known limitations (Aether Bar)
- The system tray / notification area of the stock taskbar is not reproduced yet (hidden together with the taskbar).
- Primary monitor only.

## [1.1.0] - 2026-11-13

### Changed - Dynamic Island visuals (full rewrite of `IslandWindow`)
- Pure black body like the real hardware cut-out; neon / frosted-glass look removed.
- Real **continuous-curvature squircle** corners (Figma/Apple style smoothing) that animate cleanly from capsule to card.
- Soft contact shadow and hairline rim light (respects `enableShadow` / `shadowIntensity`).
- Content now **blurs and scales** in and out while the island morphs, and is clipped to the animated shape.
- New media layout: cover art, title/artist, remaining-time scrubber, large transport controls.
- Long titles scroll with soft edge fades (marquee) instead of being cut off.
- Play / pause icons crossfade with a spring; buttons shrink on press and fire on release.
- Activity-ring style system view (CPU / RAM / GPU) with animated fill; hard-coded "GPU 4060" label removed.
- Volume HUD with level-aware speaker icon and a clean slider; notifications show an app icon tile and two-line text.
- Text now uses cached text layouts, grayscale AA and Segoe UI Variable with automatic Segoe UI fallback on Windows 10.

### Added
- **Album-art accent colour**: dominant vibrant colour is extracted from the cover and tints the visualizer, glow and track-change pulse.
- **"Peek" banner**: a newly started song briefly shows its title, like iOS.
- **Real audio visualizer**: bars follow the actual output level of the default audio device (`IAudioMeterInformation`).
- **Privacy indicators**: orange dot while the microphone is in use, green dot for the camera.
- Draggable scrubber and volume slider; mouse wheel over the island changes volume; hand cursor over controls.
- Respects `reducedMotion`, `timeScale`, `showClockIdle`, `showAudioVisualizer` from the config.
- DPI-aware window size (no clipping at 150 % / 200 %), handles `WM_DPICHANGED` and display changes.
- Missing `CMakeLists.txt` (Dear ImGui is fetched automatically if `third_party/imgui` is absent).

### Fixed
- Album art was re-read from the media session every 350 ms because the title was compared before it was read.
- `GetCurrentTrack()` copied the whole cover image on every call, hundreds of times per second.
- Media-changed handler touched UI springs from the worker thread (data race); the UI now polls a thread-safe snapshot.
- Art was cleared whenever playback was paused / resumed.
- Microphone state was never populated.
- Frames were rendered at full refresh rate for ambient animation; now limited to ~60 fps when only audio bars / marquee move, and the island sleeps completely when nothing changes.

### Removed
- Per-frame desktop screenshot (`BitBlt` at ~12 fps) used for the fake blur: less CPU, no stale background.

### Internal
- `MediaService`: new `GetTrackLite()`, `FetchAlbumArt()`, `GetAudioPeak()`, `IsCameraActive()`; track info gains `artVersion`.
- `DCompLayer::CreateSquircleGeometry(bounds, radius, smoothing)` and `GetTargetBitmap()`; text AA set to grayscale, DPI fixed to 96.
- Shared brush reuse instead of creating brushes every frame.

## [1.0.0]
- Initial release: Dynamic Island, widgets, taskbar styling, settings center.
