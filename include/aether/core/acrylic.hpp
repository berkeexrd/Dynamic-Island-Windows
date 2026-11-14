#pragma once
// Frosted-glass backdrop for a top-level window: DWM blur-behind restricted to a region (so the blur follows
// rounded panels instead of filling the whole window rectangle). The tint comes from the window's own
// translucent drawing. The region is given in client coordinates; the caller keeps ownership of it.
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace aether::core {

inline void ApplyBlurRegion(HWND hwnd, HRGN region) {
    if (!hwnd) return;
    DWM_BLURBEHIND bb{};
    bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.fEnable = TRUE;
    bb.hRgnBlur = region;   // DWM copies it
    DwmEnableBlurBehindWindow(hwnd, &bb);
}

inline void ApplyBlurRoundRect(HWND hwnd, int x, int y, int w, int h, int radiusPx) {
    HRGN r = CreateRoundRectRgn(x, y, x + w + 1, y + h + 1, radiusPx * 2, radiusPx * 2);
    ApplyBlurRegion(hwnd, r);
    DeleteObject(r);
}

} // namespace aether::core
