// AetherShell: the session manager of a complete Aether desktop.
//
//  * starts and supervises AetherBar, AetherDesktop and Aether (island)             - restarts them after a crash
//  * runs the logon startup items that explorer.exe normally runs (Run / RunOnce / Startup folders)
//  * provides what explorer.exe provides globally: Alt+Tab, Win+Tab, Win+R, Win+M, Win+I
//  * Shift held at logon (or %APPDATA%\Aether\use-explorer) starts the normal Windows shell instead - the escape hatch
//
// When explorer.exe is still the running shell (testing from a normal session) only the hotkeys / switcher are active.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"
#include "aether/render/mini_gfx.hpp"

using namespace aether;
using aether::mg::Color;
using aether::mg::Rect;
using aether::mg::kWhite;
using aether::mg::WithA;
using aether::mg::Mix;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kMsgSwStart = WM_APP + 1, kMsgSwNext = WM_APP + 2, kMsgSwMove = WM_APP + 3, kMsgSwCommit = WM_APP + 4, kMsgSwCancel = WM_APP + 5;
constexpr UINT kMsgRun = WM_APP + 6, kMsgMinAll = WM_APP + 7, kMsgSettings = WM_APP + 8;
constexpr UINT_PTR kTimerSupervise = 1;

HWND g_ctl = nullptr;
bool g_swActive = false, g_swSticky = false;

std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
std::wstring AppData() {
    wchar_t buf[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(buf) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
bool FileExists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES; }
bool DirExists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

void ForceForeground(HWND h) {
    if (!h) return;
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
    if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
}

void MaskKeys() {                                    // dummy key: stops Start / the menu bar from reacting to a lone Win / Alt release
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

std::wstring ProcessPath(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2] = {}; DWORD n = MAX_PATH * 2;
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) r = buf;
    CloseHandle(p);
    return r;
}

// ---- windows that belong in Alt+Tab / "minimize all" -----------------------------------------------------------------------
bool IsAppWindow(HWND h, bool includeMinimized) {
    if (!IsWindowVisible(h)) return false;
    if (!includeMinimized && IsIconic(h)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if ((ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    BOOL cloaked = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    static const wchar_t* skip[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"AetherBarClass", L"AetherStartMenuClass",
                                    L"AetherDesktopWindow", L"AetherSwitcherWindow", L"AetherRunWindow", L"Windows.UI.Core.CoreWindow"};
    for (auto* s : skip) if (!wcscmp(cls, s)) return false;
    return true;
}
std::vector<HWND> AppWindows(bool includeMinimized) {
    std::vector<HWND> v;
    struct Ctx { std::vector<HWND>* v; bool m; } c{&v, includeMinimized};
    EnumWindows([](HWND h, LPARAM l) -> BOOL { auto* c = reinterpret_cast<Ctx*>(l); if (IsAppWindow(h, c->m)) c->v->push_back(h); return TRUE; }, reinterpret_cast<LPARAM>(&c));
    return v;
}
void Activate(HWND h) {
    if (!h || !IsWindow(h)) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    ForceForeground(h);
}

HMONITOR WorkMonitor() {
    HWND fg = GetForegroundWindow();
    if (fg) return MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    POINT pt; GetCursorPos(&pt);
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

// =============================================================================================================================
//   Alt+Tab / Win+Tab switcher
// =============================================================================================================================
class Switcher {
public:
    void Start(bool sticky, bool reverse);
    void Next(int dir);
    void Move(UINT vk);
    void Commit();
    void Cancel();
    bool Active() const { return m_active; }
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    struct Entry { HWND h; std::wstring title; ComPtr<ID2D1Bitmap1> icon; bool min; };
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    std::vector<Entry> m_items;
    std::map<std::wstring, ComPtr<ID2D1Bitmap1>> m_iconCache;
    int m_sel{0}, m_hover{-1};
    bool m_active{false}, m_shown{false}, m_sticky{false};
    float m_a{0.0f}, m_scale{1.0f}, m_W{0}, m_H{0};
    int m_cols{1}, m_rows{1};
    float m_k{1.0f};
    Rect m_panel{0, 0, 0, 0};
    LARGE_INTEGER m_freq{}, m_last{};

    bool EnsureWindow();
    void BuildList();
    ComPtr<ID2D1Bitmap1> IconFor(HWND h);
    void Show();
    void Hide();
    void Layout();
    Rect Card(int i) const;
    void Render();
    int HitCard(float x, float y) const;
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
};
Switcher g_sw;

bool Switcher::EnsureWindow() {
    if (m_hwnd) return true;
    static bool reg = false;
    if (!reg) {
        reg = true;
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"AetherSwitcherWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    }
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(WorkMonitor(), &mi);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"AetherSwitcherWindow", L"", WS_POPUP,
                             mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return false;
    QueryPerformanceFrequency(&m_freq);
    RECT rc; GetClientRect(m_hwnd, &rc);
    m_dc = std::make_unique<render::DCompLayer>();
    if (!m_dc->Initialize(m_hwnd, rc.right, rc.bottom)) { DestroyWindow(m_hwnd); m_hwnd = nullptr; return false; }
    m_g.Init(m_dc.get());
    return true;
}

ComPtr<ID2D1Bitmap1> Switcher::IconFor(HWND h) {
    auto* ctx = m_dc->GetContext();
    const std::wstring path = ProcessPath(h);
    const bool frame = path.empty() || StrStrIW(path.c_str(), L"ApplicationFrameHost") != nullptr;
    if (!frame) {
        auto it = m_iconCache.find(path);
        if (it != m_iconCache.end()) return it->second;
        int w = 0, hh = 0; std::vector<uint32_t> px;
        ComPtr<ID2D1Bitmap1> bmp;
        if (mg::ShellImage(path, 96, false, w, hh, px)) bmp = mg::BitmapFromPixels(ctx, px, w, hh);
        m_iconCache[path] = bmp;
        if (bmp) return bmp;
    }
    DWORD_PTR r = 0; HICON ic = nullptr;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 60, &r) && r) ic = reinterpret_cast<HICON>(r);
    if (!ic) ic = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON));
    if (!ic && SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 60, &r) && r) ic = reinterpret_cast<HICON>(r);
    return mg::BitmapFromHicon(ctx, ic);
}

void Switcher::BuildList() {
    m_items.clear();
    for (HWND h : AppWindows(true)) {
        wchar_t t[256] = {}; GetWindowTextW(h, t, 256);
        m_items.push_back({h, t, nullptr, IsIconic(h) != FALSE});
    }
}

void Switcher::Start(bool sticky, bool reverse) {
    m_active = true; m_sticky = sticky; m_hover = -1;
    if (!EnsureWindow()) { m_active = false; g_swActive = false; return; }
    BuildList();
    if (m_items.empty()) { m_active = false; g_swActive = false; return; }
    const int n = static_cast<int>(m_items.size());
    m_sel = n > 1 ? (reverse ? n - 1 : 1) : 0;
    if (sticky) Show(); else SetTimer(m_hwnd, 1, 110, nullptr);       // a quick Alt+Tab never shows anything
}

void Switcher::Next(int dir) {
    if (!m_active || m_items.empty()) return;
    const int n = static_cast<int>(m_items.size());
    m_sel = ((m_sel + dir) % n + n) % n;
    if (!m_shown) Show();
    Render();
}

void Switcher::Move(UINT vk) {
    if (!m_active || !m_shown) { if (vk == VK_LEFT || vk == VK_UP) Next(-1); else Next(1); return; }
    const int n = static_cast<int>(m_items.size());
    if (vk == VK_LEFT) Next(-1);
    else if (vk == VK_RIGHT) Next(1);
    else {
        const int step = vk == VK_UP ? -m_cols : m_cols;
        const int t = m_sel + step;
        if (t >= 0 && t < n) { m_sel = t; Render(); }
    }
}

void Switcher::Commit() {
    if (!m_active) return;
    const HWND target = (m_sel >= 0 && m_sel < static_cast<int>(m_items.size())) ? m_items[m_sel].h : nullptr;
    Hide();
    m_active = false; g_swActive = false;
    Activate(target);
}
void Switcher::Cancel() { Hide(); m_active = false; g_swActive = false; }

void Switcher::Show() {
    if (m_shown || !m_hwnd) return;
    for (auto& e : m_items) e.icon = IconFor(e.h);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(WorkMonitor(), &mi);
    const int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    SetWindowPos(m_hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_NOACTIVATE);
    m_dc->Resize(w, h);
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    m_W = w / m_scale; m_H = h / m_scale;
    m_g.ClearFormats();
    Layout();
    m_a = 0.0f; m_shown = true;
    QueryPerformanceCounter(&m_last);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(m_hwnd, 2, 15, nullptr);
    Render();
}

void Switcher::Hide() {
    KillTimer(m_hwnd, 1); KillTimer(m_hwnd, 2);
    if (m_shown) ShowWindow(m_hwnd, SW_HIDE);
    m_shown = false;
}

void Switcher::Layout() {
    const int n = static_cast<int>(m_items.size());
    m_k = 1.0f;
    const float pad = 26.0f, head = 46.0f;
    for (;;) {
        const float cw = 150 * m_k, ch = 158 * m_k, gap = 14 * m_k;
        m_cols = std::max(1, std::min(n, static_cast<int>((m_W * 0.86f - 2 * pad + gap) / (cw + gap))));
        m_rows = (n + m_cols - 1) / m_cols;
        const float ph = 2 * pad + head + m_rows * (ch + gap) - gap;
        if (ph <= m_H * 0.84f || m_k <= 0.55f) break;
        m_k -= 0.05f;
    }
    const float cw = 150 * m_k, ch = 158 * m_k, gap = 14 * m_k;
    const float pw = 2 * pad + m_cols * (cw + gap) - gap, ph = 2 * pad + head + m_rows * (ch + gap) - gap;
    m_panel = Rect{(m_W - pw) / 2, (m_H - ph) / 2, pw, ph};
}

Rect Switcher::Card(int i) const {
    const float cw = 150 * m_k, ch = 158 * m_k, gap = 14 * m_k, pad = 26.0f, head = 46.0f;
    const int col = i % m_cols, row = i / m_cols;
    // the last row is centred
    const int inRow = (row == m_rows - 1) ? static_cast<int>(m_items.size()) - row * m_cols : m_cols;
    const float rowW = inRow * (cw + gap) - gap;
    const float x0 = m_panel.x + (m_panel.width - rowW) / 2;
    return Rect{x0 + col * (cw + gap), m_panel.y + pad + head + row * (ch + gap), cw, ch};
}

int Switcher::HitCard(float x, float y) const {
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) if (Card(i).Contains(x, y)) return i;
    return -1;
}

void Switcher::Render() {
    if (!m_shown || !m_dc) return;
    if (!m_g.Begin()) return;
    auto* ctx = m_g.ctx;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    const float e = 1.0f - (1.0f - m_a) * (1.0f - m_a);
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    m_g.FillRect(Rect{0, 0, m_W, m_H}, Color{0.0f, 0.0f, 0.02f, 0.46f * e});
    Rect p = m_panel; p.y += (1.0f - e) * 16.0f;
    const float dy = p.y - m_panel.y;
    m_g.Glow(p, 22, v, 22.0f, 0.8f * e);
    m_g.VGradientRR(p, 22, Color{0.09f, 0.06f, 0.19f, 0.94f * e}, Color{0.04f, 0.03f, 0.10f, 0.96f * e});
    m_g.StrokeRR(p, 22, WithA(vl, 0.38f * e), 1.2f);
    if (m_sel >= 0 && m_sel < static_cast<int>(m_items.size()))
        m_g.Text(m_items[m_sel].title, Rect{p.x + 26, p.y + 16, p.width - 52, 32}, 17.0f, 600, WithA(kWhite, e), DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        Rect c = Card(i); c.y += dy;
        const bool sel = i == m_sel, hot = i == m_hover;
        if (sel) { m_g.Glow(c, 16, v, 12.0f, e); m_g.FillRR(c, 16, WithA(Mix(v, vl, 0.2f), 0.34f * e)); m_g.StrokeRR(c, 16, WithA(vl, 0.75f * e), 1.4f); }
        else if (hot) { m_g.FillRR(c, 16, WithA(vl, 0.12f * e)); m_g.StrokeRR(c, 16, WithA(vl, 0.28f * e), 1.0f); }
        const float isz = 72 * m_k;
        const Rect ir{c.x + (c.width - isz) / 2, c.y + 16 * m_k, isz, isz};
        const float ia = (m_items[i].min ? 0.62f : 1.0f) * e;
        if (m_items[i].icon) {
            const D2D1_RECT_F dr = mg::D(ir);
            ctx->DrawBitmap(m_items[i].icon.Get(), &dr, ia, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else m_g.FillRR(ir, 14, WithA(kWhite, 0.14f * e));
        m_g.TextBox(m_items[i].title, Rect{c.x + 8, ir.Bottom() + 12 * m_k, c.width - 16, 34 * m_k}, 12.0f, sel ? 600 : 400, WithA(kWhite, (sel ? 1.0f : 0.82f) * e));
    }
    m_g.End();
}

LRESULT CALLBACK Switcher::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Switcher* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Switcher*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else self = reinterpret_cast<Switcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Switcher::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps); Render(); return 0; }
    case WM_TIMER:
        if (w == 1) { KillTimer(m_hwnd, 1); if (m_active && !m_shown) Show(); }
        else if (w == 2) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const float dt = static_cast<float>(now.QuadPart - m_last.QuadPart) / static_cast<float>(m_freq.QuadPart);
            m_last = now;
            m_a = std::min(1.0f, m_a + dt / 0.16f);
            Render();
            if (m_a >= 1.0f) KillTimer(m_hwnd, 2);
        }
        return 0;
    case WM_MOUSEMOVE: {
        const int hit = HitCard(GET_X_LPARAM(l) / m_scale, GET_Y_LPARAM(l) / m_scale);
        if (hit != m_hover) { m_hover = hit; Render(); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(l) / m_scale, y = GET_Y_LPARAM(l) / m_scale;
        const int hit = HitCard(x, y - ((1.0f - m_a) * 16.0f));
        if (hit >= 0) { m_sel = hit; Commit(); }
        else if (!m_panel.Contains(x, y)) Cancel();
        return 0;
    }
    case WM_MBUTTONDOWN: {                         // middle click closes the window under the cursor
        const int hit = HitCard(GET_X_LPARAM(l) / m_scale, GET_Y_LPARAM(l) / m_scale);
        if (hit >= 0) {
            PostMessageW(m_items[hit].h, WM_CLOSE, 0, 0);
            m_items.erase(m_items.begin() + hit);
            if (m_items.empty()) { Cancel(); return 0; }
            m_sel = std::min(m_sel, static_cast<int>(m_items.size()) - 1);
            Layout(); Render();
        }
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// =============================================================================================================================
//   Win+R : run box
// =============================================================================================================================
class RunBox {
public:
    void Open();
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
private:
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    std::wstring m_text, m_err;
    size_t m_caret{0};
    std::vector<std::wstring> m_hist;
    int m_histPos{-1};
    float m_scale{1.0f};
    bool m_ok{false};
    static constexpr float kW = 580.0f, kH = 176.0f;

    bool EnsureWindow();
    void Render();
    void Execute();
    void LoadHist();
    void SaveHist();
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    void Insert(const std::wstring& s) { m_text.insert(m_caret, s); m_caret += s.size(); m_err.clear(); }
};
RunBox g_run;

void RunBox::LoadHist() {
    m_hist.clear();
    const std::wstring p = AppData() + L"\\run.hist";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string b(1 << 16, '\0'); DWORD n = 0;
    ReadFile(h, b.data(), static_cast<DWORD>(b.size() - 1), &n, nullptr); CloseHandle(h); b.resize(n);
    const int wl = MultiByteToWideChar(CP_UTF8, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, wl)), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, b.data(), static_cast<int>(b.size()), w.data(), wl);
    size_t pos = 0;
    while (pos < w.size() && m_hist.size() < 20) {
        size_t e = w.find(L'\n', pos); if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(pos, e - pos); pos = e + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) m_hist.push_back(line);
    }
}
void RunBox::SaveHist() {
    std::wstring w;
    for (const auto& s : m_hist) w += s + L"\n";
    const int bl = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string b(static_cast<size_t>(std::max(0, bl)), '\0');
    if (bl > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), b.data(), bl, nullptr, nullptr);
    HANDLE h = CreateFileW((AppData() + L"\\run.hist").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr); CloseHandle(h);
}

bool RunBox::EnsureWindow() {
    if (m_hwnd) return true;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"AetherRunWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, L"AetherRunWindow", L"Çalıştır", WS_POPUP, 0, 0, 600, 200, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return false;
    m_dc = std::make_unique<render::DCompLayer>();
    if (!m_dc->Initialize(m_hwnd, 600, 200)) { DestroyWindow(m_hwnd); m_hwnd = nullptr; return false; }
    m_g.Init(m_dc.get());
    return true;
}

void RunBox::Open() {
    if (!EnsureWindow()) return;
    LoadHist(); m_text.clear(); m_caret = 0; m_err.clear(); m_histPos = -1;
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(WorkMonitor(), &mi);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 100, 100, SWP_NOACTIVATE | SWP_NOMOVE | SWP_HIDEWINDOW);
    SetWindowPos(m_hwnd, HWND_TOPMOST, mi.rcWork.left, mi.rcWork.top, 100, 100, SWP_NOACTIVATE | SWP_NOSIZE);
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    const int pw = static_cast<int>(kW * m_scale), ph = static_cast<int>(kH * m_scale);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - pw) / 2, y = mi.rcWork.top + static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.3f);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE);
    m_dc->Resize(pw, ph);
    m_g.ClearFormats();
    ShowWindow(m_hwnd, SW_SHOW);
    ForceForeground(m_hwnd);
    SetFocus(m_hwnd);
    Render();
}

void RunBox::Render() {
    if (!m_g.Begin()) return;
    auto* ctx = m_g.ctx;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    const Rect panel{10, 10, kW - 20, kH - 20};
    m_g.Glow(panel, 20, v, 10.0f, 0.8f);
    m_g.VGradientRR(panel, 20, Color{0.09f, 0.06f, 0.19f, 0.97f}, Color{0.04f, 0.03f, 0.10f, 0.98f});
    m_g.StrokeRR(panel, 20, WithA(vl, 0.40f), 1.2f);
    m_g.Glyph(0xE756, Rect{panel.x + 18, panel.y + 12, 30, 30}, 17.0f, vl);
    m_g.Text(L"Çalıştır", Rect{panel.x + 52, panel.y + 12, 200, 30}, 16.0f, 600, kWhite);
    const Rect field{panel.x + 20, panel.y + 56, panel.width - 40, 44};
    m_g.FillRR(field, 12, Color{0.0f, 0.0f, 0.0f, 0.38f});
    m_g.StrokeRR(field, 12, WithA(vl, 0.75f), 1.4f);
    const Rect tx{field.x + 14, field.y, field.width - 28, field.height};
    if (m_text.empty()) m_g.Text(L"Program, klasör, belge veya web adresi yazın", tx, 14.0f, 400, WithA(kWhite, 0.38f));
    else m_g.Text(m_text, tx, 15.0f, 400, kWhite);
    if ((GetTickCount64() / 500) % 2 == 0 || true) {
        const float cx = m_g.TextWidth(m_text.substr(0, m_caret), 15.0f, 400);
        m_g.FillRR(Rect{tx.x + cx + 0.5f, field.y + 11, 1.6f, 22}, 0.8f, kWhite);
    }
    if (!m_err.empty()) m_g.Text(m_err, Rect{panel.x + 22, panel.y + 106, panel.width - 44, 24}, 12.5f, 400, Color{1.0f, 0.55f, 0.55f, 1.0f});
    else m_g.Text(L"Enter: çalıştır      Esc: kapat      ↑ ↓: geçmiş", Rect{panel.x + 22, panel.y + 106, panel.width - 44, 24}, 12.0f, 400, WithA(kWhite, 0.45f));
    m_g.End();
}

void RunBox::Execute() {
    std::wstring cmd = m_text;
    while (!cmd.empty() && iswspace(cmd.front())) cmd.erase(0, 1);
    while (!cmd.empty() && iswspace(cmd.back())) cmd.pop_back();
    if (cmd.empty()) return;
    wchar_t ex[4096] = {};
    ExpandEnvironmentStringsW(cmd.c_str(), ex, 4096);
    const std::wstring expanded = ex;
    std::wstring file, args;
    if (!expanded.empty() && expanded[0] == L'"') {
        const size_t q = expanded.find(L'"', 1);
        file = expanded.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
        if (q != std::wstring::npos) args = expanded.substr(q + 1);
    } else if (FileExists(expanded)) file = expanded;
    else {
        const size_t sp = expanded.find(L' ');
        file = expanded.substr(0, sp);
        if (sp != std::wstring::npos) args = expanded.substr(sp + 1);
    }
    while (!args.empty() && args.front() == L' ') args.erase(0, 1);
    bool ok = false;
    if (DirExists(file)) {                                        // folders open in Aether Explorer
        const std::wstring ae = ExeDir() + L"\\AetherExplorer.exe";
        const std::wstring a = L"\"" + file + L"\"";
        ok = FileExists(ae) ? reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", ae.c_str(), a.c_str(), nullptr, SW_SHOWNORMAL)) > 32
                            : reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    } else {
        ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
    }
    if (!ok) { m_err = L"“" + file + L"” bulunamadı. Adı doğru yazdığınızdan emin olun."; Render(); return; }
    m_hist.erase(std::remove(m_hist.begin(), m_hist.end(), cmd), m_hist.end());
    m_hist.insert(m_hist.begin(), cmd);
    if (m_hist.size() > 20) m_hist.resize(20);
    SaveHist();
    ShowWindow(m_hwnd, SW_HIDE);
}

LRESULT CALLBACK RunBox::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    RunBox* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<RunBox*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else self = reinterpret_cast<RunBox*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT RunBox::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps); Render(); return 0; }
    case WM_KILLFOCUS: if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE); return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (w) {
            case VK_ESCAPE: ShowWindow(m_hwnd, SW_HIDE); return 0;
            case VK_RETURN: Execute(); return 0;
            case VK_LEFT: if (m_caret) --m_caret; break;
            case VK_RIGHT: if (m_caret < m_text.size()) ++m_caret; break;
            case VK_HOME: m_caret = 0; break;
            case VK_END: m_caret = m_text.size(); break;
            case VK_DELETE: if (m_caret < m_text.size()) { m_text.erase(m_caret, 1); m_err.clear(); } break;
            case VK_UP: if (m_histPos + 1 < static_cast<int>(m_hist.size())) { m_text = m_hist[++m_histPos]; m_caret = m_text.size(); m_err.clear(); } break;
            case VK_DOWN:
                if (m_histPos > 0) { m_text = m_hist[--m_histPos]; m_caret = m_text.size(); }
                else if (m_histPos == 0) { m_histPos = -1; m_text.clear(); m_caret = 0; }
                break;
            case 'V':
                if (ctrl && OpenClipboard(m_hwnd)) {
                    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
                        if (auto* p = static_cast<const wchar_t*>(GlobalLock(hd))) {
                            std::wstring s = p; GlobalUnlock(hd);
                            s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }), s.end());
                            Insert(s);
                        }
                    }
                    CloseClipboard();
                }
                break;
            case 'A': if (ctrl) m_caret = m_text.size(); break;
        }
        Render();
        return 0;
    }
    case WM_CHAR:
        if (w == 8) { if (m_caret) { m_text.erase(--m_caret, 1); m_err.clear(); } }
        else if (w >= 32 && w != 127) Insert(std::wstring(1, static_cast<wchar_t>(w)));
        Render();
        return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// =============================================================================================================================
//   the global keyboard hook
// =============================================================================================================================
HHOOK g_hook = nullptr;

LRESULT CALLBACK KeyHook(int n, WPARAM w, LPARAM l) {
    if (n != HC_ACTION) return CallNextHookEx(nullptr, n, w, l);
    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
    if (k->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, n, w, l);
    const bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
    const DWORD vk = k->vkCode;
    auto pressed = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
    const bool alt = (k->flags & LLKHF_ALTDOWN) != 0, win = pressed(VK_LWIN) || pressed(VK_RWIN), ctrl = pressed(VK_CONTROL), shift = pressed(VK_SHIFT);

    if (g_swActive) {
        if (vk == VK_TAB) { if (down) PostMessageW(g_ctl, kMsgSwNext, shift ? static_cast<WPARAM>(-1) : 1, 0); return 1; }
        if (vk == VK_ESCAPE) { if (down) PostMessageW(g_ctl, kMsgSwCancel, 0, 0); return 1; }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) { if (down) PostMessageW(g_ctl, kMsgSwMove, vk, 0); return 1; }
        if (g_swSticky && (vk == VK_RETURN || vk == VK_SPACE)) { if (down) PostMessageW(g_ctl, kMsgSwCommit, 0, 0); return 1; }
        if (!g_swSticky && !down && (vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU)) {       // Alt released: switch
            g_swActive = false;
            PostMessageW(g_ctl, kMsgSwCommit, 0, 0);
            MaskKeys();
        }
        return CallNextHookEx(nullptr, n, w, l);
    }
    if (down && vk == VK_TAB && alt && !ctrl && !win) {
        g_swActive = true; g_swSticky = false;
        PostMessageW(g_ctl, kMsgSwStart, 0, shift ? 1 : 0);
        return 1;
    }
    if (vk == VK_TAB && win && !ctrl && !alt) {
        if (down) { g_swActive = true; g_swSticky = true; MaskKeys(); PostMessageW(g_ctl, kMsgSwStart, 1, shift ? 1 : 0); }
        return 1;
    }
    if (win && !ctrl && !alt) {
        if (vk == 'R') { if (down) { MaskKeys(); PostMessageW(g_ctl, kMsgRun, 0, 0); } return 1; }
        if (vk == 'M') { if (down) { MaskKeys(); PostMessageW(g_ctl, kMsgMinAll, shift ? 1 : 0, 0); } return 1; }
        if (vk == 'I') { if (down) { MaskKeys(); PostMessageW(g_ctl, kMsgSettings, 0, 0); } return 1; }
    }
    return CallNextHookEx(nullptr, n, w, l);
}

// =============================================================================================================================
//   supervisor + startup items
// =============================================================================================================================
struct Proc {
    std::wstring exe;
    bool required{false};
    HANDLE h{nullptr};
    int crashes{0};
    ULONGLONG windowStart{0};
};
std::vector<Proc> g_procs;
bool g_realShell = false, g_fallbackStarted = false;
std::vector<HWND> g_minimized;

bool StartProc(Proc& p) {
    if (!FileExists(p.exe)) return false;
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    std::wstring cl = L"\"" + p.exe + L"\"";
    const std::wstring dir = ExeDir();
    if (!CreateProcessW(p.exe.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    if (p.h) CloseHandle(p.h);
    p.h = pi.hProcess;
    return true;
}

void StartExplorerFallback() {
    if (g_fallbackStarted) return;
    g_fallbackStarted = true;
    wchar_t sys[MAX_PATH]; GetWindowsDirectoryW(sys, MAX_PATH);
    const std::wstring ex = std::wstring(sys) + L"\\explorer.exe";
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    std::wstring cl = L"\"" + ex + L"\"";
    if (CreateProcessW(ex.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

void Supervise() {
    const ULONGLONG now = GetTickCount64();
    for (auto& p : g_procs) {
        if (!p.h) continue;
        if (WaitForSingleObject(p.h, 0) != WAIT_OBJECT_0) continue;
        DWORD code = 0; GetExitCodeProcess(p.h, &code);
        CloseHandle(p.h); p.h = nullptr;
        if (code == 0) continue;                                   // closed on purpose: stay closed
        if (now - p.windowStart > 120000) { p.windowStart = now; p.crashes = 0; }
        if (++p.crashes > 4) {                                      // crash loop
            if (p.required && g_realShell) StartExplorerFallback();
            continue;
        }
        Sleep(400);
        StartProc(p);
    }
}

// ---- logon startup items (what explorer.exe does) -------------------------------------------------------------------------
bool StartupApproved(HKEY root, const wchar_t* sub, const std::wstring& name) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return true;
    BYTE data[16] = {}; DWORD sz = sizeof(data), type = 0;
    const LONG r = RegQueryValueExW(k, name.c_str(), nullptr, &type, data, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_BINARY || sz < 1) return true;
    return (data[0] & 1) == 0;                                      // 02 / 06 = enabled, 03 / 07 = disabled by the user
}

void RunCommand(const std::wstring& raw) {
    if (raw.empty()) return;
    wchar_t ex[4096] = {};
    ExpandEnvironmentStringsW(raw.c_str(), ex, 4096);
    std::wstring cl = ex;
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return; }
    ShellExecuteW(nullptr, L"open", cl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RunRegKey(HKEY root, const wchar_t* sub, REGSAM view, const wchar_t* approvedSub, bool once) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | (once ? KEY_WRITE : 0) | view, &k) != ERROR_SUCCESS) return;
    std::vector<std::pair<std::wstring, std::wstring>> items;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512]; DWORD nl = 512; BYTE data[8192]; DWORD dl = sizeof(data), type = 0;
        const LONG r = RegEnumValueW(k, i, name, &nl, nullptr, &type, data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        items.emplace_back(name, reinterpret_cast<wchar_t*>(data));
    }
    for (auto& it : items) {
        if (approvedSub && !StartupApproved(root, approvedSub, it.first)) continue;
        if (once) RegDeleteValueW(k, it.first.c_str());               // RunOnce entries are consumed before they run
        RunCommand(it.second);
        Sleep(120);
    }
    RegCloseKey(k);
}

void RunStartupFolder(const KNOWNFOLDERID& id, HKEY approvedRoot, const wchar_t* approvedSub) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) return;
    const std::wstring dir = p; CoTaskMemFree(p);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
        if (!StartupApproved(approvedRoot, approvedSub, fd.cFileName)) continue;
        ShellExecuteW(nullptr, L"open", (dir + L"\\" + fd.cFileName).c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        Sleep(150);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void RunStartupItems() {
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Sleep(2500);                                                // let the bar / desktop come up first, like Windows does
        const wchar_t* approvedBase = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved";
        const std::wstring run = std::wstring(approvedBase) + L"\\Run", run32 = std::wstring(approvedBase) + L"\\Run32", folder = std::wstring(approvedBase) + L"\\StartupFolder";
        RunRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", KEY_WOW64_64KEY, nullptr, true);
        RunRegKey(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0, nullptr, true);
        RunRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_64KEY, run.c_str(), false);
        RunRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_32KEY, run32.c_str(), false);
        RunRegKey(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, run.c_str(), false);
        RunStartupFolder(FOLDERID_CommonStartup, HKEY_LOCAL_MACHINE, folder.c_str());
        RunStartupFolder(FOLDERID_Startup, HKEY_CURRENT_USER, folder.c_str());
        CoUninitialize();
    }).detach();
}

// ---- Win+M ------------------------------------------------------------------------------------------------------------------
void MinimizeAll(bool restore) {
    if (restore) {
        for (auto it = g_minimized.rbegin(); it != g_minimized.rend(); ++it) if (IsWindow(*it) && IsIconic(*it)) ShowWindow(*it, SW_RESTORE);
        g_minimized.clear();
        return;
    }
    g_minimized.clear();
    for (HWND h : AppWindows(false)) if (GetWindowLongPtrW(h, GWL_STYLE) & WS_MINIMIZEBOX) { g_minimized.push_back(h); ShowWindow(h, SW_SHOWMINNOACTIVE); }
}

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case kMsgSwStart: g_sw.Start(w != 0, l != 0); return 0;
    case kMsgSwNext: g_sw.Next(static_cast<int>(static_cast<INT_PTR>(w))); return 0;
    case kMsgSwMove: g_sw.Move(static_cast<UINT>(w)); return 0;
    case kMsgSwCommit: g_sw.Commit(); return 0;
    case kMsgSwCancel: g_sw.Cancel(); return 0;
    case kMsgRun: g_run.Open(); return 0;
    case kMsgMinAll: MinimizeAll(w != 0); return 0;
    case kMsgSettings: ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL); return 0;
    case WM_TIMER: if (w == kTimerSupervise) Supervise(); return 0;
    case WM_ENDSESSION: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Aether.Shell.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetCurrentProcessExplicitAppUserModelID(L"Aether.Shell");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_realShell = GetShellWindow() == nullptr;                      // nobody is the shell yet: we are being started by Windows
    if (g_realShell) {
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift || FileExists(AppData() + L"\\use-explorer")) { StartExplorerFallback(); return 0; }   // the escape hatch
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = CtlProc; wc.hInstance = inst; wc.lpszClassName = L"AetherShellControl";
    RegisterClassExW(&wc);
    g_ctl = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_ctl) return 1;

    const std::wstring dir = ExeDir();
    if (g_realShell && !FileExists(AppData() + L"\\no-intro")) {          // greeting animation at logon; the desktop loads behind it
        const std::wstring intro = dir + L"\\AetherIntro.exe";
        if (FileExists(intro)) {
            STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
            std::wstring cl = L"\"" + intro + L"\"";
            if (CreateProcessW(intro.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        }
    }
    g_procs.push_back({dir + L"\\AetherBar.exe", true});
    g_procs.push_back({dir + L"\\AetherDesktop.exe", false});
    g_procs.push_back({dir + L"\\Aether.exe", false});
    for (auto& p : g_procs) {
        p.windowStart = GetTickCount64();
        if (StartProc(p)) Sleep(250);
        else if (p.required && g_realShell) StartExplorerFallback();   // no bar = no usable desktop: fall back to Windows' own
    }
    SetTimer(g_ctl, kTimerSupervise, 2000, nullptr);
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    if (g_realShell) RunStartupItems();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}
