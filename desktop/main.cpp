// AetherDesktop: Aether's own desktop. A borderless window that sits directly above Windows' desktop (below every normal window)
// and draws the wallpaper and the desktop icons in the Aether glass style, instead of Explorer's SysListView.
//
//  * wallpaper: whatever Windows' wallpaper setting points to (fill / fit / stretch / center / tile), re-read when it changes
//  * items: the user's Desktop + Public Desktop folders, plus This PC / Recycle Bin / user folder / Network
//  * icons: real shell icons and thumbnails fetched on a worker thread (shortcuts, exes, images ...), vector icons for the system items
//  * interaction: select, ctrl/shift, marquee, drag & drop (move on the grid, into folders, into the bin, out to other windows, in from other windows),
//    rename, copy / cut / paste, delete, open, genuine Windows context menu behind "More options", arrow keys, type-to-select
//  * layout: free placement on a snapping grid (positions persisted) or auto arrange; three icon sizes; sorting
//
// Nothing here touches Explorer's own desktop: closing this exe simply uncovers it again.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <ole2.h>
#include <oleidl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "aether/bar/ui_common.hpp"
#include "aether/render/context_menu.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"
#include "aether/system/wallpaper.hpp"
#include "model.hpp"
#include "shellutil.hpp"

using namespace aether;
using core::Color;
using core::Rect;
using Microsoft::WRL::ComPtr;
namespace bui = aether::bar::ui;
namespace su = aether::shellutil;
using aether::explorer::JoinPath;
using aether::explorer::ExtOf;
using aether::explorer::Lower;

namespace {

constexpr UINT kMsgIcon = WM_APP + 1;      // lParam = IconRes*
constexpr UINT kMsgChanged = WM_APP + 2;   // the Desktop folder changed
constexpr UINT kMsgOpDone = WM_APP + 3;    // a file operation finished (lParam = malloc'ed name to select, or null)
constexpr UINT kMsgShowDesk = WM_APP + 4, kMsgWinE = WM_APP + 5;
constexpr UINT_PTR kTimerSync = 1, kTimerReload = 2, kTimerCaret = 3, kTimerSlide = 4, kTimerGuard = 5, kTimerSave = 6;

const Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline D2D1_RECT_F D(const Rect& r) { return D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()); }
inline D2D1_COLOR_F DC(const Color& c, float a = -1.0f) { return D2D1::ColorF(c.r, c.g, c.b, a < 0 ? c.a : a); }
inline Color Mix(const Color& a, const Color& b, float k) { return Color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k}; }
inline Color WithA(const Color& c, float a) { return Color{c.r, c.g, c.b, a}; }
inline float Ease(float v, float target, float dt, float speed) { return v + (target - v) * (1.0f - std::exp(-speed * dt)); }
inline bool IEquals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

struct Metrics { float icon, cw, ch; };
const Color kAccents[6] = {{0.55f, 0.30f, 0.95f, 1.0f}, {0.30f, 0.55f, 1.0f, 1.0f}, {0.95f, 0.35f, 0.70f, 1.0f}, {0.20f, 0.78f, 0.52f, 1.0f}, {1.0f, 0.60f, 0.20f, 1.0f}, {0.20f, 0.80f, 0.90f, 1.0f}};
const int kFitCodes[6] = {-1, 10, 6, 2, 0, 100};

enum Kind { K_File = 0, K_Folder, K_PC, K_Bin, K_User, K_Net };

struct Item {
    std::wstring key, path, label, ext;
    Kind kind{K_File};
    bool dir{false}, lnk{false};
    FILETIME mtime{};
    uint64_t size{0};
    int col{-1}, row{-1};
    std::wstring disp; float layW{-1}; ComPtr<IDWriteTextLayout> lay; float lx{0}, lw{0}, lh{0};   // label fitted to two lines (cached layout)
    bool Special() const { return kind == K_PC || kind == K_Bin || kind == K_Net; }
};

// A wallpaper baked once at screen size (so a frame is a single 1:1 bitmap blit, never a per-frame rescale).
struct Wall {
    ComPtr<ID2D1Bitmap1> bmp;
    Rect dst{0, 0, 0, 0};
    bool tile{false};
};

std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
std::wstring CfgPath() {
    wchar_t buf[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(buf) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\desktop.cfg";
}
std::wstring KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) { r = p; CoTaskMemFree(p); }
    return r;
}
bool IsThumbExt(const std::wstring& e) {
    static const wchar_t* k[] = {L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp", L".tif", L".tiff", L".mp4", L".mkv", L".avi", L".mov", L".webm", L".pdf"};
    for (auto* x : k) if (e == x) return true;
    return false;
}

// ---- shell icons on a worker thread --------------------------------------------------------------------------------------
struct IconRes { std::wstring key; int w{0}, h{0}; std::vector<uint32_t> px; };

// An icon's place in the world: which monitor and which grid cell. Written to the config per monitor,
// so every Aether desktop window (one per monitor) owns the icons assigned to it.
struct Pos3 { int mon{0}, col{-1}, row{-1}; };

class IconQueue {
    struct Req { HWND hwnd; std::wstring key, path; int px; bool thumb; };
    std::mutex m_mu; std::condition_variable m_cv; std::deque<Req> m_q; int m_started{0};
    static IconRes* Fetch(const Req& r) {
        auto* res = new IconRes(); res->key = r.key;
        ComPtr<IShellItemImageFactory> f;
        if (SUCCEEDED(SHCreateItemFromParsingName(r.path.c_str(), nullptr, IID_PPV_ARGS(&f))) && f) {
            HBITMAP hbm = nullptr;
            const SIIGBF flags = r.thumb ? SIIGBF_BIGGERSIZEOK : static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK);
            if (SUCCEEDED(f->GetImage(SIZE{r.px, r.px}, flags, &hbm)) && hbm) {
                BITMAP bm{}; GetObjectW(hbm, sizeof(bm), &bm);
                if (bm.bmWidth > 0 && bm.bmHeight > 0) {
                    res->w = bm.bmWidth; res->h = bm.bmHeight;
                    res->px.resize(static_cast<size_t>(bm.bmWidth) * bm.bmHeight);
                    BITMAPINFO bi = {};
                    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = bm.bmWidth; bi.bmiHeader.biHeight = -bm.bmHeight;
                    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                    HDC dc = GetDC(nullptr);
                    if (GetDIBits(dc, hbm, 0, bm.bmHeight, res->px.data(), &bi, DIB_RGB_COLORS)) {
                        bool any = false;
                        for (uint32_t p : res->px) if (p >> 24) { any = true; break; }
                        if (!any) for (auto& p : res->px) if (r.thumb || (p & 0x00FFFFFF)) p |= 0xFF000000;
                    } else { res->w = res->h = 0; res->px.clear(); }
                    ReleaseDC(nullptr, dc);
                }
                DeleteObject(hbm);
            }
        }
        return res;
    }
    void Run() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            Req r;
            { std::unique_lock<std::mutex> lk(m_mu); m_cv.wait(lk, [&] { return !m_q.empty(); }); r = std::move(m_q.front()); m_q.pop_front(); }
            IconRes* res = Fetch(r);
            if (!IsWindow(r.hwnd) || !PostMessageW(r.hwnd, kMsgIcon, 0, reinterpret_cast<LPARAM>(res))) delete res;
        }
    }
public:
    void Push(HWND h, std::wstring key, std::wstring path, int px, bool thumb) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_q.push_back({h, std::move(key), std::move(path), px, thumb});
        while (m_started < 2) { ++m_started; std::thread([this] { Run(); }).detach(); }
        m_cv.notify_one();
    }
    void Clear(HWND h) { std::lock_guard<std::mutex> lk(m_mu); for (auto it = m_q.begin(); it != m_q.end();) { if (it->hwnd == h) it = m_q.erase(it); else ++it; } }
};
IconQueue& IQ() { static IconQueue* q = new IconQueue(); return *q; }

// Win+D / Win+E belong to Aether: the desktop handles them itself instead of leaving them to Explorer.
HWND g_deskWnd = nullptr;
HHOOK g_hook = nullptr;
LRESULT CALLBACK KeyHook(int n, WPARAM w, LPARAM l) {
    static bool swallowD = false, swallowE = false;
    if (n == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
        const bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN, up = w == WM_KEYUP || w == WM_SYSKEYUP;
        if (!(k->flags & LLKHF_INJECTED) && (k->vkCode == 'D' || k->vkCode == 'E')) {
            bool& sw = k->vkCode == 'D' ? swallowD : swallowE;
            if (down) {
                const bool win = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
                const bool other = ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
                if (win && !other && g_deskWnd) {
                    sw = true;
                    PostMessageW(g_deskWnd, k->vkCode == 'D' ? kMsgShowDesk : kMsgWinE, 0, 0);
                    INPUT in[2] = {};                              // a dummy key press stops the Start menu opening when Win is released
                    in[0].type = in[1].type = INPUT_KEYBOARD;
                    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
                    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, in, sizeof(INPUT));
                    return 1;
                }
            } else if (up && sw) { sw = false; return 1; }
        }
    }
    return CallNextHookEx(nullptr, n, w, l);
}

// ".lnk" pointing at a folder: open it in Aether Explorer rather than letting the shell start explorer.exe
std::wstring LinkFolderTarget(const std::wstring& lnk) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) || !sl) return L"";
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf)) || FAILED(pf->Load(lnk.c_str(), STGM_READ))) return L"";
    wchar_t t[MAX_PATH * 2] = {}, e[MAX_PATH * 2] = {};
    if (FAILED(sl->GetPath(t, MAX_PATH * 2, nullptr, SLGP_RAWPATH)) || !t[0]) return L"";
    ExpandEnvironmentStringsW(t, e, MAX_PATH * 2);
    const DWORD a = GetFileAttributesW(e);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? std::wstring(e) : std::wstring();
}

class Desktop;
class DesktopDropTarget;

class Desktop {
public:
    bool Create(HINSTANCE inst, HMONITOR mon, int index);
    bool NeedsFrame() const { return m_dirty || m_animating || m_pdirty; }
    bool Alive() const { return m_hwnd && IsWindow(m_hwnd); }
    void Frame();
    static std::vector<Desktop*>& Siblings();   // every live desktop window, for wWinMain and cross-monitor handoff
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);

    // drop target entry points (screen coordinates)
    DWORD DragOverAt(POINT sp, DWORD keys, DWORD allowed);
    void DragLeaveAll() { m_dropHover = -1; Kick(); }
    void DropAt(IDataObject* obj, POINT sp, DWORD keys, DWORD* effect);

private:
    // ---- state
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ID2D1DeviceContext* m_ctx{nullptr};
    ComPtr<ID2D1SolidColorBrush> m_brush;
    float m_dpi{96};
    float m_W{1920}, m_H{1080};                // window size in DIPs
    Rect m_work{0, 0, 1920, 1040};             // work area in DIPs (excludes the Aether bar)
    bool m_dirty{true}, m_animating{false}, m_inFrame{false};
    LARGE_INTEGER m_freq{}, m_last{};
    float m_mx{-1}, m_my{-1};
    std::wstring m_textFont{L"Segoe UI Variable Text"}, m_iconFont{L"Segoe Fluent Icons"};
    std::unordered_map<uint32_t, ComPtr<IDWriteTextFormat>> m_fmts;
    std::map<int, ComPtr<ID2D1PathGeometry>> m_polys;
    UINT m_taskbarCreated{0};

    // wallpaper
    Wall m_wall, m_wallPrev; float m_wallFade{1.0f}; std::wstring m_wallKey;
    Color m_bgColor{0.05f, 0.03f, 0.10f, 1.0f};
    bool m_tracking{false};
    std::unordered_map<std::wstring, ULONGLONG> m_iconT;            // arrival time of an icon (fade-in)
    int m_lastMin{-1};
    std::vector<HWND> m_hidden; bool m_showDesk{false}; int m_cfgVer{0};

    // customization panel
    struct Ctrl { Rect r; int id; };
    std::vector<Ctrl> m_ctrls; std::map<int, Rect> m_tracks;
    bool m_panel{false}; float m_panelA{0.0f}; Rect m_panelR{0, 0, 0, 0}, m_panelView{0, 0, 0, 0};
    float m_panelScroll{0.0f}, m_panelContentH{640.0f};
    int m_dragCtl{0}, m_hotCtl{0}, m_ptab{0};
    bool m_panelDrag{false}, m_panelMoved{false}; float m_panelGrabX{0}, m_panelGrabY{0}, m_panelX{0}, m_panelY{0};
    bool m_hotHeader{false};

    // multi-monitor: one Desktop per monitor; icons are assigned to a monitor and can be carried across
    int m_mon{0}; bool m_primary{true}; HMONITOR m_monitor{nullptr};
    Desktop* m_dragTo{nullptr};                                         // active cross-monitor handoff target (source side)
    std::vector<std::wstring> m_hoKeys;                                 // handoff state on the receiving side
    std::vector<int> m_hoIdx;
    std::vector<std::pair<int, int>> m_hoDelta;
    int m_hoAnchor{-1}, m_hoTcol{-1}, m_hoTrow{-1};
    bool m_hoValid{false};
    float m_hoGdx{0}, m_hoGdy{0}, m_hoCx{-1}, m_hoCy{-1};
    FILETIME m_cfgWrite{};                                              // our own last config write (sibling sync skips it)
    // The customisation panel lives in its own top-most window so it always sits above desktop widgets.
    HWND m_pwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_pdcomp;
    ComPtr<ID2D1SolidColorBrush> m_pbrush;
    std::unordered_map<uint32_t, ComPtr<IDWriteTextFormat>> m_pfmts;
    bool m_pdirty{false}, m_pTracking{false};
    float m_pOx{0.0f}, m_pOy{0.0f};
    int m_pPx{0}, m_pPy{0}, m_pLastX{-99999}, m_pLastY{-99999}, m_pLastW{0}, m_pLastH{0}, m_pSzW{64}, m_pSzH{64};

    // items
    std::vector<Item> m_items;
    std::vector<char> m_sel;
    int m_cursor{-1};
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> m_icons;
    std::set<std::wstring> m_iconPending, m_iconFailed;
    std::unordered_map<int, float> m_hover;
    int m_hot{-1};
    int m_cols{1}, m_rows{1};
    bool m_binFull{false};
    std::wstring m_desktopDir, m_publicDir;

    // settings
    float m_iconSize{48.0f};
    float m_dim{0.0f}; int m_blur{0}; int m_fit{-1};                // wallpaper look (fit -1 = follow Windows)
    int m_labelStyle{0}; int m_accent{0}; float m_glow{1.0f}; bool m_clock{false};
    bool m_slide{false}; int m_slideMin{5}; std::wstring m_slideDir; int m_slideIdx{0};
    std::vector<std::wstring> m_slides;
    bool m_auto{false}, m_showIcons{true}, m_showPC{true}, m_showBin{true}, m_showUser{false}, m_showNet{false};
    int m_sortKey{0};                          // 0 name 1 size 2 type 3 date (used by auto arrange)
    std::map<std::wstring, Pos3> m_pos;
    std::map<std::wstring, std::pair<Pos3, ULONGLONG>> m_pendingDrop;   // external drops waiting for the copied file to appear
    std::map<std::wstring, std::wstring> m_applied;                     // cfg values as last loaded: keeps sibling edits alive on save

    // interaction
    bool m_leftDown{false};
    int m_pressItem{-1};
    float m_pressX{0}, m_pressY{0}, m_grabDX{0}, m_grabDY{0};
    bool m_pressSelectOnUp{false}, m_dragging{false}, m_marquee{false}, m_oleDrag{false};
    float m_mqX{0}, m_mqY{0};
    std::vector<char> m_mqBase;
    int m_dropHover{-1};                       // folder / bin under a dragged item
    int m_tcol{-1}, m_trow{-1}; bool m_tvalid{false};   // snapping target of the primary dragged item
    bool m_edit{false}; int m_editIdx{-1}; std::wstring m_editText, m_editExt;
    std::wstring m_pendingSelect; bool m_pendingRename{false};
    std::wstring m_type; ULONGLONG m_typeTick{0};
    std::vector<std::wstring> m_clip; bool m_clipCut{false};
    ComPtr<IContextMenu2> m_cm2; ComPtr<IContextMenu3> m_cm3;
    ComPtr<IDropTarget> m_dropTarget;
    HANDLE m_watchStop{nullptr};

    // ---- helpers
    float Scale() const { return m_dpi / 96.0f; }
    void Kick() { m_dirty = true; }
    void Tick();
    void Render();
    int HitItem(float x, float y) const;
    Rect CellRect(int col, int row) const {
        const auto z = Z();
        return Rect{m_work.x + 8 + col * z.cw, m_work.y + 8 + row * z.ch, z.cw - 4, z.ch - 4};
    }
    Rect ItemRect(int i) const { return CellRect(m_items[i].col, m_items[i].row); }
    int Count() const { return static_cast<int>(m_items.size()); }
    std::vector<int> SelectedIdx() const {
        std::vector<int> v;
        for (int i = 0; i < Count() && i < static_cast<int>(m_sel.size()); ++i) if (m_sel[i]) v.push_back(i);
        return v;
    }
    std::vector<std::wstring> SelectedPaths(bool filesOnly = true) const {
        std::vector<std::wstring> v;
        for (int i : SelectedIdx()) if (!filesOnly || !m_items[i].Special()) v.push_back(m_items[i].path);
        return v;
    }
    void SelectOnly(int i) { std::fill(m_sel.begin(), m_sel.end(), 0); if (i >= 0 && i < static_cast<int>(m_sel.size())) m_sel[i] = 1; m_cursor = i; }

    // ---- drawing primitives
    void FillRR(const Rect& r, float rad, const Color& c);
    void StrokeRR(const Rect& r, float rad, const Color& c, float w = 1.0f);
    void GlowRR(const Rect& r, float rad, const Color& c, float spread, float strength);
    IDWriteTextFormat* Fmt(float size, int weight, bool icon, bool wrap);
    void Glyph(wchar_t g, const Rect& r, float size, const Color& c);
    void UnitPoly(int id, std::initializer_list<D2D1_POINT_2F> pts, const Rect& r);
    void DrawWallpaper();
    void DrawItem(int i, const Rect& cell, float alpha, bool ghost);
    void DrawIcon(const Item& it, const Rect& r, float alpha);
    void DrawLabel(Item& it, const Rect& cell, float top, float alpha, bool sel);
    void DrawEdit(const Item& it, const Rect& cell, float top);
    std::wstring FitLabel(const std::wstring& s, float width);

    // ---- model
    void LoadCfg();
    void SaveCfg();
    void LoadWorkArea();
    void LoadWallpaper(bool force);
    void LoadItems();
    void Place();
    void ArrangeBy(int key);
    void RequestIcon(const Item& it);
    void OnIcon(IconRes* r);
    void CheckBin();
    void StartWatch();
    void AnchorZ();

    // ---- commands
    void OpenItem(int i);
    void OpenFolder(const std::wstring& path);
    void DeleteSel(bool perm);
    void CopySel(bool cut);
    void Paste();
    void NewFolder();
    void NewTextFile();
    void BeginRename(int i);
    void CommitEdit();
    void CancelEdit() { m_edit = false; m_editIdx = -1; KillTimer(m_hwnd, kTimerCaret); Kick(); }
    void Properties(int i);
    void ShellMenu(const std::vector<std::wstring>& paths, POINT pt);
    void ItemMenu(int i, POINT pt);
    void BackgroundMenu(POINT pt);
    void ViewMenu(POINT pt);
    void SortMenu(POINT pt);
    void NewMenu(POINT pt);
    void PickWallpaper();
    void StartOleDrag();
    void FinishOwnDrag();
    // cross-monitor handoff: carry a selection onto a sibling desktop window in this process
    static Desktop* SiblingAt(HWND h);
    void BeginHandoff(Desktop* other, const POINT& sp);
    void HandoffIn(const std::vector<std::wstring>& keys, const std::wstring& anchor,
                   const std::vector<std::pair<int, int>>& delta, float gdx, float gdy, const POINT& sp);
    void HandoffMove(const POINT& sp);
    void HandoffCancel();
    std::vector<std::pair<std::wstring, Pos3>> HandoffDrop(const POINT& sp);
    void OnKey(WPARAM vk);
    void OnChar(wchar_t c);
    void Move(int dx, int dy, bool extend);
    bool Occupied(int col, int row, const std::vector<int>& ignore) const;
    std::wstring OpDoneSel(LPARAM l) {
        auto* p = reinterpret_cast<wchar_t*>(l); std::wstring s = p ? p : L""; free(p); return s;
    }
    Metrics Z() const { return Metrics{m_iconSize, m_iconSize + 32.0f, m_iconSize + 54.0f}; }
    Color Acc() const { return m_accent == 0 ? render::glass::kViolet : kAccents[m_accent]; }
    Color AccL() const { return m_accent == 0 ? render::glass::kVioletLight : Mix(kAccents[m_accent], kWhite, 0.38f); }
    bool BakeWall(const std::wstring& path, int style, Wall& out);
    void DrawWall(const Wall& w, float a);
    void LoadSlides();
    void ApplySlideTimer();
    void PickSlideFolder();
    void EnsureLabel(Item& it, float w);
    void DrawClock();
    void DrawPanel();
    Rect PanelGeom() const;
    bool EnsurePanelWnd();
    void RenderPanel();
    void KickP() { m_pdirty = true; }
    LRESULT HandlePanel(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l);
    void PText(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, DWRITE_TEXT_ALIGNMENT al, bool shadow = false);
    void PanelAction(int id);
    void PanelSlider(int id, float mx);
    int CtlAt(float x, float y) const;
    void ResetLook();
    void ToggleShowDesktop();
    friend class DesktopDropTarget;
};

class DesktopDropTarget : public IDropTarget {
    LONG m_ref{1};
    Desktop* m_app;
public:
    explicit DesktopDropTarget(Desktop* a) : m_app(a) {}
    HRESULT __stdcall QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDropTarget) { *o = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG __stdcall Release() override { const LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT __stdcall DragEnter(IDataObject*, DWORD keys, POINTL pt, DWORD* eff) override { *eff = m_app->DragOverAt(POINT{pt.x, pt.y}, keys, *eff); return S_OK; }
    HRESULT __stdcall DragOver(DWORD keys, POINTL pt, DWORD* eff) override { *eff = m_app->DragOverAt(POINT{pt.x, pt.y}, keys, *eff); return S_OK; }
    HRESULT __stdcall DragLeave() override { m_app->DragLeaveAll(); return S_OK; }
    HRESULT __stdcall Drop(IDataObject* obj, DWORD keys, POINTL pt, DWORD* eff) override {
        m_app->DropAt(obj, POINT{pt.x, pt.y}, keys, eff);
        m_app->DragLeaveAll();
        return S_OK;
    }
};

// ---- window plumbing ---------------------------------------------------------------------------------------------------
bool Desktop::Create(HINSTANCE inst, HMONITOR mon, int index) {
    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_last);
    render::glass::SyncTheme();
    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"AetherDesktopWindow";
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    m_monitor = mon; m_mon = index;
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    m_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;   // the Windows primary, not merely the left-most screen
    const int x = mi.rcMonitor.left, y = mi.rcMonitor.top, w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top - 1;   // 1 px short: not a "full-screen app" for the game-mode detector
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Aether Desktop", WS_POPUP, x, y, w, h, nullptr, nullptr, inst, this);
    if (!m_hwnd) return false;
    m_dpi = static_cast<float>(GetDpiForWindow(m_hwnd));

    RECT rc; GetClientRect(m_hwnd, &rc);
    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, rc.right, rc.bottom)) return false;
    m_brush = m_dcomp->CreateSolidBrush(kWhite);

    ComPtr<IDWriteFontCollection> col;
    m_dcomp->GetDWriteFactory()->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL ex = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &ex)) && ex; };
    if (!has(L"Segoe Fluent Icons")) m_iconFont = L"Segoe MDL2 Assets";
    if (!has(L"Segoe UI Variable Text")) m_textFont = L"Segoe UI";

    m_desktopDir = KnownFolder(FOLDERID_Desktop);
    m_publicDir = KnownFolder(FOLDERID_PublicDesktop);
    LoadCfg();
    if (m_cfgVer < 2) m_iconSize = 48.0f;
    LoadSlides();
    ApplySlideTimer();
    LoadWorkArea();
    LoadItems();
    LoadWallpaper(true);
    CheckBin();

    m_dropTarget.Attach(new DesktopDropTarget(this));
    RegisterDragDrop(m_hwnd, m_dropTarget.Get());
    SetTimer(m_hwnd, kTimerSync, 1000, nullptr);
    SetTimer(m_hwnd, kTimerGuard, 250, nullptr);
    if (m_primary) {
        g_deskWnd = m_hwnd;
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    }
    AnchorZ();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    StartWatch();
    return true;
}

// The desktop windows of Explorer: Progman, or the WorkerW that hosts the icon view. We sit directly above the higher of the two.
static HWND DesktopAnchor() {
    HWND anchor = FindWindowW(L"Progman", nullptr);
    struct Ctx { HWND found{nullptr}; } c;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
        if (wcscmp(cls, L"WorkerW") == 0 && FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) { reinterpret_cast<Ctx*>(l)->found = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    return c.found ? c.found : anchor;
}

void Desktop::AnchorZ() {
    HWND a = DesktopAnchor();
    SetWindowPos(m_hwnd, a ? a : HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK Desktop::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Desktop* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Desktop*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else self = reinterpret_cast<Desktop*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT Desktop::Handle(UINT m, WPARAM w, LPARAM l) {
    const float s = Scale();
    if (m == m_taskbarCreated && m_taskbarCreated) { AnchorZ(); LoadWorkArea(); Place(); Kick(); return 0; }
    switch (m) {
    case WM_WINDOWPOSCHANGING: {                    // never rise above normal windows
        auto* wp = reinterpret_cast<WINDOWPOS*>(l);
        if (HWND a = DesktopAnchor()) { wp->hwndInsertAfter = a; wp->flags &= ~SWP_NOZORDER; }
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps); m_dirty = true; Frame(); return 0; }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && m_dcomp) { m_dcomp->Resize(LOWORD(l), HIWORD(l)); LoadWorkArea(); Place(); LoadWallpaper(true); m_dirty = true; Frame(); }
        return 0;
    case WM_DISPLAYCHANGE: {
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(m_monitor ? m_monitor : MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            SetWindowPos(m_hwnd, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top - 1, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DPICHANGED:
        m_dpi = static_cast<float>(LOWORD(w)); m_fmts.clear(); m_pfmts.clear();
        for (auto& it : m_items) { it.lay.Reset(); it.layW = -1; }
        LoadWorkArea(); Place(); LoadWallpaper(true); Kick();
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETDESKWALLPAPER || w == SPI_SETWORKAREA || w == 0) { LoadWorkArea(); LoadWallpaper(false); Place(); Kick(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor((m_panel && m_hotCtl) ? bui::Link() : m_hotHeader ? LoadCursorW(nullptr, IDC_SIZEALL) : bui::Arrow()); return TRUE; }
        break;
    case WM_MOUSEMOVE: {
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        if (m_panelDrag) {                       // moving the customize panel by its title strip
            if (w & MK_LBUTTON) { m_panelX = m_mx - m_panelGrabX; m_panelY = m_my - m_panelGrabY; m_panelMoved = true; Kick(); }
            else m_panelDrag = false;
            return 0;
        }
        if (!m_tracking) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, m_hwnd, 0}; TrackMouseEvent(&t); m_tracking = true; }
        bool changed = false;
        const bool overPanel = m_panel && m_panelR.Contains(m_mx, m_my);
        const int hc = overPanel ? CtlAt(m_mx, m_my) : 0;
        const bool hh = overPanel && hc == 0 && m_my < m_panelR.y + 66.0f;   // empty title strip: drag handle
        if (hh != m_hotHeader) { m_hotHeader = hh; changed = true; }
        if (hc != m_hotCtl) { m_hotCtl = hc; changed = true; }
        if (m_dragCtl) { PanelSlider(m_dragCtl, m_mx); changed = true; }
        const int hot = (m_showIcons && !overPanel && !m_dragCtl) ? HitItem(m_mx, m_my) : -1;
        if (hot != m_hot) { m_hot = hot; changed = true; }
        if (m_leftDown && !m_dragCtl && (w & MK_LBUTTON)) {
            if (m_pressItem >= 0 && !m_dragging && (std::abs(m_mx - m_pressX) > 5 || std::abs(m_my - m_pressY) > 5)) {
                m_dragging = true;
                if (m_pressItem < static_cast<int>(m_sel.size()) && !m_sel[m_pressItem]) SelectOnly(m_pressItem);
                m_pressSelectOnUp = false;
            }
            if (m_dragging) {
                POINT sp = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(m_hwnd, &sp);
                HWND under = GetAncestor(WindowFromPoint(sp), GA_ROOT);
                Desktop* other = under ? SiblingAt(under) : nullptr;
                if (other && other != this) {                    // the selection is being carried onto the sibling screen
                    if (!m_dragTo) BeginHandoff(other, sp);
                    if (m_dragTo) { m_dragTo->HandoffMove(sp); Kick(); return 0; }
                } else if (m_dragTo) {                           // back over our own desktop: take the selection back
                    m_dragTo->HandoffCancel(); m_dragTo = nullptr;
                }
                if (under && under != m_hwnd && !SelectedPaths().empty()) { StartOleDrag(); return 0; }   // leaves the desktop: real OLE drag
                m_dropHover = -1;
                for (int i = 0; i < Count(); ++i) {
                    if (i < static_cast<int>(m_sel.size()) && m_sel[i]) continue;
                    if (!(m_items[i].dir || m_items[i].kind == K_Bin)) continue;
                    if (ItemRect(i).Contains(m_mx, m_my)) { m_dropHover = i; break; }
                }
                const auto z = Z();
                const float gx = m_mx - m_grabDX, gy = m_my - m_grabDY;
                const int tc = static_cast<int>(std::lround((gx - (m_work.x + 8)) / z.cw)), tr = static_cast<int>(std::lround((gy - (m_work.y + 8)) / z.ch));
                const int dc = tc - m_items[m_pressItem].col, dr = tr - m_items[m_pressItem].row;
                m_tcol = tc; m_trow = tr; m_tvalid = true;
                const auto sel = SelectedIdx();
                for (int i : sel) {
                    const int c = m_items[i].col + dc, r = m_items[i].row + dr;
                    if (c < 0 || r < 0 || c >= m_cols || r >= m_rows || Occupied(c, r, sel)) { m_tvalid = false; break; }
                }
            }
        }
        if (m_marquee) {
            if (!(w & MK_LBUTTON)) m_marquee = false;
            else {
                const float x0 = std::min(m_mqX, m_mx), x1 = std::max(m_mqX, m_mx), y0 = std::min(m_mqY, m_my), y1 = std::max(m_mqY, m_my);
                for (int i = 0; i < Count(); ++i) {
                    const Rect c = ItemRect(i);
                    const bool in = c.x < x1 && c.Right() > x0 && c.y < y1 && c.Bottom() > y0;
                    m_sel[i] = in ? 1 : (i < static_cast<int>(m_mqBase.size()) ? m_mqBase[i] : 0);
                }
            }
        }
        if (changed || m_dragging || m_marquee) Kick();      // a plain mouse move over empty desktop costs nothing
        return 0;
    }
    case WM_MOUSELEAVE: m_tracking = false; m_mx = m_my = -1; m_hot = -1; m_hotCtl = 0; m_hotHeader = false; Kick(); return 0;
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(m_hwnd, &p);
        const bool overPanel = m_panel && m_panelR.Contains(p.x / s, p.y / s);
        if ((GET_KEYSTATE_WPARAM(w) & MK_CONTROL) && !overPanel) {            // Ctrl + wheel: icon size, like Windows
            m_iconSize = std::clamp(std::round(m_iconSize + GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 6.0f), 32.0f, 110.0f);
            Place(); Kick();
            SetTimer(m_hwnd, kTimerSave, 600, nullptr);
            return 0;
        }
        if (overPanel) {
            const float maxS = std::max(0.0f, m_panelContentH - m_panelView.height);
            m_panelScroll = std::clamp(m_panelScroll - GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 56.0f, 0.0f, maxS);
            Kick();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(m_hwnd); SetCapture(m_hwnd);
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        if (m_panel) {
            if (m_panelR.Contains(m_mx, m_my)) {
                const int id = CtlAt(m_mx, m_my);
                if (id == 0 && m_my < m_panelR.y + 66.0f) {          // grab the empty title strip and drag the panel anywhere
                    m_panelDrag = true; m_panelMoved = true;
                    m_panelGrabX = m_mx - m_panelR.x; m_panelGrabY = m_my - m_panelR.y;
                    Kick(); return 0;
                }
                m_hotCtl = id;
                if (id >= 60 && id <= 62) { m_dragCtl = id; PanelSlider(id, m_mx); }
                else if (id) PanelAction(id);
                Kick();
                return 0;
            }
            m_panel = false; m_hotCtl = 0;
        }
        if (m_edit) CommitEdit();
        m_leftDown = true; m_dragging = false; m_marquee = false; m_pressSelectOnUp = false;
        const bool ctrl = (w & MK_CONTROL) != 0, shift = (w & MK_SHIFT) != 0;
        const int i = m_showIcons ? HitItem(m_mx, m_my) : -1;
        m_pressItem = i;
        if (i >= 0) {
            m_pressX = m_mx; m_pressY = m_my;
            const Rect r = ItemRect(i); m_grabDX = m_mx - r.x; m_grabDY = m_my - r.y;
            if (shift && m_cursor >= 0) {
                const int a = std::min(m_cursor, i), b = std::max(m_cursor, i);
                std::fill(m_sel.begin(), m_sel.end(), 0);
                for (int k = a; k <= b; ++k) m_sel[k] = 1;
            } else if (ctrl) { m_sel[i] = !m_sel[i]; m_cursor = i; }
            else if (m_sel[i]) { m_pressSelectOnUp = true; m_cursor = i; }
            else SelectOnly(i);
        } else {
            m_marquee = true; m_mqX = m_mx; m_mqY = m_my;
            if (ctrl) m_mqBase = m_sel; else { m_mqBase.assign(m_sel.size(), 0); std::fill(m_sel.begin(), m_sel.end(), 0); }
        }
        Kick();
        return 0;
    }
    case WM_LBUTTONUP:
        if (m_panelDrag) { m_panelDrag = false; ReleaseCapture(); Kick(); return 0; }
        if (m_dragCtl) { m_dragCtl = 0; SaveCfg(); ReleaseCapture(); Kick(); return 0; }
        if (m_dragging) {
            if (m_dragTo) {                                      // released on the sibling screen: commit there
                POINT sp = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(m_hwnd, &sp);
                const auto placed = m_dragTo->HandoffDrop(sp);
                m_dragTo = nullptr;
                if (!placed.empty()) {
                    m_auto = false;
                    for (const auto& p : placed) m_pos[p.first] = p.second;
                    LoadItems();
                }
            } else FinishOwnDrag();
        }
        else if (m_pressSelectOnUp && m_pressItem >= 0) SelectOnly(m_pressItem);
        m_dragging = false; m_marquee = false; m_leftDown = false; m_pressItem = -1; m_pressSelectOnUp = false; m_dropHover = -1;
        ReleaseCapture(); Kick();
        return 0;
    case WM_CAPTURECHANGED:
        if (m_dragTo) { m_dragTo->HandoffCancel(); m_dragTo = nullptr; }   // capture was stolen mid-carry: abort cleanly
        return 0;
    case WM_LBUTTONDBLCLK: {
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        if (m_panel && m_panelR.Contains(m_mx, m_my)) return Handle(WM_LBUTTONDOWN, w, l);
        const int i = m_showIcons ? HitItem(m_mx, m_my) : -1;
        if (i >= 0) OpenItem(i);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        if (m_panel && m_panelR.Contains(m_mx, m_my)) return 0;
        if (m_edit) CommitEdit();
        const int i = m_showIcons ? HitItem(m_mx, m_my) : -1;
        if (i >= 0 && !m_sel[i]) SelectOnly(i);
        else if (i < 0) std::fill(m_sel.begin(), m_sel.end(), 0);
        Kick();
        return 0;
    }
    case WM_RBUTTONUP: {
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        if (m_panel && m_panelR.Contains(m_mx, m_my)) return 0;
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(m_hwnd, &pt);
        const int i = m_showIcons ? HitItem(m_mx, m_my) : -1;
        if (i >= 0) { m_cursor = i; ItemMenu(i, pt); } else BackgroundMenu(pt);
        return 0;
    }
    case WM_KEYDOWN: OnKey(w); return 0;
    case WM_CHAR: OnChar(static_cast<wchar_t>(w)); return 0;
    case kMsgShowDesk: ToggleShowDesktop(); return 0;
    case kMsgWinE: OpenFolder(L""); return 0;
    case kMsgIcon: OnIcon(reinterpret_cast<IconRes*>(l)); return 0;
    case kMsgChanged: SetTimer(m_hwnd, kTimerReload, 200, nullptr); return 0;
    case kMsgOpDone: {
        const std::wstring sel = OpDoneSel(l);
        if (!sel.empty()) m_pendingSelect = sel;
        LoadItems(); CheckBin();
        return 0;
    }
    case WM_TIMER:
        if (w == kTimerSync) {
            if (render::glass::SyncTheme()) Kick();
            // a sibling desktop saved the shared config (or carried icons here): pick its changes up within a second
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(CfgPath().c_str(), GetFileExInfoStandard, &fa) &&
                (fa.ftLastWriteTime.dwLowDateTime != m_cfgWrite.dwLowDateTime || fa.ftLastWriteTime.dwHighDateTime != m_cfgWrite.dwHighDateTime)) {
                m_cfgWrite = fa.ftLastWriteTime;
                LoadCfg(); LoadItems(); LoadWallpaper(false); CheckBin(); Kick();
            }
            const ULONGLONG tn = GetTickCount64();
            for (auto it = m_pendingDrop.begin(); it != m_pendingDrop.end();) if (tn - it->second.second > 15000) it = m_pendingDrop.erase(it); else ++it;
            static int n = 0;
            if (++n % 5 == 0) { CheckBin(); LoadWallpaper(false); }
            if (GetWindow(m_hwnd, GW_HWNDPREV) == nullptr) AnchorZ();
            if (m_clock) { SYSTEMTIME st; GetLocalTime(&st); if (st.wMinute != m_lastMin) { m_lastMin = st.wMinute; Kick(); } }
        } else if (w == kTimerGuard) {
            // whatever "show desktop" / "minimize all" does, the desktop itself stays put
            if (IsIconic(m_hwnd) || !IsWindowVisible(m_hwnd)) { ShowWindow(m_hwnd, SW_SHOWNOACTIVATE); AnchorZ(); Kick(); }
            else { HWND a = DesktopAnchor(); if (a && GetWindow(a, GW_HWNDPREV) != m_hwnd) AnchorZ(); }   // Explorer raised its own desktop: climb back above it
        } else if (w == kTimerSave) { KillTimer(m_hwnd, kTimerSave); SaveCfg(); }
        else if (w == kTimerReload) { KillTimer(m_hwnd, kTimerReload); LoadItems(); CheckBin(); }
        else if (w == kTimerSlide) { if (!m_slides.empty()) { m_slideIdx = (m_slideIdx + 1) % static_cast<int>(m_slides.size()); LoadWallpaper(true); } }
        else if (w == kTimerCaret) Kick();
        return 0;
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
        if (m_cm3) { LRESULT res = 0; if (SUCCEEDED(m_cm3->HandleMenuMsg2(m, w, l, &res))) return res; }
        else if (m_cm2) { if (SUCCEEDED(m_cm2->HandleMenuMsg(m, w, l))) return 0; }
        break;
    case WM_DESTROY:
        if (m_watchStop) SetEvent(m_watchStop);
        if (m_primary && g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        if (m_primary) g_deskWnd = nullptr;
        if (m_pwnd) { DestroyWindow(m_pwnd); m_pwnd = nullptr; }
        SaveCfg(); RevokeDragDrop(m_hwnd);
        {   // the process quits only when the last desktop window goes away
            auto& sib = Siblings();
            bool last = true;
            for (Desktop* d : sib) if (d != this && d->m_hwnd && d->m_hwnd != m_hwnd && IsWindow(d->m_hwnd)) { last = false; break; }
            sib.erase(std::remove(sib.begin(), sib.end(), this), sib.end());
            if (last) PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// ---- frame loop ---------------------------------------------------------------------------------------------------------
void Desktop::Frame() {
    if (m_inFrame) return;
    m_inFrame = true;
    Tick();
    const bool full = m_dirty || m_animating;
    m_dirty = false;
    if (full) Render();
    m_pdirty = false;
    RenderPanel();
    m_inFrame = false;
}

void Desktop::Tick() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - m_last.QuadPart) / static_cast<float>(m_freq.QuadPart);
    m_last = now;
    dt = std::clamp(dt, 0.001f, 0.05f);
    bool anim = false;
    if (m_hot >= 0) m_hover[m_hot];
    for (auto it = m_hover.begin(); it != m_hover.end();) {
        const float target = (it->first == m_hot) ? 1.0f : 0.0f;
        const float old = it->second;
        it->second = Ease(old, target, dt, 22.0f);
        if (std::fabs(it->second - target) < 0.02f) it->second = target;
        if (it->second != old) anim = true;
        if (it->second <= 0.0f && target == 0.0f) it = m_hover.erase(it); else ++it;
    }
    if (m_edit) anim = true;          // caret blink
    if (m_wallFade < 1.0f) {          // wallpaper cross-fade
        m_wallFade = std::min(1.0f, m_wallFade + dt / 0.7f);
        if (m_wallFade >= 1.0f) m_wallPrev = Wall{};
        anim = true;
    }
    const float pt = m_panel ? 1.0f : 0.0f;
    if (m_panelA != pt) {
        m_panelA = Ease(m_panelA, pt, dt, 16.0f);
        if (std::fabs(m_panelA - pt) < 0.01f) m_panelA = pt;
        anim = true;
    }
    if (!m_iconT.empty()) {           // icon fade-in
        const ULONGLONG t = GetTickCount64();
        for (auto it = m_iconT.begin(); it != m_iconT.end();) { if (t - it->second >= 200) it = m_iconT.erase(it); else { anim = true; ++it; } }
    }
    m_animating = anim;
}

int Desktop::HitItem(float x, float y) const {
    for (int i = Count() - 1; i >= 0; --i)
        if (m_items[i].col >= 0 && ItemRect(i).Contains(x, y)) return i;
    return -1;
}

bool Desktop::Occupied(int col, int row, const std::vector<int>& ignore) const {
    for (int i = 0; i < Count(); ++i) {
        if (m_items[i].col != col || m_items[i].row != row) continue;
        if (std::find(ignore.begin(), ignore.end(), i) != ignore.end()) continue;
        return true;
    }
    return false;
}

// ---- settings ---------------------------------------------------------------------------------------------------------
static std::wstring ReadCfgText(const std::wstring& p) {
    std::wstring w;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return w;
    std::string buf(1 << 20, '\0'); DWORD n = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(buf.size() - 1), &n, nullptr);
    CloseHandle(h);
    buf.resize(n);
    const int wl = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0);
    w.assign(static_cast<size_t>(std::max(0, wl)), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), w.data(), wl);
    return w;
}

void Desktop::LoadCfg() {
    const std::wstring p = CfgPath();
    if (p.empty()) return;
    const std::wstring w = ReadCfgText(p);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) m_cfgWrite = fa.ftLastWriteTime;
    size_t pos = 0;
    while (pos < w.size()) {
        size_t e = w.find(L'\n', pos);
        if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = line.substr(0, eq), val = line.substr(eq + 1);
        m_applied[k] = val;                             // remember what was on disk: SaveCfg keeps sibling edits alive
        const int iv = _wtoi(val.c_str());
        if (k == L"isize") m_iconSize = std::clamp(static_cast<float>(iv), 32.0f, 110.0f);
        else if (k == L"size") m_iconSize = iv == 0 ? 36.0f : (iv == 2 ? 72.0f : 48.0f);      // older config
        else if (k == L"ver") m_cfgVer = iv;
        else if (k == L"auto") m_auto = iv != 0;
        else if (k == L"icons") m_showIcons = iv != 0;
        else if (k == L"pc") m_showPC = iv != 0;
        else if (k == L"bin") m_showBin = iv != 0;
        else if (k == L"user") m_showUser = iv != 0;
        else if (k == L"net") m_showNet = iv != 0;
        else if (k == L"sort") m_sortKey = std::clamp(iv, 0, 3);
        else if (k == L"dim") m_dim = std::clamp(iv / 100.0f, 0.0f, 0.7f);
        else if (k == L"blur") m_blur = std::clamp(iv, 0, 3);
        else if (k == L"fit") m_fit = (iv == -1 || iv == 0 || iv == 2 || iv == 6 || iv == 10 || iv == 100) ? iv : -1;
        else if (k == L"label") m_labelStyle = std::clamp(iv, 0, 2);
        else if (k == L"accent") m_accent = std::clamp(iv, 0, 5);
        else if (k == L"glow") m_glow = std::clamp(iv / 100.0f, 0.0f, 2.0f);
        else if (k == L"clock") m_clock = iv != 0;
        else if (k == L"slide") m_slide = iv != 0;
        else if (k == L"slidemin") m_slideMin = std::clamp(iv, 1, 600);
        else if (k == L"slidedir") m_slideDir = val;
        else if (k == L"pos") {
            // v3: name <tab> monitor <tab> col <tab> row — v2 (and older) had no monitor and meant the primary
            std::vector<std::wstring> tk; size_t s0 = 0;
            for (;;) { const size_t t = val.find(L'\t', s0); if (t == std::wstring::npos) { tk.push_back(val.substr(s0)); break; } tk.push_back(val.substr(s0, t - s0)); s0 = t + 1; }
            if (tk.size() >= 4) m_pos[tk[0]] = Pos3{_wtoi(tk[1].c_str()), _wtoi(tk[2].c_str()), _wtoi(tk[3].c_str())};
            else if (tk.size() == 3) m_pos[tk[0]] = Pos3{0, _wtoi(tk[1].c_str()), _wtoi(tk[2].c_str())};
        }
    }
}

void Desktop::SaveCfg() {
    const std::wstring p = CfgPath();
    if (p.empty()) return;
    auto I = [](int v) { return std::to_wstring(v); };
    // what the other desktop windows wrote most recently: their appearance choices and their monitors' positions stay
    std::map<std::wstring, std::wstring> disk;
    const std::wstring old = ReadCfgText(p);
    {
        size_t pos = 0;
        while (pos < old.size()) {
            size_t e = old.find(L'\n', pos);
            if (e == std::wstring::npos) e = old.size();
            std::wstring line = old.substr(pos, e - pos);
            pos = e + 1;
            while (!line.empty() && line.back() == L'\r') line.pop_back();
            const size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            disk[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    std::wstring w = L"ver=3\n";
    auto put = [&](const wchar_t* key, const std::wstring& mine) {
        // a value we did not touch since the last load must not clobber what a sibling desktop just saved
        auto a = m_applied.find(key), d = disk.find(key);
        const std::wstring use = (a != m_applied.end() && a->second == mine && d != disk.end()) ? d->second : mine;
        w += std::wstring(key) + L"=" + use + L"\n";
        m_applied[key] = use;
    };
    put(L"isize", I(static_cast<int>(std::lround(m_iconSize))));
    put(L"auto", I(m_auto));
    put(L"icons", I(m_showIcons));
    put(L"pc", I(m_showPC));
    put(L"bin", I(m_showBin));
    put(L"user", I(m_showUser));
    put(L"net", I(m_showNet));
    put(L"sort", I(m_sortKey));
    put(L"dim", I(static_cast<int>(std::lround(m_dim * 100))));
    put(L"blur", I(m_blur));
    put(L"fit", I(m_fit));
    put(L"label", I(m_labelStyle));
    put(L"accent", I(m_accent));
    put(L"glow", I(static_cast<int>(std::lround(m_glow * 100))));
    put(L"clock", I(m_clock));
    put(L"slide", I(m_slide));
    put(L"slidemin", I(m_slideMin));
    put(L"slidedir", m_slideDir);
    std::map<std::wstring, std::wstring> others;        // the other monitors' placements, one line per item key
    size_t pos = 0;
    while (pos < old.size()) {
        size_t e = old.find(L'\n', pos);
        if (e == std::wstring::npos) e = old.size();
        std::wstring line = old.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(L"pos=", 0) != 0) continue;
        std::vector<std::wstring> tk; size_t s0 = 4;
        for (;;) { const size_t t = line.find(L'\t', s0); if (t == std::wstring::npos) { tk.push_back(line.substr(s0)); break; } tk.push_back(line.substr(s0, t - s0)); s0 = t + 1; }
        const int mon = tk.size() >= 4 ? _wtoi(tk[1].c_str()) : 0;
        if (mon != m_mon && tk.size() >= 2) others[tk[0]] = line;
    }
    for (const auto& it : m_items) {
        if (it.col < 0) continue;
        others.erase(it.key);                           // an item lives on exactly one monitor, never both
        w += L"pos=" + it.key + L"\t" + I(m_mon) + L"\t" + I(it.col) + L"\t" + I(it.row) + L"\n";
    }
    for (const auto& o : others) w += o.second + L"\n";
    const int bl = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string b(static_cast<size_t>(std::max(0, bl)), '\0');
    if (bl > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), b.data(), bl, nullptr, nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr); CloseHandle(h);
    WIN32_FILE_ATTRIBUTE_DATA fa;                     // remember our own write: the 1s sync timer must not reload it as a sibling edit
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) m_cfgWrite = fa.ftLastWriteTime;
}

// ---- work area, wallpaper, items ----------------------------------------------------------------------------------------
void Desktop::LoadWorkArea() {
    MONITORINFO mi = {sizeof(mi)};                                   // this window's own monitor: secondary screens have their own work area
    if (!GetMonitorInfoW(m_monitor ? m_monitor : MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    RECT wr{}; GetWindowRect(m_hwnd, &wr);
    RECT cr{}; GetClientRect(m_hwnd, &cr);
    const float s = Scale();
    m_W = cr.right / s; m_H = cr.bottom / s;
    m_work = Rect{(mi.rcWork.left - wr.left) / s, (mi.rcWork.top - wr.top) / s, (mi.rcWork.right - mi.rcWork.left) / s, (mi.rcWork.bottom - mi.rcWork.top) / s};
}

bool Desktop::BakeWall(const std::wstring& path, int style, Wall& out) {
    if (!m_dcomp || !m_dcomp->GetContext()) return false;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) || !dec) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr)) || !fr) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    // EXIF orientation: phone photos are stored turned on disk. Windows honours the flag when it draws a wallpaper;
    // without it a portrait photo comes out sideways and every fit mode looks cropped / out of proportion.
    UINT orient = 1;
    {
        ComPtr<IWICMetadataQueryReader> qr;
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(fr->GetMetadataQueryReader(&qr)) && qr &&
            SUCCEEDED(qr->GetMetadataByName(L"/app1/ifd/exif/{ushort=274}", &v))) {
            if (v.vt == VT_UI2) orient = v.uiVal;
            else if (v.vt == VT_I2 && v.iVal > 0) orient = static_cast<UINT>(v.iVal);
            else if (v.vt == VT_UI1) orient = v.bVal;
        }
        PropVariantClear(&v);
    }
    ComPtr<IWICBitmapSource> oriented = conv;
    auto rot = [&](WICBitmapTransformOptions o) {
        ComPtr<IWICBitmapFlipRotator> f;
        if (SUCCEEDED(wic->CreateBitmapFlipRotator(&f)) && SUCCEEDED(f->Initialize(oriented.Get(), o))) oriented = f;
    };
    switch (orient) {
        case 2: rot(WICBitmapTransformFlipHorizontal); break;
        case 3: rot(WICBitmapTransformRotate180); break;
        case 4: rot(WICBitmapTransformFlipVertical); break;
        case 5: rot(WICBitmapTransformFlipHorizontal); rot(WICBitmapTransformRotate90); break;
        case 6: rot(WICBitmapTransformRotate90); break;
        case 7: rot(WICBitmapTransformFlipHorizontal); rot(WICBitmapTransformRotate270); break;
        case 8: rot(WICBitmapTransformRotate270); break;
        default: break;
    }
    UINT iw = 0, ih = 0; oriented->GetSize(&iw, &ih);
    if (!iw || !ih) return false;
    const float s = Scale();
    const double sw = m_W * s, sh = m_H * s;
    double tw = iw, th = ih;
    switch (style) {
        case 2: tw = sw; th = sh; break;                                                           // stretch
        case 6: { const double k = std::min(sw / iw, sh / ih); tw = iw * k; th = ih * k; break; }     // fit
        case 0: case 100: break;                                                                  // center / tile: 1:1
        default: { const double k = std::max(sw / iw, sh / ih); tw = iw * k; th = ih * k; break; }    // fill / span
    }
    const bool tile = style == 100;
    static const int kDiv[4] = {1, 5, 10, 18};                    // blur = bake small, draw stretched with linear filtering
    const int div = tile ? 1 : kDiv[std::clamp(m_blur, 0, 3)];
    double bw = tw / div + 0.5, bh = th / div + 0.5;
    if (bw > 8192.0 || bh > 8192.0) {                             // GPU-safe cap, both axes together so the aspect never skews
        const double c = 8192.0 / std::max(bw, bh);
        bw = std::floor(bw * c); bh = std::floor(bh * c);
    }
    const UINT BW = std::clamp<UINT>(static_cast<UINT>(std::max(1.0, std::floor(bw))), 1u, 8192u);
    const UINT BH = std::clamp<UINT>(static_cast<UINT>(std::max(1.0, std::floor(bh))), 1u, 8192u);
    ComPtr<IWICBitmapSource> src = oriented;
    if (BW != iw || BH != ih) {
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(oriented.Get(), BW, BH, WICBitmapInterpolationModeFant))) src = sc;
    }
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(m_dcomp->GetContext()->CreateBitmapFromWicBitmap(src.Get(), nullptr, &bmp)) || !bmp) return false;
    out.bmp = bmp; out.tile = tile;
    // destination from the bitmap that was actually baked: pixels and drawn rectangle can never disagree in aspect
    const D2D1_SIZE_U px = bmp->GetPixelSize();
    const float dw = px.width * div / s, dh = px.height * div / s;
    out.dst = Rect{(m_W - dw) / 2, (m_H - dh) / 2, dw, dh};
    return true;
}

void Desktop::LoadWallpaper(bool force) {
    int wstyle = 10; bool wtile = false;
    {
        wchar_t st[16] = L"10", tl[16] = L"0";
        DWORD n = sizeof(st); RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", RRF_RT_REG_SZ, nullptr, st, &n);
        n = sizeof(tl); RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", RRF_RT_REG_SZ, nullptr, tl, &n);
        wstyle = _wtoi(st); wtile = _wtoi(tl) != 0;
        wchar_t bg[32] = {}; n = sizeof(bg);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background", RRF_RT_REG_SZ, nullptr, bg, &n) == ERROR_SUCCESS) {
            int r = 0, g = 0, b = 0;
            if (swscanf(bg, L"%d %d %d", &r, &g, &b) == 3) m_bgColor = Color{r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
        }
    }
    std::wstring path;
    if (m_slide && !m_slides.empty()) path = m_slides[static_cast<size_t>(m_slideIdx) % m_slides.size()];
    else path = system::wallpaper::Current();
    const int style = m_fit >= 0 ? m_fit : (wtile ? 100 : wstyle);
    WIN32_FILE_ATTRIBUTE_DATA ad{};
    if (!path.empty()) GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &ad);
    wchar_t kb[96];
    swprintf(kb, 96, L"|%d|%d|%d|%d|%08x%08x", style, m_blur, static_cast<int>(m_W * Scale()), static_cast<int>(m_H * Scale()),
             static_cast<unsigned>(ad.ftLastWriteTime.dwHighDateTime), static_cast<unsigned>(ad.ftLastWriteTime.dwLowDateTime));
    const std::wstring key = path + kb;
    if (!force && key == m_wallKey) return;                 // unchanged (also when the file could not be decoded: do not retry every few seconds)
    m_wallKey = key;
    Wall nw;
    const bool ok = !path.empty() && BakeWall(path, style, nw);
    if (m_wall.bmp) { m_wallPrev = m_wall; m_wallFade = 0.0f; }
    m_wall = ok ? nw : Wall{};
    Kick();
}

void Desktop::LoadSlides() {
    m_slides.clear();
    if (m_slideDir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(JoinPath(m_slideDir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring e = Lower(ExtOf(fd.cFileName));
        if (e == L".jpg" || e == L".jpeg" || e == L".png" || e == L".bmp" || e == L".webp" || e == L".tif" || e == L".tiff") m_slides.push_back(JoinPath(m_slideDir, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(m_slides.begin(), m_slides.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
}

void Desktop::ApplySlideTimer() {
    KillTimer(m_hwnd, kTimerSlide);
    if (m_slide && !m_slides.empty()) SetTimer(m_hwnd, kTimerSlide, static_cast<UINT>(m_slideMin) * 60000u, nullptr);
}

void Desktop::PickSlideFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
    DWORD o = 0; dlg->GetOptions(&o); dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Slayt gösterisi klasörü");
    if (FAILED(dlg->Show(m_hwnd))) return;
    ComPtr<IShellItem> it; PWSTR p = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        m_slideDir = p; CoTaskMemFree(p);
        m_slideIdx = 0; LoadSlides();
    }
}

void Desktop::LoadItems() {
    std::set<std::wstring> keepSel;
    for (int i : SelectedIdx()) keepSel.insert(m_items[i].key);
    std::vector<Item> v;
    auto special = [&](const wchar_t* key, const wchar_t* clsid, const wchar_t* label, Kind k) {
        Item it; it.key = key; it.path = clsid; it.label = label; it.kind = k; it.dir = true; v.push_back(std::move(it));
    };
    if (m_showPC) special(L"::pc", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"Bu Bilgisayar", K_PC);
    if (m_showUser) {
        const std::wstring prof = KnownFolder(FOLDERID_Profile);
        wchar_t un[256] = {}; DWORD n = 256; GetUserNameW(un, &n);
        if (!prof.empty()) { Item it; it.key = Lower(prof); it.path = prof; it.label = un; it.kind = K_User; it.dir = true; v.push_back(std::move(it)); }
    }
    if (m_showNet) special(L"::net", L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", L"Ağ", K_Net);
    if (m_showBin) special(L"::bin", L"::{645FF040-5081-101B-9F08-00AA002F954E}", L"Geri Dönüşüm Kutusu", K_Bin);

    std::vector<Item> files;
    for (const std::wstring& dir : {m_desktopDir, m_publicDir}) {
        if (dir.empty()) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;
            Item it;
            it.path = JoinPath(dir, fd.cFileName);
            it.key = Lower(it.path);
            it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            it.kind = it.dir ? K_Folder : K_File;
            it.ext = it.dir ? L"" : ExtOf(fd.cFileName);
            it.lnk = it.ext == L".lnk" || it.ext == L".url";
            it.label = fd.cFileName;
            if (it.lnk) it.label.erase(it.label.size() - it.ext.size());
            it.mtime = fd.ftLastWriteTime;
            it.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            files.push_back(std::move(it));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto less = [&](const Item& a, const Item& b) {
        if (a.dir != b.dir) return a.dir;
        switch (m_sortKey) {
            case 1: if (a.size != b.size) return a.size > b.size; break;
            case 2: { const int c = _wcsicmp(a.ext.c_str(), b.ext.c_str()); if (c) return c < 0; break; }
            case 3: { const int c = CompareFileTime(&a.mtime, &b.mtime); if (c) return c > 0; break; }
        }
        return StrCmpLogicalW(a.label.c_str(), b.label.c_str()) < 0;
    };
    std::stable_sort(files.begin(), files.end(), less);
    for (auto& f : files) v.push_back(std::move(f));

    m_items = std::move(v);
    m_sel.assign(m_items.size(), 0);
    m_cursor = -1;
    for (int i = 0; i < Count(); ++i) if (keepSel.count(m_items[i].key)) { m_sel[i] = 1; if (m_cursor < 0) m_cursor = i; }
    bool consumed = false;                            // external drops that asked for this monitor while the file was still copying
    const ULONGLONG now = GetTickCount64();
    for (auto pit = m_pendingDrop.begin(); pit != m_pendingDrop.end();) {
        if (now - pit->second.second > 15000) { pit = m_pendingDrop.erase(pit); continue; }
        auto f = std::find_if(m_items.begin(), m_items.end(), [&](const Item& it) { return IEquals(it.label, pit->first) || IEquals(it.path.substr(it.path.find_last_of(L'\\') + 1), pit->first); });
        if (f != m_items.end()) { m_pos[f->key] = pit->second.first; pit = m_pendingDrop.erase(pit); consumed = true; }
        else ++pit;
    }
    Place();
    if (!m_pendingSelect.empty()) {
        for (int i = 0; i < Count(); ++i)
            if (IEquals(m_items[i].label, m_pendingSelect) || IEquals(m_items[i].path.substr(m_items[i].path.find_last_of(L'\\') + 1), m_pendingSelect)) {
                SelectOnly(i);
                if (m_pendingRename) BeginRename(i);
                break;
            }
        m_pendingSelect.clear(); m_pendingRename = false;
    }
    if (consumed) SaveCfg();
    Kick();
}

// Stored positions first — but only the ones aimed at this monitor — then every still-unplaced item takes the first
// free cell (top-to-bottom, left-to-right) on the primary desktop. A secondary screen hosts exactly what was carried
// onto it, nothing else.
void Desktop::Place() {
    const auto z = Z();
    m_rows = std::max(1, static_cast<int>((m_work.height - 16) / z.ch));
    m_cols = std::max(1, static_cast<int>((m_work.width - 16) / z.cw));
    std::vector<char> occ(static_cast<size_t>(m_rows) * m_cols, 0);
    auto at = [&](int c, int r) -> char& { return occ[static_cast<size_t>(c) * m_rows + r]; };
    const bool ignorePos = m_auto && m_primary;       // auto arrange: every icon flows into place like Windows
    for (auto& it : m_items) {
        it.col = it.row = -1;
        if (ignorePos) continue;
        auto f = m_pos.find(it.key);
        if (f == m_pos.end() || f->second.mon != m_mon) continue;   // not placed yet, or owned by another monitor
        if (f->second.col >= 0 && f->second.row >= 0 && f->second.col < m_cols && f->second.row < m_rows && !at(f->second.col, f->second.row)) {
            it.col = f->second.col; it.row = f->second.row; at(it.col, it.row) = 1;
        }
    }
    if (!m_primary) return;                           // a secondary screen never auto-places: only carried items live there
    int c = 0, r = 0;
    for (auto& it : m_items) {
        if (it.col >= 0) continue;
        auto f = m_pos.find(it.key);
        if (f != m_pos.end() && f->second.mon != m_mon) continue;   // carried to the other screen: it must not appear here
        while (c < m_cols && at(c, r)) { if (++r >= m_rows) { r = 0; ++c; } }
        if (c >= m_cols) { c = m_cols - 1; r = m_rows - 1; }          // desktop full: stack in the last cell
        it.col = c; it.row = r; at(c, r) = 1;
        if (++r >= m_rows) { r = 0; ++c; }
    }
}

void Desktop::ArrangeBy(int key) {
    m_sortKey = key;
    std::map<std::wstring, Pos3> keep;                // the other monitors' placements survive an arrange on this one
    for (const auto& p : m_pos) if (p.second.mon != m_mon) keep[p.first] = p.second;
    m_pos = std::move(keep);
    for (auto& it : m_items) it.col = it.row = -1;
    const bool wasAuto = m_auto; m_auto = false;
    LoadItems();                         // re-sorts and places every item in order
    for (const auto& it : m_items) m_pos[it.key] = Pos3{m_mon, it.col, it.row};
    m_auto = wasAuto;
    SaveCfg(); Kick();
}

void Desktop::CheckBin() {
    if (!m_showBin) return;
    SHQUERYRBINFO qi = {sizeof(qi)};
    const bool full = SUCCEEDED(SHQueryRecycleBinW(nullptr, &qi)) && qi.i64NumItems > 0;
    if (full != m_binFull) { m_binFull = full; Kick(); }
}

void Desktop::StartWatch() {
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) return;
    m_watchStop = stop;
    const HWND hwnd = m_hwnd; const std::wstring a = m_desktopDir, b = m_publicDir;
    std::thread([=]() {
        HANDLE h[3]; int n = 0;
        h[n++] = stop;
        HANDLE ha = a.empty() ? INVALID_HANDLE_VALUE : FindFirstChangeNotificationW(a.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        HANDLE hb = b.empty() ? INVALID_HANDLE_VALUE : FindFirstChangeNotificationW(b.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (ha != INVALID_HANDLE_VALUE) h[n++] = ha;
        if (hb != INVALID_HANDLE_VALUE) h[n++] = hb;
        while (n > 1) {
            const DWORD r = WaitForMultipleObjects(n, h, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
            if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + static_cast<DWORD>(n)) {
                PostMessageW(hwnd, kMsgChanged, 0, 0);
                Sleep(150);
                FindNextChangeNotification(h[r - WAIT_OBJECT_0]);
            }
        }
        if (ha != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ha);
        if (hb != INVALID_HANDLE_VALUE) FindCloseChangeNotification(hb);
        CloseHandle(stop);
    }).detach();
}

// ---- icons ---------------------------------------------------------------------------------------------------------------
void Desktop::RequestIcon(const Item& it) {
    const std::wstring& key = it.key;
    if (m_iconPending.count(key) || m_iconFailed.count(key) || m_iconPending.size() > 64) return;
    m_iconPending.insert(key);
    IQ().Push(m_hwnd, key, it.path, 128, IsThumbExt(it.ext));
}
void Desktop::OnIcon(IconRes* r) {
    std::unique_ptr<IconRes> res(r);
    m_iconPending.erase(res->key);
    if (res->w <= 0 || res->px.empty() || !m_dcomp || !m_dcomp->GetContext()) { m_iconFailed.insert(res->key); Kick(); return; }
    ComPtr<ID2D1Bitmap1> bmp;
    if (SUCCEEDED(m_dcomp->GetContext()->CreateBitmap(D2D1::SizeU(res->w, res->h), res->px.data(), res->w * 4,
                  D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp)))
        { m_icons[res->key] = bmp; m_iconT[res->key] = GetTickCount64(); }
    else m_iconFailed.insert(res->key);
    Kick();
}

// ---- drawing -------------------------------------------------------------------------------------------------------------
void Desktop::FillRR(const Rect& r, float rad, const Color& c) {
    if (c.a <= 0.001f) return;
    m_brush->SetColor(DC(c));
    m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), rad, rad), m_brush.Get());
}
void Desktop::StrokeRR(const Rect& r, float rad, const Color& c, float w) {
    if (c.a <= 0.001f) return;
    m_brush->SetColor(DC(c));
    const float h = w * 0.5f;
    m_ctx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + h, r.y + h, r.Right() - h, r.Bottom() - h), rad, rad), m_brush.Get(), w);
}
void Desktop::GlowRR(const Rect& r, float rad, const Color& c, float spread, float strength) {
    if (render::glass::CurrentTheme().flat) return;
    const int N = 10;
    for (int i = 0; i < N; ++i) {
        const float t = (i + 0.5f) / N, grow = t * spread;
        const float a = strength * (1.0f - t) * (1.0f - t) * 0.30f;
        if (a < 0.004f) continue;
        StrokeRR(Rect{r.x - grow, r.y - grow, r.width + 2 * grow, r.height + 2 * grow}, rad + grow, WithA(c, a), spread / N * 1.9f);
    }
}
IDWriteTextFormat* Desktop::Fmt(float size, int weight, bool icon, bool wrap) {
    const uint32_t key = static_cast<uint32_t>(size * 10) | (static_cast<uint32_t>(weight / 100) << 14) | (icon ? 1u << 22 : 0u) | (wrap ? 1u << 23 : 0u);
    auto it = m_fmts.find(key);
    if (it != m_fmts.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(m_dcomp->GetDWriteFactory()->CreateTextFormat(icon ? m_iconFont.c_str() : m_textFont.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return nullptr;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f->SetParagraphAlignment(icon ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_EMERGENCY_BREAK : DWRITE_WORD_WRAPPING_NO_WRAP);
    m_fmts[key] = f;
    return f.Get();
}
void Desktop::Glyph(wchar_t g, const Rect& r, float size, const Color& c) {
    auto* f = Fmt(size, 400, true, false);
    if (!f) return;
    m_brush->SetColor(DC(c));
    m_ctx->DrawText(&g, 1, f, D(r), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}
void Desktop::UnitPoly(int id, std::initializer_list<D2D1_POINT_2F> pts, const Rect& r) {
    auto& g = m_polys[id];
    if (!g) {
        ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(m_dcomp->GetD2DFactory()->CreatePathGeometry(&pg))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(pg->Open(&sink))) {
                bool first = true;
                for (const auto& p : pts) { if (first) { sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED); first = false; } else sink->AddLine(p); }
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                g = pg;
            }
        }
    }
    if (!g) return;
    D2D1_MATRIX_3X2_F old; m_ctx->GetTransform(&old);
    m_ctx->SetTransform(D2D1::Matrix3x2F::Scale(r.width, r.height) * D2D1::Matrix3x2F::Translation(r.x, r.y) * old);
    m_ctx->FillGeometry(g.Get(), m_brush.Get());
    m_ctx->SetTransform(old);
}

void Desktop::DrawWall(const Wall& w, float a) {
    if (!w.bmp) return;
    if (w.tile) {
        const D2D1_SIZE_U px = w.bmp->GetPixelSize();
        const float s = Scale(), tw = static_cast<float>(px.width) / s, th = static_cast<float>(px.height) / s;
        for (float y = 0; y < m_H; y += th) for (float x = 0; x < m_W; x += tw) { const D2D1_RECT_F d = D2D1::RectF(x, y, x + tw, y + th); m_ctx->DrawBitmap(w.bmp.Get(), &d, a, D2D1_INTERPOLATION_MODE_LINEAR); }
        return;
    }
    const D2D1_RECT_F d = D(w.dst);
    m_ctx->DrawBitmap(w.bmp.Get(), &d, a, D2D1_INTERPOLATION_MODE_LINEAR);
}

void Desktop::DrawWallpaper() {
    const Rect all{0, 0, m_W, m_H};
    m_brush->SetColor(DC(m_bgColor));
    m_ctx->FillRectangle(D(all), m_brush.Get());
    if (!m_wall.bmp && !m_wallPrev.bmp) {
        // no picture: the theme's own violet backdrop
        const auto& th = render::glass::CurrentTheme();
        if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(th.bodyTop, 1.0f), WithA(th.bodyBot, 1.0f), D2D1::Point2F(0, 0), D2D1::Point2F(m_W * 0.5f, m_H)))
            m_ctx->FillRectangle(D(all), g.Get());
    } else {
        if (m_wallFade < 1.0f && m_wallPrev.bmp) DrawWall(m_wallPrev, 1.0f);
        DrawWall(m_wall, m_wallFade < 1.0f ? m_wallFade : 1.0f);
    }
    if (m_dim > 0.004f) { m_brush->SetColor(D2D1::ColorF(0, 0, 0, m_dim)); m_ctx->FillRectangle(D(all), m_brush.Get()); }
    // faint vignette: keeps white labels readable on bright wallpapers without hiding the picture
    if (auto g = m_dcomp->CreateLinearGradientBrush(Color{0, 0, 0, 0.0f}, Color{0.02f, 0.0f, 0.06f, 0.26f}, D2D1::Point2F(0, m_H * 0.45f), D2D1::Point2F(0, m_H)))
        m_ctx->FillRectangle(D(all), g.Get());
}

void Desktop::DrawIcon(const Item& it, const Rect& r, float alpha) {
    const float w = r.width, h = r.height;
    auto rr = [&](const Rect& q, float rad, const Color& a, const Color& b) {
        if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(a, a.a * alpha), WithA(b, b.a * alpha), D2D1::Point2F(q.x, q.y), D2D1::Point2F(q.x, q.Bottom())))
            m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(q), rad, rad), g.Get());
    };
    auto folder = [&](const Color& a, const Color& b) {
        FillRR(Rect{r.x - 2, r.y + h * 0.20f, w + 4, h * 0.74f}, h * 0.16f, WithA(Acc(), 0.16f * alpha));      // soft glow under the folder
        rr(Rect{r.x + w * 0.04f, r.y + h * 0.10f, w * 0.44f, h * 0.26f}, h * 0.07f, b, b);
        rr(Rect{r.x + w * 0.04f, r.y + h * 0.24f, w * 0.92f, h * 0.66f}, h * 0.12f, a, b);
        FillRR(Rect{r.x + w * 0.12f, r.y + h * 0.26f, w * 0.76f, 1.4f}, 0.7f, Color{1.0f, 1.0f, 1.0f, 0.5f * alpha});
    };
    // theme palette: folders follow the accent, the user folder the second accent, PC / net / bin tints follow too
    const Color& acc = render::glass::kViolet;
    const Color& accL = render::glass::kVioletLight;
    const Color& acc2 = render::glass::kAccent2;
    const Color& acc2L = render::glass::kAccent2Light;
    const Color& accD = render::glass::kVioletDeep;
    switch (it.kind) {
    case K_Folder: folder(Mix(accL, kWhite, 0.25f), acc); return;
    case K_User:
        folder(Mix(acc2L, kWhite, 0.30f), acc2);
        Glyph(0xE77B, Rect{r.x, r.y + h * 0.28f, w, h * 0.62f}, h * 0.36f, Color{1.0f, 1.0f, 1.0f, 0.92f * alpha});
        return;
    case K_PC:
        FillRR(Rect{r.x, r.y + h * 0.04f, w, h * 0.74f}, h * 0.12f, WithA(Acc(), 0.16f * alpha));
        rr(Rect{r.x + w * 0.06f, r.y + h * 0.10f, w * 0.88f, h * 0.58f}, h * 0.07f, accL, accD);
        FillRR(Rect{r.x + w * 0.10f, r.y + h * 0.14f, w * 0.80f, h * 0.50f}, h * 0.05f, Color{0.05f, 0.04f, 0.16f, 0.55f * alpha});
        FillRR(Rect{r.x + w * 0.40f, r.y + h * 0.68f, w * 0.20f, h * 0.12f}, 1.0f, WithA(accD, alpha));
        FillRR(Rect{r.x + w * 0.26f, r.y + h * 0.80f, w * 0.48f, h * 0.08f}, h * 0.04f, WithA(accL, alpha));
        return;
    case K_Net:
        FillRR(Rect{r.x, r.y, w, h}, h * 0.3f, WithA(Acc(), 0.14f * alpha));
        Glyph(0xE774, r, h * 0.92f, WithA(accL, alpha));
        return;
    case K_Bin: {
        FillRR(Rect{r.x, r.y + h * 0.04f, w, h * 0.92f}, h * 0.14f, WithA(Acc(), 0.14f * alpha));
        if (m_binFull) {
            m_brush->SetColor(D2D1::ColorF(0.96f, 0.96f, 1.0f, 0.95f * alpha));
            UnitPoly(201, {{0.0f, 0.0f}, {1.0f, 0.08f}, {0.92f, 1.0f}, {0.04f, 0.92f}}, Rect{r.x + w * 0.24f, r.y + h * 0.00f, w * 0.34f, h * 0.30f});
            m_brush->SetColor(D2D1::ColorF(0.82f, 0.86f, 1.0f, 0.95f * alpha));
            UnitPoly(202, {{0.08f, 0.0f}, {1.0f, 0.12f}, {0.88f, 1.0f}, {0.0f, 0.88f}}, Rect{r.x + w * 0.46f, r.y + h * 0.02f, w * 0.30f, h * 0.26f});
        }
        rr(Rect{r.x + w * 0.38f, r.y + h * 0.10f, w * 0.24f, h * 0.08f}, h * 0.03f, Mix(accL, kWhite, 0.15f), acc2);
        rr(Rect{r.x + w * 0.10f, r.y + h * 0.16f, w * 0.80f, h * 0.10f}, h * 0.04f, Mix(accL, kWhite, 0.35f), acc2);
        m_brush->SetColor(DC(Mix(acc, kWhite, 0.30f), 0.96f * alpha));
        UnitPoly(200, {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.88f, 1.0f}, {0.12f, 1.0f}}, Rect{r.x + w * 0.16f, r.y + h * 0.28f, w * 0.68f, h * 0.66f});
        for (int i = 0; i < 3; ++i) FillRR(Rect{r.x + w * (0.34f + 0.16f * i), r.y + h * 0.38f, 1.8f, h * 0.44f}, 0.9f, WithA(accD, 0.55f * alpha));
        return;
    }
    default: break;
    }
    // files and shortcuts: the real shell icon / thumbnail
    auto f = m_icons.find(it.key);
    {
        auto ft = m_iconT.find(it.key);
        if (ft != m_iconT.end()) alpha *= std::min(1.0f, static_cast<float>(GetTickCount64() - ft->second) / 180.0f);
    }
    if (f != m_icons.end() && f->second) {
        const D2D1_SIZE_U px = f->second->GetPixelSize();
        const float a = static_cast<float>(px.width) / std::max<UINT>(1, px.height);
        Rect d = r;
        if (a > 1.0f) { d.height = r.width / a; d.y = r.y + (r.height - d.height) / 2; } else if (a < 1.0f) { d.width = r.height * a; d.x = r.x + (r.width - d.width) / 2; }
        FillRR(Rect{d.x + d.width * 0.1f, d.y + d.height * 0.55f, d.width * 0.8f, d.height * 0.42f}, d.height * 0.2f, Color{0.0f, 0.0f, 0.0f, 0.10f * alpha});     // ground shadow
        const D2D1_RECT_F dr = D(d);
        m_ctx->DrawBitmap(f->second.Get(), &dr, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }
    // nothing until the worker delivers (icons fade in); a plain page if the shell has no icon
    if (m_iconFailed.count(it.key)) rr(Rect{r.x + w * 0.16f, r.y + h * 0.04f, w * 0.68f, h * 0.92f}, w * 0.10f, Color{0.95f, 0.95f, 1.0f, 0.55f}, Color{0.62f, 0.62f, 0.80f, 0.55f});
}

std::wstring Desktop::FitLabel(const std::wstring& s, float width) {
    auto* f = Fmt(12.0f, 400, false, true);
    auto lines = [&](const std::wstring& t) {
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(m_dcomp->GetDWriteFactory()->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), f, width, 1000.0f, &lay)) || !lay) return 1u;
        DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm); return tm.lineCount;
    };
    if (!f || lines(s) <= 2) return s;
    size_t lo = 1, hi = s.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (lines(s.substr(0, mid) + L"…") <= 2) lo = mid; else hi = mid - 1;
    }
    return s.substr(0, lo) + L"…";
}

void Desktop::EnsureLabel(Item& it, float w) {
    if (it.lay && it.layW == w) return;
    it.lay.Reset();
    it.disp = FitLabel(it.label, w);
    auto* f = Fmt(12.0f, 400, false, true);
    if (!f) return;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(m_dcomp->GetDWriteFactory()->CreateTextLayout(it.disp.c_str(), static_cast<UINT32>(it.disp.size()), f, w, 40.0f, &lay)) || !lay) return;
    DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
    it.lay = lay; it.layW = w; it.lx = tm.left; it.lw = tm.width; it.lh = tm.height;
}

void Desktop::DrawLabel(Item& it, const Rect& cell, float top, float alpha, bool sel) {
    if (m_labelStyle == 2) return;
    const float w = cell.width - 8;
    EnsureLabel(it, w);
    if (!it.lay) return;
    const float ox = cell.x + 4;
    if (m_labelStyle == 1) {            // glass capsule behind the text
        const Rect pill{ox + it.lx - 7, top - 2, it.lw + 14, it.lh + 4};
        FillRR(pill, 9, sel ? WithA(Acc(), 0.62f * alpha) : Color{0.04f, 0.02f, 0.10f, 0.50f * alpha});
        StrokeRR(pill, 9, WithA(AccL(), (sel ? 0.55f : 0.16f) * alpha), 1.0f);
    } else {
        m_brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f * alpha));
        m_ctx->DrawTextLayout(D2D1::Point2F(ox, top + 1.2f), it.lay.Get(), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        m_brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f * alpha));
        m_ctx->DrawTextLayout(D2D1::Point2F(ox + 1.0f, top + 2.0f), it.lay.Get(), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
    m_ctx->DrawTextLayout(D2D1::Point2F(ox, top), it.lay.Get(), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Desktop::DrawEdit(const Item& it, const Rect& cell, float top) {
    (void)it;
    const Color vl = AccL();
    const float w = cell.width - 4;
    auto* f = Fmt(12.0f, 400, false, true);
    if (!f) return;
    const std::wstring shown = m_editText;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(m_dcomp->GetDWriteFactory()->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()), f, w - 8, 60.0f, &lay)) || !lay) return;
    DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
    const Rect box{cell.x - 1, top - 3, w + 2, std::max(22.0f, tm.height + 8)};
    FillRR(box, 6, Color{0.04f, 0.02f, 0.10f, 0.96f});
    StrokeRR(box, 6, WithA(vl, 0.8f), 1.2f);
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    m_ctx->DrawTextLayout(D2D1::Point2F(box.x + 4, top + 1), lay.Get(), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if ((GetTickCount64() / 500) % 2 == 0) {
        float cx = 0, cy = 0; DWRITE_HIT_TEST_METRICS hm{};
        lay->HitTestTextPosition(static_cast<UINT32>(shown.size()), FALSE, &cx, &cy, &hm);
        FillRR(Rect{box.x + 4 + cx, top + 1 + cy, 1.4f, 16}, 0.5f, kWhite);
    }
}

void Desktop::DrawItem(int i, const Rect& cell, float alpha, bool ghost) {
    Item& it = m_items[i];
    const Color v = Acc(), vl = AccL();
    const auto z = Z();
    const bool sel = i < static_cast<int>(m_sel.size()) && m_sel[i];
    float hv = 0.0f;
    auto h = m_hover.find(i); if (h != m_hover.end()) hv = h->second;
    const bool dropT = (i == m_dropHover);
    if (!ghost) {
        if (sel) {
            GlowRR(cell, 12, v, 10.0f, 0.9f * m_glow);
            FillRR(cell, 12, WithA(Mix(v, vl, 0.2f), 0.30f));
            StrokeRR(cell, 12, WithA(vl, 0.60f), 1.2f);
        } else if (hv > 0.01f) {
            FillRR(cell, 12, WithA(vl, 0.10f * hv));
            StrokeRR(cell, 12, WithA(vl, 0.26f * hv), 1.0f);
        }
        if (dropT) { GlowRR(cell, 12, vl, 12.0f, 1.4f * m_glow); FillRR(cell, 12, WithA(vl, 0.30f)); StrokeRR(cell, 12, WithA(kWhite, 0.8f), 1.4f); }
    } else FillRR(cell, 12, WithA(v, 0.20f * alpha));
    const float lift = ghost ? 0.0f : hv;
    const float isz = z.icon * (1.0f + 0.07f * lift);                    // icons swell slightly under the cursor
    const float ix = cell.x + (cell.width - isz) / 2, iy = cell.y + 10 - (isz - z.icon) / 2 - 2.0f * lift;
    const bool cutDim = !m_clip.empty() && m_clipCut && std::find(m_clip.begin(), m_clip.end(), it.path) != m_clip.end();
    DrawIcon(it, Rect{ix, iy, isz, isz}, alpha * (cutDim ? 0.45f : 1.0f));
    const float top = cell.y + 10 + z.icon + 7;
    if (m_edit && m_editIdx == i && !ghost) DrawEdit(it, cell, top);
    else DrawLabel(it, cell, top, alpha, sel);
    if (!ghost && it.kind == K_File && m_icons.find(it.key) == m_icons.end()) RequestIcon(it);
}

void Desktop::PText(const std::wstring& str, const Rect& r, float size, int weight, const Color& c, DWRITE_TEXT_ALIGNMENT al, bool shadow) {
    auto* f = Fmt(size, weight, false, false);
    if (!f || str.empty()) return;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(m_dcomp->GetDWriteFactory()->CreateTextLayout(str.c_str(), static_cast<UINT32>(str.size()), f, r.width, r.height, &lay)) || !lay) return;
    lay->SetTextAlignment(al);
    lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (shadow) { m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.45f * c.a)); m_ctx->DrawTextLayout(D2D1::Point2F(r.x + 1, r.y + 2), lay.Get(), m_brush.Get()); }
    m_brush->SetColor(DC(c));
    m_ctx->DrawTextLayout(D2D1::Point2F(r.x, r.y), lay.Get(), m_brush.Get());
}

void Desktop::DrawClock() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t t[16]; swprintf(t, 16, L"%02d:%02d", st.wHour, st.wMinute);
    wchar_t d[96] = {}; GetDateFormatEx(L"tr-TR", 0, &st, L"d MMMM dddd", d, 96, nullptr);
    const float rx = m_work.Right() - 26.0f;
    PText(t, Rect{rx - 320, m_work.y + 14, 320, 64}, 56.0f, 300, kWhite, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    PText(d, Rect{rx - 320, m_work.y + 76, 320, 24}, 15.0f, 400, WithA(kWhite, 0.88f), DWRITE_TEXT_ALIGNMENT_TRAILING, true);
}

// ---- customization panel ---------------------------------------------------------------------------------------------------
int Desktop::CtlAt(float x, float y) const {
    if (!m_panelView.Contains(x, y)) { for (const auto& c : m_ctrls) if ((c.id == 1 || c.id == 7 || (c.id >= 70 && c.id <= 72)) && c.r.Contains(x, y)) return c.id; return 0; }
    for (int i = static_cast<int>(m_ctrls.size()) - 1; i >= 0; --i) if (m_ctrls[i].r.Contains(x, y)) return m_ctrls[i].id;
    return 0;
}

void Desktop::PanelSlider(int id, float mx) {
    auto t = m_tracks.find(id);
    if (t == m_tracks.end()) return;
    const float v = std::clamp((mx - t->second.x) / t->second.width, 0.0f, 1.0f);
    if (id == 60) m_dim = v * 0.7f;
    else if (id == 61) { m_iconSize = std::round(32.0f + v * 78.0f); Place(); }
    else if (id == 62) m_glow = v * 2.0f;
}

void Desktop::ToggleShowDesktop() {
    bool anyIconic = false;
    for (HWND h : m_hidden) if (IsWindow(h) && IsIconic(h)) { anyIconic = true; break; }
    if (m_showDesk && anyIconic) {                                      // second press: bring everything back (topmost one last)
        for (auto it = m_hidden.rbegin(); it != m_hidden.rend(); ++it) if (IsWindow(*it) && IsIconic(*it)) ShowWindow(*it, SW_RESTORE);
        m_hidden.clear(); m_showDesk = false;
        return;
    }
    m_hidden.clear();
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) || (ex & WS_EX_NOACTIVATE)) return TRUE;
        if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return TRUE;
        if (!(GetWindowLongPtrW(h, GWL_STYLE) & WS_MINIMIZEBOX)) return TRUE;
        if (GetWindowTextLengthW(h) == 0) return TRUE;
        BOOL cloaked = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
        reinterpret_cast<std::vector<HWND>*>(l)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&m_hidden));
    for (HWND h : m_hidden) ShowWindow(h, SW_SHOWMINNOACTIVE);
    m_showDesk = !m_hidden.empty();
    m_sel.assign(m_items.size(), 0);
    Kick();
}

void Desktop::ResetLook() {
    m_dim = 0; m_blur = 0; m_fit = -1; m_labelStyle = 0; m_accent = 0; m_glow = 1.0f; m_iconSize = 48.0f; m_clock = false; m_auto = false; m_slide = false;
    ApplySlideTimer(); Place(); LoadWallpaper(true);
}

void Desktop::PanelAction(int id) {
    switch (id) {
        case 1: m_panel = false; m_hotCtl = 0; break;
        case 70: case 71: case 72: m_ptab = id - 70; m_panelScroll = 0.0f; m_panelContentH = 0.0f; break;
        case 2: m_slide = false; ApplySlideTimer(); PickWallpaper(); break;
        case 3:
            m_slide = !m_slide;
            if (m_slide && m_slides.empty()) { PickSlideFolder(); if (m_slides.empty()) m_slide = false; }
            ApplySlideTimer(); LoadWallpaper(true);
            break;
        case 4: PickSlideFolder(); ApplySlideTimer(); if (m_slide) LoadWallpaper(true); break;
        case 5: m_clock = !m_clock; m_lastMin = -1; break;
        case 6: m_auto = !m_auto; Place(); break;
        case 7: ResetLook(); break;
        default:
            if (id >= 10 && id <= 15) { m_fit = kFitCodes[id - 10]; LoadWallpaper(true); }
            else if (id >= 20 && id <= 23) { m_blur = id - 20; LoadWallpaper(true); }
            else if (id >= 30 && id <= 32) m_labelStyle = id - 30;
            else if (id >= 40 && id <= 45) m_accent = id - 40;
            else if (id >= 50 && id <= 53) { static const int mins[4] = {1, 5, 15, 60}; m_slideMin = mins[id - 50]; ApplySlideTimer(); }
            break;
    }
    SaveCfg(); Kick();
}

void Desktop::DrawPanel() {
    const Color v = Acc(), vl = AccL(), txt = render::glass::kText, mut = render::glass::kMuted;
    const float SB = 250.0f;
    const float e = 1.0f - (1.0f - m_panelA) * (1.0f - m_panelA);
    m_panelR = PanelGeom();
    const Rect R = m_panelR;
    m_ctrls.clear(); m_tracks.clear();
    D2D1_MATRIX_3X2_F old; m_ctx->GetTransform(&old);
    m_ctx->SetTransform(D2D1::Matrix3x2F::Translation(0, (1.0f - e) * 18.0f) * old);

    GlowRR(R, 20, v, 18.0f, 0.8f * m_glow);
    FillRR(R, 20, Color{0.045f, 0.028f, 0.105f, 0.95f});
    if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(v, 0.22f), WithA(v, 0.0f), D2D1::Point2F(0, R.y), D2D1::Point2F(0, R.y + 200)))
        m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(R), 20, 20), g.Get());
    StrokeRR(R, 20, WithA(vl, 0.40f), 1.2f);
    FillRR(Rect{R.x + SB, R.y + 18, 1, R.height - 36}, 0.5f, WithA(kWhite, 0.08f));

    auto reg = [&](const Rect& r, int id) { m_ctrls.push_back({r, id}); };

    // ---- sidebar: title, live preview, tabs, reset ----
    const float px = R.x + 22, pw = SB - 44;
    PText(L"Özelleştir", Rect{px, R.y + 20, pw, 30}, 21.0f, 600, txt, DWRITE_TEXT_ALIGNMENT_LEADING);
    PText(L"Masaüstünü kendine göre ayarla", Rect{px, R.y + 50, pw, 18}, 11.5f, 400, mut, DWRITE_TEXT_ALIGNMENT_LEADING);
    {   // live preview of the desktop with the current settings
        const Rect pr{px, R.y + 84, pw, pw * 0.60f};
        if (auto g = m_dcomp->CreateLinearGradientBrush(Color{v.r * 0.55f, v.g * 0.55f, v.b * 0.55f, 1.0f}, Color{0.03f, 0.015f, 0.09f, 1.0f}, D2D1::Point2F(pr.x, pr.y), D2D1::Point2F(pr.Right(), pr.Bottom())))
            m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(pr), 10, 10), g.Get());
        if (m_dim > 0.004f) FillRR(pr, 10, Color{0, 0, 0, m_dim});
        const float ic = std::clamp(m_iconSize / 110.0f * 24.0f + 8.0f, 14.0f, 32.0f);
        for (int k = 0; k < 6; ++k) {
            const float cx0 = pr.x + 12 + (k % 3) * (ic + 14), cy0 = pr.y + 12 + (k / 3) * (ic + 16);
            const Rect ir{cx0, cy0, ic, ic};
            if (m_glow > 0.05f && k == 0) GlowRR(ir, ic * 0.3f, vl, 5.0f, m_glow * 0.6f);
            FillRR(ir, ic * 0.28f, WithA(k == 0 ? vl : kWhite, k == 0 ? 0.85f : 0.55f));
            if (m_labelStyle == 1) FillRR(Rect{cx0 - 2, cy0 + ic + 3, ic + 4, 6}, 3, Color{0, 0, 0, 0.55f});
            else if (m_labelStyle == 0) FillRR(Rect{cx0 + 1, cy0 + ic + 4, ic - 2, 3}, 1.5f, WithA(kWhite, 0.75f));
        }
        if (m_clock) {
            PText(L"14:32", Rect{pr.Right() - 74, pr.y + 8, 66, 22}, 17.0f, 300, kWhite, DWRITE_TEXT_ALIGNMENT_TRAILING);
            FillRR(Rect{pr.Right() - 48, pr.y + 30, 40, 3}, 1.5f, WithA(kWhite, 0.5f));
        }
        FillRR(Rect{pr.x + 10, pr.Bottom() - 11, pr.width - 20, 5}, 2.5f, WithA(kWhite, 0.22f));
        StrokeRR(pr, 10, WithA(kWhite, 0.14f), 1.0f);
        struct Tab { int id; wchar_t g; const wchar_t* t; };
        const Tab tabs[3] = {{70, 0xE91B, L"Duvar kağıdı"}, {71, 0xE8A9, L"Simgeler ve renk"}, {72, 0xE7F4, L"Masaüstü"}};
        float ty = pr.Bottom() + 20;
        for (const Tab& t : tabs) {
            const Rect r{px - 6, ty, pw + 12, 40};
            const bool on = m_ptab == t.id - 70, hot = m_hotCtl == t.id;
            if (on) { FillRR(r, 11, WithA(v, 0.34f)); StrokeRR(r, 11, WithA(vl, 0.55f), 1.0f); }
            else if (hot) FillRR(r, 11, WithA(kWhite, 0.08f));
            Glyph(t.g, Rect{r.x + 8, r.y, 28, r.height}, 15.0f, on ? kWhite : txt);
            PText(t.t, Rect{r.x + 42, r.y, r.width - 48, r.height}, 13.5f, on ? 600 : 400, on ? kWhite : txt, DWRITE_TEXT_ALIGNMENT_LEADING);
            reg(r, t.id);
            ty += 46;
        }
        const Rect rb{px, R.Bottom() - 56, pw, 36};
        const bool hot = m_hotCtl == 7;
        FillRR(rb, 11, WithA(kWhite, hot ? 0.15f : 0.07f));
        StrokeRR(rb, 11, WithA(kWhite, 0.14f), 1.0f);
        Glyph(0xE777, Rect{rb.x + 8, rb.y, 28, rb.height}, 14.0f, txt);
        PText(L"Varsayılana döndür", Rect{rb.x + 40, rb.y, rb.width - 46, rb.height}, 12.5f, 500, txt, DWRITE_TEXT_ALIGNMENT_LEADING);
        reg(rb, 7);
    }

    // ---- header of the content area ----
    static const wchar_t* kTitles[3] = {L"Duvar kağıdı", L"Simgeler ve renk", L"Masaüstü"};
    static const wchar_t* kSubs[3] = {L"Resim, karartma, bulanıklık ve slayt gösterisi", L"Boyut, etiketler, parlama ve vurgu rengi", L"Düzen ve saat"};
    const float cx = R.x + SB + 30, cwFull = R.width - SB - 30 - 36, cw = std::min(cwFull, 500.0f);
    PText(kTitles[m_ptab], Rect{cx, R.y + 20, cwFull, 28}, 19.0f, 600, txt, DWRITE_TEXT_ALIGNMENT_LEADING);
    PText(kSubs[m_ptab], Rect{cx, R.y + 50, cwFull - 50, 18}, 12.0f, 400, mut, DWRITE_TEXT_ALIGNMENT_LEADING);
    const Rect cb{R.Right() - 52, R.y + 16, 32, 32};
    FillRR(cb, 16, WithA(kWhite, m_hotCtl == 1 ? 0.20f : 0.08f));
    Glyph(0xE711, cb, 13.0f, txt);
    reg(cb, 1);

    m_panelView = Rect{R.x + SB, R.y + 82, R.width - SB, R.height - 82 - 12};
    m_ctx->PushAxisAlignedClip(D(m_panelView), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = m_panelView.y - m_panelScroll + 4;
    const float x0 = cx;

    auto section = [&](const wchar_t* t) { PText(t, Rect{x0, y, cw, 18}, 11.0f, 600, WithA(vl, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING); y += 26; };
    auto caption = [&](const wchar_t* t) { PText(t, Rect{x0, y, cw, 18}, 12.0f, 400, mut, DWRITE_TEXT_ALIGNMENT_LEADING); y += 24; };
    auto chip = [&](const Rect& r, const std::wstring& label, bool on, int id) {
        const bool hot = m_hotCtl == id;
        FillRR(r, 10, on ? WithA(v, 0.58f) : WithA(kWhite, hot ? 0.15f : 0.07f));
        StrokeRR(r, 10, on ? WithA(vl, 0.9f) : WithA(kWhite, 0.12f), 1.0f);
        PText(label, r, 12.5f, on ? 600 : 400, on ? kWhite : txt, DWRITE_TEXT_ALIGNMENT_CENTER);
        reg(r, id);
    };
    auto chipRow = [&](const std::vector<std::pair<int, std::wstring>>& items, int perRow, const std::function<bool(int)>& isOn) {
        const float gap = 8, cwid = (cw - gap * (perRow - 1)) / perRow;
        for (size_t k = 0; k < items.size(); ++k) {
            const int col = static_cast<int>(k) % perRow, row = static_cast<int>(k) / perRow;
            chip(Rect{x0 + col * (cwid + gap), y + row * 38, cwid, 31}, items[k].second, isOn(items[k].first), items[k].first);
        }
        y += static_cast<float>((items.size() + perRow - 1) / perRow) * 38 + 8;
    };
    auto slider = [&](const wchar_t* label, float v01, const std::wstring& val, int id) {
        PText(label, Rect{x0, y, cw - 70, 20}, 13.0f, 400, txt, DWRITE_TEXT_ALIGNMENT_LEADING);
        PText(val, Rect{x0 + cw - 70, y, 70, 20}, 12.0f, 400, mut, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 28;
        const Rect tr{x0, y + 5, cw, 6};
        FillRR(tr, 3, WithA(kWhite, 0.13f));
        FillRR(Rect{tr.x, tr.y, std::max(6.0f, tr.width * v01), 6}, 3, WithA(v, 0.95f));
        const float kx = tr.x + tr.width * v01;
        const bool act = m_dragCtl == id || m_hotCtl == id;
        if (act) GlowRR(Rect{kx - 9, y - 2, 18, 18}, 9, vl, 8.0f, 1.0f);
        FillRR(Rect{kx - 9, y - 2, 18, 18}, 9, kWhite);
        m_tracks[id] = tr;
        reg(Rect{x0 - 8, y - 6, cw + 16, 28}, id);
        y += 36;
    };
    auto toggle = [&](const wchar_t* label, bool on, int id) {
        const bool hot = m_hotCtl == id;
        FillRR(Rect{x0 - 10, y - 4, cw + 20, 38}, 10, WithA(kWhite, hot ? 0.06f : 0.0f));
        PText(label, Rect{x0, y + 3, cw - 60, 28}, 13.5f, 400, txt, DWRITE_TEXT_ALIGNMENT_LEADING);
        const Rect tr{x0 + cw - 46, y + 3, 44, 24};
        FillRR(tr, 12, on ? WithA(v, 0.90f) : WithA(kWhite, 0.15f));
        FillRR(Rect{tr.x + (on ? 22.0f : 2.0f), tr.y + 2, 20, 20}, 10, kWhite);
        reg(Rect{x0 - 10, y - 4, cw + 20, 38}, id);
        y += 42;
    };
    auto button = [&](const std::wstring& label, wchar_t glyph, int id) {
        const Rect r{x0, y, cw, 36};
        const bool hot = m_hotCtl == id;
        FillRR(r, 10, WithA(v, hot ? 0.46f : 0.28f));
        StrokeRR(r, 10, WithA(vl, 0.5f), 1.0f);
        Glyph(glyph, Rect{r.x + 8, r.y, 28, r.height}, 14.0f, kWhite);
        PText(label, Rect{r.x + 40, r.y, r.width - 48, r.height}, 13.0f, 500, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);
        reg(r, id);
        y += 46;
    };

    if (m_ptab == 0) {
        section(L"DUVAR KAĞIDI");
        button(L"Resim seç...", 0xE91B, 2);
        chipRow({{10, L"Sistem"}, {11, L"Doldur"}, {12, L"Sığdır"}, {13, L"Uzat"}, {14, L"Ortala"}, {15, L"Döşe"}}, 3, [&](int id) { return m_fit == kFitCodes[id - 10]; });
        slider(L"Karartma", m_dim / 0.7f, std::to_wstring(static_cast<int>(std::lround(m_dim * 100))) + L"%", 60);
        caption(L"Bulanıklık");
        chipRow({{20, L"Yok"}, {21, L"Az"}, {22, L"Orta"}, {23, L"Çok"}}, 4, [&](int id) { return m_blur == id - 20; });
        y += 6;
        section(L"SLAYT GÖSTERİSİ");
        toggle(L"Duvar kağıdı slaytı", m_slide, 3);
        if (m_slide || !m_slideDir.empty()) {
            chipRow({{50, L"1 dk"}, {51, L"5 dk"}, {52, L"15 dk"}, {53, L"1 sa"}}, 4, [&](int id) { static const int mins[4] = {1, 5, 15, 60}; return m_slideMin == mins[id - 50]; });
            const size_t sl = m_slideDir.find_last_of(L'\\');
            const std::wstring leaf = m_slideDir.empty() ? L"Klasör seç..." : L"Klasör: " + (sl == std::wstring::npos ? m_slideDir : m_slideDir.substr(sl + 1)) + L"  (" + std::to_wstring(m_slides.size()) + L")";
            button(leaf, 0xE838, 4);
        }
    } else if (m_ptab == 1) {
        section(L"SİMGELER");
        slider(L"Simge boyutu", (m_iconSize - 32.0f) / 78.0f, std::to_wstring(static_cast<int>(std::lround(m_iconSize))), 61);
        slider(L"Parlama", m_glow / 2.0f, std::to_wstring(static_cast<int>(std::lround(m_glow * 100))) + L"%", 62);
        caption(L"Etiketler");
        chipRow({{30, L"Gölge"}, {31, L"Kapsül"}, {32, L"Gizli"}}, 3, [&](int id) { return m_labelStyle == id - 30; });
        y += 4;
        section(L"VURGU RENGİ");
        for (int k = 0; k < 6; ++k) {
            const Rect r{x0 + k * 52.0f, y, 36, 36};
            const Color c = k == 0 ? render::glass::kViolet : kAccents[k];
            if (m_accent == k) GlowRR(r, 18, c, 8.0f, 1.0f);
            FillRR(r, 18, c);
            if (m_accent == k) StrokeRR(Rect{r.x - 3, r.y - 3, r.width + 6, r.height + 6}, 21, WithA(kWhite, 0.9f), 1.6f);
            else if (m_hotCtl == 40 + k) StrokeRR(Rect{r.x - 2, r.y - 2, r.width + 4, r.height + 4}, 20, WithA(kWhite, 0.4f), 1.2f);
            reg(r, 40 + k);
        }
        y += 50;
        caption(L"İpucu: Ctrl + fare tekerleği ile simgeleri doğrudan yeniden boyutlandırabilirsin.");
    } else {
        section(L"DÜZEN");
        toggle(L"Otomatik düzenle", m_auto, 6);
        toggle(L"Saat ve tarih göster", m_clock, 5);
        y += 6;
        caption(L"Win+D masaüstünü gösterir, sağ tık menüsünden yeni öğe oluşturabilirsin.");
    }

    const float content = (y + m_panelScroll) - m_panelView.y + 4;
    if (std::fabs(content - m_panelContentH) > 1.0f) { m_panelContentH = content; Kick(); }
    m_ctx->PopAxisAlignedClip();
    const float maxS = std::max(0.0f, m_panelContentH - m_panelView.height);
    if (m_panelScroll > maxS) { m_panelScroll = maxS; Kick(); }
    if (maxS > 0.0f) {                                           // slim scrollbar
        const float th = m_panelView.height * (m_panelView.height / m_panelContentH);
        const float ty = m_panelView.y + (m_panelView.height - th) * (m_panelScroll / maxS);
        FillRR(Rect{R.Right() - 12, ty, 3, th}, 1.5f, WithA(kWhite, 0.30f));
    }
    m_ctx->SetTransform(old);
}

Rect Desktop::PanelGeom() const {
    const float W = std::min(860.0f, std::max(520.0f, m_work.width - 60.0f)), H = std::min(590.0f, std::max(420.0f, m_work.height - 60.0f));
    float x = m_work.x + (m_work.width - W) * 0.5f, y = m_work.y + (m_work.height - H) * 0.5f;
    if (m_panelMoved) { x = m_panelX; y = m_panelY; }   // user dragged it: keep their spot, clamped to the work area
    x = std::clamp(x, m_work.x + 6.0f, std::max(m_work.x + 6.0f, m_work.Right() - W - 6.0f));
    y = std::clamp(y, m_work.y + 6.0f, std::max(m_work.y + 6.0f, m_work.Bottom() - H - 6.0f));
    return Rect{x, y, W, H};
}

bool Desktop::EnsurePanelWnd() {
    if (m_pwnd) return true;
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = PanelProc; wc.hInstance = inst; wc.lpszClassName = L"AetherDesktopPanel";
        RegisterClassExW(&wc); reg = true;
    }
    m_pwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"AetherDesktopPanel", L"Aether Desktop Panel",
                             WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst, this);
    if (!m_pwnd) return false;
    m_pdcomp = std::make_unique<render::DCompLayer>();
    if (!m_pdcomp->Initialize(m_pwnd, 64, 64)) { DestroyWindow(m_pwnd); m_pwnd = nullptr; m_pdcomp.reset(); return false; }
    m_pbrush = m_pdcomp->CreateSolidBrush(kWhite);
    m_pSzW = m_pSzH = 64;
    return true;
}

LRESULT CALLBACK Desktop::PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Desktop* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Desktop*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else self = reinterpret_cast<Desktop*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->HandlePanel(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// Mouse input on the panel window is re-expressed in desktop-window client coordinates and fed to the normal handler.
LRESULT Desktop::HandlePanel(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto conv = [&](LPARAM p) { return MAKELPARAM(static_cast<WORD>(static_cast<short>(GET_X_LPARAM(p) + m_pPx)), static_cast<WORD>(static_cast<short>(GET_Y_LPARAM(p) + m_pPy))); };
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(m_hotCtl ? bui::Link() : m_hotHeader ? LoadCursorW(nullptr, IDC_SIZEALL) : bui::Arrow()); return TRUE; }
        break;
    case WM_MOUSEMOVE: {
        if (!m_pTracking) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); m_pTracking = true; }
        const bool tr = m_tracking; m_tracking = true;            // the desktop window must not start its own leave tracking
        Handle(WM_MOUSEMOVE, w, conv(l));
        m_tracking = tr;
        KickP();
        return 0;
    }
    case WM_MOUSELEAVE:
        m_pTracking = false;
        if (!m_dragCtl) { m_hotCtl = 0; KickP(); }
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        return Handle(WM_LBUTTONDOWN, w, conv(l));
    case WM_LBUTTONUP:
        return Handle(WM_LBUTTONUP, w, conv(l));
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return 0;
    case WM_MOUSEWHEEL: return Handle(m, w, l);       // screen coordinates, same as for the desktop window
    }
    return DefWindowProcW(h, m, w, l);
}

void Desktop::RenderPanel() {
    if (!(m_panel || m_panelA > 0.01f)) {
        if (m_pwnd && IsWindowVisible(m_pwnd)) { ShowWindow(m_pwnd, SW_HIDE); m_pLastW = 0; }
        return;
    }
    if (!EnsurePanelWnd() || IsIconic(m_hwnd)) return;
    const float s = Scale(), M = 24.0f;
    const Rect g = PanelGeom();
    RECT dr; GetWindowRect(m_hwnd, &dr);
    m_pPx = static_cast<int>(std::lround((g.x - M) * s)); m_pPy = static_cast<int>(std::lround((g.y - M) * s));
    m_pOx = static_cast<float>(m_pPx) / s; m_pOy = static_cast<float>(m_pPy) / s;
    const int px = dr.left + m_pPx, py = dr.top + m_pPy;
    const int pw = static_cast<int>(std::lround((g.width + 2 * M) * s)), ph = static_cast<int>(std::lround((g.height + 2 * M) * s));
    const bool vis = IsWindowVisible(m_pwnd) != FALSE;
    if (!vis || px != m_pLastX || py != m_pLastY || pw != m_pLastW || ph != m_pLastH) {
        SetWindowPos(m_pwnd, HWND_TOPMOST, px, py, pw, ph, SWP_NOACTIVATE | (vis ? 0 : SWP_SHOWWINDOW));
        m_pLastX = px; m_pLastY = py; m_pLastW = pw; m_pLastH = ph;
        if (pw != m_pSzW || ph != m_pSzH) { m_pdcomp->Resize(pw, ph); m_pSzW = pw; m_pSzH = ph; }
    }
    struct Swap {
        Desktop& d;
        explicit Swap(Desktop& x) : d(x) { std::swap(d.m_dcomp, d.m_pdcomp); std::swap(d.m_brush, d.m_pbrush); std::swap(d.m_fmts, d.m_pfmts); }
        ~Swap() { std::swap(d.m_dcomp, d.m_pdcomp); std::swap(d.m_brush, d.m_pbrush); std::swap(d.m_fmts, d.m_pfmts); }
    } sw(*this);
    m_ctx = m_dcomp->BeginDraw();
    if (!m_ctx) return;
    m_ctx->SetTransform(D2D1::Matrix3x2F::Translation(-m_pOx, -m_pOy) * D2D1::Matrix3x2F::Scale(s, s));
    DrawPanel();
    m_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
    m_ctx = nullptr;
}

void Desktop::Render() {
    if (!m_dcomp || IsIconic(m_hwnd)) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    if (rc.right < 8 || rc.bottom < 8) return;
    m_ctx = m_dcomp->BeginDraw();
    if (!m_ctx) return;
    const float s = Scale();
    m_W = rc.right / s; m_H = rc.bottom / s;
    m_ctx->SetTransform(D2D1::Matrix3x2F::Scale(s, s));
    m_ctx->Clear(D2D1::ColorF(0, 0, 0, 1));
    DrawWallpaper();
    if (m_clock) DrawClock();

    if (m_showIcons) {
        for (int i = 0; i < Count(); ++i) {
            if (m_dragging && i < static_cast<int>(m_sel.size()) && m_sel[i]) continue;        // drawn as ghosts below
            DrawItem(i, ItemRect(i), 1.0f, false);
        }
        if (m_dragging && m_pressItem >= 0 && !m_dragTo) {
            const auto sel = SelectedIdx();
            const Rect pr = ItemRect(m_pressItem);
            if (m_dropHover < 0 && m_tvalid) {                                                  // snapping preview
                const int dc = m_tcol - m_items[m_pressItem].col, dr = m_trow - m_items[m_pressItem].row;
                for (int i : sel) {
                    const Rect t = CellRect(m_items[i].col + dc, m_items[i].row + dr);
                    StrokeRR(t, 12, WithA(AccL(), 0.55f), 1.4f);
                    FillRR(t, 12, WithA(Acc(), 0.12f));
                }
            }
            const float gx = m_mx - m_grabDX, gy = m_my - m_grabDY;
            for (int i : sel) {
                const Rect c = ItemRect(i);
                DrawItem(i, Rect{gx + (c.x - pr.x), gy + (c.y - pr.y), c.width, c.height}, 0.72f, true);
            }
        }
        if (!m_hoIdx.empty() && m_hoAnchor >= 0) {              // a selection carried in from the sibling desktop
            const auto z = Z();
            if (m_hoValid)
                for (size_t k = 0; k < m_hoIdx.size(); ++k) {
                    const Rect t = CellRect(m_hoTcol + m_hoDelta[k].first, m_hoTrow + m_hoDelta[k].second);
                    StrokeRR(t, 12, WithA(AccL(), 0.55f), 1.4f);
                    FillRR(t, 12, WithA(Acc(), 0.12f));
                }
            const float gx = m_hoCx - m_hoGdx, gy = m_hoCy - m_hoGdy;
            for (size_t k = 0; k < m_hoIdx.size(); ++k) {
                const int i = m_hoIdx[k];
                if (i < 0) continue;
                DrawItem(i, Rect{gx + m_hoDelta[k].first * z.cw, gy + m_hoDelta[k].second * z.ch, z.cw - 4, z.ch - 4}, 0.72f, true);
            }
        }
        if (m_marquee) {
            const Rect q{std::min(m_mqX, m_mx), std::min(m_mqY, m_my), std::abs(m_mx - m_mqX), std::abs(m_my - m_mqY)};
            FillRR(q, 3, WithA(Acc(), 0.20f));
            StrokeRR(q, 3, WithA(AccL(), 0.75f), 1.2f);
        }
    }
    m_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
    m_ctx = nullptr;
}

// ---- commands -------------------------------------------------------------------------------------------------------------
void Desktop::OpenFolder(const std::wstring& path) {
    const std::wstring ex = ExeDir() + L"\\AetherExplorer.exe";
    if (GetFileAttributesW(ex.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring arg = path.empty() ? L"" : L"\"" + path + L"\"";
        ShellExecuteW(m_hwnd, L"open", ex.c_str(), arg.empty() ? nullptr : arg.c_str(), nullptr, SW_SHOWNORMAL);
    } else ShellExecuteW(m_hwnd, L"open", path.empty() ? L"shell:MyComputerFolder" : path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Desktop::OpenItem(int i) {
    if (i < 0 || i >= Count()) return;
    const Item& it = m_items[i];
    switch (it.kind) {
        case K_PC: OpenFolder(L""); return;
        case K_Bin: {
            const std::wstring ab = ExeDir() + L"\\AetherBin.exe";
            if (GetFileAttributesW(ab.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(m_hwnd, L"open", ab.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else ShellExecuteW(m_hwnd, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        case K_Net: OpenFolder(L"net:"); return;
        case K_Folder: case K_User: OpenFolder(it.path); return;
        default: break;
    }
    if (it.ext == L".lnk") { const std::wstring t = LinkFolderTarget(it.path); if (!t.empty()) { OpenFolder(t); return; } }
    const size_t sl = it.path.find_last_of(L'\\');
    const std::wstring dir = sl == std::wstring::npos ? L"" : it.path.substr(0, sl);
    ShellExecuteW(m_hwnd, L"open", it.path.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
}

void Desktop::DeleteSel(bool perm) {
    const auto p = SelectedPaths();
    if (p.empty()) return;
    su::RunFileOp(m_hwnd, kMsgOpDone, perm ? su::Op::DeletePerm : su::Op::Delete, p, L"", L"", L"");
}
void Desktop::CopySel(bool cut) {
    auto p = SelectedPaths();
    if (p.empty()) return;
    if (su::SetClipFiles(m_hwnd, p, cut)) { m_clip = std::move(p); m_clipCut = cut; }
    Kick();
}
void Desktop::Paste() {
    std::vector<std::wstring> files; bool cut = false;
    if (m_desktopDir.empty() || !su::GetClipFiles(m_hwnd, files, cut)) return;
    su::RunFileOp(m_hwnd, kMsgOpDone, cut ? su::Op::Move : su::Op::Copy, files, m_desktopDir, L"", L"");
    if (cut) { if (OpenClipboard(m_hwnd)) { EmptyClipboard(); CloseClipboard(); } m_clip.clear(); }
}
void Desktop::NewFolder() {
    if (m_desktopDir.empty()) return;
    const std::wstring name = su::UniqueName(m_desktopDir, L"Yeni klasör", L"");
    if (CreateDirectoryW(JoinPath(m_desktopDir, name).c_str(), nullptr)) { m_pendingSelect = name; m_pendingRename = true; LoadItems(); }
}
void Desktop::NewTextFile() {
    if (m_desktopDir.empty()) return;
    const std::wstring name = su::UniqueName(m_desktopDir, L"Yeni metin belgesi", L".txt");
    HANDLE h = CreateFileW(JoinPath(m_desktopDir, name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); m_pendingSelect = name; m_pendingRename = true; LoadItems(); }
}

void Desktop::BeginRename(int i) {
    if (i < 0 || i >= Count()) return;
    const Item& it = m_items[i];
    if (it.Special() || it.kind == K_User) return;
    m_edit = true; m_editIdx = i;
    if (it.dir) { m_editText = it.label; m_editExt.clear(); }
    else if (it.lnk) { m_editText = it.label; m_editExt = it.ext; }
    else {
        const std::wstring name = it.path.substr(it.path.find_last_of(L'\\') + 1);
        const size_t d = name.find_last_of(L'.');
        if (d != std::wstring::npos && d > 0) { m_editText = name.substr(0, d); m_editExt = name.substr(d); } else { m_editText = name; m_editExt.clear(); }
    }
    SetTimer(m_hwnd, kTimerCaret, 500, nullptr);
    Kick();
}
void Desktop::CommitEdit() {
    if (!m_edit) return;
    const int i = m_editIdx;
    m_edit = false; m_editIdx = -1; KillTimer(m_hwnd, kTimerCaret);
    if (i >= 0 && i < Count()) {
        const Item& it = m_items[i];
        const std::wstring cur = it.path.substr(it.path.find_last_of(L'\\') + 1);
        const std::wstring nn = m_editText + m_editExt;
        if (!m_editText.empty() && nn != cur && nn.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos) {
            m_pos[Lower(JoinPath(it.path.substr(0, it.path.find_last_of(L'\\')), nn))] = Pos3{m_mon, it.col, it.row};     // keep the icon where it is
            su::RunFileOp(m_hwnd, kMsgOpDone, su::Op::Rename, {it.path}, L"", nn, nn);
        }
    }
    Kick();
}

void Desktop::Properties(int i) {
    if (i < 0 || i >= Count()) return;
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_INVOKEIDLIST; sei.hwnd = m_hwnd; sei.lpVerb = L"properties"; sei.lpFile = m_items[i].path.c_str(); sei.nShow = SW_SHOW;
    ShellExecuteExW(&sei);
}

void Desktop::ShellMenu(const std::vector<std::wstring>& paths, POINT pt) {
    if (paths.empty()) return;
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& p : paths) { PIDLIST_ABSOLUTE pidl = nullptr; if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) ids.push_back(pidl); }
    ComPtr<IContextMenu> cm;
    if (!ids.empty()) {
        ComPtr<IShellItemArray> arr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(ids.data()), &arr)) && arr)
            arr->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
    }
    for (auto p : ids) CoTaskMemFree(p);
    if (!cm) return;
    HMENU menu = CreatePopupMenu();
    UINT flags = CMF_NORMAL | CMF_CANRENAME | CMF_EXTENDEDVERBS;
    if (SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, flags))) {
        cm.As(&m_cm2); cm.As(&m_cm3);
        const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, m_hwnd, nullptr));
        m_cm2.Reset(); m_cm3.Reset();
        if (cmd) {
            CMINVOKECOMMANDINFOEX ici = {};
            ici.cbSize = sizeof(ici);
            ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            ici.hwnd = m_hwnd;
            ici.lpVerb = MAKEINTRESOURCEA(cmd - 1);
            ici.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
            ici.nShow = SW_SHOWNORMAL;
            ici.ptInvoke = pt;
            cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
            SetTimer(m_hwnd, kTimerReload, 700, nullptr);
        }
    }
    DestroyMenu(menu);
}

void Desktop::ItemMenu(int i, POINT pt) {
    using aether::ui::MenuItem; using aether::ui::MenuSeparator;
    const Item& it = m_items[i];
    const bool multi = SelectedIdx().size() > 1;
    std::vector<aether::ui::MenuEntry> items;
    items.push_back(MenuItem(1, L"Aç", bui::kGlyphEnter, false, L"Enter"));
    if (!multi && (it.kind == K_Folder || it.kind == K_User)) items.push_back(MenuItem(2, L"Aether Explorer'da aç", 0xE838));
    if (it.kind == K_Bin) items.push_back(MenuItem(10, L"Geri dönüşüm kutusunu boşalt", bui::kGlyphDelete, true));
    if (!it.Special()) {
        items.push_back(MenuSeparator());
        items.push_back(MenuItem(3, L"Kes", bui::kGlyphCut, false, L"Ctrl+X"));
        items.push_back(MenuItem(4, L"Kopyala", bui::kGlyphCopy, false, L"Ctrl+C"));
        if (!multi && it.kind != K_User) items.push_back(MenuItem(5, L"Yeniden adlandır", bui::kGlyphRename, false, L"F2"));
        items.push_back(MenuItem(7, L"Yolu kopyala", bui::kGlyphCopy));
        if (it.kind != K_User) { items.push_back(MenuSeparator()); items.push_back(MenuItem(6, L"Sil", bui::kGlyphDelete, true, L"Del")); }
    }
    items.push_back(MenuSeparator());
    items.push_back(MenuItem(8, L"Özellikler", bui::kGlyphInfo, false, L"Alt+Enter"));
    items.push_back(MenuItem(9, L"Diğer seçenekler (Windows menüsü)", 0xE712, false, L"Shift+F10"));
    const int c = aether::ui::ShowContextMenu(items, pt);
    switch (c) {
        case 1: for (int k : SelectedIdx()) OpenItem(k); break;
        case 2: OpenFolder(it.path); break;
        case 3: CopySel(true); break;
        case 4: CopySel(false); break;
        case 5: BeginRename(i); break;
        case 6: DeleteSel(false); break;
        case 7: { auto p = SelectedPaths(); std::wstring t; for (size_t k = 0; k < p.size(); ++k) t += (k ? L"\r\n" : L"") + p[k]; su::SetClipText(m_hwnd, t); break; }
        case 8: Properties(i); break;
        case 9: { auto p = SelectedPaths(false); ShellMenu(p, pt); break; }
        case 10: SHEmptyRecycleBinW(m_hwnd, nullptr, 0); SetTimer(m_hwnd, kTimerReload, 500, nullptr); break;
    }
}

void Desktop::ViewMenu(POINT pt) {
    using aether::ui::MenuItem; using aether::ui::MenuSeparator;
    const int sz = m_iconSize < 42.0f ? 0 : (m_iconSize < 60.0f ? 1 : 2);
    const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Küçük simgeler", 0xE80A, false, sz == 0 ? L"●" : L""), MenuItem(2, L"Orta simgeler", 0xE80A, false, sz == 1 ? L"●" : L""),
                                               MenuItem(3, L"Büyük simgeler", 0xE80A, false, sz == 2 ? L"●" : L""), MenuSeparator(),
                                               MenuItem(4, L"Otomatik düzenle", 0xE8FD, false, m_auto ? L"●" : L""),
                                               MenuItem(5, L"Masaüstü simgelerini göster", 0xE7B3, false, m_showIcons ? L"●" : L""), MenuSeparator(),
                                               MenuItem(6, L"Bu Bilgisayar", 0xE7F8, false, m_showPC ? L"●" : L""), MenuItem(7, L"Geri Dönüşüm Kutusu", 0xE74D, false, m_showBin ? L"●" : L""),
                                               MenuItem(8, L"Kullanıcı klasörü", 0xE77B, false, m_showUser ? L"●" : L""), MenuItem(9, L"Ağ", 0xE774, false, m_showNet ? L"●" : L"")}, pt);
    if (c >= 1 && c <= 3) { static const float sizes[3] = {36.0f, 48.0f, 72.0f}; m_iconSize = sizes[c - 1]; Place(); }
    else if (c == 4) { m_auto = !m_auto; Place(); }
    else if (c == 5) m_showIcons = !m_showIcons;
    else if (c == 6) { m_showPC = !m_showPC; LoadItems(); }
    else if (c == 7) { m_showBin = !m_showBin; LoadItems(); }
    else if (c == 8) { m_showUser = !m_showUser; LoadItems(); }
    else if (c == 9) { m_showNet = !m_showNet; LoadItems(); }
    if (c) { SaveCfg(); Kick(); }
}
void Desktop::SortMenu(POINT pt) {
    using aether::ui::MenuItem;
    const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Ada göre", 0xE8CB, false, m_sortKey == 0 ? L"●" : L""), MenuItem(2, L"Boyuta göre", 0xE8CB, false, m_sortKey == 1 ? L"●" : L""),
                                               MenuItem(3, L"Türe göre", 0xE8CB, false, m_sortKey == 2 ? L"●" : L""), MenuItem(4, L"Değiştirilme tarihine göre", 0xE8CB, false, m_sortKey == 3 ? L"●" : L"")}, pt);
    if (c >= 1 && c <= 4) ArrangeBy(c - 1);
}
void Desktop::NewMenu(POINT pt) {
    using aether::ui::MenuItem;
    const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Klasör", bui::kGlyphNewFolder, false, L"Ctrl+Shift+N"), MenuItem(2, L"Metin belgesi", bui::kGlyphDocument)}, pt);
    if (c == 1) NewFolder(); else if (c == 2) NewTextFile();
}
void Desktop::PickWallpaper() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC fs[] = {{L"Resimler", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.tif;*.tiff"}};
    dlg->SetFileTypes(1, fs);
    dlg->SetTitle(L"Duvar kağıdı seç");
    DWORD o = 0; dlg->GetOptions(&o); dlg->SetOptions(o | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(m_hwnd))) return;
    ComPtr<IShellItem> it; PWSTR p = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        system::wallpaper::SetFile(p);
        CoTaskMemFree(p);
        SetTimer(m_hwnd, kTimerSync, 1000, nullptr);
        LoadWallpaper(true);
    }
}

void Desktop::BackgroundMenu(POINT pt) {
    using aether::ui::MenuItem; using aether::ui::MenuSeparator;
    std::vector<aether::ui::MenuEntry> items = {
        MenuItem(1, L"Görünüm", bui::kGlyphView), MenuItem(2, L"Sırala", bui::kGlyphSort), MenuItem(3, L"Yenile", bui::kGlyphRefresh, false, L"F5"), MenuSeparator(),
        MenuItem(4, L"Yapıştır", bui::kGlyphPaste, false, L"Ctrl+V"), MenuItem(5, L"Yeni", bui::kGlyphNewFolder), MenuSeparator(),
        MenuItem(11, L"Özelleştir (Aether)", 0xE790), MenuItem(6, L"Duvar kağıdını değiştir...", 0xE91B), MenuItem(7, L"Ekran ayarları", 0xE7F4), MenuItem(8, L"Windows kişiselleştirme", 0xE771), MenuSeparator(),
        MenuItem(9, L"Aether Explorer'da aç", 0xE838), MenuItem(10, L"Terminalde aç", 0xE756)};
    items[4].enabled = IsClipboardFormatAvailable(CF_HDROP) != 0;
    const int c = aether::ui::ShowContextMenu(items, pt);
    switch (c) {
        case 1: ViewMenu(pt); break;
        case 2: SortMenu(pt); break;
        case 3: LoadItems(); m_iconT.clear(); m_icons.clear(); m_iconFailed.clear(); IQ().Clear(m_hwnd); m_iconPending.clear(); LoadWallpaper(true); CheckBin(); break;
        case 4: Paste(); break;
        case 5: NewMenu(pt); break;
        case 6: m_slide = false; ApplySlideTimer(); PickWallpaper(); break;
        case 11: m_panel = true; m_panelScroll = 0; Kick(); break;
        case 7: ShellExecuteW(m_hwnd, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL); break;
        case 8: ShellExecuteW(m_hwnd, L"open", L"ms-settings:personalization", nullptr, nullptr, SW_SHOWNORMAL); break;
        case 9: OpenFolder(m_desktopDir); break;
        case 10: {
            const std::wstring a = L"-d \"" + m_desktopDir + L"\"";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd, L"open", L"wt.exe", a.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
                ShellExecuteW(m_hwnd, L"open", L"cmd.exe", nullptr, m_desktopDir.c_str(), SW_SHOWNORMAL);
            break;
        }
    }
}

// ---- drag & drop ----------------------------------------------------------------------------------------------------------
void Desktop::FinishOwnDrag() {
    const auto sel = SelectedIdx();
    if (sel.empty()) return;
    if (m_dropHover >= 0) {
        const Item& t = m_items[m_dropHover];
        std::vector<std::wstring> src;
        for (int i : sel) if (!m_items[i].Special() && m_items[i].kind != K_User) src.push_back(m_items[i].path);
        if (!src.empty()) {
            if (t.kind == K_Bin) su::RunFileOp(m_hwnd, kMsgOpDone, su::Op::Delete, src, L"", L"", L"");
            else if (t.dir && t.kind != K_PC && t.kind != K_Net) su::RunFileOp(m_hwnd, kMsgOpDone, su::Op::Move, src, t.path, L"", L"");
        }
        return;
    }
    if (m_tvalid && m_pressItem >= 0) {
        const int dc = m_tcol - m_items[m_pressItem].col, dr = m_trow - m_items[m_pressItem].row;
        if (dc != 0 || dr != 0) {
            m_auto = false;
            for (int i : sel) { m_items[i].col += dc; m_items[i].row += dr; }
            for (const auto& it : m_items) m_pos[it.key] = Pos3{m_mon, it.col, it.row};
            SaveCfg();
        }
    }
}

void Desktop::StartOleDrag() {
    if (m_dragTo) { m_dragTo->HandoffCancel(); m_dragTo = nullptr; }
    const auto paths = SelectedPaths();
    m_dragging = false; m_dropHover = -1; m_leftDown = false;
    ReleaseCapture();
    if (paths.empty()) return;
    auto data = su::MakeDataObject(paths);
    if (!data) return;
    ComPtr<IDropSource> src;
    src.Attach(new su::ShellDropSource());
    m_oleDrag = true;
    DWORD eff = 0;
    DoDragDrop(data.Get(), src.Get(), DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    m_oleDrag = false;
    m_pressItem = -1;
    if (eff & DROPEFFECT_MOVE) SetTimer(m_hwnd, kTimerReload, 400, nullptr);
    Kick();
}

// ---- cross-monitor handoff -------------------------------------------------------------------------------------------------
std::vector<Desktop*>& Desktop::Siblings() { static std::vector<Desktop*> s; return s; }

Desktop* Desktop::SiblingAt(HWND h) {
    if (!h) return nullptr;
    for (Desktop* d : Siblings()) if (d && d->m_hwnd == h) return d;
    return nullptr;
}

void Desktop::BeginHandoff(Desktop* other, const POINT& sp) {
    if (!other || other == this || m_pressItem < 0) return;
    const auto sel = SelectedIdx();
    std::vector<std::wstring> keys;
    std::vector<std::pair<int, int>> delta;
    auto add = [&](int i) {
        if (m_items[i].Special() || m_items[i].kind == K_User) return;   // This PC / bin / network / profile folder cannot move
        keys.push_back(m_items[i].key);
        delta.push_back({m_items[i].col - m_items[m_pressItem].col, m_items[i].row - m_items[m_pressItem].row});
    };
    add(m_pressItem);
    for (int i : sel) if (i != m_pressItem) add(i);
    if (keys.empty()) return;
    m_dragTo = other;
    m_dropHover = -1; m_tvalid = false;                 // the snapping preview belongs to the target screen now
    other->HandoffIn(keys, m_items[m_pressItem].key, delta, m_grabDX, m_grabDY, sp);
}

void Desktop::HandoffIn(const std::vector<std::wstring>& keys, const std::wstring& anchor,
                        const std::vector<std::pair<int, int>>& delta, float gdx, float gdy, const POINT& sp) {
    m_hoKeys = keys; m_hoDelta = delta; m_hoGdx = gdx; m_hoGdy = gdy;
    m_hoIdx.assign(keys.size(), -1); m_hoAnchor = -1;
    for (size_t k = 0; k < keys.size(); ++k) {
        if (IEquals(keys[k], anchor)) m_hoAnchor = k;
        for (int i = 0; i < Count(); ++i) if (m_items[i].key == keys[k]) { m_hoIdx[k] = i; break; }
    }
    if (m_hoAnchor >= 0) m_hoAnchor = m_hoIdx[m_hoAnchor];
    HandoffMove(sp);
}

void Desktop::HandoffMove(const POINT& sp) {
    if (m_hoIdx.empty()) return;
    POINT cp = sp; ScreenToClient(m_hwnd, &cp);
    const float s = Scale();
    m_hoCx = cp.x / s; m_hoCy = cp.y / s;
    const auto z = Z();
    const float gx = m_hoCx - m_hoGdx, gy = m_hoCy - m_hoGdy;
    const int tc = static_cast<int>(std::lround((gx - (m_work.x + 8)) / z.cw)), tr = static_cast<int>(std::lround((gy - (m_work.y + 8)) / z.ch));
    m_hoTcol = tc; m_hoTrow = tr; m_hoValid = true;
    std::vector<int> ignore;                            // carried icons do not block their own target cells
    for (int i : m_hoIdx) if (i >= 0) ignore.push_back(i);
    for (size_t k = 0; k < m_hoDelta.size(); ++k) {
        const int c = tc + m_hoDelta[k].first, r = tr + m_hoDelta[k].second;
        if (c < 0 || r < 0 || c >= m_cols || r >= m_rows || Occupied(c, r, ignore)) { m_hoValid = false; break; }
    }
    Kick();
}

void Desktop::HandoffCancel() {
    m_hoKeys.clear(); m_hoIdx.clear(); m_hoDelta.clear();
    m_hoAnchor = -1; m_hoTcol = m_hoTrow = -1; m_hoValid = false;
    Kick();
}

std::vector<std::pair<std::wstring, Pos3>> Desktop::HandoffDrop(const POINT& sp) {
    std::vector<std::pair<std::wstring, Pos3>> out;
    if (m_hoIdx.empty()) return out;
    POINT cp = sp; ScreenToClient(m_hwnd, &cp);
    const float s = Scale();
    std::vector<std::wstring> paths;
    for (int i : m_hoIdx) if (i >= 0 && !m_items[i].Special() && m_items[i].kind != K_User) paths.push_back(m_items[i].path);
    const int hi = HitItem(cp.x / s, cp.y / s);
    if (hi >= 0 && !paths.empty()) {                    // released on a folder or the bin: act like a normal intra-desktop drop
        const Item& t = m_items[hi];
        if (t.kind == K_Bin) { su::RunFileOp(m_hwnd, kMsgOpDone, su::Op::Delete, paths, L"", L"", L""); HandoffCancel(); return out; }
        if (t.dir && t.kind != K_PC && t.kind != K_Net) { su::RunFileOp(m_hwnd, kMsgOpDone, su::Op::Move, paths, t.path, L"", L""); HandoffCancel(); return out; }
    }
    const auto z = Z();
    const float gx = (cp.x / s) - m_hoGdx, gy = (cp.y / s) - m_hoGdy;
    const int tc = std::clamp(static_cast<int>(std::lround((gx - (m_work.x + 8)) / z.cw)), 0, std::max(0, m_cols - 1));
    const int tr = std::clamp(static_cast<int>(std::lround((gy - (m_work.y + 8)) / z.ch)), 0, std::max(0, m_rows - 1));
    auto cell = [](int c, int r) { return (static_cast<long long>(c) << 32) | static_cast<unsigned>(r); };
    std::set<long long> taken;                          // every cell of this screen plus what earlier carried items already claimed
    for (const auto& it : m_items) if (it.col >= 0) taken.insert(cell(it.col, it.row));
    m_auto = false;
    for (size_t k = 0; k < m_hoIdx.size(); ++k) {
        int c = tc + m_hoDelta[k].first, r = tr + m_hoDelta[k].second;
        bool ok = false;
        for (int rad = 0; rad < m_cols + m_rows && !ok; ++rad)      // nearest free cell when the ideal spot is taken
            for (int dc = -rad; dc <= rad && !ok; ++dc)
                for (int dr = -(rad - std::abs(dc)); dr <= rad - std::abs(dc); ++dr) {
                    const int cc = c + dc, rr = r + dr;
                    if (cc < 0 || rr < 0 || cc >= m_cols || rr >= m_rows || taken.count(cell(cc, rr))) continue;
                    c = cc; r = rr; ok = true; break;
                }
        if (!ok) { c = std::max(0, m_cols - 1); r = std::max(0, m_rows - 1); }
        taken.insert(cell(c, r));
        if (int i = m_hoIdx[k]; i >= 0) { m_items[i].col = c; m_items[i].row = r; }
        const Pos3 p{m_mon, c, r};
        m_pos[m_hoKeys[k]] = p;
        out.push_back({m_hoKeys[k], p});
    }
    SaveCfg();
    LoadItems();
    CheckBin();
    HandoffCancel();
    return out;
}

DWORD Desktop::DragOverAt(POINT sp, DWORD keys, DWORD allowed) {
    if (m_oleDrag) { m_dropHover = -1; return DROPEFFECT_NONE; }         // our own drag re-entering: nothing to do
    POINT cp = sp; ScreenToClient(m_hwnd, &cp);
    const float s = Scale();
    const int i = HitItem(cp.x / s, cp.y / s);
    m_dropHover = (i >= 0 && (m_items[i].dir || m_items[i].kind == K_Bin) && m_items[i].kind != K_PC && m_items[i].kind != K_Net) ? i : -1;
    Kick();
    DWORD eff = (keys & MK_SHIFT) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    if (m_dropHover >= 0 && m_items[m_dropHover].kind == K_Bin) eff = DROPEFFECT_MOVE;
    if (!(allowed & eff)) eff = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : ((allowed & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE);
    return eff;
}

void Desktop::DropAt(IDataObject* obj, POINT sp, DWORD keys, DWORD* effect) {
    if (m_oleDrag) { *effect = DROPEFFECT_NONE; return; }
    const DWORD eff = DragOverAt(sp, keys, *effect);
    const auto paths = su::PathsFromData(obj);
    if (eff == DROPEFFECT_NONE || paths.empty()) { *effect = DROPEFFECT_NONE; return; }
    std::wstring dest = m_desktopDir;
    bool toBin = false;
    if (m_dropHover >= 0) { const Item& t = m_items[m_dropHover]; if (t.kind == K_Bin) toBin = true; else dest = t.path; }
    if (toBin) { su::RunFileOp(m_hwnd, kMsgOpDone, su::Op::Delete, paths, L"", L"", L""); *effect = DROPEFFECT_MOVE; return; }
    if (dest.empty()) { *effect = DROPEFFECT_NONE; return; }
    auto norm = [](std::wstring p) { while (p.size() > 3 && p.back() == L'\\') p.pop_back(); return p; };
    if (eff & DROPEFFECT_MOVE) {
        bool same = true;
        for (const auto& p : paths) if (!IEquals(norm(aether::explorer::ParentOf(p)), norm(dest))) { same = false; break; }
        if (same) { *effect = DROPEFFECT_NONE; return; }
    }
    if (IEquals(norm(dest), norm(m_desktopDir))) {       // landing on this screen's background: pre-assign the grid cell so the icons arrive exactly where dropped
        const auto z = Z();
        const float s = Scale();
        POINT cp = sp; ScreenToClient(m_hwnd, &cp);
        int c = std::clamp(static_cast<int>(std::lround(((cp.x / s) - (m_work.x + 8)) / z.cw)), 0, std::max(0, m_cols - 1));
        int r = std::clamp(static_cast<int>(std::lround(((cp.y / s) - (m_work.y + 8)) / z.ch)), 0, std::max(0, m_rows - 1));
        for (const auto& p : paths) {
            const ULONGLONG t = GetTickCount64();
            m_pendingDrop[p.substr(p.find_last_of(L'\\') + 1)] = {Pos3{m_mon, c, r}, t};
            if (++r >= m_rows) { r = 0; if (++c >= m_cols) c = std::max(0, m_cols - 1); }
        }
    }
    su::RunFileOp(m_hwnd, kMsgOpDone, (eff & DROPEFFECT_MOVE) ? su::Op::Move : su::Op::Copy, paths, dest, L"", L"");
    *effect = eff;
}

// ---- keyboard ---------------------------------------------------------------------------------------------------------------
void Desktop::Move(int dx, int dy, bool extend) {
    if (Count() == 0) return;
    if (m_cursor < 0 || m_cursor >= Count()) { SelectOnly(0); Kick(); return; }
    const Item& cur = m_items[m_cursor];
    int best = -1; long bestScore = 1L << 30;
    for (int i = 0; i < Count(); ++i) {
        if (i == m_cursor) continue;
        const int ddc = m_items[i].col - cur.col, ddr = m_items[i].row - cur.row;
        const int along = dx * ddc + dy * ddr, across = std::abs(dx ? ddr : ddc);
        if (along <= 0) continue;
        const long score = along * 100L + across * 10L;
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best < 0) return;
    if (extend) m_sel[best] = 1; else SelectOnly(best);
    m_cursor = best;
    Kick();
}

void Desktop::OnKey(WPARAM vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (m_edit) { if (vk == VK_ESCAPE) CancelEdit(); return; }
    if (vk == VK_ESCAPE && m_panel) { m_panel = false; m_hotCtl = 0; Kick(); return; }
    switch (vk) {
        case VK_LEFT: Move(-1, 0, shift); break;
        case VK_RIGHT: Move(1, 0, shift); break;
        case VK_UP: Move(0, -1, shift); break;
        case VK_DOWN: Move(0, 1, shift); break;
        case VK_RETURN:
            if (alt) { if (m_cursor >= 0) Properties(m_cursor); }
            else for (int i : SelectedIdx()) OpenItem(i);
            break;
        case VK_DELETE: DeleteSel(shift); break;
        case VK_F2: if (m_cursor >= 0 && m_cursor < Count() && m_sel[m_cursor]) BeginRename(m_cursor); break;
        case VK_F5: LoadItems(); m_icons.clear(); m_iconFailed.clear(); IQ().Clear(m_hwnd); m_iconPending.clear(); LoadWallpaper(true); CheckBin(); break;
        case VK_ESCAPE: std::fill(m_sel.begin(), m_sel.end(), 0); Kick(); break;
        case VK_APPS: {
            POINT pt; GetCursorPos(&pt);
            if (m_cursor >= 0 && m_cursor < Count() && m_sel[m_cursor]) ItemMenu(m_cursor, pt); else BackgroundMenu(pt);
            break;
        }
        case 'A': if (ctrl) { std::fill(m_sel.begin(), m_sel.end(), 1); Kick(); } break;
        case 'C': if (ctrl) CopySel(false); break;
        case 'X': if (ctrl) CopySel(true); break;
        case 'V': if (ctrl) Paste(); break;
        case 'N': if (ctrl && shift) NewFolder(); break;
    }
}

void Desktop::OnChar(wchar_t c) {
    if (m_edit) {
        if (c == 8) { if (!m_editText.empty()) m_editText.pop_back(); }
        else if (c == 13) { CommitEdit(); return; }
        else if (c == 27) { CancelEdit(); return; }
        else if (c >= 32 && c != 127 && !wcschr(L"\\/:*?\"<>|", c)) m_editText += c;
        Kick();
        return;
    }
    if (c < 32 || c == 127) return;
    const ULONGLONG now = GetTickCount64();
    if (now - m_typeTick > 1000) m_type.clear();
    m_typeTick = now;
    m_type += c;
    for (int i = 0; i < Count(); ++i)
        if (_wcsnicmp(m_items[i].label.c_str(), m_type.c_str(), m_type.size()) == 0) { SelectOnly(i); Kick(); return; }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Aether.Desktop.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetCurrentProcessExplicitAppUserModelID(L"Aether.Desktop");
    OleInitialize(nullptr);

    // one desktop window per monitor, left to right; the first one is the primary
    std::vector<HMONITOR> hm;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM l) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(l)->push_back(m);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hm));
    std::sort(hm.begin(), hm.end(), [](HMONITOR a, HMONITOR b) {
        MONITORINFO ma = {sizeof(ma)}, mb = {sizeof(mb)};
        GetMonitorInfoW(a, &ma); GetMonitorInfoW(b, &mb);
        return ma.rcMonitor.left < mb.rcMonitor.left;
    });
    std::vector<std::unique_ptr<Desktop>> apps;
    for (size_t i = 0; i < hm.size(); ++i) {
        auto d = std::make_unique<Desktop>();
        if (!d->Create(inst, hm[i], static_cast<int>(i))) continue;
        Desktop::Siblings().push_back(d.get());
        apps.push_back(std::move(d));
    }
    if (apps.empty()) {
        MessageBoxW(nullptr, L"Aether Desktop penceresi oluşturulamadı.", L"Aether Desktop", MB_ICONERROR);
        OleUninitialize();
        return 1;
    }
    MSG msg;
    bool quit = false;
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        bool any = false;
        for (auto& d : apps) if (d && d->Alive() && d->NeedsFrame()) { d->Frame(); any = true; }
        if (any) MsgWaitForMultipleObjects(0, nullptr, FALSE, 7, QS_ALLINPUT);   // ~140 fps cap while animating, wakes instantly on input
        else WaitMessage();
    }
    OleUninitialize();
    return 0;
}
