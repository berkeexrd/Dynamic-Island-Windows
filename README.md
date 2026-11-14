# Aether

**A native Windows desktop shell: Dynamic Island, taskbar replacement, Start menu, flyouts and desktop widgets, written in C++20 with Direct2D and DirectComposition. No Electron, no web views.**

![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D4?style=flat-square&logo=windows)
![C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B)
![Rendering](https://img.shields.io/badge/Rendering-Direct2D%20%2B%20DirectComposition-76b900?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=flat-square)

## Showcase

![Aether Showcase](./showcase.png)

Aether ships as two programs that share one look:

| Program | What it is | Needs admin |
| :--- | :--- | :---: |
| `Aether.exe` | Dynamic Island, desktop widgets and the Settings Center | no |
| `AetherBar.exe` | Taskbar replacement: dock, Start menu, system tray host, flyouts | yes (UAC) |

---

## Features

### Dynamic Island
- Spring-physics pill at the top of the screen that morphs between idle, media, system, volume and notification states.
- Live media session control (Spotify, browsers, any player that uses Windows media controls): album art, progress, play / pause / next / previous, volume HUD.
- Click-through everywhere outside the pill; modern right-click menu.

### Taskbar replacement (AetherBar)
- Multi-monitor dock with running and pinned apps and a show-desktop edge.
- Custom **Start menu** with search, pinned and recent apps, user card and power menu.
- Real **system tray host**: tray icons keep working and are rendered crisp at their native size.
- Own flyouts, all animated and themed:
  - **Quick settings**: volume, output and input device, microphone, network status, battery, shortcuts.
  - **Network centre**: Wi-Fi switch, connect with password (WPA2 / WPA3 / open), disconnect, network list.
  - **Notification centre**: real Windows toasts with the app icon, dismiss and clear all.
  - **Weather**: animated sky scene, details, hourly and 7-day forecast ([Open-Meteo](https://open-meteo.com/), no API key).
  - **Calendar** with Windows-localised dates.

### Desktop widgets
Draggable, grid-snapping cards: System performance, Network activity, Clock, Storage, Weather and Now playing.

### Themes and wallpapers
- Nine themes: **Classic** (flat Windows 11 look, the default), Violet, OLED Black, Clear Glass, Ocean, Emerald, Rose, Lavender, Sunset.
- Optional matching wallpapers: purple artwork for purple themes, a black metallic one for dark themes. Your own wallpaper can be restored at any time.

### Settings Center
Frameless, rounded window with a first-run welcome page (language, theme, start with Windows, wallpaper). English and Turkish.

### Efficiency
- Panel glow is rendered once and reused; resting panels are frame-limited.
- Gaming Mode hides the overlays while a full-screen game is running.

---

## Building

Requirements: Windows 10 1903+ or Windows 11, Visual Studio 2022+ with the *Desktop development with C++* workload, CMake 3.20+.

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Dear ImGui is taken from `third_party/imgui` when present and downloaded automatically otherwise. The wallpapers in `assets/wallpapers` are copied next to the executables.

## Running

```powershell
.\build\Release\Aether.exe              # island, widgets, settings
.\build\Release\Aether.exe --settings   # open the Settings Center directly
.\build\Release\AetherBar.exe           # bar + Start menu (asks for administrator rights)
```

`--lang=en` / `--lang=tr` force the interface language; otherwise it follows Windows or the choice made in Settings. `AetherBar.exe --keep-stock` leaves the stock Windows taskbar on auto-hide instead of hiding it completely.

**Start with Windows** (Settings > General) adds a per-user startup entry for `Aether.exe` and a highest-privilege logon task for `AetherBar.exe` (one UAC prompt).

## Controls

| Action | Result |
| :--- | :--- |
| `F9` | Cycle the Dynamic Island modes |
| Right-click the island | Settings, widgets on/off, island mode, test notification |
| Right-click the bar or a dock icon | Bar and app menus |
| Drag a widget | Reposition (snaps to the grid) |
| `Win` + `D` | Show desktop (AetherBar) |

## Files Aether writes

| Path | Content |
| :--- | :--- |
| `%APPDATA%\Aether\aether_config.json` | Settings |
| `%APPDATA%\Aether\theme.txt`, `lang.txt` | Theme and language shared by both programs |
| `%APPDATA%\Aether\orig_wallpaper.txt` | Your wallpaper from before Aether changed it |

## Project layout

```text
include/aether/   headers (core, config, render, island, widgets, ui, system, bar)
src/              Aether.exe: island, widgets, settings, media, system services
bar/              AetherBar.exe: dock, Start menu, tray host, flyouts, panels
assets/           bundled wallpapers
third_party/      Dear ImGui, nlohmann/json, stb_image
```

## Changelog

See [CHANGELOG.md](CHANGELOG.md) (Turkish: [CHANGELOG.tr.md](CHANGELOG.tr.md)).

## License

MIT, see [LICENSE](LICENSE).