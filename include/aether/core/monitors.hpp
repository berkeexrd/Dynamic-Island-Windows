#pragma once

#include <windows.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "shcore.lib")

namespace aether::core {

// Monitors in a stable order: primary first, then the others left-to-right.
inline std::vector<HMONITOR> EnumerateMonitors() {
    struct Ctx { std::vector<HMONITOR> list; } ctx;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        reinterpret_cast<Ctx*>(lp)->list.push_back(m);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    auto isPrimary = [](HMONITOR m) {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        return (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    };
    auto leftOf = [](HMONITOR m) {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        return mi.rcMonitor.left;
    };
    std::sort(ctx.list.begin(), ctx.list.end(), [&](HMONITOR a, HMONITOR b) {
        const bool pa = isPrimary(a), pb = isPrimary(b);
        if (pa != pb) return pa;
        return leftOf(a) < leftOf(b);
    });
    return ctx.list;
}

// index 0 = primary, 1 = second display, ... (falls back to primary when it does not exist)
inline HMONITOR MonitorByIndex(int index) {
    auto list = EnumerateMonitors();
    if (list.empty()) return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (index < 0 || index >= static_cast<int>(list.size())) index = 0;
    return list[index];
}

inline float MonitorScale(HMONITOR m) {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
    return static_cast<float>(dx) / 96.0f;
}

} // namespace aether::core
