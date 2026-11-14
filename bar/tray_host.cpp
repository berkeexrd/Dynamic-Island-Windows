#include "aether/bar/tray_host.hpp"
#include <shellapi.h>
#include <algorithm>
#include <cstring>

namespace aether::bar {

namespace {
// Wire format of the WM_COPYDATA payload apps send through Shell_NotifyIcon (32-bit handles on every bitness).
struct WireNotifyIcon {
    DWORD cbSize, hWnd, uID, uFlags, uCallbackMessage, hIcon;
    WCHAR szTip[128];
    DWORD dwState, dwStateMask;
    WCHAR szInfo[256];
    DWORD uTimeoutOrVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct WireTrayData {
    DWORD signature;     // 0x34753423
    DWORD message;       // NIM_ADD / MODIFY / DELETE / SETFOCUS / SETVERSION
    WireNotifyIcon nid;
};
template <class T> T* H(DWORD v) { return reinterpret_cast<T*>(static_cast<LONG_PTR>(static_cast<LONG>(v))); }
} // namespace

bool TrayHost::Create() {
    if (m_hwnd) return true;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Shell_TrayWnd";
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"Shell_TrayWnd", L"",
                             WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;
    ChangeWindowMessageFilterEx(m_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);   // we run elevated (UIPI)
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);   // apps re-register their icons
    return true;
}

void TrayHost::Destroy() {
    if (!m_hwnd) return;
    for (auto& i : m_icons) if (i.icon) DestroyIcon(i.icon);
    m_icons.clear();
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);   // hand icons back to Explorer
}

LRESULT CALLBACK TrayHost::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_COPYDATA && self) {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cds && cds->dwData == 1 && cds->lpData) self->OnData(cds->lpData, cds->cbData);
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

TrayIcon* TrayHost::Find(HWND hwnd, UINT id) {
    for (auto& i : m_icons) if (i.hwnd == hwnd && i.id == id) return &i;
    return nullptr;
}

void TrayHost::OnData(const void* data, DWORD size) {
    WireTrayData d{};
    memcpy(&d, data, std::min<size_t>(size, sizeof(d)));
    if (d.signature != 0x34753423) return;
    const WireNotifyIcon& n = d.nid;
    HWND hwnd = H<HWND__>(n.hWnd);
    const UINT id = n.uID;

    enum { kAdd = 0, kModify = 1, kDelete = 2, kSetFocus = 3, kSetVersion = 4 };
    switch (d.message) {
        case kAdd:
        case kModify: {
            TrayIcon* t = Find(hwnd, id);
            if (!t) {
                if (d.message == kModify) return;
                m_icons.push_back({}); t = &m_icons.back(); t->hwnd = hwnd; t->id = id;
            }
            if (n.uFlags & NIF_MESSAGE) t->callbackMsg = n.uCallbackMessage;
            if (n.uFlags & NIF_TIP) t->tip = n.szTip;
            if (n.uFlags & NIF_STATE) t->hidden = (n.dwStateMask & NIS_HIDDEN) && (n.dwState & NIS_HIDDEN);
            if (n.uFlags & NIF_ICON) {
                if (t->icon) DestroyIcon(t->icon);
                t->icon = n.hIcon ? CopyIcon(H<HICON__>(n.hIcon)) : nullptr;
                t->serial = ++m_serial;
            }
            break;
        }
        case kDelete: {
            auto it = std::remove_if(m_icons.begin(), m_icons.end(), [&](TrayIcon& i) {
                if (i.hwnd == hwnd && i.id == id) { if (i.icon) DestroyIcon(i.icon); return true; }
                return false; });
            m_icons.erase(it, m_icons.end());
            break;
        }
        case kSetVersion:
            if (TrayIcon* t = Find(hwnd, id)) t->version = n.uTimeoutOrVersion;
            break;
        default: break;
    }
    ++m_version;
}

void TrayHost::Prune() {
    const size_t before = m_icons.size();
    m_icons.erase(std::remove_if(m_icons.begin(), m_icons.end(), [](TrayIcon& i) {
        if (!IsWindow(i.hwnd)) { if (i.icon) DestroyIcon(i.icon); return true; }
        return false; }), m_icons.end());
    if (m_icons.size() != before) ++m_version;
}

// Delivers a click the way Explorer does, so the owning app can pop up its own menu / window.
void TrayHost::Click(const TrayIcon& t, bool right, bool dbl) {
    if (!IsWindow(t.hwnd) || !t.callbackMsg) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(t.hwnd, &pid);
    AllowSetForegroundWindow(pid);
    POINT pt; GetCursorPos(&pt);
    const bool v4 = t.version >= 4;
    const WPARAM wpV4 = MAKEWPARAM(pt.x, pt.y);
    auto send = [&](UINT event) {
        if (v4) PostMessageW(t.hwnd, t.callbackMsg, wpV4, MAKELPARAM(event, t.id));
        else    PostMessageW(t.hwnd, t.callbackMsg, t.id, event);
    };
    if (!right) {
        if (dbl) { send(WM_LBUTTONDBLCLK); send(WM_LBUTTONUP); return; }
        send(WM_LBUTTONDOWN); send(WM_LBUTTONUP);
        if (t.version >= 3) send(NIN_SELECT);
    } else {
        send(WM_RBUTTONDOWN); send(WM_RBUTTONUP);
        if (v4) send(WM_CONTEXTMENU);
    }
}

} // namespace aether::bar
