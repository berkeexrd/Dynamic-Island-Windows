#include "aether/bar/aether_bar.hpp"
#include "aether/bar/start_menu.hpp"
#include "aether/core/monitors.hpp"
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstdlib>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;

namespace {

constexpr float kDockH = 56.0f;
constexpr float kMargin = 10.0f;
constexpr float kReserved = 74.0f;
constexpr float kTipRoom = 90.0f;
constexpr float kItem = 44.0f;
constexpr float kGap = 4.0f;
constexpr float kPad = 8.0f;
constexpr ULONGLONG kIconRefreshMs = 8000;

constexpr wchar_t kGlyphChevron = 0xE70E;
constexpr wchar_t kGlyphMute = 0xE74F;
constexpr wchar_t kGlyphVol0 = 0xE992;
constexpr wchar_t kGlyphVol1 = 0xE993;
constexpr wchar_t kGlyphVol2 = 0xE994;
constexpr wchar_t kGlyphVol3 = 0xE995;

inline D2D1_RECT_F RF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// ---- pins shared by every bar --------------------------------------------------------------
struct PinStore {
    std::vector<std::wstring> pins;
    unsigned version{1};
    bool loaded{false};
    static PinStore& I() { static PinStore s; return s; }

    static std::wstring Path() {
        wchar_t appdata[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
        std::wstring dir = std::wstring(appdata) + L"\\Aether";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\bar_pins.txt";
    }
    void Load() {
        if (loaded) return;
        loaded = true;
        if (FILE* f = _wfopen(Path().c_str(), L"rt, ccs=UTF-8")) {
            wchar_t line[1024];
            while (fgetws(line, ARRAYSIZE(line), f)) {
                std::wstring s = line;
                while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
                if (!s.empty()) pins.push_back(s);
            }
            fclose(f);
        } else {
            wchar_t win[MAX_PATH];
            GetWindowsDirectoryW(win, MAX_PATH);
            pins.push_back(std::wstring(win) + L"\\explorer.exe");
            const wchar_t* edge = L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
            if (GetFileAttributesW(edge) != INVALID_FILE_ATTRIBUTES) pins.push_back(edge);
        }
    }
    void Save() {
        if (FILE* f = _wfopen(Path().c_str(), L"wt, ccs=UTF-8")) {
            for (const auto& p : pins) fwprintf(f, L"%ls\n", p.c_str());
            fclose(f);
        }
        ++version;
    }
    int IndexOf(const std::wstring& p) const {
        for (size_t i = 0; i < pins.size(); ++i) if (_wcsicmp(pins[i].c_str(), p.c_str()) == 0) return static_cast<int>(i);
        return -1;
    }
};

std::wstring ExePathOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring result;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD len = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) result.assign(buf, len);
        CloseHandle(h);
    }
    return result;
}

std::wstring FileTitle(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    if (!name.empty()) name[0] = static_cast<wchar_t>(towupper(name[0]));
    return name;
}

bool IEqual(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring WindowTitle(HWND h) {
    wchar_t buf[256];
    int n = GetWindowTextW(h, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n > 0 ? n : 0);
}

bool IsTaskWindow(HWND h, HWND self) {
    if (h == self || !IsWindowVisible(h)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"AetherBarClass") ||
        !wcscmp(cls, L"AetherStartMenuClass")) return false;
    return true;
}

struct EnumCtx { std::vector<HWND> out; HWND self; };
BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    if (IsTaskWindow(h, c->self)) c->out.push_back(h);
    return TRUE;
}

HICON WindowIcon(HWND h) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    if (HICON i = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON))) return i;
    return reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICONSM));
}

// Opens the *real* Windows flyouts (they keep working while the stock taskbar is hidden)
void SendWinCombo(WORD key) {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = key;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = key; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

ComPtr<IAudioEndpointVolume> DefaultEndpointVolume() {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioEndpointVolume> vol;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(vol.GetAddressOf()));
    }
    return vol;
}

} // namespace

// =============================================================================================
//  Stock taskbar guard
// =============================================================================================

static void ForEachShellBar(void (*fn)(HWND)) {
    if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) fn(t);
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) fn(sec);
}

void ShellTaskbarGuard::Hide(bool hard) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = tray;
    if (!m_hidden) m_prevState = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    // Auto-hide releases the work area the stock bar reserved so ours can claim it.
    abd.lParam = static_cast<LPARAM>(m_prevState | ABS_AUTOHIDE);
    SHAppBarMessage(ABM_SETSTATE, &abd);
    m_hard = hard;
    if (hard) ForEachShellBar([](HWND h) { ShowWindow(h, SW_HIDE); });
    m_hidden = true;
}

void ShellTaskbarGuard::KeepHidden() {
    if (!m_hidden || !m_hard) return;
    ForEachShellBar([](HWND h) { if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE); });
}

void ShellTaskbarGuard::Restore() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        APPBARDATA abd = {sizeof(abd)};
        abd.hWnd = tray;
        UINT state = m_hidden ? m_prevState : static_cast<UINT>(ABS_ALWAYSONTOP);
        state &= ~static_cast<UINT>(ABS_AUTOHIDE);
        abd.lParam = static_cast<LPARAM>(state | ABS_ALWAYSONTOP);
        SHAppBarMessage(ABM_SETSTATE, &abd);
    }
    ForEachShellBar([](HWND h) { ShowWindow(h, SW_SHOWNA); });
    m_hidden = false;
}

// =============================================================================================
//  Entry
// =============================================================================================

Entry::Entry()
    : appear(0.0f, animation::SpringConfig{420.0f, 30.0f, 1.0f, 0.002f})
    , hover(0.0f, animation::SpringConfig{620.0f, 36.0f, 1.0f, 0.002f})
    , active(0.0f, animation::SpringConfig{520.0f, 38.0f, 1.0f, 0.002f}) {
    appear.SetTarget(1.0f);
}

// =============================================================================================
//  Window / lifecycle
// =============================================================================================

AetherBar::AetherBar(HMONITOR monitor, bool primary, StartMenu* start)
    : m_mon(monitor), m_primary(primary), m_start(start)
    , m_startHover(0.0f, animation::SpringConfig{620.0f, 36.0f, 1.0f, 0.002f})
    , m_dockAppear(0.0f, animation::SpringConfig{300.0f, 28.0f, 1.0f, 0.002f}) {}

AetherBar::~AetherBar() {
    UnregisterAppBar();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

LRESULT CALLBACK AetherBar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AetherBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = reinterpret_cast<AetherBar*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<AetherBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void AetherBar::PositionWindow() {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(m_mon, &mi);
    m_monRect = mi.rcMonitor;
    m_dpi = core::MonitorScale(m_mon);
    m_windowW = m_monRect.right - m_monRect.left;
    m_windowH = static_cast<int>((kReserved + kTipRoom) * m_dpi);
    m_reservedPx = static_cast<int>(kReserved * m_dpi);
    if (m_hwnd) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, m_monRect.left, m_monRect.bottom - m_windowH, m_windowW, m_windowH, SWP_NOACTIVATE);
    }
}

bool AetherBar::Create() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AetherBarClass";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);  // harmless if already registered by the first bar

    PositionWindow();
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Aether Bar", WS_POPUP, m_monRect.left, m_monRect.bottom - m_windowH,
                             m_windowW, m_windowH, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, m_windowW, m_windowH)) return false;
    m_brush = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});
    CreateFonts();

    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    PinStore::I().Load();

    RegisterAppBar();
    RefreshWindows();
    m_dockAppear.SetTarget(1.0f);
    ComputeLayout();
    UpdateRegion();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void AetherBar::RegisterAppBar() {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = m_hwnd;
    abd.uCallbackMessage = WM_APPBAR;
    SHAppBarMessage(ABM_NEW, &abd);
    SetAppBarPos();
}

void AetherBar::UnregisterAppBar() {
    if (!m_hwnd) return;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = m_hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
}

void AetherBar::SetAppBarPos() {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = m_hwnd;
    abd.uEdge = ABE_BOTTOM;
    abd.rc = {m_monRect.left, m_monRect.bottom - m_reservedPx, m_monRect.right, m_monRect.bottom};
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - m_reservedPx;
    SHAppBarMessage(ABM_SETPOS, &abd);
}

void AetherBar::CreateFonts() {
    auto dw = m_dcomp->GetDWriteFactory();
    if (!dw) return;
    ComPtr<IDWriteFontCollection> col;
    dw->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) {
        UINT32 i = 0; BOOL ex = FALSE;
        return col && SUCCEEDED(col->FindFamilyName(n, &i, &ex)) && ex;
    };
    const wchar_t* display = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
    m_hasIconFont = icons != nullptr;

    auto make = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, DWRITE_FONT_WEIGHT w, float size) {
        f.Reset();
        if (!family) return;
        dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * m_dpi, L"en-us", &f);
        if (f) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };
    make(m_fontTip,  display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f);
    make(m_fontTime, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f);
    make(m_fontDate, display, DWRITE_FONT_WEIGHT_NORMAL, 11.0f);
    make(m_fontIcon, icons,   DWRITE_FONT_WEIGHT_NORMAL, 16.0f);
    if (m_fontTip) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ell;
        dw->CreateEllipsisTrimmingSign(m_fontTip.Get(), &ell);
        m_fontTip->SetTrimming(&trim, ell.Get());
    }
}

// =============================================================================================
//  Running windows + pins
// =============================================================================================

Entry* AetherBar::FindEntry(const std::wstring& key) {
    for (auto& e : m_entries) if (IEqual(e->key, key)) return e.get();
    return nullptr;
}

ComPtr<ID2D1Bitmap1> AetherBar::LoadIcon(const Entry& e) {
    HICON icon = nullptr;
    bool owned = false;
    for (HWND w : e.windows) { icon = WindowIcon(w); if (icon) break; }
    if (!icon && !e.exePath.empty()) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(e.exePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
            icon = sfi.hIcon; owned = true;
        }
    }
    ComPtr<ID2D1Bitmap1> bmp;
    if (!icon) return bmp;

    const int px = static_cast<int>(48 * std::max(1.0f, m_dpi));
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        memset(bits, 0, static_cast<size_t>(px) * px * 4);
        DrawIconEx(dc, 0, 0, icon, px, px, 0, nullptr, DI_NORMAL);
        GdiFlush();
        auto* p = static_cast<uint32_t*>(bits);
        bool anyAlpha = false;
        for (int i = 0; i < px * px; ++i) if (p[i] >> 24) { anyAlpha = true; break; }
        if (!anyAlpha) for (int i = 0; i < px * px; ++i) if (p[i] & 0x00FFFFFF) p[i] |= 0xFF000000;
        if (auto ctx = m_dcomp->GetContext()) {
            D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ctx->CreateBitmap(D2D1::SizeU(px, px), bits, px * 4, props, &bmp);
        }
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(dc);
    if (owned) DestroyIcon(icon);
    return bmp;
}

void AetherBar::RefreshWindows() {
    PinStore& store = PinStore::I();
    EnumCtx ctx{{}, m_hwnd};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));

    for (auto& e : m_entries) e->windows.clear();

    // pins first, so pinned apps exist even when nothing is running
    for (const auto& p : store.pins) {
        if (!FindEntry(p)) {
            auto e = std::make_unique<Entry>();
            e->key = p; e->exePath = p; e->label = FileTitle(p);
            m_entries.push_back(std::move(e));
        }
    }
    for (auto& e : m_entries) e->pinned = store.IndexOf(e->exePath) >= 0 && !e->exePath.empty();

    for (HWND h : ctx.out) {
        // each bar lists only the windows that live on its own monitor
        if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != m_mon) continue;
        std::wstring exe = ExePathOf(h);
        std::wstring key = exe;
        if (exe.empty() || exe.find(L"ApplicationFrameHost.exe") != std::wstring::npos) {
            key = L"hwnd:" + std::to_wstring(reinterpret_cast<uintptr_t>(h));
        }
        Entry* e = FindEntry(key);
        if (!e) {
            auto ne = std::make_unique<Entry>();
            ne->key = key; ne->exePath = exe; ne->label = exe.empty() ? WindowTitle(h) : FileTitle(exe);
            e = ne.get();
            m_entries.push_back(std::move(ne));
        }
        e->windows.push_back(h);
    }

    const ULONGLONG now = GetTickCount64();
    for (auto& e : m_entries) {
        e->appear.SetTarget((!e->Running() && !e->pinned) ? 0.0f : 1.0f);
        if (e->Running()) {
            const std::wstring t = WindowTitle(e->windows.front());
            if (!t.empty()) e->label = t;
        }
        if (!e->icon || now - e->iconStamp > kIconRefreshMs) {
            auto bmp = LoadIcon(*e);
            if (bmp) e->icon = bmp;
            e->iconStamp = now;
        }
    }
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [](const std::unique_ptr<Entry>& e) {
        return !e->pinned && !e->Running() && e->appear.GetValue() < 0.02f;
    }), m_entries.end());

    // pinned first (in pin order), everything else keeps its first-seen order
    std::stable_sort(m_entries.begin(), m_entries.end(), [&](const std::unique_ptr<Entry>& a, const std::unique_ptr<Entry>& b) {
        const int ia = a->pinned ? store.IndexOf(a->exePath) : INT_MAX;
        const int ib = b->pinned ? store.IndexOf(b->exePath) : INT_MAX;
        return ia < ib;
    });
    m_pinVersion = store.version;
}

// =============================================================================================
//  Update / layout
// =============================================================================================

bool AetherBar::IsAnimating() const {
    if (!m_startHover.IsResting() || !m_dockAppear.IsResting()) return true;
    for (const auto& e : m_entries) {
        if (!e->appear.IsResting() || !e->hover.IsResting() || !e->active.IsResting()) return true;
    }
    return std::abs(m_tipAlpha - (m_tipWanted ? 1.0f : 0.0f)) > 0.01f;
}

void AetherBar::Tick(float dt) {
    m_refreshTimer += dt;
    if (m_refreshTimer >= 0.35f || m_pinVersion != PinStore::I().version) { m_refreshTimer = 0.0f; RefreshWindows(); }
    m_keepHiddenTimer += dt;
    if (m_keepHiddenTimer >= 0.6f) { m_keepHiddenTimer = 0.0f; ShellTaskbarGuard::Instance().KeepHidden(); }

    if (m_primary) {
        m_volTimer += dt;
        if (m_volTimer >= 1.0f) {
            m_volTimer = 0.0f;
            if (auto ev = DefaultEndpointVolume()) {
                float v = 0; BOOL mute = FALSE;
                if (SUCCEEDED(ev->GetMasterVolumeLevelScalar(&v))) m_volume = v;
                if (SUCCEEDED(ev->GetMute(&mute))) m_muted = mute != FALSE;
            }
        }
    }

    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        Entry& e = *m_entries[i];
        bool isActive = false;
        for (HWND w : e.windows) if (w == fg || w == fgRoot) { isActive = true; break; }
        e.active.SetTarget(isActive ? 1.0f : 0.0f);
        e.hover.SetTarget((m_hoverKind == Hit::Item && m_hoverItem == i) ? 1.0f : 0.0f);
        e.appear.Update(dt); e.hover.Update(dt); e.active.Update(dt);
    }
    m_startHover.SetTarget(m_hoverKind == Hit::Start ? 1.0f : 0.0f);
    m_startHover.Update(dt);
    m_dockAppear.Update(dt);

    if (m_tipOverrideTimer > 0.0f) m_tipOverrideTimer -= dt;
    m_tipAlpha += ((m_tipWanted ? 1.0f : 0.0f) - m_tipAlpha) * (1.0f - std::exp(-dt * 16.0f));
}

void AetherBar::ComputeLayout() {
    const float s = m_dpi;
    const float h = kDockH * s;
    const float bottom = static_cast<float>(m_windowH) - kMargin * s;
    const float top = bottom - h;

    float contentW = 0.0f;
    for (const auto& e : m_entries) contentW += (kItem + kGap) * s * e->appear.GetValue();
    const float startW = kItem * s;
    const float dockW = (kPad * 2 + 6) * s + startW + contentW;
    const float left = (static_cast<float>(m_windowW) - dockW) * 0.5f;

    m_layout.dock = Rect{left, top, dockW, h};
    m_layout.start = Rect{left + kPad * s, top + (h - kItem * s) * 0.5f, startW, kItem * s};

    m_layout.items.assign(m_entries.size(), Rect{});
    float x = m_layout.start.Right() + 6 * s;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        m_layout.items[i] = Rect{x, top + (h - kItem * s) * 0.5f, kItem * s, kItem * s};
        x += (kItem + kGap) * s * m_entries[i]->appear.GetValue();
    }

    SYSTEM_POWER_STATUS ps{};
    const bool battery = m_primary && GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100;
    const float chevW = m_primary ? 34.0f * s : 0.0f;
    const float quickW = m_primary ? (battery ? 108.0f : 46.0f) * s : 0.0f;
    const float clockW = 84.0f * s;
    const float deskW = 14.0f * s;
    const float rightW = 10.0f * s + chevW + quickW + clockW + deskW;
    const float rx = static_cast<float>(m_windowW) - kMargin * s * 1.4f - rightW;
    m_layout.right = Rect{rx, top, rightW, h};

    float cx = rx + 10.0f * s;
    const float iy = top + 6.0f * s, ih = h - 12.0f * s;
    m_layout.chevron = Rect{cx, iy, chevW, ih}; cx += chevW;
    m_layout.quick = Rect{cx, iy, quickW, ih}; cx += quickW;
    m_layout.clock = Rect{cx, iy, clockW, ih}; cx += clockW;
    m_layout.desktop = Rect{cx, top + 12.0f * s, deskW, h - 24.0f * s};
}

void AetherBar::ComputeTooltip() {
    std::wstring text;
    float anchor = 0.0f;
    if (m_tipOverrideTimer > 0.0f && !m_tipOverride.empty()) {
        text = m_tipOverride; anchor = m_layout.quick.Center().x;
    } else {
        switch (m_hoverKind) {
            case Hit::Item:
                if (m_hoverItem >= 0 && m_hoverItem < static_cast<int>(m_entries.size())) {
                    text = m_entries[m_hoverItem]->label.empty() ? L"App" : m_entries[m_hoverItem]->label;
                    anchor = m_layout.items[m_hoverItem].Center().x;
                }
                break;
            case Hit::Start:   text = L"Start"; anchor = m_layout.start.Center().x; break;
            case Hit::Chevron: text = L"System tray (hidden icons)"; anchor = m_layout.chevron.Center().x; break;
            case Hit::Quick:   text = L"Quick settings  -  scroll to change volume"; anchor = m_layout.quick.Center().x; break;
            case Hit::Clock:   text = L"Notifications & calendar"; anchor = m_layout.clock.Center().x; break;
            case Hit::Desktop: text = L"Show desktop"; anchor = m_layout.desktop.Center().x; break;
            default: break;
        }
    }
    m_tipWanted = !text.empty();
    if (m_tipWanted) { m_tipText = text; m_tipAnchor = anchor; }

    m_tipRect = Rect{};
    if (m_tipAlpha > 0.02f && !m_tipText.empty()) {
        const float s = m_dpi;
        auto dw = m_dcomp->GetDWriteFactory();
        ComPtr<IDWriteTextLayout> tl;
        float tw = 0;
        if (dw && m_fontTip && SUCCEEDED(dw->CreateTextLayout(m_tipText.c_str(), static_cast<UINT32>(m_tipText.size()), m_fontTip.Get(), 4096, 40, &tl))) {
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); tw = m.widthIncludingTrailingWhitespace;
        }
        const float w = std::min(tw + 28.0f * s, 340.0f * s), h = 30.0f * s;
        Rect r{m_tipAnchor - w / 2, m_layout.dock.Top() - h - 10.0f * s + (1.0f - m_tipAlpha) * 6.0f * s, w, h};
        r.x = std::clamp(r.x, 8.0f * s, static_cast<float>(m_windowW) - w - 8.0f * s);
        m_tipRect = r;
    }
}

// The window region both clips drawing and decides where clicks are swallowed. Foreign windows can
// NOT be made click-through with HTTRANSPARENT, so only the pills (and tooltip) are part of the window.
void AetherBar::UpdateRegion() {
    const float s = m_dpi;
    const float lift = (1.0f - Clamp01(m_dockAppear.GetValue())) * 30.0f * s;
    auto box = [&](const Rect& r, float padX, float padTop, float padBottom, std::vector<int>& key, int out[4]) {
        out[0] = static_cast<int>(r.Left() - padX);
        out[1] = static_cast<int>(r.Top() - padTop + lift);
        out[2] = static_cast<int>(r.Right() + padX);
        out[3] = static_cast<int>(r.Bottom() + padBottom + lift);
        for (int i = 0; i < 4; ++i) key.push_back(out[i]);
    };
    std::vector<int> key;
    int dock[4], right[4], tip[4] = {0, 0, 0, 0};
    box(m_layout.dock, 14 * s, 8 * s, 18 * s, key, dock);
    box(m_layout.right, 14 * s, 8 * s, 18 * s, key, right);
    const bool hasTip = m_tipRect.width > 1.0f;
    if (hasTip) box(m_tipRect, 6 * s, 6 * s, 6 * s, key, tip);
    if (key == m_regionKey) return;
    m_regionKey = key;

    const int r = static_cast<int>(40 * s);
    HRGN rgn = CreateRoundRectRgn(dock[0], dock[1], dock[2], dock[3], r, r);
    HRGN p2 = CreateRoundRectRgn(right[0], right[1], right[2], right[3], r, r);
    CombineRgn(rgn, rgn, p2, RGN_OR);
    DeleteObject(p2);
    if (hasTip) {
        HRGN p3 = CreateRoundRectRgn(tip[0], tip[1], tip[2], tip[3], r, r);
        CombineRgn(rgn, rgn, p3, RGN_OR);
        DeleteObject(p3);
    }
    SetWindowRgn(m_hwnd, rgn, FALSE);   // the system owns the region from here on
}

// =============================================================================================
//  Rendering
// =============================================================================================

void AetherBar::DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f,
                         const D2D1_RECT_F& r, const Color& c) {
    if (!f || t.empty()) return;
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, &r, m_brush.Get());
}

void AetherBar::DrawGlyph(ID2D1DeviceContext* ctx, wchar_t g, const D2D1_RECT_F& r, const Color& c) {
    if (!m_hasIconFont || !m_fontIcon) return;
    wchar_t buf[2] = {g, 0};
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(buf, 1, m_fontIcon.Get(), &r, m_brush.Get());
}

void AetherBar::DrawPill(ID2D1DeviceContext* ctx, const Rect& r, float radius) {
    const float s = m_dpi;
    for (int i = 3; i >= 1; --i) {
        Rect sr{r.x - i * 3.0f * s, r.y + (i * 2.0f + 2.0f) * s, r.width + i * 6.0f * s, r.height + i * 2.0f * s};
        if (auto g = m_dcomp->CreateSquircleGeometry(sr, radius + i * 3.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.07f));
            ctx->FillGeometry(g.Get(), m_brush.Get());
        }
    }
    auto geo = m_dcomp->CreateSquircleGeometry(r, radius);
    if (!geo) return;
    m_brush->SetColor(D2D1::ColorF(0.055f, 0.055f, 0.07f, 0.92f));
    ctx->FillGeometry(geo.Get(), m_brush.Get());
    auto rim = m_dcomp->CreateLinearGradientBrush(Color{1.0f, 1.0f, 1.0f, 0.16f}, Color{1.0f, 1.0f, 1.0f, 0.04f},
                                                  D2D1::Point2F(r.Center().x, r.Top()), D2D1::Point2F(r.Center().x, r.Bottom()));
    if (rim) ctx->DrawGeometry(geo.Get(), rim.Get(), 1.0f * s);
}

void AetherBar::DrawStartLogo(ID2D1DeviceContext* ctx, const Vec2& c, float size, float alpha) {
    const float cell = size * 0.46f, gap = size * 0.08f;
    const float x0 = c.x - cell - gap * 0.5f, y0 = c.y - cell - gap * 0.5f;
    m_brush->SetColor(D2D1::ColorF(0.38f, 0.80f, 1.0f, alpha));
    for (int iy = 0; iy < 2; ++iy) for (int ix = 0; ix < 2; ++ix) {
        const float x = x0 + ix * (cell + gap), y = y0 + iy * (cell + gap);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(x, y, x + cell, y + cell), size * 0.07f, size * 0.07f), m_brush.Get());
    }
}

void AetherBar::DrawBattery(ID2D1DeviceContext* ctx, float x, float cy, int percent, bool charging) {
    const float s = m_dpi, w = 24 * s, h = 12 * s;
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
    ctx->DrawRoundedRectangle(D2D1::RoundedRect(RF(x, cy - h / 2, x + w, cy + h / 2), 3 * s, 3 * s), m_brush.Get(), 1.2f * s);
    ctx->FillRectangle(RF(x + w + 1.5f * s, cy - 2.5f * s, x + w + 3 * s, cy + 2.5f * s), m_brush.Get());
    const float f = Clamp01(percent / 100.0f);
    Color c = charging ? Color{0.19f, 0.82f, 0.35f, 1.0f} : (percent <= 20 ? Color{1.0f, 0.27f, 0.23f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 0.92f});
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(x + 2 * s, cy - h / 2 + 2 * s, x + 2 * s + (w - 4 * s) * f, cy + h / 2 - 2 * s), 1.5f * s, 1.5f * s), m_brush.Get());
}

void AetherBar::Render() {
    if (!m_dcomp || !m_visible) return;
    ComputeLayout();
    ComputeTooltip();
    UpdateRegion();

    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    const float s = m_dpi;
    const float dockA = Clamp01(m_dockAppear.GetValue());
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(0, (1.0f - dockA) * 30.0f * s));

    // ---- dock -----------------------------------------------------------------------------------
    DrawPill(ctx, m_layout.dock, 24.0f * s);

    const float sh = m_startHover.GetValue();
    if (sh > 0.01f) {
        if (auto g = m_dcomp->CreateSquircleGeometry(m_layout.start, 14.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.09f * sh));
            ctx->FillGeometry(g.Get(), m_brush.Get());
        }
    }
    DrawStartLogo(ctx, m_layout.start.Center(), (20.0f + 2.0f * sh) * s, 0.95f);

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = *m_entries[i];
        const float appear = Clamp01(e.appear.GetValue());
        if (appear < 0.01f) continue;
        const float hov = e.hover.GetValue(), act = e.active.GetValue();
        const Rect& slot = m_layout.items[i];
        const Vec2 c = slot.Center();

        if (act > 0.01f || hov > 0.01f) {
            if (auto g = m_dcomp->CreateSquircleGeometry(slot, 14.0f * s)) {
                m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.07f * act + 0.08f * hov));
                ctx->FillGeometry(g.Get(), m_brush.Get());
            }
        }
        const float scale = appear * (1.0f + 0.14f * hov);
        const float icon = 28.0f * s * scale;
        const float cy = c.y - 3.0f * s * hov;
        if (e.icon) {
            const D2D1_RECT_F dst = RF(c.x - icon / 2, cy - icon / 2 - 1.0f * s, c.x + icon / 2, cy + icon / 2 - 1.0f * s);
            ctx->DrawBitmap(e.icon.Get(), &dst, (e.Running() ? 1.0f : 0.78f) * appear, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        if (e.Running()) {
            const float dw = (4.0f + 12.0f * act) * s, dh = 3.6f * s;
            const float dy = slot.Bottom() - 3.0f * s;
            m_brush->SetColor(D2D1::ColorF(act > 0.5f ? 0.38f : 1.0f, act > 0.5f ? 0.80f : 1.0f, 1.0f, (0.50f + 0.45f * act) * appear));
            ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(c.x - dw / 2, dy - dh, c.x + dw / 2, dy), dh / 2, dh / 2), m_brush.Get());
        }
    }

    // ---- system pill --------------------------------------------------------------------------------
    DrawPill(ctx, m_layout.right, 24.0f * s);
    auto hoverBg = [&](const Rect& r, Hit kind) {
        if (m_hoverKind != kind || r.width < 1.0f) return;
        if (auto g = m_dcomp->CreateSquircleGeometry(r, 12.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.09f));
            ctx->FillGeometry(g.Get(), m_brush.Get());
        }
    };
    hoverBg(m_layout.chevron, Hit::Chevron);
    hoverBg(m_layout.quick, Hit::Quick);
    hoverBg(m_layout.clock, Hit::Clock);

    if (m_primary) {
        DrawGlyph(ctx, kGlyphChevron, RF(m_layout.chevron), Color{1.0f, 1.0f, 1.0f, 0.85f});

        wchar_t vg = kGlyphVol3;
        if (m_muted || m_volume <= 0.005f) vg = kGlyphMute;
        else if (m_volume < 0.10f) vg = kGlyphVol0;
        else if (m_volume < 0.34f) vg = kGlyphVol1;
        else if (m_volume < 0.67f) vg = kGlyphVol2;
        const Rect& q = m_layout.quick;
        DrawGlyph(ctx, vg, RF(q.x + 4 * s, q.y, q.x + 38 * s, q.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.92f});

        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100) {
            const float bx = q.x + 40 * s;
            DrawBattery(ctx, bx, q.Center().y, ps.BatteryLifePercent, ps.ACLineStatus == 1);
            wchar_t pct[8];
            swprintf_s(pct, L"%d%%", ps.BatteryLifePercent);
            DrawText(ctx, pct, m_fontDate.Get(), RF(bx + 30 * s, q.y, q.Right(), q.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.85f});
        }
    }

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t tm[16]; swprintf_s(tm, L"%02d:%02d", st.wHour, st.wMinute);
    wchar_t date[48];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, ARRAYSIZE(date), nullptr);
    const Rect& ck = m_layout.clock;
    DrawText(ctx, tm, m_fontTime.Get(), RF(ck.x, ck.y + 2 * s, ck.Right(), ck.y + 24 * s), Color{1.0f, 1.0f, 1.0f, 0.96f});
    DrawText(ctx, date, m_fontDate.Get(), RF(ck.x, ck.y + 22 * s, ck.Right(), ck.Bottom() - 2 * s), Color{1.0f, 1.0f, 1.0f, 0.55f});

    // show-desktop sliver
    {
        const Rect& d = m_layout.desktop;
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, m_hoverKind == Hit::Desktop ? 0.30f : 0.12f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(d.x + 4 * s, d.y, d.Right() - 2 * s, d.Bottom()), 2 * s, 2 * s), m_brush.Get());
    }

    // ---- tooltip ------------------------------------------------------------------------------------
    if (m_tipRect.width > 1.0f) {
        if (auto g = m_dcomp->CreateSquircleGeometry(m_tipRect, 12.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(0.05f, 0.05f, 0.065f, 0.96f * m_tipAlpha));
            ctx->FillGeometry(g.Get(), m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * m_tipAlpha));
            ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.0f * s);
        }
        DrawText(ctx, m_tipText, m_fontTip.Get(), RF(m_tipRect.Left() + 12 * s, m_tipRect.Top(), m_tipRect.Right() - 12 * s, m_tipRect.Bottom()),
                 Color{1.0f, 1.0f, 1.0f, 0.96f * m_tipAlpha});
    }

    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
}

// =============================================================================================
//  Interaction
// =============================================================================================

AetherBar::Hit AetherBar::HitTest(const Vec2& pt, int& index) const {
    index = -1;
    const float lift = (1.0f - Clamp01(m_dockAppear.GetValue())) * 30.0f * m_dpi;
    const Vec2 p{pt.x, pt.y - lift};
    if (m_layout.start.Contains(p)) return Hit::Start;
    for (size_t i = 0; i < m_layout.items.size(); ++i) {
        if (m_entries[i]->appear.GetValue() > 0.5f && m_layout.items[i].Contains(p)) { index = static_cast<int>(i); return Hit::Item; }
    }
    if (m_layout.chevron.width > 0 && m_layout.chevron.Contains(p)) return Hit::Chevron;
    if (m_layout.quick.width > 0 && m_layout.quick.Contains(p)) return Hit::Quick;
    if (m_layout.clock.Contains(p)) return Hit::Clock;
    if (m_layout.desktop.Contains(p)) return Hit::Desktop;
    if (m_layout.right.Contains(p)) return Hit::Right;
    if (m_layout.dock.Contains(p)) return Hit::Dock;
    return Hit::None;
}

void AetherBar::ActivateWindow(HWND hwnd) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD me = GetCurrentThreadId();
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, FALSE);
}

void AetherBar::OnItemClick(Entry& e) {
    if (!e.Running()) {
        ShellExecuteW(nullptr, L"open", e.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    if (e.windows.size() == 1) {
        HWND w = e.windows[0];
        if ((w == fg || w == fgRoot) && !IsIconic(w)) ShowWindow(w, SW_MINIMIZE);
        else ActivateWindow(w);
        return;
    }
    e.cycle = (e.cycle + 1) % static_cast<int>(e.windows.size());
    ActivateWindow(e.windows[e.cycle]);
}

void AetherBar::AdjustVolume(float delta) {
    auto ev = DefaultEndpointVolume();
    if (!ev) return;
    float v = m_volume;
    ev->GetMasterVolumeLevelScalar(&v);
    v = Clamp01(v + delta);
    ev->SetMasterVolumeLevelScalar(v, nullptr);
    if (v > 0.0f) ev->SetMute(FALSE, nullptr);
    m_volume = v; m_muted = false;
    wchar_t buf[32];
    swprintf_s(buf, L"Volume  %d%%", static_cast<int>(v * 100.0f + 0.5f));
    m_tipOverride = buf;
    m_tipOverrideTimer = 1.3f;
}

void AetherBar::ShowItemMenu(int index) {
    if (index < 0 || index >= static_cast<int>(m_entries.size())) return;
    Entry& e = *m_entries[index];
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open new window");
    if (!e.exePath.empty()) AppendMenuW(menu, MF_STRING, 2, e.pinned ? L"Unpin from bar" : L"Pin to bar");
    if (e.Running()) AppendMenuW(menu, MF_STRING, 3, e.windows.size() > 1 ? L"Close all windows" : L"Close window");
    SetForegroundWindow(m_hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    PinStore& store = PinStore::I();
    if (cmd == 1 && !e.exePath.empty()) ShellExecuteW(nullptr, L"open", e.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 2) {
        const int idx = store.IndexOf(e.exePath);
        if (idx >= 0) store.pins.erase(store.pins.begin() + idx); else store.pins.push_back(e.exePath);
        store.Save();
    }
    else if (cmd == 3) for (HWND w : e.windows) PostMessageW(w, WM_CLOSE, 0, 0);
}

void AetherBar::ShowBarMenu() {
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Task Manager");
    AppendMenuW(menu, MF_STRING, 2, L"Windows Settings");
    AppendMenuW(menu, MF_STRING, 3, L"System tray (Win+B)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 9, L"Exit Aether Bar (restore Windows taskbar)");
    SetForegroundWindow(m_hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 2) ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 3) SendWinCombo('B');
    else if (cmd == 9) PostQuitMessage(0);
}

LRESULT AetherBar::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (m_taskbarCreatedMsg && msg == m_taskbarCreatedMsg) {
        PostThreadMessageW(GetCurrentThreadId(), WM_APP + 3, 0, 0);   // manager re-applies the guard
        return 0;
    }
    switch (msg) {
        case WM_APPBAR:
            if (wp == ABN_POSCHANGED) SetAppBarPos();
            else if (wp == ABN_FULLSCREENAPP) {
                m_visible = !lp;
                ShowWindow(hwnd, m_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PostThreadMessageW(GetCurrentThreadId(), WM_APP + 2, 0, 0);   // manager rebuilds all bars
            return 0;
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            int idx;
            return HitTest(Vec2{static_cast<float>(pt.x), static_cast<float>(pt.y)}, idx) == Hit::None ? HTTRANSPARENT : HTCLIENT;
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const bool hand = m_hoverKind != Hit::None && m_hoverKind != Hit::Dock && m_hoverKind != Hit::Right;
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            m_mouse = Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            m_hoverKind = HitTest(m_mouse, m_hoverItem);
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                m_trackingMouse = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            m_trackingMouse = false; m_hoverKind = Hit::None; m_hoverItem = -1; m_mouse = Vec2{-1.0f, -1.0f};
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(pt.x), static_cast<float>(pt.y)}, idx);
            if (m_primary && (h == Hit::Quick || h == Hit::Chevron || h == Hit::Clock || h == Hit::Right)) {
                AdjustVolume(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 0.03f);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx);
            switch (h) {
                case Hit::Start:   if (m_start) m_start->Toggle(m_mon, m_reservedPx); break;
                case Hit::Item:    OnItemClick(*m_entries[idx]); break;
                case Hit::Chevron: SendWinCombo('B'); break;   // reveals the real notification area
                case Hit::Quick:   SendWinCombo('A'); break;   // quick settings (network, volume, brightness ...)
                case Hit::Clock:   SendWinCombo('N'); break;   // notification centre + calendar
                case Hit::Desktop: SendWinCombo('D'); break;
                default: break;
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx);
            if (h == Hit::Item && !m_entries[idx]->exePath.empty()) {
                ShellExecuteW(nullptr, L"open", m_entries[idx]->exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (h == Hit::Quick) {
                if (auto ev = DefaultEndpointVolume()) { ev->SetMute(!m_muted, nullptr); m_muted = !m_muted; }
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx);
            if (h == Hit::Item) ShowItemMenu(idx); else ShowBarMenu();
            return 0;
        }
        case WM_QUERYENDSESSION:
        case WM_ENDSESSION:
            ShellTaskbarGuard::Instance().Restore();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool AetherBar::Frame(float dt) {
    if (!m_hwnd) return false;
    Tick(dt);
    const bool animating = IsAnimating();
    m_idleRender += dt;
    if (animating || m_idleRender >= 0.5f || m_hoverKind != Hit::None) {
        m_idleRender = 0.0f;
        Render();
    }
    return animating;
}

} // namespace aether::bar
