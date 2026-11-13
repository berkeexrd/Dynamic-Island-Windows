#include "aether/bar/start_menu.hpp"
#include "aether/core/monitors.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <powrprof.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstdlib>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "advapi32.lib")

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;

namespace {

constexpr int   kCols = 6;
constexpr float kPanelW = 640.0f;
constexpr float kPanelMaxH = 720.0f;
constexpr float kInner = 24.0f;
constexpr float kSearchTop = 20.0f;
constexpr float kSearchH = 46.0f;
constexpr float kFooterH = 68.0f;
constexpr float kTileH = 92.0f;
constexpr float kHeaderH = 36.0f;
constexpr float kLetterH = 30.0f;

constexpr wchar_t kIconSearch = 0xE721;
constexpr wchar_t kIconPower = 0xE7E8;
constexpr wchar_t kIconSettings = 0xE713;

inline D2D1_RECT_F RF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring DataPath(const wchar_t* file) {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
    std::wstring dir = std::wstring(appdata) + L"\\Aether";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + file;
}

void EnableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp = {};
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    }
    CloseHandle(token);
}

} // namespace

StartMenu::StartMenu()
    : m_open(0.0f, animation::SpringConfig{520.0f, 36.0f, 1.0f, 0.002f})
    , m_scroll(0.0f, animation::SpringConfig{380.0f, 34.0f, 1.0f, 0.05f}) {
    wchar_t name[256];
    DWORD len = ARRAYSIZE(name);
    m_userName = GetUserNameW(name, &len) ? name : L"User";
}

StartMenu::~StartMenu() {
    if (m_loader.joinable()) m_loader.join();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

// =============================================================================================
//  App enumeration (shell:AppsFolder = exactly what the stock Start menu lists)
// =============================================================================================

void StartMenu::PreloadAsync() {
    if (m_loadState.load() != 0) return;
    m_loadState = 1;
    m_loader = std::thread([this] {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        std::vector<std::pair<std::wstring, std::wstring>> raw;
        {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) {
                ComPtr<IEnumShellItems> en;
                if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
                    ComPtr<IShellItem> item;
                    ULONG fetched = 0;
                    while (en->Next(1, &item, &fetched) == S_OK && fetched == 1) {
                        PWSTR name = nullptr, parse = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
                            SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parse)) && name && parse) {
                            raw.emplace_back(name, parse);
                        }
                        if (name) CoTaskMemFree(name);
                        if (parse) CoTaskMemFree(parse);
                        item.Reset();
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_loadMutex);
            m_loadedRaw = std::move(raw);
        }
        m_loadState = 2;
        if (com) CoUninitialize();
    });
}

void StartMenu::AdoptLoadedApps() {
    if (m_adopted || m_loadState.load() != 2) return;
    std::vector<std::pair<std::wstring, std::wstring>> raw;
    {
        std::lock_guard<std::mutex> lock(m_loadMutex);
        raw = std::move(m_loadedRaw);
    }
    for (auto& [name, parse] : raw) {
        const std::wstring lower = Lower(name);
        if (lower.find(L"uninstall") != std::wstring::npos) continue;
        App a;
        a.name = name; a.lower = lower; a.parse = parse;
        m_apps.push_back(std::move(a));
    }
    std::sort(m_apps.begin(), m_apps.end(), [](const App& a, const App& b) { return a.lower < b.lower; });
    m_adopted = true;
    LoadPins();
    BuildRows();
}

void StartMenu::LoadPins() {
    m_pins.clear();
    bool fromFile = false;
    if (FILE* f = _wfopen(DataPath(L"start_pins.txt").c_str(), L"rt, ccs=UTF-8")) {
        fromFile = true;
        wchar_t line[1024];
        while (fgetws(line, ARRAYSIZE(line), f)) {
            std::wstring s = line;
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            if (!s.empty()) m_pins.push_back(s);
        }
        fclose(f);
    }
    if (fromFile) return;
    const wchar_t* prefer[] = {L"edge", L"file explorer", L"settings", L"calculator", L"notepad", L"terminal",
                               L"photos", L"store", L"paint", L"camera", L"clock", L"snipping"};
    for (const wchar_t* key : prefer) {
        for (const auto& a : m_apps) {
            if (a.lower.find(key) != std::wstring::npos &&
                std::find(m_pins.begin(), m_pins.end(), a.parse) == m_pins.end()) {
                m_pins.push_back(a.parse);
                break;
            }
        }
    }
}

void StartMenu::SavePins() {
    if (FILE* f = _wfopen(DataPath(L"start_pins.txt").c_str(), L"wt, ccs=UTF-8")) {
        for (const auto& p : m_pins) fwprintf(f, L"%ls\n", p.c_str());
        fclose(f);
    }
}

bool StartMenu::IsPinned(const std::wstring& parse) const {
    return std::find(m_pins.begin(), m_pins.end(), parse) != m_pins.end();
}

void StartMenu::TogglePin(const std::wstring& parse) {
    auto it = std::find(m_pins.begin(), m_pins.end(), parse);
    if (it != m_pins.end()) m_pins.erase(it);
    else m_pins.push_back(parse);
    SavePins();
    BuildRows();
}

// =============================================================================================
//  Window
// =============================================================================================

LRESULT CALLBACK StartMenu::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    StartMenu* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = reinterpret_cast<StartMenu*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StartMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

bool StartMenu::Create() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AetherStartMenuClass";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    m_scale = 1.0f;
    m_pad = 26.0f;
    m_panelW = kPanelW; m_panelH = kPanelMaxH;
    m_winW = static_cast<int>(m_panelW + m_pad * 2);
    m_winH = static_cast<int>(m_panelH + m_pad * 2);

    // Activatable (no WS_EX_NOACTIVATE): it needs the keyboard for search
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName,
                             L"Aether Start", WS_POPUP, 0, 0, m_winW, m_winH, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, m_winW, m_winH)) return false;
    m_brush = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});
    CreateFonts();
    return true;
}

void StartMenu::CreateFonts() {
    auto dw = m_dcomp ? m_dcomp->GetDWriteFactory() : nullptr;
    if (!dw) return;
    ComPtr<IDWriteFontCollection> col;
    dw->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) {
        UINT32 i = 0; BOOL ex = FALSE;
        return col && SUCCEEDED(col->FindFamilyName(n, &i, &ex)) && ex;
    };
    const wchar_t* display = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* icons = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
    m_hasIconFont = icons != nullptr;

    auto make = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT w, float size,
                    DWRITE_TEXT_ALIGNMENT a, bool ellipsis) {
        f.Reset();
        if (!fam) return;
        dw->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * m_scale, L"en-us", &f);
        if (!f) return;
        f->SetTextAlignment(a);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (ellipsis) {
            DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> e;
            dw->CreateEllipsisTrimmingSign(f.Get(), &e);
            f->SetTrimming(&trim, e.Get());
        }
    };
    make(m_fontSearch, text,    DWRITE_FONT_WEIGHT_NORMAL,    15.0f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    make(m_fontHeader, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    make(m_fontLetter, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    make(m_fontTile,   text,    DWRITE_FONT_WEIGHT_NORMAL,    11.5f, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    make(m_fontUser,   display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    make(m_fontIcon,   icons,   DWRITE_FONT_WEIGHT_NORMAL,    18.0f, DWRITE_TEXT_ALIGNMENT_CENTER, false);
}

void StartMenu::Toggle(HMONITOR monitor, int reservedPx) {
    if (m_wantOpen) Hide(); else Show(monitor, reservedPx);
}

void StartMenu::Show(HMONITOR monitor, int reservedPx) {
    if (!m_hwnd) return;
    AdoptLoadedApps();
    if (m_loadState.load() == 0) PreloadAsync();

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const float s = core::MonitorScale(monitor);
    const float availH = static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top - reservedPx) - 36.0f * s;

    const float oldScale = m_scale;
    m_scale = s;
    m_pad = 26.0f * s;
    m_panelW = kPanelW * s;
    m_panelH = std::min(kPanelMaxH * s, availH - 2 * m_pad);
    const int newW = static_cast<int>(m_panelW + m_pad * 2), newH = static_cast<int>(m_panelH + m_pad * 2);
    if (newW != m_winW || newH != m_winH) {
        m_winW = newW; m_winH = newH;
        SetWindowPos(m_hwnd, nullptr, 0, 0, m_winW, m_winH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        m_dcomp->Resize(m_winW, m_winH);
    }
    if (oldScale != m_scale) CreateFonts();

    const int cx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    const int bottom = mi.rcMonitor.bottom - reservedPx - static_cast<int>(6.0f * s);
    SetWindowPos(m_hwnd, HWND_TOPMOST, cx - m_winW / 2, bottom - m_winH + static_cast<int>(m_pad), m_winW, m_winH, SWP_NOACTIVATE);

    m_query.clear();
    m_sel = -1; m_hover = -1;
    m_scroll.SnapTo(0.0f);
    BuildRows();

    m_wantOpen = true;
    m_open.SnapTo(0.0f);
    m_open.SetTarget(1.0f);
    ShowWindow(m_hwnd, SW_SHOW);

    // take the keyboard focus (needed for typing to search)
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD me = GetCurrentThreadId();
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, FALSE);
}

void StartMenu::Hide() {
    if (!m_wantOpen) return;
    m_wantOpen = false;
    m_open.SetTarget(0.0f);
}

bool StartMenu::Frame(float dt) {
    if (!m_hwnd) return false;
    if (!m_adopted) AdoptLoadedApps();
    m_time += dt;
    m_open.Update(dt);
    m_scroll.Update(dt);
    if (!m_wantOpen && m_open.IsResting() && IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
        return false;
    }
    if (!IsWindowVisible(m_hwnd)) return false;
    Render();
    return true;
}

// =============================================================================================
//  Layout
// =============================================================================================

float StartMenu::ViewportTop() const { return m_pad + (kSearchTop + kSearchH + 10.0f) * m_scale; }
float StartMenu::ViewportBottom() const { return m_pad + m_panelH - kFooterH * m_scale; }
float StartMenu::MaxScroll() const { return std::max(0.0f, m_contentH * m_scale - (ViewportBottom() - ViewportTop())); }

void StartMenu::BuildRows() {
    m_rows.clear();
    m_flat.clear();
    float y = 0.0f;

    auto addHeader = [&](const std::wstring& label, float h) {
        Row r; r.header = true; r.label = label; r.y = y; r.h = h;
        m_rows.push_back(r);
        y += h;
    };
    auto addTiles = [&](const std::vector<int>& idx) {
        if (idx.empty()) return;
        Row r; r.first = static_cast<int>(m_flat.size()); r.count = static_cast<int>(idx.size());
        r.y = y; r.h = std::ceil(idx.size() / static_cast<float>(kCols)) * kTileH;
        m_flat.insert(m_flat.end(), idx.begin(), idx.end());
        m_rows.push_back(r);
        y += r.h;
    };

    if (!m_adopted) {
        addHeader(L"Loading apps...", kHeaderH);
    } else if (!m_query.empty()) {
        const std::wstring q = Lower(m_query);
        std::vector<std::pair<int, int>> scored; // score, app index
        for (int i = 0; i < static_cast<int>(m_apps.size()); ++i) {
            const std::wstring& n = m_apps[i].lower;
            const size_t pos = n.find(q);
            if (pos == std::wstring::npos) continue;
            int score = 2;
            if (pos == 0) score = 0;
            else if (n[pos - 1] == L' ' || n[pos - 1] == L'-' || n[pos - 1] == L'.') score = 1;
            scored.emplace_back(score, i);
        }
        std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first < b.first; });
        std::vector<int> idx;
        for (auto& p : scored) idx.push_back(p.second);
        addHeader(idx.empty() ? L"No apps found" : L"Results", kHeaderH);
        addTiles(idx);
    } else {
        std::vector<int> pinned;
        for (const auto& p : m_pins) {
            for (int i = 0; i < static_cast<int>(m_apps.size()); ++i) if (m_apps[i].parse == p) { pinned.push_back(i); break; }
        }
        if (!pinned.empty()) {
            addHeader(L"Pinned", kHeaderH);
            addTiles(pinned);
            y += 10.0f;
        }
        addHeader(L"All apps", kHeaderH);
        wchar_t cur = 0;
        std::vector<int> group;
        auto flush = [&] {
            if (group.empty()) return;
            addHeader(std::wstring(1, cur), kLetterH);
            addTiles(group);
            group.clear();
        };
        for (int i = 0; i < static_cast<int>(m_apps.size()); ++i) {
            wchar_t c = static_cast<wchar_t>(towupper(m_apps[i].name.empty() ? L'#' : m_apps[i].name[0]));
            if (!iswalpha(c)) c = L'#';
            if (c != cur) { flush(); cur = c; }
            group.push_back(i);
        }
        flush();
    }
    m_contentH = y + 8.0f;
    m_scroll.SnapTo(std::clamp(m_scroll.GetValue(), 0.0f, MaxScroll()));
    if (m_sel >= static_cast<int>(m_flat.size())) m_sel = static_cast<int>(m_flat.size()) - 1;
}

Rect StartMenu::TileRect(const Row& r, int k) const {
    const float s = m_scale;
    const float tileW = (m_panelW - 2 * kInner * s) / kCols;
    const int col = k % kCols, row = k / kCols;
    return Rect{m_pad + kInner * s + col * tileW,
                ViewportTop() + (r.y + row * kTileH) * s - m_scroll.GetValue(), tileW, kTileH * s};
}

int StartMenu::HitTile(const Vec2& p) const {
    if (p.y < ViewportTop() || p.y > ViewportBottom()) return -1;
    for (const Row& r : m_rows) {
        if (r.header) continue;
        const float top = ViewportTop() + r.y * m_scale - m_scroll.GetValue();
        if (p.y < top || p.y > top + r.h * m_scale) continue;
        for (int k = 0; k < r.count; ++k) if (TileRect(r, k).Contains(p)) return r.first + k;
    }
    return -1;
}

void StartMenu::EnsureVisible(int flat) {
    for (const Row& r : m_rows) {
        if (r.header || flat < r.first || flat >= r.first + r.count) continue;
        const float top = r.y * m_scale + (static_cast<float>((flat - r.first) / kCols)) * kTileH * m_scale;
        const float h = kTileH * m_scale;
        const float viewH = ViewportBottom() - ViewportTop();
        float target = m_scroll.GetTarget();
        if (top < target) target = top - 36.0f * m_scale;
        else if (top + h > target + viewH) target = top + h - viewH + 8.0f * m_scale;
        m_scroll.SetTarget(std::clamp(target, 0.0f, MaxScroll()));
        return;
    }
}

void StartMenu::MoveSelection(int dRow, int dCol) {
    if (m_flat.empty()) return;
    if (m_sel < 0) { m_sel = 0; EnsureVisible(0); return; }
    if (dCol != 0) { m_sel = std::clamp(m_sel + dCol, 0, static_cast<int>(m_flat.size()) - 1); EnsureVisible(m_sel); return; }

    // vertical: same column in the neighbouring grid row (possibly in another section)
    int rowIdx = -1, k = 0;
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const Row& r = m_rows[i];
        if (!r.header && m_sel >= r.first && m_sel < r.first + r.count) { rowIdx = i; k = m_sel - r.first; break; }
    }
    if (rowIdx < 0) return;
    const Row& cur = m_rows[rowIdx];
    const int col = k % kCols, gridRow = k / kCols;
    int target = -1;
    if (dRow > 0) {
        const int nextRowStart = (gridRow + 1) * kCols;
        if (nextRowStart < cur.count) target = cur.first + std::min(nextRowStart + col, cur.count - 1);
        else for (int i = rowIdx + 1; i < static_cast<int>(m_rows.size()); ++i)
            if (!m_rows[i].header) { target = m_rows[i].first + std::min(col, m_rows[i].count - 1); break; }
    } else {
        if (gridRow > 0) target = cur.first + (gridRow - 1) * kCols + col;
        else for (int i = rowIdx - 1; i >= 0; --i)
            if (!m_rows[i].header) {
                const int lastRow = (m_rows[i].count - 1) / kCols;
                target = m_rows[i].first + std::min(lastRow * kCols + col, m_rows[i].count - 1);
                break;
            }
    }
    if (target >= 0) { m_sel = target; EnsureVisible(m_sel); }
}

void StartMenu::SetQuery(const std::wstring& q) {
    m_query = q;
    m_scroll.SnapTo(0.0f);
    BuildRows();
    m_sel = m_flat.empty() ? -1 : 0;
}

// =============================================================================================
//  Actions
// =============================================================================================

void StartMenu::Launch(int flat) {
    if (flat < 0 || flat >= static_cast<int>(m_flat.size())) return;
    const App& a = m_apps[m_flat[flat]];
    const std::wstring target = L"shell:AppsFolder\\" + a.parse;
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Hide();
}

void StartMenu::ShowTileMenu(int flat) {
    if (flat < 0 || flat >= static_cast<int>(m_flat.size())) return;
    const App& a = m_apps[m_flat[flat]];
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open");
    AppendMenuW(menu, MF_STRING, 2, IsPinned(a.parse) ? L"Unpin from Start" : L"Pin to Start");
    m_menuOpen = true;
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, m_hwnd, nullptr);
    m_menuOpen = false;
    DestroyMenu(menu);
    if (cmd == 1) Launch(flat);
    else if (cmd == 2) TogglePin(a.parse);
}

void StartMenu::ShowPowerMenu() {
    POINT pt = {static_cast<LONG>(0), static_cast<LONG>(0)};
    RECT wr; GetWindowRect(m_hwnd, &wr);
    pt.x = wr.left + static_cast<LONG>(m_btnPower.Right());
    pt.y = wr.top + static_cast<LONG>(m_btnPower.Top()) - 4;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Lock");
    AppendMenuW(menu, MF_STRING, 2, L"Sign out");
    AppendMenuW(menu, MF_STRING, 3, L"Sleep");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Restart");
    AppendMenuW(menu, MF_STRING, 5, L"Shut down");
    m_menuOpen = true;
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    m_menuOpen = false;
    DestroyMenu(menu);
    if (cmd == 0) return;
    Hide();
    EnableShutdownPrivilege();
    switch (cmd) {
        case 1: LockWorkStation(); break;
        case 2: ExitWindowsEx(EWX_LOGOFF, 0); break;
        case 3: SetSuspendState(FALSE, FALSE, FALSE); break;
        case 4: ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER); break;
        case 5: ExitWindowsEx(EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER); break;
    }
}

// =============================================================================================
//  Rendering
// =============================================================================================

void StartMenu::DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f,
                         const D2D1_RECT_F& r, const Color& c) {
    if (!f || t.empty()) return;
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, &r, m_brush.Get());
}

void StartMenu::DrawGlyph(ID2D1DeviceContext* ctx, wchar_t g, const D2D1_RECT_F& r, const Color& c) {
    if (!m_hasIconFont || !m_fontIcon) return;
    wchar_t buf[2] = {g, 0};
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(buf, 1, m_fontIcon.Get(), &r, m_brush.Get());
}

void StartMenu::EnsureIcon(App& app) {
    if (app.icon || app.iconTried) return;
    app.iconTried = true;
    ComPtr<IShellItem> item;
    const std::wstring path = L"shell:AppsFolder\\" + app.parse;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item.As(&factory))) return;
    const int px = static_cast<int>(48.0f * std::max(1.0f, m_scale));
    HBITMAP hbm = nullptr;
    if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY, &hbm)) || !hbm) return;

    BITMAP bm{};
    GetObjectW(hbm, sizeof(bm), &bm);
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        std::vector<uint32_t> pixels(static_cast<size_t>(bm.bmWidth) * bm.bmHeight);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, hbm, 0, bm.bmHeight, pixels.data(), &bi, DIB_RGB_COLORS)) {
            bool anyAlpha = false;
            for (uint32_t p : pixels) if (p >> 24) { anyAlpha = true; break; }
            if (!anyAlpha) for (auto& p : pixels) if (p & 0x00FFFFFF) p |= 0xFF000000;
            if (auto ctx = m_dcomp->GetContext()) {
                D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                ctx->CreateBitmap(D2D1::SizeU(bm.bmWidth, bm.bmHeight), pixels.data(), bm.bmWidth * 4, props, &app.icon);
            }
        }
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(hbm);
}

void StartMenu::Render() {
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    const float s = m_scale;
    const float o = Clamp01(m_open.GetValue());
    const Rect panel{m_pad, m_pad, m_panelW, m_panelH};

    ctx->SetTransform(D2D1::Matrix3x2F::Scale(0.95f + 0.05f * o, 0.95f + 0.05f * o, D2D1::Point2F(panel.Center().x, panel.Bottom())) *
                      D2D1::Matrix3x2F::Translation(0.0f, (1.0f - o) * 20.0f * s));
    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                       D2D1::IdentityMatrix(), o, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    ctx->PushLayer(&lp, nullptr);

    // shadow + body
    for (int i = 4; i >= 1; --i) {
        Rect sr{panel.x - i * 4.0f * s, panel.y + (i * 2.5f + 3.0f) * s, panel.width + i * 8.0f * s, panel.height + i * 3.0f * s};
        if (auto g = m_dcomp->CreateSquircleGeometry(sr, (30.0f + i * 4.0f) * s)) {
            m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.06f));
            ctx->FillGeometry(g.Get(), m_brush.Get());
        }
    }
    if (auto body = m_dcomp->CreateSquircleGeometry(panel, 30.0f * s)) {
        m_brush->SetColor(D2D1::ColorF(0.06f, 0.06f, 0.08f, 0.97f));
        ctx->FillGeometry(body.Get(), m_brush.Get());
        auto rim = m_dcomp->CreateLinearGradientBrush(Color{1.0f, 1.0f, 1.0f, 0.16f}, Color{1.0f, 1.0f, 1.0f, 0.04f},
                                                      D2D1::Point2F(panel.Center().x, panel.Top()), D2D1::Point2F(panel.Center().x, panel.Bottom()));
        if (rim) ctx->DrawGeometry(body.Get(), rim.Get(), 1.0f * s);
    }

    // search box
    const Rect box{panel.x + kInner * s, panel.y + kSearchTop * s, panel.width - 2 * kInner * s, kSearchH * s};
    if (auto g = m_dcomp->CreateSquircleGeometry(box, kSearchH * 0.5f * s)) {
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.07f));
        ctx->FillGeometry(g.Get(), m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, m_query.empty() ? 0.10f : 0.22f));
        ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.0f * s);
    }
    DrawGlyph(ctx, kIconSearch, RF(box.x + 6 * s, box.y, box.x + 46 * s, box.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.6f});
    const float tx = box.x + 48.0f * s;
    if (m_query.empty()) {
        DrawText(ctx, L"Type here to search apps", m_fontSearch.Get(), RF(tx, box.y, box.Right() - 16 * s, box.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.42f});
    } else {
        DrawText(ctx, m_query, m_fontSearch.Get(), RF(tx, box.y, box.Right() - 16 * s, box.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.96f});
    }
    if (std::fmod(m_time, 1.1f) < 0.6f) {
        float cw = 0;
        if (!m_query.empty() && m_fontSearch) {
            ComPtr<IDWriteTextLayout> tl;
            if (SUCCEEDED(m_dcomp->GetDWriteFactory()->CreateTextLayout(m_query.c_str(), static_cast<UINT32>(m_query.size()), m_fontSearch.Get(), 4096, 40, &tl))) {
                DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm); cw = std::min(tm.widthIncludingTrailingWhitespace, box.Right() - 20 * s - tx);
            }
        }
        m_brush->SetColor(D2D1::ColorF(0.38f, 0.80f, 1.0f, 0.95f));
        ctx->FillRectangle(RF(tx + cw + 1 * s, box.y + 12 * s, tx + cw + 2.5f * s, box.Bottom() - 12 * s), m_brush.Get());
    }

    // scrolling content
    const float vt = ViewportTop(), vb = ViewportBottom();
    ctx->PushAxisAlignedClip(RF(panel.x + 4 * s, vt, panel.Right() - 4 * s, vb), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    int iconBudget = 6;
    for (const Row& r : m_rows) {
        const float top = vt + r.y * s - m_scroll.GetValue();
        if (top > vb || top + r.h * s < vt) continue;
        if (r.header) {
            const bool big = r.h >= kHeaderH;
            DrawText(ctx, r.label, big ? m_fontHeader.Get() : m_fontLetter.Get(),
                     RF(panel.x + (kInner + 6) * s, top, panel.Right() - kInner * s, top + r.h * s),
                     Color{1.0f, 1.0f, 1.0f, big ? 0.92f : 0.45f});
            continue;
        }
        for (int k = 0; k < r.count; ++k) {
            const Rect t = TileRect(r, k);
            if (t.Bottom() < vt || t.Top() > vb) continue;
            const int flat = r.first + k;
            App& app = m_apps[m_flat[flat]];
            if (!app.icon && !app.iconTried && iconBudget > 0) { EnsureIcon(app); --iconBudget; }

            const bool hot = (flat == m_hover), sel = (flat == m_sel);
            if (hot || sel) {
                if (auto g = m_dcomp->CreateSquircleGeometry(Rect{t.x + 3 * s, t.y + 2 * s, t.width - 6 * s, t.height - 4 * s}, 14.0f * s)) {
                    m_brush->SetColor(D2D1::ColorF(1, 1, 1, sel ? 0.13f : 0.08f));
                    ctx->FillGeometry(g.Get(), m_brush.Get());
                    if (sel) {
                        m_brush->SetColor(D2D1::ColorF(0.38f, 0.80f, 1.0f, 0.55f));
                        ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.2f * s);
                    }
                }
            }
            const float isz = 40.0f * s;
            const float lift = hot ? 1.5f * s : 0.0f;
            if (app.icon) {
                const D2D1_RECT_F dst = RF(t.Center().x - isz / 2, t.y + 12 * s - lift, t.Center().x + isz / 2, t.y + 12 * s + isz - lift);
                ctx->DrawBitmap(app.icon.Get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            } else {
                m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.08f));
                ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(t.Center().x - isz / 2, t.y + 12 * s, t.Center().x + isz / 2, t.y + 12 * s + isz), 10 * s, 10 * s), m_brush.Get());
            }
            DrawText(ctx, app.name, m_fontTile.Get(), RF(t.x + 6 * s, t.y + 58 * s, t.Right() - 6 * s, t.y + 82 * s), Color{1.0f, 1.0f, 1.0f, 0.88f});
        }
    }
    ctx->PopAxisAlignedClip();

    // scroll indicator
    if (MaxScroll() > 1.0f) {
        const float viewH = vb - vt;
        const float thumbH = std::max(36.0f * s, viewH * viewH / (m_contentH * s));
        const float frac = m_scroll.GetValue() / MaxScroll();
        const float ty = vt + (viewH - thumbH) * frac;
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.20f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(panel.Right() - 10 * s, ty, panel.Right() - 6 * s, ty + thumbH), 2 * s, 2 * s), m_brush.Get());
    }

    // footer: user + settings + power
    const float fy = panel.Bottom() - kFooterH * s;
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.07f));
    ctx->FillRectangle(RF(panel.x + 20 * s, fy, panel.Right() - 20 * s, fy + 1.0f * s), m_brush.Get());
    const float cy = fy + kFooterH * 0.5f * s;
    const float av = 36.0f * s;
    {
        auto grad = m_dcomp->CreateLinearGradientBrush(Color{0.38f, 0.80f, 1.0f, 1.0f}, Color{0.40f, 0.35f, 0.95f, 1.0f},
                                                       D2D1::Point2F(panel.x + kInner * s, cy - av / 2), D2D1::Point2F(panel.x + kInner * s + av, cy + av / 2));
        if (grad) ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(panel.x + kInner * s + av / 2, cy), av / 2, av / 2), grad.Get());
        const std::wstring initial(1, static_cast<wchar_t>(towupper(m_userName.empty() ? L'U' : m_userName[0])));
        DrawText(ctx, initial, m_fontUser.Get(), RF(panel.x + kInner * s, cy - av / 2, panel.x + kInner * s + av, cy + av / 2), Color{1.0f, 1.0f, 1.0f, 1.0f});
        // centre the initial: format is left aligned, nudge via a second centred draw is unnecessary at this size
    }
    DrawText(ctx, m_userName, m_fontUser.Get(), RF(panel.x + (kInner + 46) * s, cy - 14 * s, panel.x + 300 * s, cy + 14 * s), Color{1.0f, 1.0f, 1.0f, 0.92f});

    const float bs = 40.0f * s;
    m_btnPower = Rect{panel.Right() - kInner * s - bs, cy - bs / 2, bs, bs};
    m_btnSettings = Rect{m_btnPower.x - bs - 6 * s, cy - bs / 2, bs, bs};
    auto drawBtn = [&](const Rect& r, wchar_t glyph, bool hot) {
        if (hot) {
            m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.10f));
            ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.Center().x, r.Center().y), r.width / 2, r.width / 2), m_brush.Get());
        }
        DrawGlyph(ctx, glyph, RF(r), Color{1.0f, 1.0f, 1.0f, hot ? 1.0f : 0.82f});
    };
    drawBtn(m_btnSettings, kIconSettings, m_hoverBtn == 1);
    drawBtn(m_btnPower, kIconPower, m_hoverBtn == 2);

    ctx->PopLayer();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
}

// =============================================================================================
//  Messages
// =============================================================================================

LRESULT StartMenu::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !m_menuOpen) Hide();
            return 0;
        case WM_MOUSEMOVE: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            m_hover = HitTile(p);
            m_hoverBtn = m_btnSettings.Contains(p) ? 1 : (m_btnPower.Contains(p) ? 2 : 0);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, (m_hover >= 0 || m_hoverBtn) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEWHEEL: {
            const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            m_scroll.SetTarget(std::clamp(m_scroll.GetTarget() - notches * 90.0f * m_scale, 0.0f, MaxScroll()));
            return 0;
        }
        case WM_LBUTTONUP: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            if (m_btnPower.Contains(p)) { ShowPowerMenu(); return 0; }
            if (m_btnSettings.Contains(p)) { ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL); Hide(); return 0; }
            const int hit = HitTile(p);
            if (hit >= 0) Launch(hit);
            return 0;
        }
        case WM_RBUTTONUP: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            const int hit = HitTile(p);
            if (hit >= 0) ShowTileMenu(hit);
            return 0;
        }
        case WM_CHAR: {
            const wchar_t c = static_cast<wchar_t>(wp);
            if (c == 8) { if (!m_query.empty()) { std::wstring q = m_query; q.pop_back(); SetQuery(q); } }
            else if (c >= 32 && c != 127) SetQuery(m_query + c);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE:
                    if (!m_query.empty()) SetQuery(L""); else Hide();
                    return 0;
                case VK_RETURN: Launch(m_sel >= 0 ? m_sel : (m_flat.empty() ? -1 : 0)); return 0;
                case VK_LEFT:  MoveSelection(0, -1); return 0;
                case VK_RIGHT: MoveSelection(0, 1); return 0;
                case VK_UP:    MoveSelection(-1, 0); return 0;
                case VK_DOWN:  MoveSelection(1, 0); return 0;
                case VK_PRIOR: m_scroll.SetTarget(std::max(0.0f, m_scroll.GetTarget() - 400.0f * m_scale)); return 0;
                case VK_NEXT:  m_scroll.SetTarget(std::min(MaxScroll(), m_scroll.GetTarget() + 400.0f * m_scale)); return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace aether::bar
