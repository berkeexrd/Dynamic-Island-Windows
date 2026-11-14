#pragma once
// Aether's own pop-up menu (replaces the classic Win32 menu): glass panel, rounded hover pills, icons,
// spring-like fade. Modal like TrackPopupMenu: returns the chosen id, or 0 when dismissed.
#include <windows.h>
#include <string>
#include <vector>

namespace aether::ui {

struct MenuEntry {
    int id{0};                // 0 with separator = true
    std::wstring label;
    wchar_t glyph{0};         // Segoe Fluent Icons code point, 0 = none
    bool separator{false};
    bool danger{false};
    bool enabled{true};
    std::wstring hint;        // right-aligned dim text (shortcut, state)
};

inline MenuEntry MenuItem(int id, std::wstring label, wchar_t glyph = 0, bool danger = false, std::wstring hint = L"") {
    MenuEntry e; e.id = id; e.label = std::move(label); e.glyph = glyph; e.danger = danger; e.hint = std::move(hint); return e;
}
inline MenuEntry MenuSeparator() { MenuEntry e; e.separator = true; return e; }

// `above` = the menu opens upwards from the point (used by the bar at the bottom of the screen).
int ShowContextMenu(const std::vector<MenuEntry>& items, POINT screenPt, bool above = false);

} // namespace aether::ui
