#include "aether/system/taskbar_service.hpp"
#include "aether/core/log.hpp"

namespace aether::system {

// Undocumented SetWindowCompositionAttribute definitions
struct ACCENT_POLICY {
    int AccentState;
    int AccentFlags;
    int GradientColor;
    int AnimationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    int Attrib;
    void* pvData;
    size_t cbData;
};

using pfnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

static pfnSetWindowCompositionAttribute GetSetWindowCompositionAttribute() {
    static pfnSetWindowCompositionAttribute fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            fn = reinterpret_cast<pfnSetWindowCompositionAttribute>(GetProcAddress(hUser32, "SetWindowCompositionAttribute"));
        }
        resolved = true;
    }
    return fn;
}

void TaskbarService::Initialize() {
    if (m_initialized) return;
    m_initialized = true;
    AETHER_LOG_INFO("TaskbarService initialized");
}

std::vector<HWND> TaskbarService::FindTaskbarWindows() {
    std::vector<HWND> hwnds;

    // Primary taskbar
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (primary) hwnds.push_back(primary);

    // Secondary taskbars on multiple monitors
    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        hwnds.push_back(secondary);
    }

    return hwnds;
}

bool TaskbarService::SetWindowAccent(HWND hwnd, int accentState, uint32_t color) {
    auto fn = GetSetWindowCompositionAttribute();
    if (!fn || !hwnd) return false;

    // Convert RGBA to AABBGGRR
    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    uint32_t abgr = (a << 24) | (b << 16) | (g << 8) | r;

    ACCENT_POLICY policy{};
    policy.AccentState = accentState;
    policy.AccentFlags = (accentState == 4) ? 0x20 | 0x40 | 0x80 | 0x100 : 0;
    policy.GradientColor = static_cast<int>(abgr);
    policy.AnimationId = 0;

    WINDOWCOMPOSITIONATTRIBDATA data{};
    data.Attrib = 19; // WCA_ACCENT_POLICY
    data.pvData = &policy;
    data.cbData = sizeof(policy);

    return fn(hwnd, &data) != FALSE;
}

void TaskbarService::ApplyStyle(config::TaskbarStyle style, uint32_t tintColor) {
    m_currentStyle = style;
    auto hwnds = FindTaskbarWindows();

    int accentState = 0;
    switch (style) {
        case config::TaskbarStyle::Default:
            accentState = 0; // ACCENT_DISABLED
            break;
        case config::TaskbarStyle::Transparent:
            accentState = 2; // ACCENT_ENABLE_TRANSPARENTGRADIENT
            tintColor = 0x00000000;
            break;
        case config::TaskbarStyle::Blur:
            accentState = 3; // ACCENT_ENABLE_BLURBEHIND
            break;
        case config::TaskbarStyle::Acrylic:
            accentState = 4; // ACCENT_ENABLE_ACRYLICBLURBEHIND
            break;
        case config::TaskbarStyle::CustomTint:
            accentState = 2; // ACCENT_ENABLE_TRANSPARENTGRADIENT with color
            break;
    }

    for (HWND hwnd : hwnds) {
        SetWindowAccent(hwnd, accentState, tintColor);
    }
    AETHER_LOG_INFO("Applied taskbar style " << static_cast<int>(style) << " to " << hwnds.size() << " taskbars");
}

void TaskbarService::RestoreDefaults() {
    auto hwnds = FindTaskbarWindows();
    for (HWND hwnd : hwnds) {
        SetWindowAccent(hwnd, 0, 0);
    }
}

} // namespace aether::system
