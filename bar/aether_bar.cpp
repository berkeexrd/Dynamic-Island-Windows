#include "aether/bar/aether_bar.hpp"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <ctime>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;

namespace {

constexpr float kDockH = 56.0f;        // logical px
constexpr float kMargin = 10.0f;       // gap between pill and screen edge
constexpr float kReserved = 74.0f;     // band reserved for maximised windows
constexpr float kTipRoom = 90.0f;      // transparent room above the band for tooltips
constexpr float kItem = 44.0f;
constexpr float kGap = 4.0f;
constexpr float kPad = 8.0f;
constexpr ULONGLONG kIconRefreshMs = 8000;

inline D2D1_RECT_F RF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

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

// Same rules the Windows taskbar uses to decide which windows deserve a button
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
        !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    return true;
}

struct EnumCtx { std::vector<HWND> out; HWND self; };
BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    if (IsTaskWindow(h, c->self)) c->out.push_back(h);
    return TRUE;
}

std::wstring PinsPath() {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
    std::wstring dir = std::wstring(appdata) + L"\\Aether";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\bar_pins.txt";
}

HICON WindowIcon(HWND h) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    if (HICON i = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON))) return i;
    return reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICONSM));
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

void ShellTaskbarGuard::Hide() {
    if (m_hidden) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = tray;
    m_prevState = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    // Auto-hide releases the work area it reserved, so our own bar can claim it.
    abd.lParam = static_cast<LPARAM>(m_prevState | ABS_AUTOHIDE);
    SHAppBarMessage(ABM_SETSTATE, &abd);
    ForEachShellBar([](HWND h) { ShowWindow(h, SW_HIDE); });
    m_hidden = true;
}

void ShellTaskbarGuard::KeepHidden() {
    if (!m_hidden) return;
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

AetherBar::AetherBar()
    : m_startHover(0.0f, animation::SpringConfig{620.0f, 36.0f, 1.0f, 0.002f})
    , m_dockAppear(0.0f, animation::SpringConfig{300.0f, 28.0f, 1.0f, 0.002f}) {}

AetherBar::~AetherBar() {
    UnregisterAppBar();
    ShellTaskbarGuard::Instance().Restore();
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
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    m_monitor = mi.rcMonitor;

    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
    m_dpi = dx / 96.0f;

    m_windowW = m_monitor.right - m_monitor.left;
    m_windowH = static_cast<int>((kReserved + kTipRoom) * m_dpi);
    m_reservedPx = static_cast<int>(kReserved * m_dpi);
    if (m_hwnd) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, m_monitor.left, m_monitor.bottom - m_windowH, m_windowW, m_windowH,
                     SWP_NOACTIVATE);
    }
}

bool AetherBar::Create() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AetherBarClass";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    PositionWindow();
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Aether Bar", WS_POPUP, m_monitor.left, m_monitor.bottom - m_windowH,
                             m_windowW, m_windowH, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, m_windowW, m_windowH)) return false;
    m_brush = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1});
    CreateFonts();

    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    LoadPins();

    ShellTaskbarGuard::Instance().Hide();
    RegisterAppBar();
    RefreshWindows();
    m_dockAppear.SetTarget(1.0f);
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
    abd.rc = {m_monitor.left, m_monitor.bottom - m_reservedPx, m_monitor.right, m_monitor.bottom};
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - m_reservedPx;
    SHAppBarMessage(ABM_SETPOS, &abd);   // maximised windows now stop above our band
}

void AetherBar::CreateFonts() {
    auto dw = m_dcomp->GetDWriteFactory();
    if (!dw) return;
    auto make = [&](ComPtr<IDWriteTextFormat>& f, DWRITE_FONT_WEIGHT w, float size, DWRITE_TEXT_ALIGNMENT a) {
        f.Reset();
        const wchar_t* family = L"Segoe UI Variable Display";
        ComPtr<IDWriteFontCollection> col;
        UINT32 idx = 0; BOOL ex = FALSE;
        if (SUCCEEDED(dw->GetSystemFontCollection(&col, FALSE)) && col) col->FindFamilyName(family, &idx, &ex);
        if (!ex) family = L"Segoe UI";
        dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             size * m_dpi, L"en-us", &f);
        if (f) {
            f->SetTextAlignment(a);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };
    make(m_fontTip,  DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(m_fontTime, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(m_fontDate, DWRITE_FONT_WEIGHT_NORMAL,    11.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (m_fontTip) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ell;
        dw->CreateEllipsisTrimmingSign(m_fontTip.Get(), &ell);
        m_fontTip->SetTrimming(&trim, ell.Get());
    }
}

// =============================================================================================
//  Pins & running windows
// =============================================================================================

void AetherBar::LoadPins() {
    std::vector<std::wstring> pins;
    const std::wstring path = PinsPath();
    if (FILE* f = _wfopen(path.c_str(), L"rt, ccs=UTF-8")) {
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
    for (const auto& p : pins) {
        auto e = std::make_unique<Entry>();
        e->key = p; e->exePath = p; e->label = FileTitle(p); e->pinned = true;
        m_entries.push_back(std::move(e));
    }
}

void AetherBar::SavePins() {
    const std::wstring path = PinsPath();
    if (FILE* f = _wfopen(path.c_str(), L"wt, ccs=UTF-8")) {
        for (const auto& e : m_entries) if (e->pinned) fwprintf(f, L"%ls\n", e->exePath.c_str());
        fclose(f);
    }
}

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
        if (!anyAlpha) for (int i = 0; i < px * px; ++i) if (p[i] & 0x00FFFFFF) p[i] |= 0xFF000000; // legacy icons
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
    EnumCtx ctx{{}, m_hwnd};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));

    for (auto& e : m_entries) e->windows.clear();

    for (HWND h : ctx.out) {
        std::wstring exe = ExePathOf(h);
        std::wstring key = exe;
        // All UWP apps live inside ApplicationFrameHost.exe - give each window its own button
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
        if (!e->Running() && !e->pinned) e->appear.SetTarget(0.0f);
        else e->appear.SetTarget(1.0f);
        if (e->Running()) e->label = WindowTitle(e->windows.front()).empty() ? e->label : WindowTitle(e->windows.front());
        if (!e->icon || now - e->iconStamp > kIconRefreshMs) {
            auto bmp = LoadIcon(*e);
            if (bmp) e->icon = bmp;
            e->iconStamp = now;
        }
    }
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [](const std::unique_ptr<Entry>& e) {
        return !e->pinned && !e->Running() && e->appear.GetValue() < 0.02f;
    }), m_entries.end());
}

// =============================================================================================
//  Update / layout
// =============================================================================================

bool AetherBar::IsAnimating() const {
    if (!m_startHover.IsResting() || !m_dockAppear.IsResting()) return true;
    for (const auto& e : m_entries) {
        if (!e->appear.IsResting() || !e->hover.IsResting() || !e->active.IsResting()) return true;
    }
    return m_tooltipAlpha > 0.01f && m_hoverItem < 0 ? true : (m_hoverItem >= 0 && m_tooltipAlpha < 0.99f);
}

void AetherBar::Tick(float dt) {
    m_refreshTimer += dt;
    if (m_refreshTimer >= 0.35f) { m_refreshTimer = 0.0f; RefreshWindows(); }
    m_keepHiddenTimer += dt;
    if (m_keepHiddenTimer >= 0.6f) { m_keepHiddenTimer = 0.0f; ShellTaskbarGuard::Instance().KeepHidden(); }

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

    const float tipTarget = (m_hoverKind == Hit::Item) ? 1.0f : 0.0f;
    m_tooltipAlpha += (tipTarget - m_tooltipAlpha) * (1.0f - std::exp(-dt * 16.0f));
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
        const float a = m_entries[i]->appear.GetValue();
        m_layout.items[i] = Rect{x, top + (h - kItem * s) * 0.5f, kItem * s, kItem * s};
        x += (kItem + kGap) * s * a;
    }

    SYSTEM_POWER_STATUS ps{};
    const bool battery = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100;
    const float rightW = (battery ? 190.0f : 130.0f) * s;
    m_layout.right = Rect{static_cast<float>(m_windowW) - kMargin * s * 1.4f - rightW, top, rightW, h};
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

void AetherBar::DrawPill(ID2D1DeviceContext* ctx, const Rect& r, float radius) {
    const float s = m_dpi;
    // layered soft shadow
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
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    const float s = m_dpi;
    const float dockA = Clamp01(m_dockAppear.GetValue());
    const float lift = (1.0f - dockA) * 30.0f * s;
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(0, lift));

    // ---- main dock ---------------------------------------------------------------------------
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

        // running indicator: dot, stretching into a pill when the app is active
        if (e.Running()) {
            const float dw = (4.0f + 12.0f * act) * s, dh = 3.6f * s;
            const float dy = slot.Bottom() - 3.0f * s;
            m_brush->SetColor(D2D1::ColorF(act > 0.5f ? 0.38f : 1.0f, act > 0.5f ? 0.80f : 1.0f, 1.0f, (0.50f + 0.45f * act) * appear));
            ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(c.x - dw / 2, dy - dh, c.x + dw / 2, dy), dh / 2, dh / 2), m_brush.Get());
        }
    }

    // ---- system pill: battery, clock, date ------------------------------------------------------
    DrawPill(ctx, m_layout.right, 24.0f * s);
    {
        SYSTEM_POWER_STATUS ps{};
        const bool battery = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100;
        float textLeft = m_layout.right.Left();
        if (battery) {
            const float bx = m_layout.right.Left() + 18.0f * s;
            DrawBattery(ctx, bx, m_layout.right.Center().y, ps.BatteryLifePercent, ps.ACLineStatus == 1);
            wchar_t pct[8];
            swprintf_s(pct, L"%d%%", ps.BatteryLifePercent);
            DrawText(ctx, pct, m_fontDate.Get(), RF(bx + 30 * s, m_layout.right.Top(), bx + 76 * s, m_layout.right.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.85f});
            textLeft = m_layout.right.Left() + 70.0f * s;
        }
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t tm[16]; swprintf_s(tm, L"%02d:%02d", st.wHour, st.wMinute);
        wchar_t date[48];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, ARRAYSIZE(date), nullptr);
        const float top = m_layout.right.Top();
        DrawText(ctx, tm, m_fontTime.Get(), RF(textLeft, top + 8 * s, m_layout.right.Right(), top + 30 * s), Color{1.0f, 1.0f, 1.0f, 0.96f});
        DrawText(ctx, date, m_fontDate.Get(), RF(textLeft, top + 28 * s, m_layout.right.Right(), top + 46 * s), Color{1.0f, 1.0f, 1.0f, 0.55f});
    }

    // ---- tooltip ----------------------------------------------------------------------------------
    if (m_tooltipAlpha > 0.02f && m_hoverItem >= 0 && m_hoverItem < static_cast<int>(m_entries.size())) {
        const Entry& e = *m_entries[m_hoverItem];
        const Rect& slot = m_layout.items[m_hoverItem];
        auto dw = m_dcomp->GetDWriteFactory();
        ComPtr<IDWriteTextLayout> tl;
        const std::wstring text = e.label.empty() ? L"App" : e.label;
        float tw = 0;
        if (dw && m_fontTip && SUCCEEDED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), m_fontTip.Get(), 4096, 40, &tl))) {
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); tw = m.widthIncludingTrailingWhitespace;
        }
        const float w = std::min(tw + 28.0f * s, 320.0f * s), h = 30.0f * s;
        Rect tip{slot.Center().x - w / 2, m_layout.dock.Top() - h - 10.0f * s + (1.0f - m_tooltipAlpha) * 6.0f * s, w, h};
        tip.x = std::clamp(tip.x, 8.0f * s, static_cast<float>(m_windowW) - w - 8.0f * s);
        if (auto g = m_dcomp->CreateSquircleGeometry(tip, 12.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(0.05f, 0.05f, 0.065f, 0.96f * m_tooltipAlpha));
            ctx->FillGeometry(g.Get(), m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * m_tooltipAlpha));
            ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.0f * s);
        }
        DrawText(ctx, text, m_fontTip.Get(), RF(tip.Left() + 12 * s, tip.Top(), tip.Right() - 12 * s, tip.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.96f * m_tooltipAlpha});
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
    if (m_layout.right.Contains(p) || m_layout.dock.Contains(p)) return Hit::Right;
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
    // several windows: cycle through them
    e.cycle = (e.cycle + 1) % static_cast<int>(e.windows.size());
    ActivateWindow(e.windows[e.cycle]);
}

void AetherBar::ShowItemMenu(int index) {
    if (index < 0 || index >= static_cast<int>(m_entries.size())) return;
    Entry& e = *m_entries[index];
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open new window");
    AppendMenuW(menu, MF_STRING, 2, e.pinned ? L"Unpin from bar" : L"Pin to bar");
    if (e.Running()) AppendMenuW(menu, MF_STRING, 3, e.windows.size() > 1 ? L"Close all windows" : L"Close window");
    SetForegroundWindow(m_hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 1 && !e.exePath.empty()) ShellExecuteW(nullptr, L"open", e.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 2 && !e.exePath.empty()) { e.pinned = !e.pinned; SavePins(); }
    else if (cmd == 3) for (HWND w : e.windows) PostMessageW(w, WM_CLOSE, 0, 0);
}

void AetherBar::ShowBarMenu() {
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Task Manager");
    AppendMenuW(menu, MF_STRING, 2, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 9, L"Exit Aether Bar (restore Windows taskbar)");
    SetForegroundWindow(m_hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 2) ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 9) PostQuitMessage(0);
}

LRESULT AetherBar::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == m_taskbarCreatedMsg && m_taskbarCreatedMsg) {
        // Explorer restarted: hide its fresh taskbar again
        ShellTaskbarGuard::Instance().Restore();
        ShellTaskbarGuard::Instance().Hide();
        RegisterAppBar();
        return 0;
    }
    switch (msg) {
        case WM_APPBAR:
            if (wp == ABN_POSCHANGED) SetAppBarPos();
            else if (wp == ABN_FULLSCREENAPP) {
                m_visible = !lp; // hide behind full-screen games / videos
                ShowWindow(hwnd, m_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PositionWindow();
            if (m_dcomp) { m_dcomp->Resize(m_windowW, m_windowH); CreateFonts(); }
            SetAppBarPos();
            return 0;
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            int idx; 
            const Hit h = HitTest(Vec2{static_cast<float>(pt.x), static_cast<float>(pt.y)}, idx);
            return h == Hit::None ? HTTRANSPARENT : HTCLIENT;   // empty areas click through
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
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
        case WM_LBUTTONUP: {
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx);
            if (h == Hit::Start) {
                // Opens the Windows Start menu (it keeps working while the stock taskbar is hidden)
                INPUT in[2] = {};
                in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_LWIN;
                in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_LWIN; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
            } else if (h == Hit::Item) {
                OnItemClick(*m_entries[idx]);
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            int idx;
            if (HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx) == Hit::Item
                && !m_entries[idx]->exePath.empty()) {
                ShellExecuteW(nullptr, L"open", m_entries[idx]->exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int AetherBar::Run() {
    LARGE_INTEGER freq, last; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last);
    bool running = true;
    float idleTimer = 0.0f;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart);
        last = now;
        dt = std::min(dt, 0.1f);

        Tick(dt);
        const bool animating = IsAnimating();
        idleTimer += dt;
        if (animating || idleTimer >= 0.5f || m_hoverKind != Hit::None) {
            idleTimer = 0.0f;
            Render();
        }
        if (!animating) MsgWaitForMultipleObjectsEx(0, nullptr, 120, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return 0;
}

} // namespace aether::bar
