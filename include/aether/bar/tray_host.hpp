#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace aether::bar {

struct TrayIcon {
    HWND hwnd{nullptr};
    UINT id{0};
    UINT callbackMsg{0};
    UINT version{0};
    HICON icon{nullptr};          // private copy, owned by the host
    std::wstring tip;
    bool hidden{false};
    unsigned serial{0};           // bumps whenever the icon image changes
};

// Receives the Shell_NotifyIcon protocol so every application's tray icon shows up in Aether's own tray.
// It owns a top-level window of class "Shell_TrayWnd": FindWindow returns the top-most match in z-order,
// so apps talk to us instead of Explorer's (hidden) taskbar.
class TrayHost {
public:
    static TrayHost& Instance() { static TrayHost h; return h; }
    bool Create();
    void Destroy();
    [[nodiscard]] bool Active() const { return m_hwnd != nullptr; }
    [[nodiscard]] const std::vector<TrayIcon>& Icons() const { return m_icons; }
    [[nodiscard]] unsigned Version() const { return m_version; }
    void Click(const TrayIcon& icon, bool right, bool dbl);
    void Prune();                 // drop icons whose owner window is gone
private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnData(const void* data, DWORD size);
    TrayIcon* Find(HWND hwnd, UINT id);

    HWND m_hwnd{nullptr};
    std::vector<TrayIcon> m_icons;
    unsigned m_version{1};
    unsigned m_serial{1};
};

} // namespace aether::bar
