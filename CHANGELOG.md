# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/), versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Complete shell: desktop, file manager, session manager, installer
- **AetherDesktop**: own desktop (wallpaper with fit modes, dim, blur and slideshow; icons with real shell icons and thumbnails, selection, marquee, drag & drop, rename, clipboard, Windows context menu behind "More options", auto arrange, `Ctrl` + wheel icon size, optional clock). On-demand rendering instead of continuous redraws.
- **Customise** window: centred, top-most (above desktop widgets), live preview, three tabs (wallpaper, icons and colour, desktop).
- **AetherExplorer**: from-scratch file manager (virtualised list, archives via 7-Zip / WinRAR / tar, network locations). **AetherBin**: Recycle Bin window (restore, delete, empty).
- **AetherShell**: starts and supervises bar, desktop and island; runs logon startup items; Alt+Tab, Win+Tab, Win+R, Win+M, Win+I, Win+D, Win+E. Can replace `explorer.exe`; escape hatches (Shift at logon, `use-explorer`, uninstall scripts).
- **AetherIntro**: logon / first-run logo animation (`no-intro` disables it).
- **AetherSetup**: single-file installer and uninstaller with a modern UI, component and shell-replacement options (this user or all users), silent mode (`/S`), embedded payload built by CMake.
- Desktop widgets: right-click menu to hide one or all widgets.
- New Aether logo and icon set.
- Fixed: the taskbar "show desktop" button raised Explorer's bare desktop over AetherDesktop; the game-mode detector no longer treats the desktop window as a full-screen app.

### Glass look matched to the concept
- **Halo is cut out of the panel**: the shadow and coloured glow no longer sit under the body, so translucent themes show the desktop instead of a muddy tint (`DrawGlassGlow` + `PunchOutPanels`; `AETHER_GLASS_PUNCH_OUT=0` restores the old behaviour). Flat themes (Classic) are unchanged.
- **Second accent colour** derived from every theme (`glass::kAccent2`, hue-shifted per theme) drives a two-hue halo, the 3-stop rim, rings, bars and the clock. Text colours follow the theme too.
- `DCompLayer::CreateLinearGradientBrush` accepts any number of stops.
- Violet / Lavender body colours re-measured from the concept (deeper purple, less grey); the rim is a softer hairline.
- Widgets: the clock is card-less (big thin digits with a gradient, date, weather line, corner brackets); rings fade in along the arc; storage bars have drive glyphs, a gradient fill and a glow; wider transparent margin so the halo is no longer cut at the window edge.
- Widgets redraw once a second instead of five times (5 Hz only briefly after a click, theme change or show), the sun icon is static.

### Network centre, widgets, themes
- New network panel: Wi-Fi switch, animated current connection, scanned network list, connect with password (WPA2/WPA3/open), disconnect, network and airplane-mode shortcuts.
- Desktop widgets: Storage, Weather and Now playing (with transport buttons). The per-widget switches in Settings now really show / hide them.
- Themes: Classic and Lavender (softer purple glass).
- Tray icons are rendered at their native size and scaled with a high-quality filter (sharper).
- Notification panel buttons aligned.

### Notifications, weather, Start menu, languages
- New own notification centre (real toasts from the Windows notification database, dismiss / clear all, animated empty state, unread badge on the bell).
- New animated weather panel: sky scene (sun, moon, stars, clouds, rain, snow, fog, lightning), details, 12-hour strip, 7-day forecast (Open-Meteo).
- Start menu and panels close on an outside click; clicks in the glow margin pass through to the dock and apps.
- Show-desktop is now a thin edge sliver; quick-settings Desktop tile became Airplane mode.
- Dates, weekdays and months follow the Windows language; UI text is English/Turkish (others fall back to English).
- Settings: removed the Taskbar page.

### Quick settings, themes, settings window
- Quick settings: choose the sound output and input device (new device list), microphone volume and mute, a working network status (uses Windows' own connectivity verdict), tile for Show desktop.
- Bar: show-desktop button and a notifications button (opens the Windows notification centre).
- Island: play / pause / next / previous run on their own thread with a media-key fallback, so the transport buttons work with every player.
- Themes: Violet, OLED Black, Clear Glass, Ocean, Emerald, Rose, Sunset. Chosen in Settings > Appearance, shared by every Aether process through %APPDATA%\Aether\theme.txt.
- Settings window redesigned: sidebar with groups, cards, Segoe UI font with Turkish letters, theme picker, translated (English / Turkish).

### Changed - new violet design (matches the mockup)
- **Start menu rewritten**: left rail (avatar, user name, Start / All apps / Documents / Downloads / Music / Pictures / Videos, Settings, Power),
  pill search box with a "stay open" pin, 6x2 pinned-app grid, "Recommended" list built from the Recent folder ("23 minutes ago", "Yesterday at 21:14").
  Search covers apps, settings, files in your user folders and a web fallback; keyboard navigation, paste, context menus.
- **Aether Bar**: purple Start button, weather pill (bottom left, Open-Meteo + IP location), tray pill with chevron / volume / network / clock+date / bell.
- **Desktop widgets**: ring-gauge System Performance (CPU / RAM / real GPU load), Network Activity with download + upload sparklines, clock card with calendar tile.
  New default position: stacked on the right edge of the primary monitor.
- Default accent colours are violet (`0xFF8B5CF6` / `0xFF5B3DF5`).
- UI text follows the Windows display language (English / Turkish); override with `--lang=en|tr`.

### Added
- **Glass rework**: the frosted look is now drawn (near-opaque dark violet body, inner light blooms, gradient rim, real Gaussian violet glow via `render/glass.hpp`) instead of the Windows blur, which showed rectangular blurred boxes. Used by the Start menu, bar pills, flyouts and widgets. Widgets resized to the mockup (264 px wide, rings with value inside, compact network row, calendar glyph) and stacked performance / network / clock on the right edge; widget positions use new config keys, so older saved positions are reset once.
- `WeatherService` (needs internet), GPU utilisation through PDH "GPU Engine" counters, upload history in `MetricsCollector`.
- New link libraries: `winhttp secur32 windowscodecs shlwapi` (AetherBar), `pdh` (Aether).

### Added - Aether Bar
- **Own system tray**: `TrayHost` receives the `Shell_NotifyIcon` protocol, so every app's tray icon appears in Aether's tray panel
  (chevron button; left / right / double click are forwarded to the owning app).
- **Own flyouts**: quick settings (volume, network, battery, shortcuts) on the volume/battery area and a calendar on the clock.
  They replace the Win+A / Win+N / Win+B windows.
- Modern procedural cursors in the bar, Start menu and flyouts.

### Changed
- The stock taskbar is now hidden completely by default and kept hidden (checked ~10x per second). `--keep-stock` restores the old auto-hide behaviour
  (`--hide-stock` is no longer needed).
- `AetherBar.exe` now requests administrator rights (UAC manifest) so it can reach elevated apps.

## [1.3.0] - 2026-11-13

### Fixed
- **Aether Bar blocked clicks around it** (above, left, right and over the tray area). A foreign window cannot be made
  click-through with `HTTRANSPARENT`, so the bar now uses a shaped window region that only covers the dock, the system pill
  and the tooltip. Everything else on the screen is clickable again.
- The island's click-blocking padding is now much tighter while it is compact (only the big expanded cards keep the larger shadow area).

### Added - Aether Bar
- **Custom Start menu** (new `start_menu.cpp`): opens above the dock with a spring animation and replaces the Windows one.
  - Pinned apps + alphabetical "All apps" with letter headers, built from the shell's AppsFolder (desktop *and* Microsoft Store apps).
  - Instant search (prefix matches first), keyboard navigation (arrows, Enter, Esc, PgUp/PgDn), smooth scrolling.
  - Right-click an app: Open / Pin to Start / Unpin. Pins are stored in `%APPDATA%\Aether\start_pins.txt`.
  - Footer with user avatar, Settings and a power menu (Lock, Sign out, Sleep, Restart, Shut down).
  - Pressing the **Windows key** opens it (low-level hook). Combos such as Win+E / Win+D / Win+B keep working. Disable with `--no-start-hook`.
- **Multi-monitor**: one bar per display, each listing only the windows that live on its monitor; rebuilt automatically when monitors or DPI change.
- **System tray access**: the stock taskbar is kept in auto-hide instead of being destroyed, so the real tray works. The chevron button (Win+B)
  reveals it. Use `--hide-stock` to hide it completely instead.
- **Quick-status pill** (primary monitor): volume icon with live level (scroll to change volume, middle-click to mute), battery, clock/date,
  and a show-desktop sliver. Clicking opens the real flyouts: quick settings (Win+A), notifications and calendar (Win+N), show desktop (Win+D).
- Tooltips for every control; pinned apps are shared across all bars and stay in pin order.

### Added - Dynamic Island
- **Option: Hide island from screenshots** (Settings > Dynamic Island). Off by default, so screenshots and recordings now include the island.
- The island now honours the **Target Monitor** setting (primary / second / third display) and moves live when it changes.

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

## Context menus & settings redesign
- New modern animated context menu (island, bar, Start) replacing native Win32 menus.
- Fixed island play/pause/seek button not responding (mouse-release ordering).
- Settings window redesigned: frameless, rounded, floating sidebar with icons, header panel.
- Classic theme reworked to the flat Windows 11 look; Lavender theme added.

## First run, wallpapers, notification icons
- Notification cards show the real app icon and name; empty-state / Wi-Fi messages centred.
- First-run welcome page (language, theme, start with Windows, wallpaper); Classic is the default theme.
- Theme-matched wallpapers (purple artwork for Violet/Rose/Lavender, black metallic for the rest) with restore option.
- Start with Windows now really works (Run entry + elevated logon task for the bar); language setting shared by all processes.
