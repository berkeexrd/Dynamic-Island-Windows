// Aether Explorer: a from-scratch file manager in the Aether glass style.
//
// Performance notes
//  * Nothing renders unless something changed (input, a finished background job, a running animation).
//    While idle the process sleeps in WaitMessage: 0% CPU.
//  * Directory listing, drive/recent/system queries and file operations run on worker threads.
//  * The file list is virtualised: only the visible rows are drawn and only their icons are loaded
//    (extension icons are cached, at most 3 new icons per frame).
//  * No blur / glow bitmaps: the glass look is plain gradients and rims, so a frame is a few dozen draw calls.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <deque>
#include <mutex>
#include <ole2.h>
#include <oleidl.h>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "aether/bar/ui_common.hpp"
#include "aether/render/context_menu.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"
#include "model.hpp"
#include "archive.hpp"
#include "netfs.hpp"

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

using namespace aether;
using namespace aether::explorer;
using core::Color;
using core::Rect;
using Microsoft::WRL::ComPtr;
namespace bui = aether::bar::ui;

namespace {

constexpr UINT kMsgFolder = WM_APP + 1;   // lParam = FolderResult*
constexpr UINT kMsgHome   = WM_APP + 2;   // lParam = HomeData*
constexpr UINT kMsgChanged = WM_APP + 6;
constexpr const wchar_t* kAppVersion = L"1.0";
constexpr const wchar_t* kRepoUrl = L"https://github.com/berkeexrd/Aether";
constexpr const wchar_t* kDonateUrl = L"https://github.com/berkeexrd/Aether";   // bağış sayfası adresini buraya yaz (GitHub Sponsors, Ko-fi, Buy Me a Coffee...)
constexpr UINT kMsgXfer = WM_APP + 7;     // lParam = new std::wstring (status text; empty clears)
constexpr UINT kMsgNav = WM_APP + 8;      // lParam = new std::wstring (path to open)
constexpr UINT kMsgThumb = WM_APP + 4;    // lParam = ThumbResult*
constexpr UINT kMsgInfo  = WM_APP + 5;    // lParam = InfoResult*
constexpr UINT kMsgOpDone = WM_APP + 3;   // wParam = 1: select + rename lParam (wchar_t* heap string, may be null)
constexpr UINT_PTR kTimerCaret = 2, kTimerRefresh = 3;

constexpr float kTitleH = 44.0f, kSideW = 236.0f, kToolbarH = 60.0f, kRowH = 38.0f, kHeadH = 34.0f, kCmdH = 44.0f, kStatusH = 30.0f;

inline void PostXfer(HWND h, const std::wstring& t) {
    if (!IsWindow(h)) return;
    auto* p = new std::wstring(t);
    if (!PostMessageW(h, kMsgXfer, 0, reinterpret_cast<LPARAM>(p))) delete p;
}
inline void PostNav(HWND h, const std::wstring& t) {
    if (!IsWindow(h)) return;
    auto* p = new std::wstring(t);
    if (!PostMessageW(h, kMsgNav, 0, reinterpret_cast<LPARAM>(p))) delete p;
}
inline void PostOpDone(HWND h, const std::wstring& sel) {
    if (!IsWindow(h)) return;
    wchar_t* b = nullptr;
    if (!sel.empty()) { b = static_cast<wchar_t*>(malloc((sel.size() + 1) * sizeof(wchar_t))); if (b) wcscpy(b, sel.c_str()); }
    if (!PostMessageW(h, kMsgOpDone, 0, reinterpret_cast<LPARAM>(b))) free(b);
}

// ---- hit ids ---------------------------------------------------------------------------------------------------
enum : int {
    H_None = 0, H_Min, H_Max, H_Close, H_Back, H_Fwd, H_Up, H_Refresh, H_Search, H_PathBox,
    H_NavHome, H_NavQuick, H_SideThisPc, H_SideNet, H_SideLinux, H_SideSettings, H_SidePower, H_SideUser, H_RecentAll, H_Scroll,
    H_CmdNew, H_CmdCut, H_CmdCopy, H_CmdPaste, H_CmdRename, H_CmdDelete, H_CmdSort,
    H_Crumb = 1000, H_CrumbSep = 1100, H_SideFolder = 2000, H_SidePin = 2100, H_SideConn = 2200, H_SideConnAdd = 12000, H_About = 12100, H_AboutClose, H_AboutGitHub, H_AboutDonate, H_AboutBlock, H_AboutCard, H_Tab = 11000, H_TabClose = 11100, H_TabNew = 11200,
    H_CmdDetails = 11300, H_CmdGrid, H_CmdPreview, H_PrevCalc, H_SideDrive = 3000, H_Banner = 4000, H_QuickTile = 5000, H_DriveCard = 6000,
    H_RecentRow = 7000, H_Tool = 9000, H_ColHead = 10000, H_Row = 100000
};

enum class SortKey { Name, Date, Type, Size };

struct HitRegion { Rect r; int id; };

const Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline Color QuickColor(size_t i) {
    static const Color c[] = {{0.38f, 0.72f, 1.0f, 1.0f}, {0.25f, 0.90f, 0.55f, 1.0f}, {0.86f, 0.86f, 0.94f, 1.0f},
                              {0.36f, 0.64f, 1.0f, 1.0f}, {1.0f, 0.42f, 0.74f, 1.0f}, {0.72f, 0.48f, 1.0f, 1.0f}};
    return c[i % 6];
}
inline D2D1_RECT_F D(const Rect& r) { return D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()); }
inline D2D1_COLOR_F DC(const Color& c, float a = -1.0f) { return D2D1::ColorF(c.r, c.g, c.b, a < 0 ? c.a : a); }
inline Color Mix(const Color& a, const Color& b, float k) { return Color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k}; }
inline Color WithA(const Color& c, float a) { return Color{c.r, c.g, c.b, a}; }
inline float Ease(float v, float target, float dt, float speed) {
    const float k = 1.0f - std::exp(-speed * dt);
    return v + (target - v) * k;
}

std::wstring FormatSize(uint64_t b) {
    wchar_t buf[64];
    if (b < 1024) swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024ull * 1024) swprintf(buf, 64, L"%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf(buf, 64, L"%.1f MB", b / 1048576.0);
    else swprintf(buf, 64, L"%.1f GB", b / 1073741824.0);
    return buf;
}
std::wstring FormatGB(uint64_t b) {
    wchar_t buf[64];
    const double gb = b / 1073741824.0;
    if (gb >= 100) swprintf(buf, 64, L"%.0f GB", gb); else swprintf(buf, 64, L"%.1f GB", gb);
    return buf;
}
std::wstring FormatDate(const FILETIME& ft) {
    FILETIME lt; SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return L"";
    wchar_t buf[64];
    swprintf(buf, 64, L"%02d.%02d.%04d %02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    return buf;
}
std::wstring FormatRelative(const FILETIME& ft) {
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b; a.LowPart = now.dwLowDateTime; a.HighPart = now.dwHighDateTime; b.LowPart = ft.dwLowDateTime; b.HighPart = ft.dwHighDateTime;
    const long long mins = a.QuadPart > b.QuadPart ? static_cast<long long>((a.QuadPart - b.QuadPart) / 600000000ull) : 0;
    wchar_t buf[64];
    if (mins < 1) return L"Az önce";
    if (mins < 60) { swprintf(buf, 64, L"%lld dk önce", mins); return buf; }
    if (mins < 60 * 24) { swprintf(buf, 64, L"%lld saat önce", mins / 60); return buf; }
    if (mins < 60 * 48) return L"Dün";
    FILETIME lt; SYSTEMTIME st; FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
    swprintf(buf, 64, L"%02d.%02d.%04d", st.wDay, st.wMonth, st.wYear);
    return buf;
}
std::wstring TypeName(const FileEntry& e) {
    if (e.dir) return L"Dosya klasörü";
    if (e.ext.empty()) return L"Dosya";
    std::wstring s = e.ext.substr(1);
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s + L" dosyası";
}
bool IEquals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool IContains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return StrStrIW(hay.c_str(), needle.c_str()) != nullptr;
}
std::wstring UniqueName(const std::wstring& dir, const std::wstring& base, const std::wstring& ext) {
    std::wstring n = base + ext;
    for (int i = 2; GetFileAttributesW(JoinPath(dir, n).c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; ++i)
        n = base + L" (" + std::to_wstring(i) + L")" + ext;
    return n;
}

// ---- file operations (IFileOperation = the shell's own engine: progress UI, undo, recycle bin, conflict dialogs) ----
enum class Op { Copy, Move, Delete, DeletePerm, Rename };

void RunFileOp(HWND owner, Op op, std::vector<std::wstring> src, std::wstring dest, std::wstring newName, std::wstring selectAfter) {
    std::thread([=]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        {
            ComPtr<IFileOperation> fo;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&fo)))) {
                fo->SetOwnerWindow(owner);
                if (op == Op::DeletePerm) fo->SetOperationFlags(FOF_NOCONFIRMMKDIR);
                else fo->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOFX_RECYCLEONDELETE);
                ComPtr<IShellItem> destItem;
                if (!dest.empty()) SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&destItem));
                for (const auto& p : src) {
                    ComPtr<IShellItem> it;
                    if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&it)))) continue;
                    switch (op) {
                        case Op::Copy:   if (destItem) fo->CopyItem(it.Get(), destItem.Get(), nullptr, nullptr); break;
                        case Op::Move:   if (destItem) fo->MoveItem(it.Get(), destItem.Get(), nullptr, nullptr); break;
                        case Op::Delete: case Op::DeletePerm: fo->DeleteItem(it.Get(), nullptr); break;
                        case Op::Rename: fo->RenameItem(it.Get(), newName.c_str(), nullptr); break;
                    }
                }
                fo->PerformOperations();
            }
        }
        CoUninitialize();
        if (IsWindow(owner)) {
            wchar_t* sel = nullptr;
            if (!selectAfter.empty()) { sel = static_cast<wchar_t*>(malloc((selectAfter.size() + 1) * sizeof(wchar_t))); if (sel) wcscpy(sel, selectAfter.c_str()); }
            PostMessageW(owner, kMsgOpDone, 0, reinterpret_cast<LPARAM>(sel));
        }
    }).detach();
}


// ---- system clipboard (CF_HDROP + Preferred DropEffect: works with Windows Explorer and every other app) ------------
bool SetClipFiles(HWND hwnd, const std::vector<std::wstring>& paths, bool cut) {
    size_t chars = 0;
    for (const auto& p : paths) chars += p.size() + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (chars + 1) * sizeof(wchar_t));
    HGLOBAL he = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!h || !he) { if (h) GlobalFree(h); if (he) GlobalFree(he); return false; }
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES); df->fWide = TRUE;
    wchar_t* d = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) { wcscpy(d, p.c_str()); d += p.size() + 1; }
    GlobalUnlock(h);
    *static_cast<DWORD*>(GlobalLock(he)) = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    GlobalUnlock(he);
    if (!OpenClipboard(hwnd)) { GlobalFree(h); GlobalFree(he); return false; }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, h);
    SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), he);
    CloseClipboard();
    return true;
}
bool GetClipFiles(HWND hwnd, std::vector<std::wstring>& out, bool& cut) {
    out.clear(); cut = false;
    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(hwnd)) return false;
    if (HDROP h = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
        const UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH * 2];
            if (DragQueryFileW(h, i, buf, MAX_PATH * 2)) out.push_back(buf);
        }
    }
    if (HANDLE he = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT))) {
        if (auto* p = static_cast<DWORD*>(GlobalLock(he))) { cut = (*p & DROPEFFECT_MOVE) != 0 && (*p & DROPEFFECT_COPY) == 0; GlobalUnlock(he); }
    }
    CloseClipboard();
    return !out.empty();
}
void SetClipText(HWND hwnd, const std::wstring& t) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (!h) return;
    wcscpy(static_cast<wchar_t*>(GlobalLock(h)), t.c_str()); GlobalUnlock(h);
    if (!OpenClipboard(hwnd)) { GlobalFree(h); return; }
    EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, h); CloseClipboard();
}
ComPtr<IDataObject> MakeDataObject(const std::vector<std::wstring>& paths) {
    ComPtr<IDataObject> d;
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& p : paths) { PIDLIST_ABSOLUTE pidl = nullptr; if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) ids.push_back(pidl); }
    if (!ids.empty()) {
        ComPtr<IShellItemArray> arr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(ids.data()), &arr)) && arr)
            arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&d));
    }
    for (auto p : ids) CoTaskMemFree(p);
    return d;
}
std::vector<std::wstring> PathsFromData(IDataObject* obj) {
    std::vector<std::wstring> out;
    if (!obj) return out;
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (SUCCEEDED(obj->GetData(&fe, &sm))) {
        if (HDROP h = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
            const UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) { wchar_t buf[MAX_PATH * 2]; if (DragQueryFileW(h, i, buf, MAX_PATH * 2)) out.push_back(buf); }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
    }
    return out;
}

// ---- thumbnails: one background thread, newest request first, results posted back as raw pixels ---------------------
struct ThumbResult { std::wstring path; int px{0}; int w{0}, h{0}; std::vector<uint32_t> pixels; };
struct InfoResult { std::wstring path; uint32_t gen{0}; int count{0}; uint64_t size{0}; };

struct ThumbQueue {
    std::mutex m; std::condition_variable cv;
    std::deque<std::pair<std::wstring, int>> q;
    HWND hwnd{nullptr}; bool started{false};
    void Clear() { std::lock_guard<std::mutex> l(m); q.clear(); }
    void Push(HWND h, const std::wstring& path, int px) {
        { std::lock_guard<std::mutex> l(m); q.emplace_back(path, px); hwnd = h; }
        if (!started) { started = true; std::thread([this] { Run(); }).detach(); }
        cv.notify_one();
    }
    void Run() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            std::pair<std::wstring, int> job; HWND h;
            { std::unique_lock<std::mutex> l(m); cv.wait(l, [this] { return !q.empty(); }); job = q.back(); q.pop_back(); h = hwnd; }
            auto* r = new ThumbResult(); r->path = job.first; r->px = job.second;
            ComPtr<IShellItemImageFactory> f;
            if (SUCCEEDED(SHCreateItemFromParsingName(job.first.c_str(), nullptr, IID_PPV_ARGS(&f))) && f) {
                HBITMAP hbm = nullptr;
                if (SUCCEEDED(f->GetImage(SIZE{job.second, job.second}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbm)) && hbm) {
                    BITMAP bm{}; GetObjectW(hbm, sizeof(bm), &bm);
                    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
                        r->w = bm.bmWidth; r->h = bm.bmHeight;
                        r->pixels.resize(static_cast<size_t>(bm.bmWidth) * bm.bmHeight);
                        BITMAPINFO bi = {};
                        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = bm.bmWidth; bi.bmiHeader.biHeight = -bm.bmHeight;
                        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                        HDC dc = GetDC(nullptr);
                        if (!GetDIBits(dc, hbm, 0, bm.bmHeight, r->pixels.data(), &bi, DIB_RGB_COLORS)) r->w = r->h = 0;
                        ReleaseDC(nullptr, dc);
                        bool anyAlpha = false;
                        for (uint32_t p : r->pixels) if (p >> 24) { anyAlpha = true; break; }
                        if (!anyAlpha) for (auto& p : r->pixels) p |= 0xFF000000;
                    }
                    DeleteObject(hbm);
                }
            }
            if (!IsWindow(h) || !PostMessageW(h, kMsgThumb, 0, reinterpret_cast<LPARAM>(r))) delete r;
        }
    }
};
ThumbQueue& TQ() { static ThumbQueue* q = new ThumbQueue(); return *q; }   // intentionally leaked (process lifetime)

bool IsThumbExt(const std::wstring& x) {
    static const wchar_t* e[] = {L".png", L".jpg", L".jpeg", L".gif", L".webp", L".bmp", L".tif", L".tiff", L".heic", L".mp4", L".mkv", L".avi", L".mov",
                                 L".webm", L".wmv", L".pdf", L".psd", L".svg", L".ico", L".docx", L".pptx", L".xlsx"};
    for (auto* p : e) if (x == p) return true;
    return false;
}

std::wstring CfgPath() {
    wchar_t buf[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(buf) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\explorer.cfg";
}

class ExplorerDropTarget;

// ---- the application ------------------------------------------------------------------------------------------
class Explorer {
public:
    bool Create(HINSTANCE inst, const std::wstring& startPath);
    HWND Hwnd() const { return m_hwnd; }
    bool NeedsFrame() const { return m_dirty || m_animating; }
    void Frame();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    // layout / drawing
    void Render();
    void DrawBackground(float W, float H);
    void DrawTitleBar(float W);
    void DrawSidebar(float H);
    void DrawToolbar(float W);
    void DrawHome(float W, float H);
    void DrawFolder(float W, float H);
    void DrawHomeRight(const Rect& pane);
    void DrawScrollbar(const Rect& track, float contentH, float viewH);

    // primitives
    void Hit(const Rect& r, int id);
    float Hover(int id) const { auto it = m_hover.find(id); return it == m_hover.end() ? 0.0f : it->second; }
    void FillRR(const Rect& r, float rad, const Color& c);
    void StrokeRR(const Rect& r, float rad, const Color& c, float w = 1.0f);
    void Card(const Rect& r, float rad, float hover, float strength = 1.0f);
    void Txt(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, int align = 0);
    void Glyph(wchar_t g, const Rect& r, float size, const Color& c);
    IDWriteTextFormat* Fmt(float size, int weight, int align, bool icon);
    void IconTile(const Rect& r, wchar_t glyph, float hover, float glyphSize = 22.0f);
    void ProgressBar(const Rect& r, float frac, bool warn);
    ID2D1Bitmap1* FileIcon(const std::wstring& fullPath, const std::wstring& ext);
    void DrawFileGlyph(const FileEntry& e, const std::wstring& fullPath, const Rect& r);
    void Icon(int kind, const Rect& r);
    void UnitPoly(int id, std::initializer_list<D2D1_POINT_2F> pts, const Rect& r);
    void Swirl(const Rect& r, float px, const Color& c);
    void Badge(const std::wstring& label, const Rect& r, const Color& a, const Color& b, const Color& tc, bool border);
    void ChipIcon(int kind, const Rect& r, const Color& c);

    // state & behaviour
    void Navigate(const std::wstring& path, bool push = true);
    void GoBack();
    void GoForward();
    void Refresh();
    void StartFolderLoad();
    void StartHomeLoad();
    void RebuildView(const std::set<std::wstring>* keepIn = nullptr);   // keepIn: names to keep selected (taken from the OLD list when m_items is replaced)
    void OnFolderResult(FolderResult* r);
    void OnHomeResult(HomeData* d);
    void OpenEntry(int viewIndex);
    void OpenPath(const std::wstring& path);
    void Click(int id, bool ctrl, bool shift);
    void DoubleClick(int id);
    void ContextMenu(int id, POINT screen);
    void OnKey(WPARAM vk);
    void OnChar(wchar_t c);
    void CommitEdit();
    void CancelEdit();
    void BeginEdit(int target, const std::wstring& text);
    void SelectOnly(int vi);
    void ExtendSelection(int vi);
    void EnsureVisible(int vi);
    std::vector<std::wstring> SelectedPaths() const;
    void CopySelection(bool cut);
    void Paste();
    void DeleteSelection(bool perm = false);
    void NewFolder();
    void Compress(int kind = 0);
    // network / archives
    bool EditConn(Conn& c);
    void AddConnection();
    void ConnectTo(int i);
    void AddMenu(POINT pt);
    void MapDrive();
    bool EnsureAuth(const std::wstring& url);
    void StartTransfer(std::vector<std::wstring> srcs, std::wstring dest, bool move);
    void RemoteOp(const std::wstring& anyUrl, std::function<bool(FtpClient&)> fn, const std::wstring& sel);
    void OpenRemoteFile(const std::wstring& url);
    void DownloadSelection();
    void BrowseArchive(const std::wstring& arc);
    void ExtractArchiveCmd(const std::wstring& arc, bool intoFolder);
    void Upload();
    void ShowProperties();
    void SortBy(SortKey k);
    void ScrollBy(float dy);
    void Kick();
    void Tick();
    void UpdateCaption(int minW);
    int HitAt(float x, float y) const;
    float Scale() const { return m_dpi / 96.0f; }

    friend class ExplorerDropTarget;
    // tabs
    struct Tab { std::wstring path; std::vector<std::wstring> hist{L""}; int pos{0}; float scroll{0}; };
    void SaveTab();
    void SwitchTab(int i);
    void NewTab(const std::wstring& path);
    void CloseTab(int i);
    std::wstring TabTitle(const std::wstring& p) const;
    void DrawTabs(float W);
    void DrawAbout(float W, float H);
    void GlowRR(const Rect& r, float rad, const Color& c, float spread, float strength);
    bool m_about{false};
    // helpers
    std::wstring FullPath(const FileEntry& e) const { return e.parent.empty() ? JoinPath(m_path, e.name) : JoinPath(e.parent, e.name); }
    void LoadCfg();
    void SaveCfg();
    Rect CellRect(int vi) const;
    void RowDown(int vi, bool ctrl, bool shift);
    void UpdateMarquee();
    void StartDrag();
    DWORD DragOverAt(POINT screen, DWORD keys, DWORD allowed);
    void DragLeaveAll();
    void DropAt(IDataObject* obj, POINT screen, DWORD keys, DWORD* effect);
    std::wstring DropTargetPath(int id) const;
    void ShellMenu(const std::vector<std::wstring>& paths, POINT pt);
    void StartDeepSearch();
    void ClearSearch();
    void DrawPreview(const Rect& pane);
    ID2D1Bitmap1* Thumb(const std::wstring& path, int px);
    void OnThumb(ThumbResult* r);
    void OnInfo(InfoResult* r);
    void StartInfo(const std::wstring& path);
    void TogglePin(const std::wstring& path);
    bool IsPinned(const std::wstring& path) const;
    void OpenTerminal(const std::wstring& dir);
    void BreadcrumbMenu(int crumbIndex, POINT pt);
    void NewFile(const std::wstring& base, const std::wstring& ext);
    void TxtWrap(const std::wstring& s, const Rect& r, float size, int weight, const Color& c);
    void ViewMenu(POINT pt);
    void StartWatch();
    void StopWatch();
    HANDLE m_watchStop{nullptr};

    // window
    HWND m_hwnd{nullptr};
    float m_dpi{96.0f};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ID2D1DeviceContext* m_ctx{nullptr};          // valid during Render()
    std::map<uint32_t, ComPtr<IDWriteTextFormat>> m_fmts;
    const wchar_t* m_iconFont{L"Segoe Fluent Icons"};
    const wchar_t* m_textFont{L"Segoe UI Variable Text"};
    const wchar_t* m_dispFont{L"Segoe UI Variable Display"};
    bool m_maximized{false}, m_tracking{false}, m_acrylic{false};
    bool m_dirty{true};

    // input / animation
    float m_mx{-1}, m_my{-1};
    int m_hot{0};
    bool m_leftDown{false};
    bool m_dragScroll{false};
    float m_dragGrab{0};
    std::vector<HitRegion> m_hits;
    Rect m_clip;
    std::unordered_map<int, float> m_hover;
    LARGE_INTEGER m_freq{}, m_last{};
    bool m_animating{false}, m_inFrame{false};
    int m_iconBudget{3};
    bool m_needRedrawForIcons{false};

    // data
    std::vector<QuickFolder> m_quick;
    HomeData m_homeData;
    bool m_homeLoaded{false};
    std::wstring m_path;                    // L"" = home
    bool m_home{true};
    std::vector<std::wstring> m_hist{L""};
    int m_histPos{0};
    std::vector<FileEntry> m_items;
    std::vector<int> m_view;                // indices into m_items
    std::vector<char> m_sel;                // per view index
    int m_anchor{-1}, m_cursor{-1};
    bool m_loading{false}, m_loadFailed{false};
    std::atomic<uint32_t> m_gen{0};
    SortKey m_sortKey{SortKey::Name};
    bool m_sortAsc{true};
    bool m_showHidden{false};
    std::wstring m_pendingSelect;
    std::vector<Tab> m_tabs{Tab{}};
    int m_tab{0};
    bool m_grid{false}, m_preview{false}, m_deep{false}, m_pendingRename{false};
    float m_iconSize{96};
    int m_cols{1};
    float m_cellW{0}, m_cellH{kRowH}, m_lx{0}, m_ly{0}, m_lw{0};
    Rect m_listRect;
    int m_pressRow{-1};
    float m_pressX{0}, m_pressY{0};
    bool m_pressSelectOnUp{false}, m_dragStarted{false}, m_marquee{false}, m_dragging{false};
    float m_mqX{0}, m_mqY{0};
    std::vector<char> m_mqBase;
    int m_dropRow{-1};
    std::wstring m_dropPath;
    std::vector<std::wstring> m_dragSrc;
    ComPtr<IDropTarget> m_dropTarget;
    std::vector<std::wstring> m_pins;
    std::vector<Conn> m_conns;
    std::wstring m_xfer;
    std::vector<std::wstring> m_ftpClip;
    bool m_ftpClipCut{false};
    DWORD m_ftpClipSeq{0};
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> m_thumbs;
    std::vector<std::wstring> m_thumbOrder;
    std::set<std::wstring> m_thumbPending;
    std::wstring m_infoPath;
    int m_infoCount{-1};
    uint64_t m_infoSize{0};
    bool m_infoBusy{false};
    std::atomic<uint32_t> m_infoGen{0};
    ComPtr<IContextMenu2> m_cm2;
    ComPtr<IContextMenu3> m_cm3;
    std::vector<std::wstring> m_clipPaths;
    bool m_clipCut{false};
    std::wstring m_typeBuf; ULONGLONG m_typeTick{0};

    // scrolling
    float m_scroll{0}, m_scrollTarget{0}, m_contentH{0}, m_viewH{0};
    float m_quickSectionY{0};
    Rect m_scrollTrack;

    // text editing (search / path / rename)
    enum { E_None = 0, E_Search, E_Path, E_Rename };
    int m_edit{E_None};
    std::wstring m_editText, m_search;
    bool m_editAll{false};                       // Ctrl+A / fresh edit: the whole text is selected, the next key replaces it
    float EditW(float size);
    int m_renameIndex{-1};

    // icon cache
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> m_icons;
    ComPtr<ID2D1PathGeometry> m_wave, m_waveFill; float m_waveW{0}, m_waveH{0};
    ComPtr<ID2D1PathGeometry> m_swirl;
    std::map<int, ComPtr<ID2D1PathGeometry>> m_polys;
    wchar_t m_sysDrive{L'C'};
    std::wstring m_status;
};

// ---- drag & drop COM objects ----------------------------------------------------------------------------------------
class ExplorerDropSource : public IDropSource {
    LONG m_ref{1};
public:
    HRESULT __stdcall QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDropSource) { *o = static_cast<IDropSource*>(this); AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG __stdcall Release() override { const LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT __stdcall QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT __stdcall GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class ExplorerDropTarget : public IDropTarget {
    LONG m_ref{1};
    Explorer* m_app;
public:
    explicit ExplorerDropTarget(Explorer* a) : m_app(a) {}
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

// ---- window plumbing ------------------------------------------------------------------------------------------
bool Explorer::Create(HINSTANCE inst, const std::wstring& startPath) {
    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_last);
    render::glass::SyncTheme();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"AetherExplorerWindow";
    wc.hCursor = nullptr;
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&wc);

    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    m_dpi = static_cast<float>(dx);
    const float s = m_dpi / 96.0f;
    const int wa = mi.rcWork.right - mi.rcWork.left, ha = mi.rcWork.bottom - mi.rcWork.top;
    int w = std::min(static_cast<int>(1320 * s), wa - 80), h = std::min(static_cast<int>(820 * s), ha - 60);
    const int x = mi.rcWork.left + (wa - w) / 2, y = mi.rcWork.top + (ha - h) / 2;

    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Explorer",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                             x, y, w, h, nullptr, nullptr, inst, this);
    if (!m_hwnd) return false;

    DWORD corner = 2;                                // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const Color rim = render::glass::kViolet;
    COLORREF border = RGB(static_cast<int>(rim.r * 120), static_cast<int>(rim.g * 80), static_cast<int>(rim.b * 160));
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    // Real frosted glass behind the window: DWM's own acrylic (GPU blur of what is really behind, live, no capture cost)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    MARGINS mg = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(m_hwnd, &mg);
    int backdropType = 3;                            // DWMSBT_TRANSIENTWINDOW = acrylic
    m_acrylic = SUCCEEDED(DwmSetWindowAttribute(m_hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdropType, sizeof(backdropType)));
    if (!m_acrylic) {                                // Windows 10: accent-policy acrylic
        struct ACCENT { int state; int flags; unsigned color; int anim; };
        struct WCA { int attr; void* data; ULONG size; };
        using Fn = BOOL(WINAPI*)(HWND, WCA*);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"))) {
            ACCENT ap = {4 /*ACRYLICBLURBEHIND*/, 0, 0x99120A1E, 0};
            WCA d = {19, &ap, sizeof(ap)};
            m_acrylic = fn(m_hwnd, &d) != FALSE;
        }
    }

    RECT rc; GetClientRect(m_hwnd, &rc);
    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, rc.right, rc.bottom)) return false;
    m_brush = m_dcomp->CreateSolidBrush(kWhite);

    // fonts present?
    ComPtr<IDWriteFontCollection> col;
    m_dcomp->GetDWriteFactory()->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL ex = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &ex)) && ex; };
    if (!has(L"Segoe Fluent Icons")) m_iconFont = L"Segoe MDL2 Assets";
    if (!has(L"Segoe UI Variable Text")) m_textFont = L"Segoe UI";
    if (!has(L"Segoe UI Variable Display")) m_dispFont = L"Segoe UI";

    // quick-access folders (known folders; cheap)
    struct Q { const KNOWNFOLDERID* id; const wchar_t* name; wchar_t glyph; };
    const Q qs[] = {{&FOLDERID_Desktop, L"Masaüstü", bui::kGlyphDesktop}, {&FOLDERID_Downloads, L"İndirilenler", bui::kGlyphDownload},
                    {&FOLDERID_Documents, L"Belgeler", bui::kGlyphDocument}, {&FOLDERID_Pictures, L"Resimler", bui::kGlyphPicture},
                    {&FOLDERID_Music, L"Müzik", bui::kGlyphMusic}, {&FOLDERID_Videos, L"Videolar", bui::kGlyphVideo}};
    for (const auto& q : qs) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*q.id, 0, nullptr, &p)) && p) { m_quick.push_back({q.name, p, q.glyph, -1}); CoTaskMemFree(p); }
    }

    { wchar_t sd[8] = {}; if (GetEnvironmentVariableW(L"SystemDrive", sd, 8) && sd[0]) m_sysDrive = static_cast<wchar_t>(towupper(sd[0])); }
    SetTimer(m_hwnd, kTimerRefresh, 1000, nullptr);   // theme sync + drive changes
    LoadCfg();
    m_dropTarget.Attach(new ExplorerDropTarget(this));
    RegisterDragDrop(m_hwnd, m_dropTarget.Get());
    ShowWindow(m_hwnd, SW_SHOW);
    Navigate(startPath, false);
    if (!startPath.empty()) { m_hist = {L"", startPath}; m_histPos = 1; }
    SaveTab();
    return true;
}

LRESULT CALLBACK Explorer::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Explorer* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Explorer*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else {
        self = reinterpret_cast<Explorer*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

int Explorer::HitAt(float x, float y) const {
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
        if (it->r.Contains(x, y)) return it->id;
    return 0;
}

LRESULT Explorer::Handle(UINT m, WPARAM w, LPARAM l) {
    const float s = Scale();
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            if (IsZoomed(m_hwnd)) {
                const int fx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, static_cast<UINT>(m_dpi)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, static_cast<UINT>(m_dpi));
                const int fy = GetSystemMetricsForDpi(SM_CYSIZEFRAME, static_cast<UINT>(m_dpi)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, static_cast<UINT>(m_dpi));
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT wr; GetWindowRect(m_hwnd, &wr);
        if (!IsZoomed(m_hwnd)) {
            const int b = static_cast<int>(6 * s);
            const bool L = pt.x < wr.left + b, R = pt.x >= wr.right - b, T = pt.y < wr.top + b, B = pt.y >= wr.bottom - b;
            if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
        }
        POINT cp = pt; ScreenToClient(m_hwnd, &cp);
        const float x = cp.x / s, y = cp.y / s;
        if (y < kTitleH && HitAt(x, y) == 0) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        mm->ptMinTrackSize.x = static_cast<LONG>(860 * s); mm->ptMinTrackSize.y = static_cast<LONG>(620 * s);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps);
        m_dirty = true; Frame();
        return 0;
    }
    case WM_SIZE: {
        if (w == SIZE_MINIMIZED) return 0;
        m_maximized = (w == SIZE_MAXIMIZED);
        if (m_dcomp) m_dcomp->Resize(LOWORD(l), HIWORD(l));
        m_dirty = true; Frame();
        return 0;
    }
    case WM_DPICHANGED: {
        m_dpi = static_cast<float>(LOWORD(w));
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        m_fmts.clear();
        Kick();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            const int id = HitAt(m_mx, m_my);
            const bool link = id != 0 && id != H_Scroll || m_edit != E_None && (id == H_Search || id == H_PathBox);
            SetCursor(link ? bui::Link() : bui::Arrow());
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        if (!m_tracking) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, m_hwnd, 0}; TrackMouseEvent(&t); m_tracking = true; }
        if (m_dragScroll) {
            const float range = m_scrollTrack.height - std::max(30.0f, m_scrollTrack.height * m_viewH / std::max(m_contentH, 1.0f));
            if (range > 1) {
                const float frac = (m_my - m_scrollTrack.y - m_dragGrab) / range;
                m_scrollTarget = m_scroll = std::clamp(frac, 0.0f, 1.0f) * std::max(0.0f, m_contentH - m_viewH);
            }
        }
        if (m_leftDown && m_pressRow >= 0 && !m_dragStarted && !m_dragScroll && (w & MK_LBUTTON) &&
            (std::abs(m_mx - m_pressX) > 6 || std::abs(m_my - m_pressY) > 6)) {
            m_dragStarted = true;
            if (m_pressRow < static_cast<int>(m_sel.size()) && !m_sel[m_pressRow]) SelectOnly(m_pressRow);
            ReleaseCapture();
            StartDrag();
            return 0;
        }
        if (m_marquee) {
            if (!(w & MK_LBUTTON)) m_marquee = false;
            else {
                if (m_my < m_listRect.y + 6) ScrollBy(-24.0f); else if (m_my > m_listRect.Bottom() - 6) ScrollBy(24.0f);
                UpdateMarquee();
            }
        }
        const int id = HitAt(m_mx, m_my);
        if (id != m_hot) m_hot = id;
        Kick();
        return 0;
    }
    case WM_MOUSELEAVE: m_tracking = false; m_mx = m_my = -1; m_hot = 0; Kick(); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(m_hwnd); SetCapture(m_hwnd);
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        const int id = HitAt(m_mx, m_my);
        m_leftDown = true;
        if (id == H_Scroll) {
            const float thumb = std::max(30.0f, m_scrollTrack.height * m_viewH / std::max(m_contentH, 1.0f));
            const float range = m_scrollTrack.height - thumb;
            const float top = m_scrollTrack.y + (m_contentH > m_viewH ? m_scroll / (m_contentH - m_viewH) : 0.0f) * range;
            m_dragGrab = (m_my >= top && m_my <= top + thumb) ? m_my - top : thumb * 0.5f;
            m_dragScroll = true;
            return 0;
        }
        if (m_edit != E_None && id != H_Search && id != H_PathBox && id != H_Row + m_renameIndex) CommitEdit();
        const bool ctrl = (w & MK_CONTROL) != 0, shift = (w & MK_SHIFT) != 0;
        m_pressRow = -1;
        if (id >= H_Row) RowDown(id - H_Row, ctrl, shift);
        else if (id == 0 && !m_home && m_listRect.Contains(m_mx, m_my)) {
            m_marquee = true; m_mqX = m_mx; m_mqY = m_my + m_scroll;
            if (ctrl) m_mqBase = m_sel; else { m_mqBase.assign(m_sel.size(), 0); std::fill(m_sel.begin(), m_sel.end(), 0); }
        } else Click(id, ctrl, shift);
        Kick();
        return 0;
    }
    case WM_LBUTTONUP:
        if (m_pressSelectOnUp && !m_dragStarted && m_pressRow >= 0) SelectOnly(m_pressRow);
        m_pressSelectOnUp = false; m_pressRow = -1; m_marquee = false;
        ReleaseCapture(); m_leftDown = false; m_dragScroll = false; Kick(); return 0;
    case WM_MBUTTONUP: {
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        const int id = HitAt(m_mx, m_my);
        if (id >= H_Tab && id < H_Tab + 100) CloseTab(id - H_Tab);
        else if (id >= H_Row) { const size_t vi = id - H_Row; if (vi < m_view.size() && m_items[m_view[vi]].dir) NewTab(FullPath(m_items[m_view[vi]])); }
        else if (id == H_TabNew) NewTab(L"");
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        DoubleClick(HitAt(m_mx, m_my));
        return 0;
    }
    case WM_RBUTTONUP: {
        if (m_about) return 0;
        m_mx = GET_X_LPARAM(l) / s; m_my = GET_Y_LPARAM(l) / s;
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(m_hwnd, &pt);
        ContextMenu(HitAt(m_mx, m_my), pt);
        return 0;
    }
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(w) == XBUTTON1) GoBack(); else GoForward();
        return TRUE;
    case WM_MOUSEWHEEL: {
        const float notches = GET_WHEEL_DELTA_WPARAM(w) / 120.0f;
        if ((GET_KEYSTATE_WPARAM(w) & MK_CONTROL) && m_grid) {
            m_iconSize = std::clamp(m_iconSize + notches * 16.0f, 48.0f, 192.0f);
            SaveCfg(); Kick();
            return 0;
        }
        ScrollBy(-notches * 96.0f);
        return 0;
    }
    case WM_KEYDOWN: OnKey(w); return 0;
    case WM_CHAR: OnChar(static_cast<wchar_t>(w)); return 0;
    case kMsgFolder: OnFolderResult(reinterpret_cast<FolderResult*>(l)); return 0;
    case kMsgHome: OnHomeResult(reinterpret_cast<HomeData*>(l)); return 0;
    case kMsgXfer: { auto* t = reinterpret_cast<std::wstring*>(l); m_xfer = *t; delete t; Kick(); return 0; }
    case kMsgNav: { auto* t = reinterpret_cast<std::wstring*>(l); const std::wstring p = *t; delete t; Navigate(p); return 0; }
    case kMsgThumb: OnThumb(reinterpret_cast<ThumbResult*>(l)); return 0;
    case kMsgInfo: OnInfo(reinterpret_cast<InfoResult*>(l)); return 0;
    case kMsgChanged:
        if (!m_home && !m_deep && !m_loading && m_edit != E_Rename) { KillTimer(m_hwnd, kTimerRefresh + 10); SetTimer(m_hwnd, kTimerRefresh + 10, 250, nullptr); }
        return 0;
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
        if (m_cm3) { LRESULT res = 0; if (SUCCEEDED(m_cm3->HandleMenuMsg2(m, w, l, &res))) return res; }
        else if (m_cm2) { if (SUCCEEDED(m_cm2->HandleMenuMsg(m, w, l))) return 0; }
        break;
    case kMsgOpDone: {
        auto* sel = reinterpret_cast<wchar_t*>(l);
        if (sel) { m_pendingSelect = sel; free(sel); }
        Refresh();
        return 0;
    }
    case WM_TIMER:
        if (w == kTimerCaret) { Kick(); }
        else if (w == kTimerRefresh + 10) { KillTimer(m_hwnd, kTimerRefresh + 10); if (!m_home && !m_deep && !m_loading) Refresh(); }
        else if (w == kTimerRefresh) {
            if (render::glass::SyncTheme()) { m_fmts.clear(); Kick(); }
        }
        return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: Kick(); return 0;
    case WM_MOUSEACTIVATE: SetFocus(m_hwnd); return MA_ACTIVATE;
    case WM_ACTIVATE: if (LOWORD(w) != WA_INACTIVE) SetFocus(m_hwnd); return 0;
    case WM_DESTROY: SaveCfg(); StopWatch(); RevokeDragDrop(m_hwnd); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// ---- animation / invalidation -----------------------------------------------------------------------------------
void Explorer::Kick() { m_dirty = true; }

// One frame: advance animations, then draw. Called from the message loop only while something changes
// (Present(1) paces it to the monitor refresh); otherwise the loop sleeps in WaitMessage.
void Explorer::Frame() {
    if (m_inFrame) return;
    m_inFrame = true;
    Tick();
    m_dirty = false;
    Render();
    m_inFrame = false;
}

void Explorer::Tick() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - m_last.QuadPart) / static_cast<float>(m_freq.QuadPart);
    m_last = now;
    dt = std::clamp(dt, 0.001f, 0.05f);
    bool anim = false;

    m_hot = HitAt(m_mx, m_my);
    if (m_hot) m_hover[m_hot];   // ensure an entry exists
    for (auto it = m_hover.begin(); it != m_hover.end();) {
        const float target = (it->first == m_hot) ? 1.0f : 0.0f;
        const float old = it->second;
        it->second = Ease(old, target, dt, 22.0f);
        if (std::fabs(it->second - target) < 0.02f) it->second = target;
        if (it->second != old) anim = true;
        if (it->second <= 0.0f && target == 0.0f) it = m_hover.erase(it); else ++it;
    }
    if (std::fabs(m_scrollTarget - m_scroll) > 0.4f && !m_dragScroll) { m_scroll = Ease(m_scroll, m_scrollTarget, dt, 20.0f); anim = true; }
    else if (!m_dragScroll) m_scroll = m_scrollTarget;
    if (m_loading || m_needRedrawForIcons) anim = true;
    m_animating = anim;
}

void Explorer::ScrollBy(float dy) {
    const float maxS = std::max(0.0f, m_contentH - m_viewH);
    m_scrollTarget = std::clamp(m_scrollTarget + dy, 0.0f, maxS);
    Kick();
}

// ---- primitives ---------------------------------------------------------------------------------------------------
void Explorer::Hit(const Rect& r, int id) {
    float x0 = std::max(r.x, m_clip.x), y0 = std::max(r.y, m_clip.y);
    float x1 = std::min(r.Right(), m_clip.Right()), y1 = std::min(r.Bottom(), m_clip.Bottom());
    if (x1 <= x0 || y1 <= y0) return;
    m_hits.push_back({Rect{x0, y0, x1 - x0, y1 - y0}, id});
}

void Explorer::FillRR(const Rect& r, float rad, const Color& c) {
    if (c.a <= 0.001f) return;
    m_brush->SetColor(DC(c));
    m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), rad, rad), m_brush.Get());
}
void Explorer::StrokeRR(const Rect& r, float rad, const Color& c, float w) {
    if (c.a <= 0.001f) return;
    m_brush->SetColor(DC(c));
    const float h = w * 0.5f;
    m_ctx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + h, r.y + h, r.Right() - h, r.Bottom() - h), rad, rad), m_brush.Get(), w);
}

// Glass card as in the concept: dark translucent body, hairline rim (light top, violet bottom), violet halo on hover
void Explorer::Card(const Rect& r, float rad, float hover, float strength) {
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    const bool flat = render::glass::CurrentTheme().flat;
    if (hover > 0.01f && !flat) {
        StrokeRR(Rect{r.x - 3, r.y - 3, r.width + 6, r.height + 6}, rad + 3, WithA(v, 0.10f * hover), 3.0f);
        StrokeRR(Rect{r.x - 1, r.y - 1, r.width + 2, r.height + 2}, rad + 1, WithA(v, 0.14f * hover), 1.5f);
    }
    const float a = std::clamp(strength, 0.4f, 1.4f);
    if (auto g = m_dcomp->CreateLinearGradientBrush(Color{0.02f, 0.01f, 0.05f, 0.40f * a}, Color{0.03f, 0.015f, 0.07f, 0.30f * a},
                                                    D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom())))
        m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), rad, rad), g.Get());
    if (hover > 0.01f) FillRR(r, rad, WithA(v, 0.13f * hover));
    if (auto g = m_dcomp->CreateLinearGradientBrush(Color{1.0f, 1.0f, 1.0f, 0.13f + 0.12f * hover}, WithA(vl, 0.20f + 0.40f * hover),
                                                    D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom()))) {
        m_ctx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + 0.5f, r.y + 0.5f, r.Right() - 0.5f, r.Bottom() - 0.5f), rad, rad), g.Get(), 1.0f);
    }
}

IDWriteTextFormat* Explorer::Fmt(float size, int weight, int align, bool icon) {
    const uint32_t key = static_cast<uint32_t>(size * 10) | (static_cast<uint32_t>(weight / 100) << 14) | (static_cast<uint32_t>(align) << 19) | (icon ? 1u << 22 : 0u);
    auto it = m_fmts.find(key);
    if (it != m_fmts.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    auto* dw = m_dcomp->GetDWriteFactory();
    const wchar_t* fam = icon ? m_iconFont : (size >= 24 ? m_dispFont : m_textFont);
    if (FAILED(dw->CreateTextFormat(fam, nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return nullptr;
    f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!icon) {
        ComPtr<IDWriteInlineObject> ell;
        dw->CreateEllipsisTrimmingSign(f.Get(), &ell);
        DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&t, ell.Get());
    }
    m_fmts[key] = f;
    return f.Get();
}

void Explorer::Txt(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, int align) {
    if (s.empty() || r.width < 2) return;
    auto* f = Fmt(size, weight, align, false);
    if (!f) return;
    m_brush->SetColor(DC(c));
    m_ctx->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, D(r), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Explorer::Glyph(wchar_t g, const Rect& r, float size, const Color& c) {
    auto* f = Fmt(size, 400, 1, true);
    if (!f) return;
    m_brush->SetColor(DC(c));
    m_ctx->DrawText(&g, 1, f, D(r), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Explorer::IconTile(const Rect& r, wchar_t glyph, float hover, float glyphSize) {
    const Color vl = render::glass::kVioletLight;
    FillRR(r, r.width * 0.28f, Color{0.02f, 0.01f, 0.06f, 0.45f});
    StrokeRR(r, r.width * 0.28f, WithA(vl, 0.30f + 0.2f * hover), 1.0f);
    Glyph(glyph, r, glyphSize, Mix(vl, kWhite, 0.45f));
}

void Explorer::ProgressBar(const Rect& r, float frac, bool) {
    const Color vl = render::glass::kVioletLight;
    FillRR(r, r.height * 0.5f, Color{1.0f, 1.0f, 1.0f, 0.085f});
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac <= 0.0f) return;
    const Rect f{r.x, r.y, std::max(r.height, r.width * frac), r.height};
    FillRR(Rect{f.x - 1, f.y - 2, f.width + 2, f.height + 4}, f.height, WithA(render::glass::kViolet, 0.16f));     // soft glow
    if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, Color{0.60f, 0.26f, 0.98f, 1.0f}}, {1.0f, Mix(vl, Color{0.86f, 0.55f, 1.0f, 1.0f}, 0.4f)}},
                                                    D2D1::Point2F(f.x, f.y), D2D1::Point2F(f.Right(), f.y)))
        m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(f), f.height * 0.5f, f.height * 0.5f), g.Get());
}

// ---- icons ------------------------------------------------------------------------------------------------------
static ComPtr<ID2D1Bitmap1> ShellImage(ID2D1DeviceContext* ctx, const std::wstring& path, int px) {
    ComPtr<ID2D1Bitmap1> out;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory) return out;
    HBITMAP hbm = nullptr;
    if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) || !hbm) return out;
    BITMAP bm{}; GetObjectW(hbm, sizeof(bm), &bm);
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        std::vector<uint32_t> px32(static_cast<size_t>(bm.bmWidth) * bm.bmHeight);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = bm.bmWidth; bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, hbm, 0, bm.bmHeight, px32.data(), &bi, DIB_RGB_COLORS)) {
            bool anyAlpha = false;
            for (uint32_t p : px32) if (p >> 24) { anyAlpha = true; break; }
            if (!anyAlpha) for (auto& p : px32) if (p & 0x00FFFFFF) p |= 0xFF000000;
            ctx->CreateBitmap(D2D1::SizeU(bm.bmWidth, bm.bmHeight), px32.data(), bm.bmWidth * 4,
                              D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &out);
        }
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(hbm);
    return out;
}

ID2D1Bitmap1* Explorer::FileIcon(const std::wstring& fullPath, const std::wstring& ext) {
    const bool perFile = (ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".url" || ext == L".scr" || ext == L".msi");
    const std::wstring key = perFile ? Lower(fullPath) : (ext.empty() ? L"." : ext);
    auto it = m_icons.find(key);
    if (it != m_icons.end()) return it->second.Get();
    if (m_iconBudget <= 0) { m_needRedrawForIcons = true; return nullptr; }
    --m_iconBudget;
    ComPtr<ID2D1Bitmap1> bmp;
    if (perFile) bmp = ShellImage(m_ctx, fullPath, 64);
    if (!bmp) {
        SHFILEINFOW sfi{};
        const std::wstring fake = L"file" + ext;
        if (SHGetFileInfoW(fake.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
            bmp = bui::IconToBitmap(m_ctx, sfi.hIcon, 32);
            DestroyIcon(sfi.hIcon);
        }
    }
    m_icons[key] = bmp;
    return bmp.Get();
}

// ---- vector icons (no Windows shell icons: everything is drawn in the Aether style) -------------------------------
enum IconKind { K_Desktop = 0, K_Downloads, K_Documents, K_Pictures, K_Music, K_Videos, K_Drive = 10, K_Windows, K_PC, K_Net, K_Linux, K_Folder };

void Explorer::UnitPoly(int id, std::initializer_list<D2D1_POINT_2F> pts, const Rect& r) {
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

// swirl of light: avatar + monitor artwork
void Explorer::Swirl(const Rect& r, float px, const Color& c) {
    if (!m_swirl) {
        ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(m_dcomp->GetD2DFactory()->CreatePathGeometry(&pg))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(pg->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(0.0f, 0.92f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(0.30f, 0.98f), D2D1::Point2F(0.42f, 0.60f), D2D1::Point2F(0.60f, 0.64f)));
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(0.80f, 0.68f), D2D1::Point2F(0.72f, 0.30f), D2D1::Point2F(1.0f, 0.12f)));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                m_swirl = pg;
            }
        }
    }
    if (!m_swirl) return;
    D2D1_MATRIX_3X2_F old; m_ctx->GetTransform(&old);
    m_ctx->SetTransform(D2D1::Matrix3x2F::Scale(r.width, r.height) * D2D1::Matrix3x2F::Translation(r.x, r.y) * old);
    m_brush->SetColor(DC(c));
    m_ctx->DrawGeometry(m_swirl.Get(), m_brush.Get(), px / std::max(1.0f, r.width));
    m_ctx->SetTransform(old);
}

void Explorer::Icon(int kind, const Rect& r) {
    const float w = r.width, h = r.height;
    auto rr = [&](const Rect& q, float rad, const Color& a, const Color& b) {
        if (auto g = m_dcomp->CreateLinearGradientBrush(a, b, D2D1::Point2F(q.x, q.y), D2D1::Point2F(q.x, q.Bottom())))
            m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(q), rad, rad), g.Get());
    };
    auto folder = [&](const Color& a, const Color& b) {
        rr(Rect{r.x, r.y + h * 0.10f, w * 0.46f, h * 0.28f}, h * 0.08f, b, b);
        rr(Rect{r.x, r.y + h * 0.24f, w, h * 0.66f}, h * 0.13f, a, b);
        FillRR(Rect{r.x + w * 0.08f, r.y + h * 0.26f, w * 0.84f, 1.2f}, 0.6f, Color{1.0f, 1.0f, 1.0f, 0.45f});
    };
    switch (kind) {
    case K_Desktop: folder(Color{0.42f, 0.76f, 1.0f, 1.0f}, Color{0.16f, 0.48f, 0.96f, 1.0f}); break;
    case K_Folder:  folder(Color{0.92f, 0.70f, 1.0f, 1.0f}, Color{0.62f, 0.34f, 0.95f, 1.0f}); break;
    case K_Downloads: Glyph(0xE896, r, h * 0.95f, Color{0.22f, 0.92f, 0.56f, 1.0f}); break;
    case K_Documents:
        rr(Rect{r.x + w * 0.16f, r.y + h * 0.04f, w * 0.68f, h * 0.92f}, w * 0.10f, Color{0.95f, 0.95f, 1.0f, 1.0f}, Color{0.62f, 0.62f, 0.80f, 1.0f});
        for (int i = 0; i < 3; ++i) FillRR(Rect{r.x + w * 0.30f, r.y + h * (0.40f + 0.15f * i), w * 0.40f, 1.6f}, 0.8f, Color{0.42f, 0.42f, 0.62f, 0.85f});
        break;
    case K_Pictures:
        rr(Rect{r.x, r.y + h * 0.06f, w, h * 0.88f}, h * 0.18f, Color{0.34f, 0.74f, 1.0f, 1.0f}, Color{0.12f, 0.42f, 0.96f, 1.0f});
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        m_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.30f, r.y + h * 0.34f), w * 0.08f, w * 0.08f), m_brush.Get());
        UnitPoly(101, {{0.0f, 1.0f}, {0.36f, 0.40f}, {0.58f, 0.72f}, {0.74f, 0.52f}, {1.0f, 1.0f}}, Rect{r.x + w * 0.05f, r.y + h * 0.36f, w * 0.90f, h * 0.52f});
        break;
    case K_Music: Glyph(0xE8D6, r, h * 0.95f, Color{1.0f, 0.40f, 0.74f, 1.0f}); break;
    case K_Videos:
        rr(Rect{r.x, r.y + h * 0.06f, w, h * 0.88f}, h * 0.22f, Color{0.74f, 0.48f, 1.0f, 1.0f}, Color{0.50f, 0.24f, 0.92f, 1.0f});
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.97f));
        UnitPoly(102, {{0.0f, 0.0f}, {1.0f, 0.5f}, {0.0f, 1.0f}}, Rect{r.x + w * 0.38f, r.y + h * 0.30f, w * 0.30f, h * 0.40f});
        break;
    case K_Drive:
        rr(Rect{r.x, r.y + h * 0.30f, w, h * 0.34f}, h * 0.10f, Color{0.90f, 0.91f, 0.95f, 1.0f}, Color{0.62f, 0.63f, 0.70f, 1.0f});
        rr(Rect{r.x, r.y + h * 0.54f, w, h * 0.22f}, h * 0.10f, Color{0.58f, 0.59f, 0.66f, 1.0f}, Color{0.36f, 0.37f, 0.44f, 1.0f});
        m_brush->SetColor(D2D1::ColorF(0.30f, 1.0f, 0.62f, 1.0f));
        m_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.16f, r.y + h * 0.65f), w * 0.035f, w * 0.035f), m_brush.Get());
        break;
    case K_Windows: {
        const float g = w * 0.07f, s = (w - g) * 0.5f;
        for (int i = 0; i < 4; ++i)
            rr(Rect{r.x + (i % 2) * (s + g), r.y + (i / 2) * (s + g), s, s}, w * 0.06f, Color{0.84f, 0.62f, 1.0f, 1.0f}, Color{0.56f, 0.30f, 0.95f, 1.0f});
        break;
    }
    case K_PC:
        rr(Rect{r.x + w * 0.04f, r.y + h * 0.10f, w * 0.92f, h * 0.60f}, h * 0.08f, Color{0.50f, 0.76f, 1.0f, 1.0f}, Color{0.16f, 0.46f, 0.95f, 1.0f});
        FillRR(Rect{r.x + w * 0.40f, r.y + h * 0.70f, w * 0.20f, h * 0.12f}, 1.0f, Color{0.45f, 0.62f, 0.95f, 1.0f});
        FillRR(Rect{r.x + w * 0.26f, r.y + h * 0.82f, w * 0.48f, h * 0.08f}, 2.0f, Color{0.55f, 0.72f, 1.0f, 1.0f});
        break;
    case K_Net: Glyph(0xE774, r, h * 0.92f, Color{0.40f, 0.72f, 1.0f, 1.0f}); break;
    case K_Linux:
        m_brush->SetColor(D2D1::ColorF(0.86f, 0.82f, 1.0f, 1.0f));
        m_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.5f, r.y + h * 0.60f), w * 0.30f, h * 0.36f), m_brush.Get());
        m_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.5f, r.y + h * 0.24f), w * 0.17f, h * 0.20f), m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(0.30f, 0.22f, 0.55f, 1.0f));
        m_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.5f, r.y + h * 0.66f), w * 0.17f, h * 0.24f), m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(1.0f, 0.78f, 0.25f, 1.0f));
        m_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.30f, r.y + h * 0.95f), w * 0.12f, h * 0.05f), m_brush.Get());
        m_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.70f, r.y + h * 0.95f), w * 0.12f, h * 0.05f), m_brush.Get());
        break;
    }
}

void Explorer::Badge(const std::wstring& label, const Rect& r, const Color& a, const Color& b, const Color& tc, bool border) {
    if (auto g = m_dcomp->CreateLinearGradientBrush(a, b, D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom())))
        m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), r.height * 0.22f, r.height * 0.22f), g.Get());
    if (border) StrokeRR(r, r.height * 0.22f, WithA(tc, 0.85f), 1.3f);
    const float sz = r.height * (label.size() <= 2 ? 0.44f : (label.size() == 3 ? 0.31f : 0.26f));
    Txt(label, r, sz, 700, tc, 1);
}

// small outlined chip / gpu / ram pictograms for the "Bu Bilgisayar" card
void Explorer::ChipIcon(int kind, const Rect& r, const Color& c) {
    const float w = r.width, h = r.height;
    if (kind == 0) {            // monitor
        StrokeRR(Rect{r.x + w * 0.06f, r.y + h * 0.12f, w * 0.88f, h * 0.58f}, 3.0f, c, 1.4f);
        FillRR(Rect{r.x + w * 0.40f, r.y + h * 0.70f, w * 0.20f, h * 0.14f}, 0.5f, c);
        FillRR(Rect{r.x + w * 0.26f, r.y + h * 0.84f, w * 0.48f, 1.5f}, 0.7f, c);
    } else if (kind == 1) {     // gpu
        StrokeRR(Rect{r.x + w * 0.04f, r.y + h * 0.18f, w * 0.92f, h * 0.60f}, 3.0f, c, 1.4f);
        m_brush->SetColor(DC(c));
        m_ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.32f, r.y + h * 0.48f), w * 0.12f, w * 0.12f), m_brush.Get(), 1.3f);
        m_ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + w * 0.68f, r.y + h * 0.48f), w * 0.12f, w * 0.12f), m_brush.Get(), 1.3f);
        FillRR(Rect{r.x + w * 0.14f, r.y + h * 0.78f, w * 0.72f, 1.6f}, 0.8f, c);
    } else {                    // ram
        StrokeRR(Rect{r.x + w * 0.04f, r.y + h * 0.22f, w * 0.92f, h * 0.46f}, 2.0f, c, 1.4f);
        for (int i = 0; i < 5; ++i) FillRR(Rect{r.x + w * (0.16f + 0.16f * i), r.y + h * 0.34f, 1.8f, h * 0.22f}, 0.9f, c);
        for (int i = 0; i < 6; ++i) FillRR(Rect{r.x + w * (0.12f + 0.15f * i), r.y + h * 0.70f, 2.2f, h * 0.12f}, 0.5f, c);
    }
}

void Explorer::DrawFileGlyph(const FileEntry& e, const std::wstring& fullPath, const Rect& r) {
    if (e.dir) { Icon(K_Folder, r); return; }
    const std::wstring& x = e.ext;
    auto in = [&](std::initializer_list<const wchar_t*> l) { for (auto* p : l) if (x == p) return true; return false; };
    if (in({L".png", L".jpg", L".jpeg", L".gif", L".webp", L".bmp", L".svg", L".ico", L".heic", L".tif", L".tiff"}) && x != L".ico") { Icon(K_Pictures, r); return; }
    if (in({L".mp4", L".mkv", L".avi", L".mov", L".webm", L".wmv", L".flv", L".m4v"})) { Icon(K_Videos, r); return; }
    if (in({L".mp3", L".wav", L".flac", L".ogg", L".m4a", L".aac", L".wma", L".opus"})) { Icon(K_Music, r); return; }
    if (in({L".txt", L".md", L".log", L".rtf", L".ini", L".cfg"})) { Icon(K_Documents, r); return; }
    if (in({L".zip", L".rar", L".7z", L".tar", L".gz", L".xz", L".bz2"})) {
        Badge(L"", r, Color{1.0f, 0.84f, 0.34f, 1.0f}, Color{0.96f, 0.60f, 0.12f, 1.0f}, kWhite, false);
        for (int i = 0; i < 4; ++i) FillRR(Rect{r.x + r.width * 0.5f - 2.0f + (i % 2) * 1.5f, r.y + r.height * (0.10f + 0.17f * i), 3.5f, r.height * 0.09f}, 0.8f, Color{0.45f, 0.26f, 0.05f, 0.85f});
        return;
    }
    if (x == L".psd") { Badge(L"Ps", r, Color{0.03f, 0.10f, 0.28f, 1.0f}, Color{0.05f, 0.17f, 0.40f, 1.0f}, Color{0.35f, 0.80f, 1.0f, 1.0f}, true); return; }
    if (in({L".sln", L".vcxproj", L".csproj"})) { Badge(L"VS", r, Color{0.70f, 0.46f, 1.0f, 1.0f}, Color{0.44f, 0.22f, 0.82f, 1.0f}, kWhite, false); return; }
    if (x == L".pdf") { Badge(L"PDF", r, Color{0.95f, 0.32f, 0.36f, 1.0f}, Color{0.72f, 0.12f, 0.22f, 1.0f}, kWhite, false); return; }
    if (in({L".doc", L".docx"})) { Badge(L"W", r, Color{0.30f, 0.56f, 1.0f, 1.0f}, Color{0.12f, 0.34f, 0.84f, 1.0f}, kWhite, false); return; }
    if (in({L".xls", L".xlsx", L".csv"})) { Badge(L"X", r, Color{0.26f, 0.78f, 0.46f, 1.0f}, Color{0.10f, 0.50f, 0.28f, 1.0f}, kWhite, false); return; }
    if (in({L".ppt", L".pptx"})) { Badge(L"P", r, Color{1.0f, 0.55f, 0.30f, 1.0f}, Color{0.84f, 0.30f, 0.12f, 1.0f}, kWhite, false); return; }
    if (in({L".exe", L".lnk", L".url", L".scr", L".msi"})) {          // the real application icon is the useful one here
        if (auto* bmp = FileIcon(fullPath, x)) { m_ctx->DrawBitmap(bmp, D(r), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); return; }
    }
    std::wstring lab = x.empty() ? L"•" : x.substr(1);
    for (auto& c : lab) c = static_cast<wchar_t>(towupper(c));
    if (lab.size() > 4) lab.resize(4);
    Badge(lab, r, Color{0.20f, 0.10f, 0.38f, 1.0f}, Color{0.12f, 0.06f, 0.26f, 1.0f}, render::glass::kVioletLight, true);
}

// ---- navigation ---------------------------------------------------------------------------------------------------
void Explorer::Navigate(const std::wstring& path, bool push) {
    std::wstring p = path;
    if (IsFtp(p)) { p = NormalizeFtp(p); if (!EnsureAuth(p)) return; }
    if (!p.empty() && p.size() == 2 && p[1] == L':') p += L'\\';
    if (push) {
        if (m_histPos < static_cast<int>(m_hist.size()) - 1) m_hist.resize(m_histPos + 1);
        if (m_hist.empty() || !IEquals(m_hist.back(), p)) m_hist.push_back(p);
        m_histPos = static_cast<int>(m_hist.size()) - 1;
    }
    StopWatch();
    m_xfer.clear();
    m_deep = false; m_search.clear();
    CancelEdit();
    m_path = p; m_home = p.empty();
    m_scroll = m_scrollTarget = 0;
    m_items.clear(); m_view.clear(); m_sel.clear(); m_anchor = m_cursor = -1;
    m_loadFailed = false; m_infoPath.clear(); m_pressRow = -1; m_marquee = false;
    TQ().Clear(); m_thumbPending.clear();
    if (m_home) StartHomeLoad(); else { StartFolderLoad(); StartWatch(); }
    SetWindowTextW(m_hwnd, m_home ? L"Bu Bilgisayar" : p.c_str());
    Kick();
}
void Explorer::GoBack() { if (m_histPos > 0) { --m_histPos; Navigate(m_hist[m_histPos], false); } }
void Explorer::GoForward() { if (m_histPos + 1 < static_cast<int>(m_hist.size())) { ++m_histPos; Navigate(m_hist[m_histPos], false); } }
void Explorer::Refresh() {
    const float keep = m_scrollTarget;
    if (m_home) StartHomeLoad(); else StartFolderLoad();
    m_scrollTarget = keep;
    Kick();
}

void Explorer::StartFolderLoad() {
    const uint32_t gen = ++m_gen;
    m_loading = true;
    const HWND hwnd = m_hwnd;
    const std::wstring path = m_path;
    std::thread([=]() {
        auto* r = new FolderResult();
        r->gen = gen; r->path = path;
        r->ok = EnumerateFolder(path, *r);
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, kMsgFolder, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void Explorer::StartHomeLoad() {
    const HWND hwnd = m_hwnd;
    std::vector<std::wstring> paths;
    for (const auto& q : m_quick) paths.push_back(q.path);
    std::thread([=]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* d = new HomeData();
        d->userName = UserDisplayName();
        d->drives = QueryDrives();
        for (const auto& p : paths) d->quickCounts.push_back(CountItems(p));
        d->recent = QueryRecent(6);
        d->cpu = CpuName();
        d->gpu = GpuName();
        MEMORYSTATUSEX ms = {sizeof(ms)};
        if (GlobalMemoryStatusEx(&ms)) {
            d->ramTotal = ms.ullTotalPhys;
            wchar_t b[96];
            swprintf(b, 96, L"%.0f GB RAM (%.1f GB Kullanılıyor)", std::ceil(ms.ullTotalPhys / 1073741824.0), (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0);
            d->ram = b;
        }
        CoUninitialize();
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, kMsgHome, 0, reinterpret_cast<LPARAM>(d))) delete d;
    }).detach();
}

void Explorer::OnHomeResult(HomeData* d) {
    m_homeData = std::move(*d);
    delete d;
    m_homeLoaded = true;
    for (size_t i = 0; i < m_quick.size() && i < m_homeData.quickCounts.size(); ++i) m_quick[i].count = m_homeData.quickCounts[i];
    Kick();
}

void Explorer::OnFolderResult(FolderResult* r) {
    std::unique_ptr<FolderResult> res(r);
    if (res->gen != m_gen.load() || m_home) return;      // stale
    m_loading = false;
    m_loadFailed = !res->ok;
    if (!res->ok && IsFtp(m_path)) { auto& P = Passwords(); std::lock_guard<std::mutex> lk(P.mu); P.pass.erase(FtpKey(ParseFtp(m_path))); }
    m_deep = res->deep;
    // m_view / m_sel still index the OLD list: read the selected names from it BEFORE the list is replaced (after a delete the
    // new list is shorter, and indexing it with the old indices ran past the end and crashed the window)
    std::set<std::wstring> keepNames;
    for (size_t i = 0; i < m_view.size() && i < m_sel.size(); ++i)
        if (m_sel[i] && m_view[i] >= 0 && m_view[i] < static_cast<int>(m_items.size())) keepNames.insert(m_items[m_view[i]].name);
    m_items = std::move(res->items);
    RebuildView(&keepNames);
    if (m_deep) m_scroll = m_scrollTarget = 0;
    if (!m_pendingSelect.empty()) {
        for (size_t i = 0; i < m_view.size(); ++i)
            if (IEquals(m_items[m_view[i]].name, m_pendingSelect)) {
                SelectOnly(static_cast<int>(i)); EnsureVisible(static_cast<int>(i));
                m_scroll = m_scrollTarget;
                if (m_pendingRename && m_edit == E_None) { m_renameIndex = static_cast<int>(i); BeginEdit(E_Rename, m_items[m_view[i]].name); }
                break;
            }
        m_pendingSelect.clear(); m_pendingRename = false;
    }
    Kick();
}

void Explorer::RebuildView(const std::set<std::wstring>* keepIn) {
    std::set<std::wstring> keep;
    if (keepIn) keep = *keepIn;
    else for (size_t i = 0; i < m_view.size() && i < m_sel.size(); ++i)
        if (m_sel[i] && m_view[i] >= 0 && m_view[i] < static_cast<int>(m_items.size())) keep.insert(m_items[m_view[i]].name);
    m_view.clear();
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].hidden && !m_showHidden) continue;
        if (!m_deep && !m_search.empty() && !IContains(m_items[i].name, m_search)) continue;
        m_view.push_back(static_cast<int>(i));
    }
    const SortKey key = m_sortKey; const bool asc = m_sortAsc;
    std::stable_sort(m_view.begin(), m_view.end(), [&](int a, int b) {
        const FileEntry& x = m_items[a]; const FileEntry& y = m_items[b];
        if (x.dir != y.dir) return x.dir;                  // folders always first
        int c = 0;
        switch (key) {
            case SortKey::Name: c = StrCmpLogicalW(x.name.c_str(), y.name.c_str()); break;
            case SortKey::Date: c = CompareFileTime(&x.mtime, &y.mtime); break;
            case SortKey::Type: c = StrCmpLogicalW(x.ext.c_str(), y.ext.c_str()); if (!c) c = StrCmpLogicalW(x.name.c_str(), y.name.c_str()); break;
            case SortKey::Size: c = x.size < y.size ? -1 : (x.size > y.size ? 1 : 0); break;
        }
        return asc ? c < 0 : c > 0;
    });
    m_sel.assign(m_view.size(), 0);
    m_anchor = m_cursor = -1;
    for (size_t i = 0; i < m_view.size(); ++i) if (keep.count(m_items[m_view[i]].name)) { m_sel[i] = 1; if (m_cursor < 0) m_anchor = m_cursor = static_cast<int>(i); }
}

void Explorer::SortBy(SortKey k) {
    if (m_sortKey == k) m_sortAsc = !m_sortAsc; else { m_sortKey = k; m_sortAsc = (k == SortKey::Name || k == SortKey::Type); }
    RebuildView(); SaveCfg(); Kick();
}

void Explorer::OpenPath(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { Navigate(path); return; }
    const size_t s = path.find_last_of(L'\\');
    const std::wstring dir = s == std::wstring::npos ? L"" : path.substr(0, s);
    ShellExecuteW(m_hwnd, L"open", path.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
}

void Explorer::OpenEntry(int vi) {
    if (vi < 0 || vi >= static_cast<int>(m_view.size())) return;
    const FileEntry& e = m_items[m_view[vi]];
    const std::wstring full = FullPath(e);
    if (e.dir) { Navigate(full); return; }
    if (IsFtp(full)) { OpenRemoteFile(full); return; }
    if (IsBrowsableArchive(e.ext)) { BrowseArchive(full); return; }
    OpenPath(full);
}

// ---- selection ----------------------------------------------------------------------------------------------------
void Explorer::SelectOnly(int vi) {
    std::fill(m_sel.begin(), m_sel.end(), 0);
    if (vi >= 0 && vi < static_cast<int>(m_sel.size())) m_sel[vi] = 1;
    m_anchor = m_cursor = vi;
}
void Explorer::ExtendSelection(int vi) {
    if (m_anchor < 0) m_anchor = vi;
    std::fill(m_sel.begin(), m_sel.end(), 0);
    for (int i = std::min(m_anchor, vi); i <= std::max(m_anchor, vi); ++i) if (i >= 0 && i < static_cast<int>(m_sel.size())) m_sel[i] = 1;
    m_cursor = vi;
}
void Explorer::EnsureVisible(int vi) {
    const int cols = std::max(1, m_cols);
    const float ch = m_grid ? m_cellH : kRowH;
    const float top = (vi / cols) * ch, bot = top + ch;
    if (top < m_scrollTarget) m_scrollTarget = top;
    else if (bot > m_scrollTarget + m_viewH) m_scrollTarget = bot - m_viewH;
    m_scrollTarget = std::clamp(m_scrollTarget, 0.0f, std::max(0.0f, m_contentH - m_viewH));
}
std::vector<std::wstring> Explorer::SelectedPaths() const {
    std::vector<std::wstring> v;
    for (size_t i = 0; i < m_view.size() && i < m_sel.size(); ++i) if (m_sel[i]) v.push_back(FullPath(m_items[m_view[i]]));
    return v;
}

// ---- commands -----------------------------------------------------------------------------------------------------
void Explorer::CopySelection(bool cut) {
    auto p = SelectedPaths();
    if (p.empty()) return;
    if (IsFtp(m_path)) { m_ftpClip = p; m_ftpClipCut = cut; m_ftpClipSeq = GetClipboardSequenceNumber(); m_clipPaths = std::move(p); m_clipCut = cut; Kick(); return; }
    m_ftpClip.clear();
    if (SetClipFiles(m_hwnd, p, cut)) { m_clipPaths = std::move(p); m_clipCut = cut; }
    Kick();
}
void Explorer::Paste() {
    if (m_home || m_deep || IsNetRoot(m_path) || IsUncServer(m_path)) return;
    if (!m_ftpClip.empty() && GetClipboardSequenceNumber() == m_ftpClipSeq) {
        StartTransfer(m_ftpClip, m_path, m_ftpClipCut);
        if (m_ftpClipCut) { m_ftpClip.clear(); m_clipPaths.clear(); }
        return;
    }
    m_ftpClip.clear();
    std::vector<std::wstring> files; bool cut = false;
    if (!GetClipFiles(m_hwnd, files, cut)) return;
    if (IsFtp(m_path)) {
        StartTransfer(files, m_path, cut);
        if (cut) { if (OpenClipboard(m_hwnd)) { EmptyClipboard(); CloseClipboard(); } m_clipPaths.clear(); }
        return;
    }
    RunFileOp(m_hwnd, cut ? Op::Move : Op::Copy, files, m_path, L"", L"");
    if (cut) { if (OpenClipboard(m_hwnd)) { EmptyClipboard(); CloseClipboard(); } m_clipPaths.clear(); }
}
void Explorer::DeleteSelection(bool perm) {
    auto p = SelectedPaths();
    if (m_home || p.empty() || IsNetRoot(m_path) || IsUncServer(m_path)) return;
    if (IsFtp(m_path)) {
        if (MessageBoxW(m_hwnd, L"Seçili öğeler sunucudan kalıcı olarak silinsin mi?", L"Aether Explorer", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        RemoteOp(m_path, [p](FtpClient& c) {
            bool ok = true;
            for (const auto& x : p) { std::wstring r = ParseFtp(x).path; while (r.size() > 1 && r.back() == L'/') r.pop_back(); ok &= FtpDelTree(c, r, nullptr); }
            return ok;
        }, L"");
        return;
    }
    RunFileOp(m_hwnd, perm ? Op::DeletePerm : Op::Delete, p, L"", L"", L"");
}
void Explorer::NewFolder() {
    if (m_home || m_deep || IsNetRoot(m_path) || IsUncServer(m_path)) return;
    if (IsFtp(m_path)) {
        std::set<std::wstring> have;
        for (const auto& it : m_items) have.insert(Lower(it.name));
        std::wstring n = L"Yeni Klasör";
        for (int i = 2; have.count(Lower(n)) && i < 1000; ++i) n = L"Yeni Klasör (" + std::to_wstring(i) + L")";
        m_pendingSelect = n; m_pendingRename = true;
        const std::wstring r = ParseFtp(m_path).path;
        RemoteOp(m_path, [r, n](FtpClient& c) { return c.Mkd(RJoin(r, n)); }, n);
        return;
    }
    const std::wstring name = UniqueName(m_path, L"Yeni Klasör", L"");
    if (CreateDirectoryW(JoinPath(m_path, name).c_str(), nullptr)) { m_pendingSelect = name; m_pendingRename = true; Refresh(); }
}
void Explorer::Compress(int kind) {
    auto p = SelectedPaths();
    if (m_home || p.empty() || IsVirtualPath(m_path)) return;
    const HWND hwnd = m_hwnd; const std::wstring dir = m_path;
    std::wstring stem = p.size() == 1 ? p[0].substr(p[0].find_last_of(L'\\') + 1) : L"Arşiv";
    if (p.size() == 1) { const size_t d = stem.find_last_of(L'.'); if (d != std::wstring::npos && d > 0) stem = stem.substr(0, d); }
    const std::wstring out = UniqueName(dir, stem, kind == 1 ? L".7z" : kind == 2 ? L".tar.gz" : L".zip");
    std::vector<std::wstring> names;
    for (const auto& f : p) names.push_back(f.substr(f.find_last_of(L'\\') + 1));
    std::thread([=]() {
        PostXfer(hwnd, L"Sıkıştırılıyor: " + out);
        const bool ok = CompressTo(kind, dir, names, JoinPath(dir, out));
        PostXfer(hwnd, ok ? L"" : (kind == 1 ? L"Sıkıştırılamadı (7-Zip gerekli)" : L"Sıkıştırılamadı"));
        PostOpDone(hwnd, ok ? out : L"");
    }).detach();
}
void Explorer::Upload() {
    if (m_home || IsNetRoot(m_path) || IsUncServer(m_path)) return;
    const HWND hwnd = m_hwnd; const std::wstring dir = m_path;
    std::thread([=]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<std::wstring> files;
        {
            ComPtr<IFileOpenDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
                DWORD opts = 0; dlg->GetOptions(&opts); dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
                dlg->SetTitle(L"Bu klasöre eklenecek dosyaları seç");
                if (SUCCEEDED(dlg->Show(hwnd))) {
                    ComPtr<IShellItemArray> arr;
                    if (SUCCEEDED(dlg->GetResults(&arr))) {
                        DWORD n = 0; arr->GetCount(&n);
                        for (DWORD i = 0; i < n; ++i) {
                            ComPtr<IShellItem> it; PWSTR p = nullptr;
                            if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { files.push_back(p); CoTaskMemFree(p); }
                        }
                    }
                }
            }
        }
        CoUninitialize();
        if (!files.empty() && IsWindow(hwnd)) {
            if (IsFtp(dir)) {
                const bool ok = TransferItems(files, dir, false, [hwnd](const std::wstring& t) { PostXfer(hwnd, t); });
                PostXfer(hwnd, ok ? L"" : L"Bazı dosyalar yüklenemedi");
                PostOpDone(hwnd, L"");
            } else RunFileOp(hwnd, Op::Copy, files, dir, L"", L"");
        }
    }).detach();
}
void Explorer::ShowProperties() {
    auto p = SelectedPaths();
    const std::wstring target = p.empty() ? m_path : p[0];
    if (target.empty()) return;
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_INVOKEIDLIST; sei.hwnd = m_hwnd; sei.lpVerb = L"properties"; sei.lpFile = target.c_str(); sei.nShow = SW_SHOW;
    ShellExecuteExW(&sei);
}

// ---- editing ------------------------------------------------------------------------------------------------------
float Explorer::EditW(float size) {
    ComPtr<IDWriteTextLayout> lay; float w = 0;
    if (!m_editText.empty() && SUCCEEDED(m_dcomp->GetDWriteFactory()->CreateTextLayout(m_editText.c_str(), static_cast<UINT32>(m_editText.size()), Fmt(size, 400, 0, false), 4000, 30, &lay)) && lay) {
        DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm); w = tm.widthIncludingTrailingWhitespace;
    }
    return w;
}

void Explorer::BeginEdit(int target, const std::wstring& text) {
    m_edit = target; m_editText = text; m_editAll = target != E_Search && !text.empty();
    SetTimer(m_hwnd, kTimerCaret, 500, nullptr);
    Kick();
}
void Explorer::CancelEdit() {
    if (m_edit == E_None) return;
    if (m_edit == E_Search) { m_edit = E_None; ClearSearch(); }
    m_edit = E_None; m_renameIndex = -1;
    KillTimer(m_hwnd, kTimerCaret);
    Kick();
}
void Explorer::CommitEdit() {
    const int mode = m_edit;
    m_edit = E_None;
    KillTimer(m_hwnd, kTimerCaret);
    if (mode == E_Path) {
        std::wstring p = m_editText;
        while (!p.empty() && p.back() == L' ') p.pop_back();
        while (!p.empty() && p.front() == L' ') p.erase(0, 1);
        if (IsFtp(p)) Navigate(p);
        else if (Lower(p) == L"net:" || Lower(p) == L"ağ") Navigate(L"net:");
        else {
            while (p.size() > 3 && p.back() == L'\\') p.pop_back();
            wchar_t exp[MAX_PATH * 2];
            if (ExpandEnvironmentStringsW(p.c_str(), exp, MAX_PATH * 2)) p = exp;
            if (p.empty() || IEquals(p, L"Bu Bilgisayar")) Navigate(L"");
            else if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) OpenPath(p);
            else if (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\') Navigate(p);                       // \\server or \\server\share
            else if (p.find(L'\\') == std::wstring::npos && p.find(L'.') != std::wstring::npos && p.find(L':') == std::wstring::npos)
                Navigate(L"ftp://" + p);                                                                // bare host name
        }
    } else if (mode == E_Rename && m_renameIndex >= 0 && m_renameIndex < static_cast<int>(m_view.size())) {
        const FileEntry& e = m_items[m_view[m_renameIndex]];
        std::wstring n = m_editText;
        if (!n.empty() && n != e.name && n.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos) {
            if (IsFtp(m_path)) {
                std::wstring ro = ParseFtp(FullPath(e)).path;
                const size_t sl = ro.find_last_of(L'/');
                const std::wstring rn = (sl == std::wstring::npos ? L"/" : ro.substr(0, sl + 1)) + n;
                RemoteOp(m_path, [ro, rn](FtpClient& c) { return c.Ren(ro, rn); }, n);
            } else if (!IsVirtualPath(m_path)) RunFileOp(m_hwnd, Op::Rename, {FullPath(e)}, L"", n, n);
        }
    } else if (mode == E_Search) {
        if (!m_home && !m_search.empty() && !IsVirtualPath(m_path)) StartDeepSearch();
    }
    m_renameIndex = -1;
    Kick();
}

void Explorer::OnChar(wchar_t c) {
    if (m_edit != E_None) {
        if (m_editAll && (c == 8 || (c >= 32 && c != 127))) { m_editText.clear(); m_editAll = false; if (c == 8) { if (m_edit == E_Search) { m_search.clear(); RebuildView(); } Kick(); return; } }
        if (c == 8) { if (!m_editText.empty()) m_editText.pop_back(); }
        else if (c == 13) { CommitEdit(); return; }
        else if (c == 27) { CancelEdit(); return; }
        else if (c >= 32 && c != 127) m_editText += c;
        if (m_edit == E_Search) { m_search = m_editText; RebuildView(); m_scroll = m_scrollTarget = 0; }
        Kick();
        return;
    }
    // type-ahead in the file list
    if (!m_home && c >= 32 && c != 127 && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        const ULONGLONG now = GetTickCount64();
        if (now - m_typeTick > 900) m_typeBuf.clear();
        m_typeTick = now; m_typeBuf += c;
        for (size_t i = 0; i < m_view.size(); ++i) {
            if (_wcsnicmp(m_items[m_view[i]].name.c_str(), m_typeBuf.c_str(), m_typeBuf.size()) == 0) {
                SelectOnly(static_cast<int>(i)); EnsureVisible(static_cast<int>(i)); break;
            }
        }
        Kick();
    }
}

void Explorer::OnKey(WPARAM vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (ctrl && !alt && vk == 'A' && !m_about) {                          // Ctrl+A: select all — in an edit box the text, otherwise every entry
        if (m_edit != E_None) { m_editAll = !m_editText.empty(); Kick(); return; }
        if (!m_home) { std::fill(m_sel.begin(), m_sel.end(), 1); if (m_cursor < 0 && !m_view.empty()) m_cursor = 0; Kick(); }
        return;
    }
    if (vk == VK_F1) { m_about = !m_about; Kick(); return; }
    if (m_about) { if (vk == VK_ESCAPE || vk == VK_RETURN) m_about = false; Kick(); return; }
    if (m_edit != E_None) {
        if (vk == VK_ESCAPE) CancelEdit();
        else if (vk == VK_DELETE && m_editAll) { m_editText.clear(); m_editAll = false; if (m_edit == E_Search) { m_search.clear(); RebuildView(); } Kick(); }
        else if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME || vk == VK_END) { m_editAll = false; Kick(); }
        return;      // text goes through WM_CHAR
    }
    if (alt && vk == VK_LEFT) { GoBack(); return; }
    if (alt && vk == VK_RIGHT) { GoForward(); return; }
    if (alt && vk == VK_UP) { Navigate(ParentOf(m_path)); return; }
    if (vk == VK_F5) { Refresh(); return; }
    if (ctrl && vk == 'T') { NewTab(m_path); return; }
    if (ctrl && vk == 'W') { CloseTab(m_tab); return; }
    if (ctrl && vk == VK_TAB) { if (m_tabs.size() > 1) SwitchTab(shift ? (m_tab + static_cast<int>(m_tabs.size()) - 1) % static_cast<int>(m_tabs.size()) : (m_tab + 1) % static_cast<int>(m_tabs.size())); return; }
    if (alt && vk == 'P') { m_preview = !m_preview; SaveCfg(); Kick(); return; }
    if (ctrl && vk == '1') { m_grid = false; SaveCfg(); m_scrollTarget = 0; Kick(); return; }
    if (ctrl && vk == '2') { m_grid = true; SaveCfg(); m_scrollTarget = 0; Kick(); return; }
    if (ctrl && shift && vk == 'C') {
        auto p = SelectedPaths(); std::wstring t;
        if (p.empty() && !m_home) p.push_back(m_path);
        for (size_t i = 0; i < p.size(); ++i) { if (i) t += L"\r\n"; t += p[i]; }
        if (!t.empty()) SetClipText(m_hwnd, t);
        return;
    }
    if (ctrl && vk == 'L') { BeginEdit(E_Path, m_path); return; }
    if ((ctrl && vk == 'F') || vk == VK_F3) { BeginEdit(E_Search, m_search); return; }
    if (ctrl && vk == 'H') { m_showHidden = !m_showHidden; RebuildView(); Kick(); return; }
    if (m_home) return;
    const int n = static_cast<int>(m_view.size());
    auto move = [&](int to) {
        if (!n) return;
        to = std::clamp(to, 0, n - 1);
        if (shift) ExtendSelection(to); else SelectOnly(to);
        EnsureVisible(to); Kick();
    };
    if (alt && vk == VK_RETURN) { if (m_cursor >= 0 && m_cursor < n) { auto p = SelectedPaths(); if (!p.empty()) { SHELLEXECUTEINFOW si = {sizeof(si)}; si.fMask = SEE_MASK_INVOKEIDLIST; si.hwnd = m_hwnd; si.lpVerb = L"properties"; si.lpFile = p[0].c_str(); si.nShow = SW_SHOW; ShellExecuteExW(&si); } } return; }
    if (vk == VK_APPS || (shift && vk == VK_F10)) {
        POINT pt; GetCursorPos(&pt);
        auto p = SelectedPaths();
        if (p.empty() && !m_home) p.push_back(m_path);
        if (!p.empty()) ShellMenu(p, pt);
        return;
    }
    const int cols = m_grid ? std::max(1, m_cols) : 1;
    const int page = std::max(1, static_cast<int>(m_viewH / (m_grid ? m_cellH : kRowH)) - 1) * cols;
    switch (vk) {
        case VK_DOWN: move(m_cursor < 0 ? 0 : m_cursor + cols); break;
        case VK_UP: move(m_cursor < 0 ? 0 : m_cursor - cols); break;
        case VK_LEFT: if (m_grid) move(m_cursor < 0 ? 0 : m_cursor - 1); break;
        case VK_RIGHT: if (m_grid) move(m_cursor < 0 ? 0 : m_cursor + 1); break;
        case VK_NEXT: move(m_cursor < 0 ? 0 : m_cursor + page); break;
        case VK_PRIOR: move(m_cursor < 0 ? 0 : m_cursor - page); break;
        case VK_HOME: move(0); break;
        case VK_END: move(n - 1); break;
        case VK_RETURN: if (m_cursor >= 0) OpenEntry(m_cursor); break;
        case VK_BACK: Navigate(ParentOf(m_path)); break;
        case VK_DELETE: DeleteSelection(shift); break;
        case VK_ESCAPE: if (!m_search.empty()) { m_search.clear(); RebuildView(); Kick(); } else { std::fill(m_sel.begin(), m_sel.end(), 0); Kick(); } break;
        case VK_F2:
            if (m_cursor >= 0 && m_cursor < n) { m_renameIndex = m_cursor; BeginEdit(E_Rename, m_items[m_view[m_cursor]].name); }
            break;
        case 'A': if (ctrl) { std::fill(m_sel.begin(), m_sel.end(), 1); Kick(); } break;
        case 'C': if (ctrl) CopySelection(false); break;
        case 'X': if (ctrl) CopySelection(true); break;
        case 'V': if (ctrl) Paste(); break;
        case 'N': if (ctrl && shift) NewFolder(); break;
    }
}

// ---- clicks / menus -----------------------------------------------------------------------------------------------
void Explorer::Click(int id, bool ctrl, bool shift) {
    if (id == 0) { if (!m_home && m_edit == E_None) { std::fill(m_sel.begin(), m_sel.end(), 0); } return; }
    if (id >= H_Row) {
        const int vi = id - H_Row;
        if (m_edit == E_Rename) CommitEdit();
        if (shift) ExtendSelection(vi);
        else if (ctrl) { if (vi < static_cast<int>(m_sel.size())) m_sel[vi] = !m_sel[vi]; m_anchor = m_cursor = vi; }
        else SelectOnly(vi);
        return;
    }
    if (id == H_About) { m_about = !m_about; return; }
    if (id == H_AboutClose || id == H_AboutBlock) { m_about = false; return; }
    if (id == H_AboutGitHub) { ShellExecuteW(m_hwnd, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL); return; }
    if (id == H_AboutDonate) { ShellExecuteW(m_hwnd, L"open", kDonateUrl, nullptr, nullptr, SW_SHOWNORMAL); return; }
    if (id == H_AboutCard) return;
    if (id >= H_SideConn && id < H_SideConn + 100) { const size_t i = id - H_SideConn; if (i < m_conns.size()) ConnectTo(static_cast<int>(i)); return; }
    if (id == H_SideConnAdd) { POINT pt; GetCursorPos(&pt); AddMenu(pt); return; }
    if (id >= H_Tab && id < H_Tab + 100) { SwitchTab(id - H_Tab); return; }
    if (id >= H_TabClose && id < H_TabClose + 100) { CloseTab(id - H_TabClose); return; }
    if (id == H_TabNew) { NewTab(L""); return; }
    if (id >= H_CrumbSep && id < H_CrumbSep + 100) {
        // open below the separator
        POINT pt; GetCursorPos(&pt);
        BreadcrumbMenu(id - H_CrumbSep, pt);
        return;
    }
    if (id >= H_SidePin && id < H_SidePin + 100) { const size_t i = id - H_SidePin; if (i < m_pins.size()) Navigate(m_pins[i]); return; }
    if (id >= H_ColHead && id < H_ColHead + 10) { SortBy(static_cast<SortKey>(id - H_ColHead)); return; }
    if (id >= H_Tool && id < H_Tool + 10) {
        static const wchar_t* cmds[] = {L"control.exe", L"taskmgr.exe", L"ms-settings:", L"msinfo32.exe"};
        ShellExecuteW(m_hwnd, L"open", cmds[id - H_Tool], nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id >= H_RecentRow && id < H_RecentRow + 100) { const size_t i = id - H_RecentRow; if (i < m_homeData.recent.size()) OpenPath(m_homeData.recent[i].path); return; }
    if (id >= H_DriveCard && id < H_DriveCard + 100) { const size_t i = id - H_DriveCard; if (i < m_homeData.drives.size()) Navigate(m_homeData.drives[i].root); return; }
    if (id >= H_QuickTile && id < H_QuickTile + 100) { const size_t i = id - H_QuickTile; if (i < m_quick.size()) Navigate(m_quick[i].path); return; }
    if (id >= H_SideFolder && id < H_SideFolder + 100) { const size_t i = id - H_SideFolder; if (i < m_quick.size()) Navigate(m_quick[i].path); return; }
    if (id >= H_SideDrive && id < H_SideDrive + 100) { const size_t i = id - H_SideDrive; if (i < m_homeData.drives.size()) Navigate(m_homeData.drives[i].root); return; }
    if (id >= H_Banner && id < H_Banner + 10) {
        switch (id - H_Banner) {
            case 0: if (m_home) Navigate(m_quick.size() > 2 ? m_quick[2].path : L""); NewFolder(); break;
            case 1: if (m_home && m_quick.size() > 1) Navigate(m_quick[1].path); Upload(); break;
            case 2: Compress(); break;
            case 3: ShellExecuteW(m_hwnd, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL); break;
        }
        return;
    }
    if (id >= H_Crumb && id < H_Crumb + 100) {
        // rebuild crumb path from the current path
        std::vector<std::wstring> paths;
        std::wstring p = m_path;
        while (!p.empty()) { paths.insert(paths.begin(), p); p = ParentOf(p); }
        const int i = id - H_Crumb;
        if (i == 0) Navigate(L""); else if (i - 1 < static_cast<int>(paths.size())) Navigate(paths[i - 1]);
        return;
    }
    switch (id) {
        case H_Min: ShowWindow(m_hwnd, SW_MINIMIZE); break;
        case H_Max: ShowWindow(m_hwnd, IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case H_Close: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
        case H_Back: GoBack(); break;
        case H_Fwd: GoForward(); break;
        case H_Up: Navigate(ParentOf(m_path)); break;
        case H_Refresh: Refresh(); break;
        case H_Search: if (m_edit != E_Search) BeginEdit(E_Search, m_search); break;
        case H_PathBox: if (m_edit != E_Path) BeginEdit(E_Path, m_path); break;
        case H_NavHome: case H_SideThisPc: Navigate(L""); break;
        case H_NavQuick: Navigate(L""); m_scrollTarget = std::max(0.0f, m_quickSectionY); break;
        case H_SideNet: Navigate(L"net:"); break;
        case H_SideLinux: if (GetFileAttributesW(L"\\\\wsl$") != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(L"\\\\wsl.localhost") != INVALID_FILE_ATTRIBUTES) Navigate(L"\\\\wsl.localhost"); break;
        case H_SideSettings: ShellExecuteW(m_hwnd, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL); break;
        case H_SideUser: ShellExecuteW(m_hwnd, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL); break;
        case H_SidePower: {
            POINT pt; GetCursorPos(&pt);
            const int c = aether::ui::ShowContextMenu({aether::ui::MenuItem(1, L"Ekranı kilitle", bui::kGlyphLock), aether::ui::MenuItem(2, L"Oturumu kapat", bui::kGlyphSignOut),
                                               aether::ui::MenuItem(3, L"Yeniden başlat", bui::kGlyphRestart), aether::ui::MenuItem(4, L"Bilgisayarı kapat", bui::kGlyphPower, true)}, pt, true);
            if (c == 1) LockWorkStation();
            else if (c == 2) ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/l", nullptr, SW_HIDE);
            else if (c == 3) ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/r /t 0", nullptr, SW_HIDE);
            else if (c == 4) ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/s /t 0", nullptr, SW_HIDE);
            break;
        }
        case H_RecentAll: {
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &p)) && p) { Navigate(p); CoTaskMemFree(p); }
            break;
        }
        case H_CmdDetails: m_grid = false; SaveCfg(); m_scrollTarget = 0; break;
        case H_CmdGrid: { POINT pt; GetCursorPos(&pt); ViewMenu(pt); break; }
        case H_CmdPreview: m_preview = !m_preview; SaveCfg(); break;
        case H_CmdNew: NewFolder(); break;
        case H_CmdCut: CopySelection(true); break;
        case H_CmdCopy: CopySelection(false); break;
        case H_CmdPaste: Paste(); break;
        case H_CmdRename: if (m_cursor >= 0 && m_cursor < static_cast<int>(m_view.size())) { m_renameIndex = m_cursor; BeginEdit(E_Rename, m_items[m_view[m_cursor]].name); } break;
        case H_CmdDelete: DeleteSelection(); break;
        case H_CmdSort: {
            POINT pt; GetCursorPos(&pt);
            const int c = aether::ui::ShowContextMenu({aether::ui::MenuItem(1, L"Ada göre", 0, false, m_sortKey == SortKey::Name ? L"●" : L""), aether::ui::MenuItem(2, L"Tarihe göre", 0, false, m_sortKey == SortKey::Date ? L"●" : L""),
                                               aether::ui::MenuItem(3, L"Türe göre", 0, false, m_sortKey == SortKey::Type ? L"●" : L""), aether::ui::MenuItem(4, L"Boyuta göre", 0, false, m_sortKey == SortKey::Size ? L"●" : L""),
                                               aether::ui::MenuSeparator(), aether::ui::MenuItem(5, m_showHidden ? L"Gizli öğeleri gizle" : L"Gizli öğeleri göster", 0, false, L"Ctrl+H")}, pt);
            if (c >= 1 && c <= 4) { m_sortKey = static_cast<SortKey>(c - 1); m_sortAsc = (c == 1 || c == 3); RebuildView(); }
            else if (c == 5) { m_showHidden = !m_showHidden; RebuildView(); }
            break;
        }
    }
}

void Explorer::DoubleClick(int id) {
    if (id >= H_Row) OpenEntry(id - H_Row);
}

void Explorer::ContextMenu(int id, POINT pt) {
    using aether::ui::MenuItem; using aether::ui::MenuSeparator;
    std::vector<aether::ui::MenuEntry> items;

    // ---- tabs
    if (id >= H_Tab && id < H_Tab + 100) {
        const int ti = id - H_Tab;
        const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Sekmeyi kapat", 0xE711), MenuItem(2, L"Diğer sekmeleri kapat", 0xE8BB), MenuItem(3, L"Sekmeyi çoğalt", 0xE8C8)}, pt);
        if (c == 1) CloseTab(ti);
        else if (c == 2) { SwitchTab(ti); while (m_tabs.size() > 1) CloseTab(m_tab == 0 ? 1 : 0); }
        else if (c == 3 && ti < static_cast<int>(m_tabs.size())) { SwitchTab(ti); NewTab(m_path); }
        return;
    }

    // ---- saved connections / network entry of the sidebar
    if (id >= H_SideConn && id < H_SideConn + 100) {
        const int ci = id - H_SideConn;
        if (ci >= static_cast<int>(m_conns.size())) return;
        const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Bağlan", bui::kGlyphEnter), MenuItem(2, L"Yeni sekmede bağlan", 0xE710), MenuSeparator(),
                                                   MenuItem(3, L"Düzenle", bui::kGlyphRename), MenuItem(4, L"Bağlantıyı sil", bui::kGlyphDelete, true)}, pt);
        if (c == 1) ConnectTo(ci);
        else if (c == 2) { SetFtpPass(ParseFtp(m_conns[ci].Url()), m_conns[ci].pass); NewTab(m_conns[ci].Url()); }
        else if (c == 3) { Conn cc = m_conns[ci]; if (EditConn(cc)) { m_conns[ci] = cc; SaveCfg(); ConnectTo(ci); } }
        else if (c == 4) { m_conns.erase(m_conns.begin() + ci); SaveCfg(); Kick(); }
        return;
    }
    if (id == H_SideNet || id == H_SideConnAdd) { AddMenu(pt); return; }

    // ---- sidebar folders, pins, drives, quick tiles
    std::wstring target; bool isDrive = false, isPin = false;
    auto inR = [&](int base) { return id >= base && id < base + 100; };
    if (inR(H_SideFolder) && static_cast<size_t>(id - H_SideFolder) < m_quick.size()) target = m_quick[id - H_SideFolder].path;
    else if (inR(H_QuickTile) && static_cast<size_t>(id - H_QuickTile) < m_quick.size()) target = m_quick[id - H_QuickTile].path;
    else if (inR(H_SidePin) && static_cast<size_t>(id - H_SidePin) < m_pins.size()) { target = m_pins[id - H_SidePin]; isPin = true; }
    else if (inR(H_SideDrive) && static_cast<size_t>(id - H_SideDrive) < m_homeData.drives.size()) { target = m_homeData.drives[id - H_SideDrive].root; isDrive = true; }
    else if (inR(H_DriveCard) && static_cast<size_t>(id - H_DriveCard) < m_homeData.drives.size()) { target = m_homeData.drives[id - H_DriveCard].root; isDrive = true; }
    if (!target.empty()) {
        items = {MenuItem(1, L"Aç", bui::kGlyphEnter), MenuItem(2, L"Yeni sekmede aç", 0xE710), MenuItem(3, L"Terminalde aç", 0xE756), MenuSeparator(), MenuItem(4, L"Yolu kopyala", bui::kGlyphCopy)};
        if (!isDrive) items.push_back(MenuItem(5, IsPinned(target) ? L"Sabitlemeyi kaldır" : L"Hızlı erişime sabitle", bui::kGlyphPin));
        if (isDrive) items.push_back(MenuItem(6, L"Disk Temizleme", bui::kGlyphDelete));
        items.push_back(MenuSeparator());
        items.push_back(MenuItem(7, L"Özellikler", bui::kGlyphInfo));
        (void)isPin;
        const int c = aether::ui::ShowContextMenu(items, pt);
        if (c == 1) Navigate(target);
        else if (c == 2) NewTab(target);
        else if (c == 3) OpenTerminal(target);
        else if (c == 4) SetClipText(m_hwnd, target);
        else if (c == 5) TogglePin(target);
        else if (c == 6) { const std::wstring a = L"/d " + target.substr(0, 1); ShellExecuteW(m_hwnd, L"open", L"cleanmgr.exe", a.c_str(), nullptr, SW_SHOWNORMAL); }
        else if (c == 7) { SHELLEXECUTEINFOW sei = {sizeof(sei)}; sei.fMask = SEE_MASK_INVOKEIDLIST; sei.hwnd = m_hwnd; sei.lpVerb = L"properties"; sei.lpFile = target.c_str(); sei.nShow = SW_SHOW; ShellExecuteExW(&sei); }
        return;
    }

    // ---- recent files on the home page
    if (inR(H_RecentRow) && static_cast<size_t>(id - H_RecentRow) < m_homeData.recent.size()) {
        const auto& r = m_homeData.recent[id - H_RecentRow];
        const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Aç", bui::kGlyphEnter), MenuItem(2, L"Dosya konumunu aç", bui::kGlyphFolder), MenuItem(3, L"Yolu kopyala", bui::kGlyphCopy)}, pt);
        if (c == 1) OpenPath(r.path);
        else if (c == 2) { m_pendingSelect = r.name; Navigate(r.dir); }
        else if (c == 3) SetClipText(m_hwnd, r.path);
        return;
    }
    if (m_home) return;

    // ---- items of the file list
    if (id >= H_Row) {
        const int vi = id - H_Row;
        if (vi < static_cast<int>(m_sel.size()) && !m_sel[vi]) SelectOnly(vi);
        m_cursor = vi;
        Kick();
        const FileEntry& e = m_items[m_view[vi]];
        const bool multi = std::count(m_sel.begin(), m_sel.end(), 1) > 1;
        const std::wstring full = FullPath(e);
        if (IsVirtualPath(m_path)) {
            const bool ftp = IsFtp(m_path);
            items.push_back(MenuItem(1, L"Aç", bui::kGlyphEnter, false, L"Enter"));
            if (e.dir && !multi) items.push_back(MenuItem(2, L"Yeni sekmede aç", 0xE710));
            if (ftp) {
                items.push_back(MenuSeparator());
                items.push_back(MenuItem(30, L"İndir...", 0xE896));
                items.push_back(MenuItem(3, L"Kes", bui::kGlyphCut, false, L"Ctrl+X"));
                items.push_back(MenuItem(4, L"Kopyala", bui::kGlyphCopy, false, L"Ctrl+C"));
                if (!multi) items.push_back(MenuItem(5, L"Yeniden adlandır", bui::kGlyphRename, false, L"F2"));
            }
            items.push_back(MenuSeparator());
            items.push_back(MenuItem(7, L"Yolu kopyala", bui::kGlyphCopy));
            if (ftp) { items.push_back(MenuSeparator()); items.push_back(MenuItem(10, L"Sil", bui::kGlyphDelete, true, L"Del")); }
            const int c = aether::ui::ShowContextMenu(items, pt);
            switch (c) {
                case 1: OpenEntry(vi); break;
                case 2: NewTab(full); break;
                case 3: CopySelection(true); break;
                case 4: CopySelection(false); break;
                case 5: m_renameIndex = vi; BeginEdit(E_Rename, e.name); break;
                case 7: { auto p = SelectedPaths(); std::wstring t; for (size_t i = 0; i < p.size(); ++i) t += (i ? L"\r\n" : L"") + p[i]; SetClipText(m_hwnd, t); break; }
                case 10: DeleteSelection(false); break;
                case 30: DownloadSelection(); break;
            }
            return;
        }
        items.push_back(MenuItem(1, L"Aç", bui::kGlyphEnter, false, L"Enter"));
        if (e.dir && !multi) items.push_back(MenuItem(2, L"Yeni sekmede aç", 0xE710));
        if (!e.dir && (e.ext == L".exe" || e.ext == L".bat" || e.ext == L".cmd" || e.ext == L".msi") && !multi) items.push_back(MenuItem(16, L"Yönetici olarak çalıştır", 0xE7EF));
        if (m_deep && !multi) items.push_back(MenuItem(15, L"Konumunu aç", bui::kGlyphFolder));
        items.push_back(MenuSeparator());
        items.push_back(MenuItem(3, L"Kes", bui::kGlyphCut, false, L"Ctrl+X"));
        items.push_back(MenuItem(4, L"Kopyala", bui::kGlyphCopy, false, L"Ctrl+C"));
        if (!multi) items.push_back(MenuItem(5, L"Yeniden adlandır", bui::kGlyphRename, false, L"F2"));
        if (!multi && !e.dir && IsArchiveExt(e.ext)) items.push_back(MenuItem(20, L"Arşiv işlemleri", 0xE8B7));
        items.push_back(MenuItem(6, L"Sıkıştır", 0xF012));
        items.push_back(MenuSeparator());
        items.push_back(MenuItem(7, L"Yolu kopyala", bui::kGlyphCopy, false, L"Ctrl+Shift+C"));
        if (e.dir && !multi) { items.push_back(MenuItem(8, L"Terminalde aç", 0xE756)); items.push_back(MenuItem(9, IsPinned(full) ? L"Sabitlemeyi kaldır" : L"Hızlı erişime sabitle", bui::kGlyphPin)); }
        items.push_back(MenuSeparator());
        items.push_back(MenuItem(10, L"Sil", bui::kGlyphDelete, true, L"Del"));
        items.push_back(MenuItem(11, L"Kalıcı olarak sil", bui::kGlyphDelete, true, L"Shift+Del"));
        items.push_back(MenuSeparator());
        items.push_back(MenuItem(12, L"Özellikler", bui::kGlyphInfo, false, L"Alt+Enter"));
        items.push_back(MenuItem(13, L"Diğer seçenekler (Windows menüsü)", 0xE712, false, L"Shift+F10"));
        const int c = aether::ui::ShowContextMenu(items, pt);
        switch (c) {
            case 1: OpenEntry(vi); break;
            case 2: NewTab(full); break;
            case 3: CopySelection(true); break;
            case 4: CopySelection(false); break;
            case 5: m_renameIndex = vi; BeginEdit(E_Rename, e.name); break;
            case 6: {
                std::vector<aether::ui::MenuEntry> ci{MenuItem(1, L"ZIP olarak sıkıştır", 0xF012), MenuItem(3, L"TAR.GZ olarak sıkıştır", 0xF012)};
                if (!Find7z().empty()) ci.insert(ci.begin() + 1, MenuItem(2, L"7z olarak sıkıştır", 0xF012));
                const int k = aether::ui::ShowContextMenu(ci, pt);
                if (k == 1) Compress(0); else if (k == 2) Compress(1); else if (k == 3) Compress(2);
                break;
            }
            case 20: {
                std::wstring stem = e.name; const size_t d = stem.find_last_of(L'.'); if (d != std::wstring::npos && d > 0) stem = stem.substr(0, d);
                const int k = aether::ui::ShowContextMenu({MenuItem(1, L"İçeriğine göz at", bui::kGlyphFolder), MenuItem(2, L"Buraya çıkar", 0xE8DE),
                                                           MenuItem(3, L"\"" + stem + L"\\\" klasörüne çıkar", 0xE8DE)}, pt);
                if (k == 1) BrowseArchive(full); else if (k == 2) ExtractArchiveCmd(full, false); else if (k == 3) ExtractArchiveCmd(full, true);
                break;
            }
            case 7: { auto p = SelectedPaths(); std::wstring t; for (size_t i = 0; i < p.size(); ++i) t += (i ? L"\r\n" : L"") + p[i]; SetClipText(m_hwnd, t); break; }
            case 8: OpenTerminal(full); break;
            case 9: TogglePin(full); break;
            case 10: DeleteSelection(false); break;
            case 11: DeleteSelection(true); break;
            case 12: ShowProperties(); break;
            case 13: ShellMenu(SelectedPaths(), pt); break;
            case 15: m_pendingSelect = e.name; Navigate(e.parent); break;
            case 16: ShellExecuteW(m_hwnd, L"runas", full.c_str(), nullptr, e.parent.empty() ? m_path.c_str() : e.parent.c_str(), SW_SHOWNORMAL); break;
        }
        return;
    }

    // ---- empty space of the folder
    if (m_deep) return;
    if (IsFtp(m_path)) {
        items = {MenuItem(10, L"Yeni klasör", bui::kGlyphNewFolder, false, L"Ctrl+Shift+N"), MenuItem(11, L"Yapıştır", bui::kGlyphPaste, false, L"Ctrl+V"),
                 MenuItem(12, L"Buraya dosya yükle...", 0xE898), MenuSeparator(), MenuItem(13, L"Görünüm", bui::kGlyphView), MenuItem(14, L"Sırala", bui::kGlyphSort),
                 MenuSeparator(), MenuItem(16, L"Yenile", bui::kGlyphRefresh, false, L"F5"), MenuItem(18, L"Yolu kopyala", bui::kGlyphCopy), MenuItem(19, L"Bağlantıyı kaydet", bui::kGlyphPin)};
        items[1].enabled = (!m_ftpClip.empty() && GetClipboardSequenceNumber() == m_ftpClipSeq) || IsClipboardFormatAvailable(CF_HDROP) != 0;
        const int c = aether::ui::ShowContextMenu(items, pt);
        switch (c) {
            case 10: NewFolder(); break;
            case 11: Paste(); break;
            case 12: Upload(); break;
            case 13: ViewMenu(pt); break;
            case 14: Click(H_CmdSort, false, false); break;
            case 16: Refresh(); break;
            case 18: SetClipText(m_hwnd, m_path); break;
            case 19: {
                const FtpUrl u = ParseFtp(m_path);
                Conn cc; cc.name = u.host; cc.host = u.host; cc.port = u.port; cc.user = u.user; cc.path = u.path;
                GetFtpPass(u, cc.pass);
                m_conns.push_back(cc); SaveCfg(); Kick();
                break;
            }
        }
        return;
    }
    if (IsVirtualPath(m_path)) {
        const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Yenile", bui::kGlyphRefresh, false, L"F5"), MenuItem(2, L"Ağ sürücüsü bağla...", bui::kGlyphDrive),
                                                   MenuItem(3, L"FTP bağlantısı ekle...", 0xE839), MenuItem(4, L"Yolu kopyala", bui::kGlyphCopy)}, pt);
        if (c == 1) Refresh(); else if (c == 2) MapDrive(); else if (c == 3) AddConnection(); else if (c == 4) SetClipText(m_hwnd, m_path);
        return;
    }
    items = {MenuItem(10, L"Yeni klasör", bui::kGlyphNewFolder, false, L"Ctrl+Shift+N"), MenuItem(11, L"Yeni metin belgesi", bui::kGlyphDocument), MenuItem(12, L"Yapıştır", bui::kGlyphPaste, false, L"Ctrl+V"),
             MenuSeparator(), MenuItem(13, L"Görünüm", bui::kGlyphView), MenuItem(14, L"Sırala", bui::kGlyphSort), MenuItem(15, m_showHidden ? L"Gizli öğeleri gizle" : L"Gizli öğeleri göster", 0, false, L"Ctrl+H"),
             MenuSeparator(), MenuItem(16, L"Yenile", bui::kGlyphRefresh, false, L"F5"), MenuItem(17, L"Terminalde aç", 0xE756), MenuItem(18, L"Yolu kopyala", bui::kGlyphCopy),
             MenuSeparator(), MenuItem(19, L"Özellikler", bui::kGlyphInfo), MenuItem(20, L"Diğer seçenekler (Windows menüsü)", 0xE712)};
    items[2].enabled = IsClipboardFormatAvailable(CF_HDROP) != 0 || (!m_ftpClip.empty() && GetClipboardSequenceNumber() == m_ftpClipSeq);
    const int c = aether::ui::ShowContextMenu(items, pt);
    switch (c) {
        case 10: NewFolder(); break;
        case 11: NewFile(L"Yeni Metin Belgesi", L".txt"); break;
        case 12: Paste(); break;
        case 13: ViewMenu(pt); break;
        case 14: Click(H_CmdSort, false, false); break;
        case 15: m_showHidden = !m_showHidden; SaveCfg(); RebuildView(); break;
        case 16: Refresh(); break;
        case 17: OpenTerminal(m_path); break;
        case 18: SetClipText(m_hwnd, m_path); break;
        case 19: ShowProperties(); break;
        case 20: ShellMenu({m_path}, pt); break;
    }
}

// ---- network: connections, transfers, archives ---------------------------------------------------------------------
bool Explorer::EditConn(Conn& c) {
    std::vector<DlgField> f{{L"Ad (kenar çubuğunda görünür)", c.name, false}, {L"Sunucu (ör. ftp.ornek.com)", c.host, false},
                            {L"Port", std::to_wstring(c.port), false}, {L"Kullanıcı adı (boş bırakılırsa anonim)", c.user, false},
                            {L"Parola", c.pass, true}, {L"Başlangıç klasörü", c.path, false}};
    if (!InputDialog(m_hwnd, L"FTP bağlantısı", f, L"Kaydet ve bağlan")) return false;
    Conn n;
    std::wstring host = f[1].value;
    if (IsFtp(host)) host = host.substr(6);
    const size_t sl = host.find(L'/'); if (sl != std::wstring::npos) host.erase(sl);
    const size_t at = host.find_last_of(L'@'); if (at != std::wstring::npos) host.erase(0, at + 1);
    const size_t pc = host.find_last_of(L':'); if (pc != std::wstring::npos) host.erase(pc);
    while (!host.empty() && host.back() == L' ') host.pop_back();
    n.host = host;
    if (n.host.empty()) return false;
    n.name = f[0].value.empty() ? n.host : f[0].value;
    const int port = _wtoi(f[2].value.c_str());
    n.port = port > 0 && port < 65536 ? static_cast<unsigned>(port) : 21u;
    n.user = f[3].value; n.pass = f[4].value;
    n.path = f[5].value.empty() ? L"/" : f[5].value;
    if (n.path[0] != L'/') n.path = L"/" + n.path;
    c = n;
    return true;
}
void Explorer::AddConnection() {
    Conn c;
    if (!EditConn(c)) return;
    m_conns.push_back(c);
    SaveCfg();
    ConnectTo(static_cast<int>(m_conns.size()) - 1);
}
void Explorer::ConnectTo(int i) {
    if (i < 0 || i >= static_cast<int>(m_conns.size())) return;
    const Conn& c = m_conns[i];
    if (!c.user.empty() && !c.pass.empty()) SetFtpPass(ParseFtp(c.Url()), c.pass);
    Navigate(c.Url());
}
bool Explorer::EnsureAuth(const std::wstring& url) {
    const FtpUrl u = ParseFtp(url);
    if (!u.ok || u.user.empty()) return true;
    std::wstring pw;
    if (GetFtpPass(u, pw)) return true;
    std::vector<DlgField> f{{u.user + L"@" + u.host + L" için parola", L"", true}};
    if (!InputDialog(m_hwnd, L"FTP oturumu", f, L"Bağlan")) return false;
    SetFtpPass(u, f[0].value);
    return true;
}
void Explorer::MapDrive() {
    NETRESOURCEW nr{}; nr.dwType = RESOURCETYPE_DISK;
    CONNECTDLGSTRUCTW cd{}; cd.cbStructure = sizeof(cd); cd.hwndOwner = m_hwnd; cd.lpConnRes = &nr;
    if (WNetConnectionDialog1W(&cd) == NO_ERROR) {
        if (cd.dwDevNum > 0 && cd.dwDevNum <= 26) Navigate(std::wstring(1, static_cast<wchar_t>(L'A' + cd.dwDevNum - 1)) + L":\\");
        else if (m_home) StartHomeLoad();
    }
}
void Explorer::AddMenu(POINT pt) {
    using aether::ui::MenuItem;
    const int c = aether::ui::ShowContextMenu({MenuItem(1, L"FTP bağlantısı ekle...", 0xE839), MenuItem(2, L"Ağ sürücüsü bağla...", bui::kGlyphDrive),
                                               MenuItem(3, L"Ağ yolu yaz (\\\\sunucu\\paylaşım)", bui::kGlyphEnter), MenuItem(4, L"Ağdaki bilgisayarları göster", bui::kGlyphFolder)}, pt);
    if (c == 1) AddConnection();
    else if (c == 2) MapDrive();
    else if (c == 3) BeginEdit(E_Path, L"\\\\");
    else if (c == 4) Navigate(L"net:");
}
void Explorer::StartTransfer(std::vector<std::wstring> srcs, std::wstring dest, bool move) {
    const HWND hwnd = m_hwnd;
    std::thread([=]() {
        const bool ok = TransferItems(srcs, dest, move, [hwnd](const std::wstring& t) { PostXfer(hwnd, t); });
        PostXfer(hwnd, ok ? L"" : L"Bazı öğeler aktarılamadı");
        PostOpDone(hwnd, L"");
    }).detach();
}
void Explorer::RemoteOp(const std::wstring& anyUrl, std::function<bool(FtpClient&)> fn, const std::wstring& sel) {
    const HWND hwnd = m_hwnd;
    std::thread([=]() {
        FtpClient c;
        const bool ok = c.Open(anyUrl) && fn(c);
        PostXfer(hwnd, ok ? L"" : L"İşlem sunucuda başarısız oldu");
        PostOpDone(hwnd, ok ? sel : L"");
    }).detach();
}
void Explorer::OpenRemoteFile(const std::wstring& url) {
    const HWND hwnd = m_hwnd;
    std::thread([=]() {
        const std::wstring leaf = LeafOf(url);
        PostXfer(hwnd, L"İndiriliyor: " + leaf);
        wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
        wchar_t hs[32]; swprintf(hs, 32, L"%zx", std::hash<std::wstring>{}(url));
        const std::wstring dir = JoinPath(JoinPath(tmp, L"AetherFtp"), hs);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        const std::wstring local = JoinPath(dir, leaf);
        FtpClient c;
        const bool ok = c.Open(url) && c.Get(ParseFtp(url).path, local);
        PostXfer(hwnd, ok ? L"" : L"İndirilemedi: " + leaf);
        if (ok) ShellExecuteW(hwnd, L"open", local.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    }).detach();
}
void Explorer::DownloadSelection() {
    auto p = SelectedPaths();
    if (p.empty()) return;
    const std::wstring dest = PickFolder(m_hwnd, L"Dosyaların indirileceği klasörü seç");
    if (!dest.empty()) StartTransfer(p, dest, false);
}
void Explorer::BrowseArchive(const std::wstring& arc) {
    const HWND hwnd = m_hwnd;
    WIN32_FILE_ATTRIBUTE_DATA ad{};
    GetFileAttributesExW(arc.c_str(), GetFileExInfoStandard, &ad);
    const size_t key = std::hash<std::wstring>{}(Lower(arc)) ^ (static_cast<size_t>(ad.nFileSizeLow) * 31u) ^ ad.ftLastWriteTime.dwLowDateTime;
    wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
    wchar_t ks[32]; swprintf(ks, 32, L"%zx", key);
    std::wstring stem = LeafOf(arc); const size_t d = stem.find_last_of(L'.'); if (d != std::wstring::npos && d > 0) stem.erase(d);
    const std::wstring dir = JoinPath(JoinPath(tmp, L"AetherArc"), stem + L"_" + ks);
    std::thread([=]() {
        bool have = false;
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
            if (h != INVALID_HANDLE_VALUE) {
                do { if (!(fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))) { have = true; break; } } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (!have) {
            PostXfer(hwnd, L"Arşiv açılıyor: " + LeafOf(arc));
            std::wstring tool;
            if (!ExtractArchive(arc, dir, tool)) {
                std::wstring d2 = dir; d2.push_back(0); d2.push_back(0);
                SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = d2.c_str(); op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
                SHFileOperationW(&op);
                PostXfer(hwnd, tool.empty() ? L"Arşiv açılamadı: uygun araç bulunamadı (7-Zip veya WinRAR kurun)"
                                            : L"Arşiv açılamadı (" + tool + L"). RAR/7z için 7-Zip veya WinRAR kurulu olmalı.");
                return;
            }
        }
        PostXfer(hwnd, L"Arşiv içeriği (geçici, salt okunur kopya)");
        PostNav(hwnd, dir);
    }).detach();
}
void Explorer::ExtractArchiveCmd(const std::wstring& arc, bool intoFolder) {
    const HWND hwnd = m_hwnd;
    const std::wstring parent = arc.substr(0, arc.find_last_of(L'\\'));
    std::wstring stem = LeafOf(arc); const size_t d = stem.find_last_of(L'.'); if (d != std::wstring::npos && d > 0) stem.erase(d);
    if (ExtOf(stem) == L".tar") stem.erase(stem.size() - 4);
    const std::wstring folder = intoFolder ? UniqueName(parent, stem, L"") : L"";
    const std::wstring dest = intoFolder ? JoinPath(parent, folder) : parent;
    std::thread([=]() {
        PostXfer(hwnd, L"Çıkarılıyor: " + LeafOf(arc));
        std::wstring tool;
        const bool ok = ExtractArchive(arc, dest, tool);
        PostXfer(hwnd, ok ? L"" : L"Çıkarılamadı" + (tool.empty() ? std::wstring(L": uygun araç yok") : L" (" + tool + L"). RAR/7z için 7-Zip veya WinRAR gerekebilir."));
        PostOpDone(hwnd, ok ? folder : L"");
    }).detach();
}

// ---- tabs ---------------------------------------------------------------------------------------------------------
std::wstring Explorer::TabTitle(const std::wstring& p) const {
    if (p.empty()) return L"Bu Bilgisayar";
    if (IsNetRoot(p)) return L"Ağ";
    if (IsFtp(p)) {
        const FtpUrl u = ParseFtp(p);
        std::wstring r = u.path;
        while (r.size() > 1 && r.back() == L'/') r.pop_back();
        return r.size() <= 1 ? u.host : r.substr(r.find_last_of(L'/') + 1);
    }
    std::wstring q = p;
    while (q.size() > 3 && q.back() == L'\\') q.pop_back();
    if (q.size() <= 3) return q;
    const size_t i = q.find_last_of(L'\\');
    return i == std::wstring::npos ? q : q.substr(i + 1);
}
void Explorer::SaveTab() {
    if (m_tab < 0 || m_tab >= static_cast<int>(m_tabs.size())) return;
    Tab& t = m_tabs[m_tab];
    t.path = m_path; t.hist = m_hist; t.pos = m_histPos; t.scroll = m_scrollTarget;
}
void Explorer::SwitchTab(int i) {
    if (i == m_tab || i < 0 || i >= static_cast<int>(m_tabs.size())) return;
    SaveTab();
    m_tab = i;
    const Tab t = m_tabs[i];
    m_hist = t.hist; m_histPos = t.pos;
    Navigate(t.path, false);
    m_scrollTarget = m_scroll = t.scroll;
}
void Explorer::NewTab(const std::wstring& path) {
    if (m_tabs.size() >= 24) return;
    SaveTab();
    Tab t; t.path = path; t.hist = {path}; t.pos = 0;
    m_tabs.insert(m_tabs.begin() + m_tab + 1, t);
    ++m_tab;
    m_hist = t.hist; m_histPos = 0;
    Navigate(path, false);
}
void Explorer::CloseTab(int i) {
    if (i < 0 || i >= static_cast<int>(m_tabs.size())) return;
    if (m_tabs.size() == 1) { PostMessageW(m_hwnd, WM_CLOSE, 0, 0); return; }
    SaveTab();
    m_tabs.erase(m_tabs.begin() + i);
    if (i < m_tab) --m_tab;
    else if (i == m_tab) {
        m_tab = std::min(m_tab, static_cast<int>(m_tabs.size()) - 1);
        const Tab t = m_tabs[m_tab];
        m_hist = t.hist; m_histPos = t.pos;
        Navigate(t.path, false);
        m_scrollTarget = m_scroll = t.scroll;
    }
    Kick();
}

void Explorer::DrawTabs(float W) {
    const Color vl = render::glass::kVioletLight, v = render::glass::kViolet, text = render::glass::kText, muted = render::glass::kMuted;
    const float x0 = kSideW + 70, x1 = W - 3 * 46 - 58;
    const int n = static_cast<int>(m_tabs.size());
    const float plus = 32;
    const float tw = std::clamp((x1 - x0 - plus - 4) / std::max(1, n), 76.0f, 200.0f);
    float x = x0;
    for (int i = 0; i < n; ++i) {
        const Rect r{x, 8, tw - 4, 30};
        const bool act = i == m_tab;
        const int id = H_Tab + i;
        const float hv = Hover(id);
        if (act) {
            GlowRR(r, 10, v, 7.0f, 0.9f);
            if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(Mix(v, vl, 0.2f), 0.42f), WithA(render::glass::kVioletDeep, 0.20f), D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom())))
                m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), 10, 10), g.Get());
            StrokeRR(r, 10, WithA(vl, 0.5f));
        } else FillRR(r, 10, WithA(vl, 0.12f * hv));
        Icon(15, Rect{r.x + 10, r.y + 8, 17, 15});
        const std::wstring title = TabTitle(act ? m_path : m_tabs[i].path);
        Txt(title, Rect{r.x + 34, r.y, r.width - 34 - (n > 1 ? 28.0f : 8.0f), r.height}, 12.5f, act ? 600 : 400, act ? kWhite : text);
        Hit(r, id);
        if (n > 1 && (act || hv > 0.05f)) {
            const Rect c{r.Right() - 26, r.y + 4, 22, 22};
            FillRR(c, 7, WithA(vl, 0.22f * Hover(H_TabClose + i)));
            Glyph(0xE711, c, 9.0f, text);
            Hit(c, H_TabClose + i);
        }
        x += tw;
    }
    const Rect pr{x + 2, 8, plus, 30};
    FillRR(pr, 10, WithA(vl, 0.16f * Hover(H_TabNew)));
    Glyph(0xE710, pr, 11.0f, Mix(muted, kWhite, 0.4f));
    Hit(pr, H_TabNew);
}

// ---- settings -----------------------------------------------------------------------------------------------------
void Explorer::LoadCfg() {
    const std::wstring p = CfgPath();
    if (p.empty()) return;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string buf(1 << 16, '\0'); DWORD n = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(buf.size() - 1), &n, nullptr);
    CloseHandle(h);
    buf.resize(n);
    const int wl = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, wl)), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), w.data(), wl);
    size_t pos = 0;
    while (pos < w.size()) {
        size_t e = w.find(L'\n', pos);
        if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = line.substr(0, eq), val = line.substr(eq + 1);
        const int iv = _wtoi(val.c_str());
        if (k == L"grid") m_grid = iv != 0;
        else if (k == L"preview") m_preview = iv != 0;
        else if (k == L"sort") m_sortKey = static_cast<SortKey>(std::clamp(iv, 0, 3));
        else if (k == L"asc") m_sortAsc = iv != 0;
        else if (k == L"hidden") m_showHidden = iv != 0;
        else if (k == L"icon") m_iconSize = std::clamp(static_cast<float>(iv), 48.0f, 192.0f);
        else if (k == L"pin" && !val.empty() && m_pins.size() < 12) m_pins.push_back(val);
        else if (k == L"conn" && m_conns.size() < 24) {
            std::vector<std::wstring> f; size_t a = 0;
            for (;;) { const size_t t = val.find(L'\t', a); if (t == std::wstring::npos) { f.push_back(val.substr(a)); break; } f.push_back(val.substr(a, t - a)); a = t + 1; }
            if (f.size() >= 6 && !f[1].empty()) {
                Conn c; c.name = f[0]; c.host = f[1]; c.port = static_cast<unsigned>(std::max(1, _wtoi(f[2].c_str()))); c.user = f[3]; c.pass = UnprotectPass(f[4]); c.path = f[5];
                m_conns.push_back(c);
            }
        }
    }
}
void Explorer::SaveCfg() {
    const std::wstring p = CfgPath();
    if (p.empty()) return;
    std::wstring w = L"grid=" + std::to_wstring(m_grid ? 1 : 0) + L"\npreview=" + std::to_wstring(m_preview ? 1 : 0) + L"\nsort=" + std::to_wstring(static_cast<int>(m_sortKey)) +
                     L"\nasc=" + std::to_wstring(m_sortAsc ? 1 : 0) + L"\nhidden=" + std::to_wstring(m_showHidden ? 1 : 0) + L"\nicon=" + std::to_wstring(static_cast<int>(m_iconSize)) + L"\n";
    for (const auto& pin : m_pins) w += L"pin=" + pin + L"\n";
    for (const auto& c : m_conns)
        w += L"conn=" + c.name + L"\t" + c.host + L"\t" + std::to_wstring(c.port) + L"\t" + c.user + L"\t" + ProtectPass(c.pass) + L"\t" + c.path + L"\n";
    const int bl = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string b(static_cast<size_t>(std::max(0, bl)), '\0');
    if (bl > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), b.data(), bl, nullptr, nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr); CloseHandle(h);
}

bool Explorer::IsPinned(const std::wstring& p) const {
    for (const auto& x : m_pins) if (IEquals(x, p)) return true;
    return false;
}
void Explorer::TogglePin(const std::wstring& p) {
    for (size_t i = 0; i < m_pins.size(); ++i) if (IEquals(m_pins[i], p)) { m_pins.erase(m_pins.begin() + i); SaveCfg(); Kick(); return; }
    if (m_pins.size() < 12) m_pins.push_back(p);
    SaveCfg(); Kick();
}

// ---- geometry / selection helpers ---------------------------------------------------------------------------------
Rect Explorer::CellRect(int vi) const {
    if (!m_grid) return Rect{m_lx, m_ly + 4 + vi * kRowH - m_scroll, m_lw - 14, kRowH - 2};
    const int c = vi % std::max(1, m_cols), r = vi / std::max(1, m_cols);
    return Rect{m_lx + c * m_cellW, m_ly + 4 + r * m_cellH - m_scroll, m_cellW - 8, m_cellH - 8};
}

void Explorer::RowDown(int vi, bool ctrl, bool shift) {
    if (m_edit == E_Rename) { if (vi == m_renameIndex) return; CommitEdit(); }
    m_pressRow = vi; m_pressX = m_mx; m_pressY = m_my; m_pressSelectOnUp = false; m_dragStarted = false;
    if (vi < 0 || vi >= static_cast<int>(m_sel.size())) return;
    if (shift) ExtendSelection(vi);
    else if (ctrl) { m_sel[vi] = !m_sel[vi]; m_anchor = m_cursor = vi; }
    else if (m_sel[vi]) { m_pressSelectOnUp = true; m_cursor = vi; }      // keep a multi-selection so it can be dragged
    else SelectOnly(vi);
}

void Explorer::UpdateMarquee() {
    const float x0 = std::min(m_mqX, m_mx), x1 = std::max(m_mqX, m_mx);
    const float ay = m_mqY - m_scroll;
    const float y0 = std::min(ay, m_my), y1 = std::max(ay, m_my);
    if (m_mqBase.size() == m_sel.size()) m_sel = m_mqBase; else std::fill(m_sel.begin(), m_sel.end(), 0);
    for (int i = 0; i < static_cast<int>(m_view.size()); ++i) {
        const Rect c = CellRect(i);
        if (c.Bottom() < y0) continue;
        if (c.y > y1) { if (!m_grid) break; continue; }
        if (c.x < x1 && c.Right() > x0) m_sel[i] = 1;
    }
    Kick();
}

void Explorer::StartDrag() {
    auto paths = SelectedPaths();
    if (paths.empty() || IsVirtualPath(m_path)) return;
    auto data = MakeDataObject(paths);
    if (!data) return;
    ComPtr<IDropSource> src;
    src.Attach(new ExplorerDropSource());
    m_dragSrc = paths; m_dragging = true;
    DWORD eff = 0;
    DoDragDrop(data.Get(), src.Get(), DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    m_dragging = false; m_dropRow = -1; m_dragSrc.clear(); m_dropPath.clear();
    m_leftDown = false; m_pressRow = -1; m_pressSelectOnUp = false;
    ReleaseCapture();
    if (eff & DROPEFFECT_MOVE) Refresh();
    Kick();
}

std::wstring Explorer::DropTargetPath(int id) const {
    if (id >= H_Row) {
        const size_t vi = static_cast<size_t>(id - H_Row);
        if (vi < m_view.size() && m_items[m_view[vi]].dir) return FullPath(m_items[m_view[vi]]);
        return (m_home || m_deep) ? L"" : m_path;
    }
    if (id >= H_QuickTile && id < H_QuickTile + 100) { const size_t i = id - H_QuickTile; return i < m_quick.size() ? m_quick[i].path : L""; }
    if (id >= H_SideFolder && id < H_SideFolder + 100) { const size_t i = id - H_SideFolder; return i < m_quick.size() ? m_quick[i].path : L""; }
    if (id >= H_SidePin && id < H_SidePin + 100) { const size_t i = id - H_SidePin; return i < m_pins.size() ? m_pins[i] : L""; }
    if (id >= H_SideDrive && id < H_SideDrive + 100) { const size_t i = id - H_SideDrive; return i < m_homeData.drives.size() ? m_homeData.drives[i].root : L""; }
    if (id >= H_DriveCard && id < H_DriveCard + 100) { const size_t i = id - H_DriveCard; return i < m_homeData.drives.size() ? m_homeData.drives[i].root : L""; }
    if (id == 0 || id == H_Scroll) return (m_home || m_deep) ? L"" : m_path;
    return L"";
}

DWORD Explorer::DragOverAt(POINT sp, DWORD keys, DWORD allowed) {
    POINT cp = sp; ScreenToClient(m_hwnd, &cp);
    const float s = Scale();
    const int id = HitAt(cp.x / s, cp.y / s);
    m_dropPath = DropTargetPath(id);
    m_dropRow = -1;
    if (!m_dropPath.empty()) {
        if (id >= H_Row && static_cast<size_t>(id - H_Row) < m_view.size() && m_items[m_view[id - H_Row]].dir) m_dropRow = id - H_Row;
        else if (id == 0 || id >= H_Row) m_dropRow = -2;
    }
    Kick();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (m_dropPath.empty()) return DROPEFFECT_NONE;
    if (m_dragging) for (const auto& p : m_dragSrc) if (IEquals(p, m_dropPath)) return DROPEFFECT_NONE;
    DWORD eff;
    if (keys & MK_SHIFT) eff = DROPEFFECT_MOVE;
    else if (keys & MK_CONTROL) eff = DROPEFFECT_COPY;
    else if (m_dragging && !m_dragSrc.empty() && !m_dragSrc[0].empty() && towupper(m_dragSrc[0][0]) == towupper(m_dropPath[0])) eff = DROPEFFECT_MOVE;
    else eff = DROPEFFECT_COPY;
    if (!(allowed & eff)) eff = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : ((allowed & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE);
    return eff;
}
void Explorer::DragLeaveAll() { m_dropRow = -1; m_dropPath.clear(); Kick(); InvalidateRect(m_hwnd, nullptr, FALSE); }

void Explorer::DropAt(IDataObject* obj, POINT sp, DWORD keys, DWORD* effect) {
    const DWORD eff = DragOverAt(sp, keys, *effect);
    const auto paths = PathsFromData(obj);
    if (eff == DROPEFFECT_NONE || paths.empty() || m_dropPath.empty()) { *effect = DROPEFFECT_NONE; return; }
    auto norm = [](std::wstring p) { while (p.size() > 3 && p.back() == L'\\') p.pop_back(); return p; };
    if (eff & DROPEFFECT_MOVE) {
        bool same = true;
        for (const auto& p : paths) if (!IEquals(norm(ParentOf(p)), norm(m_dropPath))) { same = false; break; }
        if (same) { *effect = DROPEFFECT_NONE; return; }
    }
    if (IsFtp(m_dropPath)) { StartTransfer(paths, m_dropPath, (eff & DROPEFFECT_MOVE) != 0); *effect = DROPEFFECT_COPY; return; }
    if (IsVirtualPath(m_dropPath)) { *effect = DROPEFFECT_NONE; return; }
    RunFileOp(m_hwnd, (eff & DROPEFFECT_MOVE) ? Op::Move : Op::Copy, paths, m_dropPath, L"", L"");
    *effect = eff;
}

// ---- the genuine Windows context menu (Open with, Share, extensions...) -----------------------------------------
void Explorer::ShellMenu(const std::vector<std::wstring>& paths, POINT pt) {
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
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (GetKeyState(VK_SHIFT) & 0x8000) flags |= CMF_EXTENDEDVERBS;
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
            SetTimer(m_hwnd, kTimerRefresh + 10, 700, nullptr);          // one-shot refresh after the shell finished its work
        }
    }
    DestroyMenu(menu);
}

// ---- watching the folder, deep search, thumbnails, folder statistics ------------------------------------------------
void Explorer::StopWatch() {
    if (m_watchStop) { SetEvent(m_watchStop); m_watchStop = nullptr; }
}
void Explorer::StartWatch() {
    StopWatch();
    if (m_home || m_deep || m_path.empty() || IsVirtualPath(m_path)) return;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) return;
    m_watchStop = stop;
    const HWND hwnd = m_hwnd; const std::wstring path = m_path;
    std::thread([=]() {
        HANDLE h = FindFirstChangeNotificationW(path.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES);
        if (h != INVALID_HANDLE_VALUE) {
            HANDLE hs[2] = {stop, h};
            for (;;) {
                const DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                if (r != WAIT_OBJECT_0 + 1) break;
                Sleep(300);                                          // debounce bursts of changes
                if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) break;
                if (IsWindow(hwnd)) PostMessageW(hwnd, kMsgChanged, 0, 0);
                if (!FindNextChangeNotification(h)) break;
            }
            FindCloseChangeNotification(h);
        }
        CloseHandle(stop);
    }).detach();
}

void Explorer::StartDeepSearch() {
    if (m_home || m_search.empty()) return;
    StopWatch();
    const uint32_t gen = ++m_gen;
    m_loading = true;
    const HWND hwnd = m_hwnd; const std::wstring root = m_path, q = m_search; const bool hid = m_showHidden;
    std::thread([=, this]() {
        auto* r = new FolderResult();
        r->gen = gen; r->path = root; r->deep = true;
        DeepSearch(root, q, m_gen, gen, hid, *r);
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, kMsgFolder, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
    Kick();
}

void Explorer::ClearSearch() {
    m_search.clear();
    if (m_deep) { m_deep = false; StartFolderLoad(); StartWatch(); }
    else RebuildView();
    Kick();
}

ID2D1Bitmap1* Explorer::Thumb(const std::wstring& path, int px) {
    if (IsFtp(path)) return nullptr;
    const std::wstring key = path + (px > 200 ? L"|L" : L"");
    auto it = m_thumbs.find(key);
    if (it != m_thumbs.end()) return it->second.Get();
    if (m_thumbPending.size() < 48 && m_thumbPending.insert(key).second) TQ().Push(m_hwnd, path, px);
    return nullptr;
}
void Explorer::OnThumb(ThumbResult* r) {
    std::unique_ptr<ThumbResult> res(r);
    const std::wstring key = res->path + (res->px > 200 ? L"|L" : L"");
    m_thumbPending.erase(key);
    ComPtr<ID2D1Bitmap1> bmp;
    if (res->w > 0 && res->h > 0 && m_dcomp && m_dcomp->GetContext()) {
        m_dcomp->GetContext()->CreateBitmap(D2D1::SizeU(res->w, res->h), res->pixels.data(), res->w * 4,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    }
    if (!m_thumbs.count(key)) m_thumbOrder.push_back(key);
    m_thumbs[key] = bmp;
    while (m_thumbOrder.size() > 500) { m_thumbs.erase(m_thumbOrder.front()); m_thumbOrder.erase(m_thumbOrder.begin()); }
    Kick();
}

void Explorer::StartInfo(const std::wstring& path) {
    const uint32_t gen = ++m_infoGen;
    m_infoPath = path; m_infoCount = -1; m_infoSize = 0; m_infoBusy = true;
    const HWND hwnd = m_hwnd;
    std::thread([=, this]() {
        auto* r = new InfoResult();
        r->path = path; r->gen = gen;
        FolderStats(path, m_infoGen, gen, r->count, r->size);
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, kMsgInfo, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}
void Explorer::OnInfo(InfoResult* r) {
    std::unique_ptr<InfoResult> res(r);
    if (res->gen != m_infoGen.load()) return;
    m_infoCount = res->count; m_infoSize = res->size; m_infoBusy = false;
    Kick();
}

void Explorer::OpenTerminal(const std::wstring& dir) {
    if (dir.empty()) return;
    const std::wstring args = L"-d \"" + dir + L"\"";
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd, L"open", L"wt.exe", args.c_str(), dir.c_str(), SW_SHOWNORMAL));
    if (r <= 32) {
        const std::wstring a2 = L"/K cd /d \"" + dir + L"\"";
        ShellExecuteW(m_hwnd, L"open", L"cmd.exe", a2.c_str(), dir.c_str(), SW_SHOWNORMAL);
    }
}

void Explorer::NewFile(const std::wstring& base, const std::wstring& ext) {
    if (m_home || m_deep || IsVirtualPath(m_path)) return;
    const std::wstring name = UniqueName(m_path, base, ext);
    HANDLE h = CreateFileW(JoinPath(m_path, name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); m_pendingSelect = name; m_pendingRename = true; Refresh(); }
}

void Explorer::BreadcrumbMenu(int idx, POINT pt) {
    using aether::ui::MenuItem;
    std::vector<aether::ui::MenuEntry> items;
    std::vector<std::wstring> targets;
    if (idx == 0) {
        for (const auto& d : m_homeData.drives) { items.push_back(MenuItem(static_cast<int>(items.size()) + 1, d.label, bui::kGlyphDrive)); targets.push_back(d.root); }
    } else {
        std::vector<std::wstring> paths;
        for (std::wstring p = m_path; !p.empty(); p = ParentOf(p)) paths.insert(paths.begin(), p);
        if (idx - 1 >= static_cast<int>(paths.size())) return;
        const std::wstring base = paths[idx - 1];
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(JoinPath(base, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && !m_showHidden) continue;
                names.push_back(fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
        for (size_t i = 0; i < names.size() && i < 24; ++i) { items.push_back(MenuItem(static_cast<int>(items.size()) + 1, names[i], bui::kGlyphFolder)); targets.push_back(JoinPath(base, names[i])); }
    }
    if (items.empty()) return;
    const int c = aether::ui::ShowContextMenu(items, pt);
    if (c >= 1 && c <= static_cast<int>(targets.size())) Navigate(targets[c - 1]);
}

void Explorer::ViewMenu(POINT pt) {
    using aether::ui::MenuItem;
    const int c = aether::ui::ShowContextMenu({MenuItem(1, L"Ayrıntılar", bui::kGlyphView, false, !m_grid ? L"●" : L""), MenuItem(2, L"Küçük simgeler", 0xE80A, false, m_grid && m_iconSize < 80 ? L"●" : L""),
                                               MenuItem(3, L"Büyük simgeler", 0xE80A, false, m_grid && m_iconSize >= 80 && m_iconSize < 120 ? L"●" : L""),
                                               MenuItem(4, L"Çok büyük simgeler", 0xE80A, false, m_grid && m_iconSize >= 120 ? L"●" : L""),
                                               aether::ui::MenuSeparator(), MenuItem(5, L"Önizleme paneli", 0xE8A1, false, m_preview ? L"●" : L"")}, pt);
    if (c == 1) m_grid = false;
    else if (c == 2) { m_grid = true; m_iconSize = 64; }
    else if (c == 3) { m_grid = true; m_iconSize = 96; }
    else if (c == 4) { m_grid = true; m_iconSize = 144; }
    else if (c == 5) m_preview = !m_preview;
    if (c) { SaveCfg(); m_scrollTarget = std::min(m_scrollTarget, 1e6f); Kick(); }
}

void Explorer::TxtWrap(const std::wstring& s, const Rect& r, float size, int weight, const Color& c) {
    if (s.empty()) return;
    const uint32_t key = static_cast<uint32_t>(size * 10) | (static_cast<uint32_t>(weight / 100) << 14) | (1u << 24);
    auto it = m_fmts.find(key);
    if (it == m_fmts.end()) {
        ComPtr<IDWriteTextFormat> f;
        auto* dw = m_dcomp->GetDWriteFactory();
        if (FAILED(dw->CreateTextFormat(m_textFont, nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return;
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        ComPtr<IDWriteInlineObject> ell;
        dw->CreateEllipsisTrimmingSign(f.Get(), &ell);
        DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&t, ell.Get());
        it = m_fmts.emplace(key, f).first;
    }
    m_brush->SetColor(DC(c));
    m_ctx->DrawText(s.c_str(), static_cast<UINT32>(s.size()), it->second.Get(), D(r), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// ---- rendering ----------------------------------------------------------------------------------------------------
void Explorer::Render() {
    if (!m_dcomp || IsIconic(m_hwnd)) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    if (rc.right < 8 || rc.bottom < 8) return;
    m_ctx = m_dcomp->BeginDraw();
    if (!m_ctx) return;
    const float s = Scale();
    const float W = rc.right / s, H = rc.bottom / s;
    m_ctx->SetTransform(D2D1::Matrix3x2F::Scale(s, s));
    m_ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    m_hits.clear();
    m_clip = Rect{0, 0, W, H};
    m_iconBudget = 3; m_needRedrawForIcons = false;

    DrawBackground(W, H);
    if (m_home) DrawHome(W, H); else DrawFolder(W, H);
    DrawSidebar(H);
    DrawToolbar(W);
    DrawTitleBar(W);
    if (m_about) DrawAbout(W, H);

    m_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
    m_ctx = nullptr;
}

// Soft outer glow around a rounded rect: stacked strokes that fade out (cheap stand-in for the Gaussian halo of the island panels).
void Explorer::GlowRR(const Rect& r, float rad, const Color& c, float spread, float strength) {
    if (render::glass::CurrentTheme().flat) return;
    const int N = 12;
    for (int i = 0; i < N; ++i) {
        const float t = (i + 0.5f) / N;                       // 0 = at the edge, 1 = fully faded
        const float grow = t * spread;
        const float a = strength * (1.0f - t) * (1.0f - t) * 0.30f;
        if (a < 0.004f) continue;
        StrokeRR(Rect{r.x - grow, r.y - grow, r.width + 2 * grow, r.height + 2 * grow}, rad + grow, WithA(c, a), spread / N * 1.9f);
    }
}

void Explorer::DrawAbout(float W, float H) {
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight, text = render::glass::kText, muted = render::glass::kMuted;
    m_clip = Rect{0, 0, W, H};
    m_brush->SetColor(DC(Color{0.01f, 0.0f, 0.03f, 0.62f}));
    m_ctx->FillRectangle(D2D1::RectF(0, kTitleH, W, H), m_brush.Get());
    Hit(Rect{0, kTitleH, W, H - kTitleH}, H_AboutBlock);

    const float cw = 540, ch = 500;
    const Rect c{std::floor((W - cw) / 2), std::floor((H - ch) / 2), cw, ch};
    GlowRR(c, 22, Mix(v, vl, 0.35f), 30.0f, 1.6f);
    FillRR(c, 22, Color{0.055f, 0.03f, 0.11f, 0.98f});
    Card(c, 22, 0.0f, 1.2f);
    if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, WithA(vl, 0.85f)}, {0.5f, WithA(v, 0.40f)}, {1.0f, WithA(render::glass::kAccent2, 0.85f)}}, D2D1::Point2F(c.x, c.y), D2D1::Point2F(c.Right(), c.Bottom())))
        m_ctx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x + 0.75f, c.y + 0.75f, c.Right() - 0.75f, c.Bottom() - 0.75f), 22, 22), g.Get(), 1.5f);
    Hit(c, H_AboutCard);

    // emblem
    const Rect em{c.x + cw / 2 - 42, c.y + 28, 84, 84};
    if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(Mix(v, vl, 0.25f), 0.95f), WithA(render::glass::kVioletDeep, 0.95f), D2D1::Point2F(em.x, em.y), D2D1::Point2F(em.x, em.Bottom())))
        m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(em), 22, 22), g.Get());
    StrokeRR(em, 22, WithA(vl, 0.65f), 1.5f);
    Icon(K_Folder, Rect{em.x + 18, em.y + 20, 48, 44});

    float y = c.y + 126;
    Txt(L"Aether Explorer", Rect{c.x, y, cw, 32}, 23, 700, kWhite, 1); y += 36;
    Txt(std::wstring(L"Sürüm ") + kAppVersion + L"  ·  Hızlı ve hafif dosya yöneticisi", Rect{c.x, y, cw, 20}, 12, 400, muted, 1); y += 36;
    Txt(L"Kerem Berke Yılmaz tarafından geliştirildi", Rect{c.x, y, cw, 24}, 15, 600, Mix(text, vl, 0.25f), 1); y += 38;

    m_brush->SetColor(DC(vl, 0.14f)); m_ctx->FillRectangle(D2D1::RectF(c.x + 40, y, c.Right() - 40, y + 1), m_brush.Get()); y += 16;
    Txt(L"LİSANS", Rect{c.x, y, cw, 18}, 11, 700, WithA(vl, 0.85f), 1); y += 26;
    TxtWrap(L"MIT Lisansı. © 2026 Kerem Berke Yılmaz. Yazılımı kullanabilir, değiştirebilir ve dağıtabilirsiniz; telif ve lisans bildiriminin korunması yeterlidir. Yazılım hiçbir garanti verilmeden “olduğu gibi” sunulur.",
            Rect{c.x + 44, y, cw - 88, 72}, 12, 400, text); y += 84;
    Txt(L"DESTEK", Rect{c.x, y, cw, 18}, 11, 700, WithA(vl, 0.85f), 1); y += 26;
    TxtWrap(L"Aether açık kaynaklıdır ve bağışlara açıktır. Beğendiysen GitHub'da yıldız verebilir ya da bağışla geliştirmeye destek olabilirsin. Teşekkürler!",
            Rect{c.x + 44, y, cw - 88, 56}, 12, 400, muted);

    // buttons
    const float by = c.Bottom() - 70, bw = 142, gap = 12;
    const float bx = c.x + (cw - (bw * 3 + gap * 2)) / 2;
    struct B { const wchar_t* t; int id; bool primary; };
    const B bs[] = {{L"GitHub", H_AboutGitHub, false}, {L"♥  Bağış yap", H_AboutDonate, true}, {L"Kapat", H_AboutClose, false}};
    for (int i = 0; i < 3; ++i) {
        const Rect r{bx + i * (bw + gap), by, bw, 38};
        const float hv = Hover(bs[i].id);
        if (bs[i].primary) {
            GlowRR(r, 12, vl, 14.0f, 0.9f + 0.6f * hv);
            if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(Mix(v, vl, 0.10f + 0.2f * hv), 0.95f), WithA(render::glass::kVioletDeep, 0.9f), D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom())))
                m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), 12, 12), g.Get());
            StrokeRR(r, 12, WithA(vl, 0.6f));
        } else {
            Card(r, 12, hv, 1.0f);
        }
        Txt(bs[i].t, r, 13, 600, kWhite, 1);
        Hit(r, bs[i].id);
    }
    Txt(L"F1 / Esc ile kapat", Rect{c.x, c.Bottom() - 26, cw, 18}, 10.5f, 400, WithA(muted, 0.55f), 1);
}

void Explorer::DrawBackground(float W, float H) {
    const auto& th = render::glass::CurrentTheme();
    Color top = th.bodyTop, bot = th.bodyBot;
    // translucent when real acrylic sits behind the window, nearly opaque otherwise
    top.a = m_acrylic ? 0.52f : 0.96f; bot.a = m_acrylic ? 0.66f : 0.98f;
    if (th.flat) { top = Color{0.085f, 0.085f, 0.095f, top.a}; bot = Color{0.060f, 0.060f, 0.070f, bot.a}; }
    if (auto g = m_dcomp->CreateLinearGradientBrush(top, bot, D2D1::Point2F(0, 0), D2D1::Point2F(W * 0.5f, H)))
        m_ctx->FillRectangle(D2D1::RectF(0, 0, W, H), g.Get());
    if (!th.flat) {
        const Color v = render::glass::kViolet;
        if (auto g = m_dcomp->CreateRadialGradientBrush(WithA(v, 0.26f), WithA(v, 0.0f), D2D1::Point2F(0, H), W * 0.42f, H * 0.55f))
            m_ctx->FillRectangle(D2D1::RectF(0, 0, W, H), g.Get());
        if (auto g = m_dcomp->CreateRadialGradientBrush(WithA(render::glass::kAccent2, 0.20f), WithA(v, 0.0f), D2D1::Point2F(W, 0), W * 0.40f, H * 0.5f))
            m_ctx->FillRectangle(D2D1::RectF(0, 0, W, H), g.Get());
        // glowing violet rim around the whole window: a bright hairline plus a soft inner halo (same two-tone as the island panels)
        if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, WithA(render::glass::kVioletLight, 1.0f)}, {0.5f, WithA(v, 0.55f)}, {1.0f, WithA(render::glass::kAccent2, 1.0f)}},
                                                        D2D1::Point2F(0, 0), D2D1::Point2F(W, H))) {
            for (int i = 14; i >= 1; --i) {
                const float t = static_cast<float>(i) / 14.0f;
                g->SetOpacity(0.13f * (1.0f - t) * (1.0f - t) + 0.02f * (1.0f - t));
                const float o = 0.75f + i * 1.15f;
                m_ctx->DrawRectangle(D2D1::RectF(o, o, W - o, H - o), g.Get(), 2.4f);
            }
            g->SetOpacity(0.95f);
            m_ctx->DrawRectangle(D2D1::RectF(0.75f, 0.75f, W - 0.75f, H - 0.75f), g.Get(), 1.5f);
        }
    }
}

void Explorer::DrawTitleBar(float W) {
    const Color vl = render::glass::kVioletLight, text = render::glass::kText, muted = render::glass::kMuted;
    m_clip = Rect{0, 0, W, kTitleH};
    m_brush->SetColor(DC(vl, 0.08f));
    m_ctx->FillRectangle(D2D1::RectF(0, kTitleH - 1, W, kTitleH), m_brush.Get());

    Icon(K_Folder, Rect{18, 11, 24, 22});
    Txt(L"Explorer", Rect{54, 0, 100, kTitleH}, 13.5f, 600, kWhite);
    DrawTabs(W);

    const bool canBack = m_histPos > 0, canFwd = m_histPos + 1 < static_cast<int>(m_hist.size());
    for (int i = 0; i < 2; ++i) {
        const bool en = i == 0 ? canBack : canFwd;
        const Rect r{kSideW - 8.0f + i * 34.0f, 7, 30, 30};
        const int id = i == 0 ? H_Back : H_Fwd;
        const float h = en ? Hover(id) : 0.0f;
        FillRR(r, 9, WithA(vl, 0.18f * h));
        Glyph(i == 0 ? bui::kGlyphBack : bui::kGlyphForward, r, 13.0f, en ? text : WithA(muted, 0.35f));
        if (en) Hit(r, id);
    }

    {
        const Rect r{W - 46.0f * 3 - 40, 7, 34, 30};
        FillRR(r, 9, WithA(vl, 0.18f * Hover(H_About)));
        Glyph(0xE946, r, 14.0f, m_about ? kWhite : text);
        Hit(r, H_About);
    }
    const float bw = 46;
    for (int i = 0; i < 3; ++i) {
        const int id = i == 0 ? H_Min : i == 1 ? H_Max : H_Close;
        const Rect r{W - bw * (3 - i), 0, bw, kTitleH - 1};
        const float h = Hover(id);
        if (i == 2) FillRR(r, 0, Color{0.91f, 0.18f, 0.25f, 0.95f * h});
        else FillRR(r, 0, WithA(vl, 0.18f * h));
        const wchar_t g = i == 0 ? 0xE921 : i == 1 ? (m_maximized ? 0xE923 : 0xE922) : 0xE8BB;
        Glyph(g, r, 10.0f, (i == 2 && h > 0.5f) ? kWhite : text);
        Hit(r, id);
    }
}

void Explorer::DrawSidebar(float H) {
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight, text = render::glass::kText, muted = render::glass::kMuted;
    const Rect side{0, kTitleH, kSideW, H - kTitleH};
    m_clip = side;
    if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, WithA(vl, 0.0f)}, {0.5f, WithA(vl, 0.16f)}, {1.0f, WithA(vl, 0.0f)}}, D2D1::Point2F(0, kTitleH), D2D1::Point2F(0, H)))
        m_ctx->FillRectangle(D2D1::RectF(kSideW - 1, kTitleH, kSideW, H), g.Get());

    float y = kTitleH + 12;
    auto hline = [&]() { m_brush->SetColor(DC(vl, 0.11f)); m_ctx->FillRectangle(D2D1::RectF(20, y, kSideW - 20, y + 1), m_brush.Get()); y += 9; };

    // user
    {
        const Rect r{10, y, kSideW - 20, 56};
        FillRR(r, 14, WithA(vl, 0.10f * Hover(H_SideUser)));
        const Rect av{18, y + 6, 44, 44};
        const D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(av.x + 22, av.y + 22), 22, 22);
        if (auto g = m_dcomp->CreateRadialGradientBrush(Color{0.62f, 0.30f, 1.0f, 1.0f}, Color{0.07f, 0.03f, 0.16f, 1.0f}, D2D1::Point2F(av.x + 30, av.y + 30), 34, 34))
            m_ctx->FillEllipse(el, g.Get());
        m_ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f), nullptr);
        Swirl(Rect{av.x + 3, av.y + 8, 38, 30}, 1.6f, Color{0.95f, 0.85f, 1.0f, 0.9f});
        m_ctx->PopLayer();
        m_brush->SetColor(DC(vl, 0.75f));
        m_ctx->DrawEllipse(el, m_brush.Get(), 1.5f);
        Txt(m_homeData.userName.empty() ? L"Kullanıcı" : m_homeData.userName, Rect{74, y + 7, kSideW - 90, 22}, 14, 600, kWhite);
        Txt(L"Yerel Hesap", Rect{74, y + 28, kSideW - 90, 18}, 11.5f, 400, muted);
        Hit(r, H_SideUser);
        y += 62;
    }
    hline();

    auto navItem = [&](const std::wstring& label, int kind, wchar_t glyph, const Color& gc, int id, bool active, bool pinned, float h, float indent, bool bold) {
        const Rect r{10, y, kSideW - 20, h};
        const float hv = Hover(id);
        if (active) {
            GlowRR(r, 12, v, 9.0f, 1.1f);
            if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(Mix(v, vl, 0.10f), 0.90f), WithA(render::glass::kVioletDeep, 0.80f), D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom())))
                m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), 12, 12), g.Get());
            StrokeRR(r, 12, WithA(vl, 0.55f));
            FillRR(Rect{r.x + 8, r.y + 1, r.width - 16, 1}, 0.5f, Color{1.0f, 1.0f, 1.0f, 0.25f});
        } else FillRR(r, 12, WithA(vl, 0.13f * hv));
        const float ix = r.x + 14 + indent;
        if (kind >= 0) Icon(kind, Rect{ix, y + (h - 22) / 2, 22, 22});
        else Glyph(glyph, Rect{ix - 2, y, 26, h}, 16.0f, active ? kWhite : gc);
        Txt(label, Rect{ix + 36, y, r.Right() - ix - 60, h}, 13.5f, (active || bold) ? 600 : 400, kWhite);
        if (pinned) Glyph(bui::kGlyphPin, Rect{r.Right() - 30, y, 22, h}, 11.0f, WithA(muted, 0.85f));
        Hit(r, id);
        y += h + 2;
    };

    navItem(L"Giriş", -1, bui::kGlyphHome, kWhite, H_NavHome, m_home, false, 40, 0, true);
    navItem(L"Hızlı Erişim", -1, 0xE734, Mix(vl, kWhite, 0.2f), H_NavQuick, false, false, 38, 0, false);
    y += 2; hline();
    for (size_t i = 0; i < m_quick.size(); ++i)
        navItem(m_quick[i].name, static_cast<int>(i), 0, kWhite, H_SideFolder + static_cast<int>(i), !m_home && IEquals(m_path, m_quick[i].path), i == 0, 34, 0, false);
    for (size_t i = 0; i < m_pins.size() && i < 6; ++i) {
        std::wstring nm = m_pins[i].substr(m_pins[i].find_last_of(L'\\') + 1);
        if (nm.empty()) nm = m_pins[i];
        navItem(nm, K_Folder, 0, kWhite, H_SidePin + static_cast<int>(i), !m_home && IEquals(m_path, m_pins[i]), true, 34, 0, false);
    }
    y += 2; hline();

    // This PC tree
    {
        const Rect r{10, y, kSideW - 20, 36};
        FillRR(r, 12, WithA(vl, 0.13f * Hover(H_SideThisPc)));
        Glyph(0xE70D, Rect{r.x + 2, y, 16, 36}, 8.0f, muted);
        Icon(K_PC, Rect{r.x + 24, y + 7, 22, 22});
        Txt(L"Bu Bilgisayar", Rect{r.x + 60, y, r.width - 70, 36}, 13.5f, 600, kWhite);
        Hit(r, H_SideThisPc);
        y += 38;
    }
    for (size_t i = 0; i < m_homeData.drives.size() && i < 8; ++i) {
        const auto& d = m_homeData.drives[i];
        const bool act = !m_home && IEquals(m_path.substr(0, 3), d.root);
        const Rect r{10, y, kSideW - 20, 34};
        if (act) { FillRR(r, 12, WithA(vl, 0.22f)); StrokeRR(r, 12, WithA(vl, 0.4f)); }
        else FillRR(r, 12, WithA(vl, 0.13f * Hover(H_SideDrive + static_cast<int>(i))));
        Glyph(bui::kGlyphChevronRight, Rect{r.x + 22, y, 14, 34}, 8.0f, muted);
        Icon(K_Drive, Rect{r.x + 42, y + 7, 22, 20});
        Txt(d.label, Rect{r.x + 76, y, r.width - 84, 34}, 13.5f, 400, kWhite);
        Hit(r, H_SideDrive + static_cast<int>(i));
        y += 36;
    }
    y += 2; hline();
    {
        const float lim = H - 64;                              // above the settings / power row
        const bool netAct = !m_home && (IsNetRoot(m_path) || IsUncServer(m_path));
        const Rect r{10, y, kSideW - 20, 34};
        if (netAct) { FillRR(r, 12, WithA(vl, 0.22f)); StrokeRR(r, 12, WithA(vl, 0.4f)); }
        else FillRR(r, 12, WithA(vl, 0.13f * Hover(H_SideNet)));
        Glyph(0xE70D, Rect{r.x + 2, y, 16, 34}, 8.0f, muted);
        Icon(K_Net, Rect{r.x + 24, y + 6, 22, 22});
        Txt(L"Ağ", Rect{r.x + 60, y, r.width - 70, 34}, 13.5f, 600, kWhite);
        Hit(r, H_SideNet);
        y += 36;
        const FtpUrl cur = (!m_home && IsFtp(m_path)) ? ParseFtp(m_path) : FtpUrl{};
        for (size_t i = 0; i < m_conns.size() && i < 6; ++i) {
            if (y + 36 * 3 > lim) break;                       // keep room for "add" and "Linux"
            const Conn& c = m_conns[i];
            const bool act = cur.ok && IEquals(cur.host, c.host) && cur.port == c.port && IEquals(cur.user, c.user);
            const int id = H_SideConn + static_cast<int>(i);
            const Rect cr{10, y, kSideW - 20, 34};
            if (act) { FillRR(cr, 12, WithA(vl, 0.22f)); StrokeRR(cr, 12, WithA(vl, 0.4f)); }
            else FillRR(cr, 12, WithA(vl, 0.13f * Hover(id)));
            Glyph(0xE839, Rect{cr.x + 38, y, 22, 34}, 14.0f, Mix(vl, kWhite, 0.25f));
            Txt(c.name, Rect{cr.x + 70, y, cr.width - 78, 34}, 13.5f, act ? 600 : 400, kWhite);
            Hit(cr, id);
            y += 36;
        }
        if (y + 36 * 2 <= lim) {
            const Rect ar{10, y, kSideW - 20, 34};
            FillRR(ar, 12, WithA(vl, 0.13f * Hover(H_SideConnAdd)));
            Glyph(0xE710, Rect{ar.x + 38, y, 22, 34}, 12.0f, WithA(muted, 0.95f));
            Txt(L"Bağlantı ekle", Rect{ar.x + 70, y, ar.width - 78, 34}, 13.0f, 400, muted);
            Hit(ar, H_SideConnAdd);
            y += 36;
        }
        if (y + 36 <= lim) {
            const Rect r2{10, y, kSideW - 20, 34};
            FillRR(r2, 12, WithA(vl, 0.13f * Hover(H_SideLinux)));
            Icon(K_Linux, Rect{r2.x + 26, y + 6, 20, 22});
            Txt(L"Linux", Rect{r2.x + 60, y, r2.width - 70, 34}, 13.5f, 400, kWhite);
            Hit(r2, H_SideLinux);
        }
    }

    // bottom: settings | power
    const float by = H - 56;
    m_clip = Rect{0, by - 8, kSideW, 64};
    y = by - 8; hline();
    const float half = (kSideW - 20) * 0.5f;
    m_brush->SetColor(DC(vl, 0.14f));
    m_ctx->FillRectangle(D2D1::RectF(kSideW * 0.5f, by + 4, kSideW * 0.5f + 1, by + 36), m_brush.Get());
    for (int i = 0; i < 2; ++i) {
        const int id = i == 0 ? H_SideSettings : H_SidePower;
        const Rect r{10 + i * half, by, half, 40};
        FillRR(Rect{r.x + 10, r.y, r.width - 20, r.height}, 12, WithA(vl, 0.14f * Hover(id)));
        Glyph(i == 0 ? bui::kGlyphSettings : bui::kGlyphPower, r, 17.0f, Mix(text, vl, 0.35f));
        Hit(r, id);
    }
}

void Explorer::DrawToolbar(float W) {
    const Color vl = render::glass::kVioletLight, text = render::glass::kText, muted = render::glass::kMuted;
    const float x0 = kSideW + 16, y0 = kTitleH + 10, h = 40;
    m_clip = Rect{kSideW, kTitleH, W - kSideW, kToolbarH};

    // flat back / forward / up arrows
    {
        const bool canBack = m_histPos > 0, canFwd = m_histPos + 1 < static_cast<int>(m_hist.size());
        struct A { int id; wchar_t g; bool en; };
        const A arrows[] = {{H_Back, bui::kGlyphBack, canBack}, {H_Fwd, bui::kGlyphForward, canFwd}, {H_Up, bui::kGlyphUp, !m_home}};
        for (int i = 0; i < 3; ++i) {
            const Rect r{x0 + i * 34.0f, y0, 32, h};
            const float hv = arrows[i].en ? Hover(arrows[i].id) : 0.0f;
            FillRR(Rect{r.x, r.y + 4, r.width, r.height - 8}, 10, WithA(vl, 0.16f * hv));
            Glyph(arrows[i].g, r, 14.0f, arrows[i].en ? text : WithA(muted, 0.35f));
            if (arrows[i].en) Hit(r, arrows[i].id);
        }
    }
    const float searchW = std::clamp((W - kSideW) * 0.24f, 190.0f, 300.0f);
    const float cx = x0 + 108, cw = W - cx - searchW - 34;
    // path pill
    {
        const Rect r{cx, y0, cw, h};
        Card(r, h * 0.5f, m_edit == E_Path ? 0.7f : Hover(H_PathBox) * 0.5f, 0.9f);
        Hit(r, H_PathBox);
        // refresh at the right end
        const Rect rf{r.Right() - 40, r.y + 4, 34, h - 8};
        FillRR(rf, 12, WithA(vl, 0.18f * Hover(H_Refresh)));
        Glyph(bui::kGlyphRefresh, rf, 13.0f, Mix(muted, vl, 0.3f));
        Hit(rf, H_Refresh);
        Glyph(bui::kGlyphChevronRight, Rect{r.x + 12, r.y, 20, h}, 11.0f, text);
        const float tx = r.x + 40, tw = r.width - 90;
        if (m_edit == E_Path) {
            if (m_editAll && !m_editText.empty()) FillRR(Rect{tx - 2, r.y + 8, std::min(EditW(13.0f) + 4, tw), h - 16}, 4, WithA(vl, 0.4f));
            Txt(m_editText, Rect{tx, r.y, tw, h}, 13.0f, 400, text);
            if ((GetTickCount64() / 500) % 2 == 0) {
                ComPtr<IDWriteTextLayout> lay; float w = 0;
                if (SUCCEEDED(m_dcomp->GetDWriteFactory()->CreateTextLayout(m_editText.c_str(), static_cast<UINT32>(m_editText.size()), Fmt(13.0f, 400, 0, false), 4000, 30, &lay)) && lay) {
                    DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm); w = tm.widthIncludingTrailingWhitespace;
                }
                FillRR(Rect{tx + std::min(w, tw - 2), r.y + 10, 1.5f, h - 20}, 0.5f, vl);
            }
        } else {
            std::vector<std::wstring> labels{L"Bu Bilgisayar"};
            if (!m_home) {
                std::vector<std::wstring> paths;
                for (std::wstring p = m_path; !p.empty(); p = ParentOf(p)) paths.insert(paths.begin(), p);
                for (const auto& p : paths) {
                    if (IsNetRoot(p)) labels.push_back(L"Ağ");
                    else if (IsFtp(p)) {
                        const FtpUrl u = ParseFtp(p);
                        std::wstring r = u.path;
                        while (r.size() > 1 && r.back() == L'/') r.pop_back();
                        labels.push_back(r.size() <= 1 ? u.host : r.substr(r.find_last_of(L'/') + 1));
                    }
                    else if (p.size() <= 3) labels.push_back(p.substr(0, 2));
                    else if (p[0] == L'\\' && p.find(L'\\', 2) == std::wstring::npos) labels.push_back(p);
                    else labels.push_back(p.substr(p.find_last_of(L'\\') + 1));
                }
            }
            std::vector<float> widths;
            auto measure = [&](const std::wstring& t, int weight) {
                ComPtr<IDWriteTextLayout> lay; float w = 40;
                if (SUCCEEDED(m_dcomp->GetDWriteFactory()->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), Fmt(13.0f, weight, 0, false), 4000, 30, &lay)) && lay) {
                    DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm); w = tm.width;
                }
                return w + 18;
            };
            for (size_t i = 0; i < labels.size(); ++i) widths.push_back(measure(labels[i], i + 1 == labels.size() ? 600 : 400));
            size_t first = 0; float total = 0;
            for (size_t i = labels.size(); i-- > 0;) { if (total + widths[i] + 16 > tw - 20 && i + 1 < labels.size()) { first = i + 1; break; } total += widths[i] + 16; }
            float x = tx - 8;
            if (first > 0) { Txt(L"…", Rect{x, r.y, 20, h}, 13.0f, 400, muted); x += 20; }
            for (size_t i = first; i < labels.size(); ++i) {
                const Rect cr{x, r.y + 5, widths[i], h - 10};
                const int id = H_Crumb + static_cast<int>(i);
                FillRR(cr, 9, WithA(vl, 0.20f * Hover(id)));
                const bool last = i + 1 == labels.size();
                Txt(labels[i], Rect{cr.x + 9, cr.y, cr.width - 8, cr.height}, 13.0f, last ? 600 : 400, last ? kWhite : text);
                if (!last) Hit(cr, id);
                x += widths[i];
                if (!last) {
                    const Rect sp{x, r.y + 5, 16, h - 10};
                    FillRR(sp, 8, WithA(vl, 0.20f * Hover(H_CrumbSep + static_cast<int>(i))));
                    Glyph(bui::kGlyphChevronRight, Rect{x, r.y, 16, h}, 9.0f, WithA(muted, 0.8f));
                    Hit(sp, H_CrumbSep + static_cast<int>(i));
                    x += 16;
                }
            }
        }
    }
    // search pill
    {
        const Rect r{W - searchW - 18, y0, searchW, h};
        Card(r, h * 0.5f, m_edit == E_Search ? 0.7f : Hover(H_Search) * 0.5f, 0.9f);
        Glyph(bui::kGlyphSearch, Rect{r.x + 8, r.y, 32, h}, 14.0f, text);
        const std::wstring& shown = m_edit == E_Search ? m_editText : m_search;
        if (shown.empty()) Txt(m_home ? L"Bu bilgisayarda ara..." : L"Bu klasörde ara...", Rect{r.x + 44, r.y, r.width - 56, h}, 12.5f, 400, WithA(muted, 0.95f));
        else Txt(shown, Rect{r.x + 44, r.y, r.width - 56, h}, 13.0f, 400, text);
        if (m_edit == E_Search && (GetTickCount64() / 500) % 2 == 0) {
            ComPtr<IDWriteTextLayout> lay; float tw = 0;
            if (SUCCEEDED(m_dcomp->GetDWriteFactory()->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()), Fmt(13.0f, 400, 0, false), 4000, 30, &lay)) && lay) {
                DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm); tw = tm.widthIncludingTrailingWhitespace;
            }
            FillRR(Rect{r.x + 44 + std::min(tw, r.width - 60), r.y + 10, 1.5f, h - 20}, 0.5f, vl);
        }
        Hit(r, H_Search);
    }
    if (m_loading) {
        const float t = static_cast<float>(GetTickCount64() % 1200) / 1200.0f;
        const float bw = (W - kSideW) * 0.25f, bx = kSideW + t * ((W - kSideW) + bw) - bw;
        if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, WithA(vl, 0)}, {0.5f, WithA(vl, 0.9f)}, {1.0f, WithA(vl, 0)}}, D2D1::Point2F(bx, 0), D2D1::Point2F(bx + bw, 0)))
            m_ctx->FillRectangle(D2D1::RectF(std::max(bx, kSideW), kTitleH + kToolbarH - 2, std::min(bx + bw, W), kTitleH + kToolbarH), g.Get());
    }
}

void Explorer::DrawScrollbar(const Rect& track, float contentH, float viewH) {
    m_scrollTrack = track;
    if (contentH <= viewH + 1) return;
    const float thumb = std::max(30.0f, track.height * viewH / contentH);
    const float range = track.height - thumb;
    const float frac = std::clamp(m_scroll / (contentH - viewH), 0.0f, 1.0f);
    const float hv = std::max(Hover(H_Scroll), m_dragScroll ? 1.0f : 0.0f);
    const float w = 4.0f + 3.0f * hv;
    const Rect t{track.Right() - w - 2, track.y + frac * range, w, thumb};
    FillRR(t, w * 0.5f, WithA(render::glass::kVioletLight, 0.28f + 0.35f * hv));
    Hit(Rect{track.Right() - 14, track.y, 14, track.height}, H_Scroll);
}

// ---- home -----------------------------------------------------------------------------------------------------------
void Explorer::DrawHome(float W, float H) {
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight, muted = render::glass::kMuted;
    const float top = kTitleH + kToolbarH;
    const bool rightPane = (W - kSideW) >= 1000;
    const float rightW = 300;
    const float mainX = kSideW, mainW = W - kSideW - (rightPane ? rightW + 18 : 0);
    const Rect view{mainX, top, mainW, H - top};
    m_viewH = view.height;
    m_clip = view;
    m_ctx->PushAxisAlignedClip(D(view), D2D1_ANTIALIAS_MODE_ALIASED);

    const float x = mainX + 18, iw = mainW - 36 - 8;
    const float y0 = top + 6 - m_scroll;
    float y = y0;
    auto section = [&](const wchar_t* title, wchar_t glyph) {
        Glyph(glyph, Rect{x + 2, y, 22, 26}, 15.0f, Mix(vl, kWhite, 0.15f));
        Txt(title, Rect{x + 30, y, 360, 26}, 15, 600, kWhite);
        y += 38;
    };

    // ---- welcome banner (dark glass with a violet hill of light)
    {
        const Rect b{x, y, iw, 112};
        Card(b, 20, 0.0f, 1.0f);
        if (!m_wave || std::fabs(m_waveW - b.width) > 1 || std::fabs(m_waveH - b.height) > 1) {
            m_waveW = b.width; m_waveH = b.height; m_wave.Reset(); m_waveFill.Reset();
            auto curve = [&](ID2D1GeometrySink* sink, float dy, bool fill) {
                const float w = b.width, h = b.height;
                sink->BeginFigure(D2D1::Point2F(w * 0.30f, h + dy), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(w * 0.47f, h * 0.98f + dy), D2D1::Point2F(w * 0.56f, h * 0.14f + dy), D2D1::Point2F(w * 0.665f, h * 0.20f + dy)));
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(w * 0.75f, h * 0.24f + dy), D2D1::Point2F(w * 0.80f, h * 0.70f + dy), D2D1::Point2F(w * 0.89f, h * 0.80f + dy)));
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(w * 0.93f, h * 0.85f + dy), D2D1::Point2F(w * 0.97f, h * 0.88f + dy), D2D1::Point2F(w + 4, h * 0.90f + dy)));
                if (fill) { sink->AddLine(D2D1::Point2F(w + 4, h + 30)); sink->AddLine(D2D1::Point2F(w * 0.30f, h + 30)); }
                sink->EndFigure(fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            };
            ComPtr<ID2D1PathGeometry> pf, pl;
            auto* fac = m_dcomp->GetD2DFactory();
            if (SUCCEEDED(fac->CreatePathGeometry(&pf))) { ComPtr<ID2D1GeometrySink> sk; if (SUCCEEDED(pf->Open(&sk))) { curve(sk.Get(), 0, true); sk->Close(); m_waveFill = pf; } }
            if (SUCCEEDED(fac->CreatePathGeometry(&pl))) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(pl->Open(&sk))) { curve(sk.Get(), 0, false); curve(sk.Get(), 7, false); curve(sk.Get(), 15, false); sk->Close(); m_wave = pl; }
            }
        }
        if (m_wave && m_waveFill) {
            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            m_dcomp->GetD2DFactory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D(b), 20, 20), &clipGeo);
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D(b), clipGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
            m_ctx->PushLayer(&lp, nullptr);
            D2D1_MATRIX_3X2_F old; m_ctx->GetTransform(&old);
            m_ctx->SetTransform(D2D1::Matrix3x2F::Translation(b.x, b.y) * old);
            if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(Mix(vl, v, 0.3f), 0.95f), WithA(render::glass::kVioletDeep, 0.10f), D2D1::Point2F(0, b.height * 0.15f), D2D1::Point2F(0, b.height)))
                m_ctx->FillGeometry(m_waveFill.Get(), g.Get());
            if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, WithA(vl, 0.0f)}, {0.35f, WithA(vl, 0.55f)}, {0.7f, Color{1.0f, 0.95f, 1.0f, 0.85f}}, {1.0f, WithA(vl, 0.4f)}},
                                                            D2D1::Point2F(b.width * 0.30f, 0), D2D1::Point2F(b.width, 0)))
                m_ctx->DrawGeometry(m_wave.Get(), g.Get(), 1.3f);
            m_ctx->SetTransform(old);
            m_ctx->PopLayer();
        }
        Txt(L"Hoş geldin, " + (m_homeData.userName.empty() ? std::wstring(L"Kullanıcı") : m_homeData.userName), Rect{b.x + 26, b.y + 24, 520, 34}, 20, 600, kWhite);
        Txt(L"Dosyalarına hızlıca eriş, yönet ve düzenle.", Rect{b.x + 26, b.y + 58, 520, 22}, 12.5f, 400, muted);
        struct A { const wchar_t* label; wchar_t glyph; };
        const A acts[] = {{L"Yeni Klasör", bui::kGlyphNewFolder}, {L"Dosya Yükle", 0xE898}, {L"Sıkıştır", 0xF012}, {L"Ayarlar", bui::kGlyphSettings}};
        const float bwid = 78, bh = 66, gap = 14;
        int n = 4;
        while (n > 0 && b.width - 440 < n * (bwid + gap)) --n;
        for (int i = 0; i < n; ++i) {
            const Rect r{b.Right() - 22 - (n - i) * (bwid + gap) + gap, b.y + (b.height - bh) / 2, bwid, bh};
            const int id = H_Banner + i;
            Card(r, 14, Hover(id), 1.1f);
            Glyph(acts[i].glyph, Rect{r.x, r.y + 8, r.width, 26}, 18.0f, Mix(vl, kWhite, 0.45f));
            Txt(acts[i].label, Rect{r.x + 2, r.y + 38, r.width - 4, 20}, 11.0f, 500, kWhite, 1);
            Hit(r, id);
        }
        y += b.height + 26;
    }

    // ---- quick access
    m_quickSectionY = y - y0 - 4;
    section(L"Hızlı Erişim", 0xE735);
    {
        const int cols = iw >= 820 ? 6 : (iw >= 560 ? 3 : 2);
        const float gap = 16, tw = (iw - gap * (cols - 1)) / cols, th = 114;
        for (size_t i = 0; i < m_quick.size(); ++i) {
            const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
            const Rect r{x + col * (tw + gap), y + row * (th + gap), tw, th};
            const int id = H_QuickTile + static_cast<int>(i);
            const float hv = Hover(id);
            Card(r, 16, hv);
            Icon(static_cast<int>(i), Rect{r.x + 16, r.y + 14, 38, 36});
            Txt(m_quick[i].name, Rect{r.x + 16, r.y + 64, r.width - 24, 22}, 13.5f, 600, kWhite);
            wchar_t b[48];
            if (m_quick[i].count >= 0) swprintf(b, 48, L"%d öğe", m_quick[i].count); else swprintf(b, 48, L"—");
            Txt(b, Rect{r.x + 16, r.y + 85, r.width - 24, 18}, 11.5f, 400, muted);
            Hit(r, id);
        }
        const int rows = (static_cast<int>(m_quick.size()) + cols - 1) / cols;
        y += rows * (th + gap) + 8;
    }

    // ---- drives
    section(L"Cihazlar ve Sürücüler", 0xE735);
    {
        const int cols = iw >= 900 ? 3 : (iw >= 600 ? 2 : 1);
        const float gap = 16, cw2 = (iw - gap * (cols - 1)) / cols, ch = 86;
        for (size_t i = 0; i < m_homeData.drives.size(); ++i) {
            const auto& d = m_homeData.drives[i];
            const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
            const Rect r{x + col * (cw2 + gap), y + row * (ch + gap), cw2, ch};
            const int id = H_DriveCard + static_cast<int>(i);
            const float hv = Hover(id);
            const bool sys = !d.root.empty() && towupper(d.root[0]) == m_sysDrive;
            Card(r, 16, hv, sys ? 1.25f : 1.0f);
            if (sys) FillRR(r, 16, WithA(v, 0.10f));
            if (sys) Icon(K_Windows, Rect{r.x + 16, r.y + 22, 40, 40}); else Icon(K_Drive, Rect{r.x + 14, r.y + 24, 44, 38});
            Txt(d.label, Rect{r.x + 76, r.y + 11, r.width - 120, 24}, 13.5f, 600, kWhite);
            Glyph(0xE712, Rect{r.Right() - 38, r.y + 11, 26, 24}, 14.0f, Mix(muted, kWhite, 0.3f));
            const uint64_t used = d.total > d.freeBytes ? d.total - d.freeBytes : 0;
            const float frac = d.total ? static_cast<float>(static_cast<double>(used) / d.total) : 0.0f;
            ProgressBar(Rect{r.x + 76, r.y + 40, r.width - 92, 7}, frac, false);
            Txt(d.total ? FormatGB(d.freeBytes) + L" boş, " + FormatGB(d.total) + L" toplam" : L"Kullanılamıyor", Rect{r.x + 76, r.y + 52, r.width - 92, 22}, 11.5f, 400, muted);
            Hit(r, id);
        }
        const int rows = std::max(1, (static_cast<int>(m_homeData.drives.size()) + cols - 1) / cols);
        y += rows * (ch + gap) + 8;
    }

    // ---- recent files (plain rows with hairline separators, like the concept)
    Glyph(0xE823, Rect{x + 2, y, 22, 26}, 15.0f, Mix(vl, kWhite, 0.15f));
    Txt(L"Son Dosyalar", Rect{x + 30, y, 300, 26}, 15, 600, kWhite);
    {
        const Rect lnk{x + iw - 140, y, 140, 26};
        Txt(L"Tümünü Gör", Rect{lnk.x, lnk.y, lnk.width - 24, 26}, 12, 400, Mix(muted, kWhite, 0.4f + 0.4f * Hover(H_RecentAll)), 2);
        Glyph(0xE8A7, Rect{lnk.Right() - 22, lnk.y, 20, 26}, 11.0f, Mix(muted, kWhite, 0.4f));
        Hit(lnk, H_RecentAll);
    }
    y += 38;
    {
        const size_t n = std::min<size_t>(m_homeData.recent.size(), 6);
        const float rh = 40;
        if (n == 0) {
            Card(Rect{x, y, iw, 64}, 16, 0);
            Txt(m_homeLoaded ? L"Henüz son dosya yok" : L"Yükleniyor…", Rect{x, y, iw, 64}, 13, 400, muted, 1);
            y += 72;
        } else {
            for (size_t i = 0; i < n; ++i) {
                const auto& rcn = m_homeData.recent[i];
                const Rect r{x + 4, y + i * rh, iw - 8, rh};
                const int id = H_RecentRow + static_cast<int>(i);
                FillRR(Rect{r.x, r.y + 1, r.width, r.height - 2}, 10, WithA(vl, 0.12f * Hover(id)));
                if (i) { m_brush->SetColor(DC(vl, 0.09f)); m_ctx->FillRectangle(D2D1::RectF(r.x + 6, r.y, r.Right() - 6, r.y + 1), m_brush.Get()); }
                FileEntry fe; fe.ext = rcn.ext; fe.name = rcn.name;
                DrawFileGlyph(fe, rcn.path, Rect{r.x + 14, r.y + 7, 26, 26});
                Txt(rcn.name, Rect{r.x + 52, r.y, r.width * 0.36f, rh}, 13, 400, kWhite);
                Txt(rcn.dir, Rect{r.x + r.width * 0.43f, r.y, r.width * 0.34f, rh}, 12, 400, Mix(muted, kWhite, 0.25f));
                Txt(FormatRelative(rcn.t), Rect{r.Right() - 140, r.y, 124, rh}, 12, 400, Mix(muted, kWhite, 0.25f), 2);
                Hit(r, id);
            }
            y += n * rh + 8;
        }
    }
    m_contentH = y - y0 + 20;
    m_ctx->PopAxisAlignedClip();
    m_scrollTarget = std::clamp(m_scrollTarget, 0.0f, std::max(0.0f, m_contentH - m_viewH));
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, m_contentH - m_viewH));
    m_clip = Rect{0, 0, W, H};
    DrawScrollbar(Rect{view.Right() - 10, top + 4, 10, view.height - 8}, m_contentH, m_viewH);
    if (rightPane) DrawHomeRight(Rect{W - rightW - 16, top + 4, rightW, H - top - 14});
}

void Explorer::DrawHomeRight(const Rect& pane) {
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight, text = render::glass::kText, muted = render::glass::kMuted;
    m_clip = pane;
    m_ctx->PushAxisAlignedClip(D(pane), D2D1_ANTIALIAS_MODE_ALIASED);
    Card(pane, 20, 0.0f, 1.0f);
    const float cx = pane.x + pane.width * 0.5f;
    float y = pane.y;

    // monitor with a swirl of light
    {
        const Rect scr{cx - 50, y + 20, 100, 66};
        if (auto g = m_dcomp->CreateLinearGradientBrush(Color{0.10f, 0.04f, 0.22f, 1.0f}, Color{0.34f, 0.12f, 0.62f, 1.0f}, D2D1::Point2F(scr.x, scr.y), D2D1::Point2F(scr.Right(), scr.Bottom())))
            m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(scr), 6, 6), g.Get());
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        m_dcomp->GetD2DFactory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D(scr), 6, 6), &geo);
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D(scr), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
        m_ctx->PushLayer(&lp, nullptr);
        Swirl(Rect{scr.x - 2, scr.y + 8, scr.width + 4, scr.height - 12}, 2.2f, Color{0.95f, 0.80f, 1.0f, 0.95f});
        Swirl(Rect{scr.x - 2, scr.y + 14, scr.width + 4, scr.height - 12}, 1.2f, Color{0.75f, 0.50f, 1.0f, 0.6f});
        m_ctx->PopLayer();
        StrokeRR(scr, 6, WithA(vl, 0.85f), 1.8f);
        FillRR(Rect{cx - 7, scr.Bottom() + 1, 14, 8}, 1.5f, WithA(vl, 0.55f));
        FillRR(Rect{cx - 24, scr.Bottom() + 8, 48, 4}, 2.0f, WithA(vl, 0.75f));
        y += 112;
    }
    Txt(L"Bu Bilgisayar", Rect{pane.x, y, pane.width, 26}, 16, 700, kWhite, 1);
    y += 36;
    const std::wstring* vals[] = {&m_homeData.cpu, &m_homeData.gpu, &m_homeData.ram};
    for (int i = 0; i < 3; ++i) {
        ChipIcon(i, Rect{pane.x + 20, y + 2, 22, 20}, Mix(muted, kWhite, 0.5f));
        Txt(vals[i]->empty() ? std::wstring(L"…") : *vals[i], Rect{pane.x + 54, y, pane.width - 70, 24}, 12.5f, 400, kWhite);
        y += 28;
    }
    y += 8;

    // storage
    uint64_t tot = 0, fre = 0;
    std::vector<const DriveInfo*> drives;
    for (const auto& d : m_homeData.drives) if (d.total && (d.type == DRIVE_FIXED || d.type == DRIVE_REMOVABLE)) { drives.push_back(&d); if (d.type == DRIVE_FIXED) { tot += d.total; fre += d.freeBytes; } }
    {
        const size_t n = std::min<size_t>(drives.size(), 3);
        const Rect sp{pane.x + 10, y, pane.width - 20, 58.0f + std::max<size_t>(n, 1) * 46.0f};
        Card(sp, 16, 0.0f, 0.55f);
        const int pct = tot ? static_cast<int>((tot - fre) * 100.0 / tot + 0.5) : 0;
        Txt(L"Depolama", Rect{sp.x + 14, sp.y + 10, 120, 26}, 14.5f, 700, kWhite);
        wchar_t b[48]; swprintf(b, 48, L"%%%d Kullanılıyor", pct);
        Txt(b, Rect{sp.x + 120, sp.y + 10, sp.width - 134, 26}, 11.5f, 400, Mix(muted, kWhite, 0.4f), 2);
        for (size_t i = 0; i < n; ++i) {
            const auto& d = *drives[i];
            const float ly = sp.y + 46 + i * 46.0f;
            Icon(K_Drive, Rect{sp.x + 12, ly + 4, 28, 26});
            Txt(d.label, Rect{sp.x + 54, ly, sp.width - 160, 20}, 12, 600, kWhite);
            Txt(FormatGB(d.total - d.freeBytes) + L" / " + FormatGB(d.total), Rect{sp.x + sp.width - 130, ly, 118, 20}, 11, 400, Mix(muted, kWhite, 0.3f), 2);
            const uint64_t used = d.total - d.freeBytes;
            ProgressBar(Rect{sp.x + 54, ly + 24, sp.width - 68, 6}, static_cast<float>(static_cast<double>(used) / d.total), false);
        }
        y += sp.height + 16;
    }

    // quick tools
    Txt(L"Hızlı Araçlar", Rect{pane.x + 20, y, 220, 26}, 14.5f, 700, kWhite);
    y += 34;
    {
        struct T { const wchar_t* label; wchar_t glyph; };
        const T tools[] = {{L"Denetim Masası", 0xE9E9}, {L"Görev Yöneticisi", 0xE9D9}, {L"Ayarlar", bui::kGlyphSettings}, {L"Sistem Bilgisi", bui::kGlyphInfo}};
        const float gap = 10, gw = (pane.width - 20 - gap) / 2, gh = 66;
        for (int i = 0; i < 4; ++i) {
            const Rect r{pane.x + 10 + (i % 2) * (gw + gap), y + (i / 2) * (gh + gap), gw, gh};
            const int id = H_Tool + i;
            Card(r, 14, Hover(id), 0.9f);
            Glyph(tools[i].glyph, Rect{r.x, r.y + 8, r.width, 26}, 18.0f, Mix(vl, kWhite, 0.45f));
            Txt(tools[i].label, Rect{r.x + 4, r.y + 38, r.width - 8, 22}, 11.5f, 600, kWhite, 1);
            Hit(r, id);
        }
    }
    m_ctx->PopAxisAlignedClip();
    (void)v; (void)text;
}

void Explorer::DrawPreview(const Rect& pane) {
    const Color vl = render::glass::kVioletLight, muted = render::glass::kMuted;
    m_clip = pane;
    m_ctx->PushAxisAlignedClip(D(pane), D2D1_ANTIALIAS_MODE_ALIASED);
    Card(pane, 20, 0.0f, 1.0f);
    const float cx = pane.x + pane.width * 0.5f;
    float y = pane.y + 18;

    std::vector<int> picked;
    for (size_t i = 0; i < m_sel.size(); ++i) if (m_sel[i]) picked.push_back(static_cast<int>(i));
    auto row = [&](const wchar_t* k, const std::wstring& v) {
        Txt(k, Rect{pane.x + 18, y, 96, 22}, 11.5f, 500, Mix(muted, kWhite, 0.1f));
        Txt(v, Rect{pane.x + 112, y, pane.width - 128, 22}, 12, 400, kWhite);
        y += 26;
    };

    if (picked.size() == 1) {
        const FileEntry& e = m_items[m_view[picked[0]]];
        const std::wstring full = FullPath(e);
        const Rect box{cx - 80, y, 160, 130};
        bool drawn = false;
        if (!e.dir && IsThumbExt(e.ext)) {
            if (auto* bmp = Thumb(full, 320)) {
                const D2D1_SIZE_F sz = bmp->GetSize();
                const float sc = std::min(box.width / sz.width, box.height / sz.height);
                const Rect tr{cx - sz.width * sc * 0.5f, box.y + (box.height - sz.height * sc) * 0.5f, sz.width * sc, sz.height * sc};
                m_ctx->DrawBitmap(bmp, D(tr), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                StrokeRR(tr, 4, WithA(vl, 0.3f), 1.0f);
                drawn = true;
            }
        }
        if (!drawn) DrawFileGlyph(e, full, Rect{cx - 48, box.y + 17, 96, 96});
        y += 144;
        TxtWrap(e.name, Rect{pane.x + 14, y, pane.width - 28, 44}, 14, 600, kWhite);
        y += 48;
        Txt(e.dir ? L"Dosya klasörü" : TypeName(e), Rect{pane.x + 14, y, pane.width - 28, 20}, 12, 400, muted, 1);
        y += 30;
        m_brush->SetColor(DC(vl, 0.12f)); m_ctx->FillRectangle(D2D1::RectF(pane.x + 18, y, pane.Right() - 18, y + 1), m_brush.Get()); y += 12;
        if (e.dir) {
            if (m_infoPath != full && !IsFtp(full)) StartInfo(full);
            row(L"İçerik", m_infoBusy ? std::wstring(L"hesaplanıyor…") : (std::to_wstring(std::max(0, m_infoCount)) + L" öğe"));
            row(L"Boyut", m_infoBusy ? std::wstring(L"hesaplanıyor…") : FormatSize(m_infoSize));
        } else row(L"Boyut", FormatSize(e.size));
        row(L"Değiştirme", FormatDate(e.mtime));
        row(L"Oluşturma", FormatDate(e.ctime));
        std::wstring at;
        if (e.attr & FILE_ATTRIBUTE_READONLY) at += L"Salt okunur  ";
        if (e.attr & FILE_ATTRIBUTE_HIDDEN) at += L"Gizli  ";
        if (e.attr & FILE_ATTRIBUTE_SYSTEM) at += L"Sistem  ";
        if (e.attr & FILE_ATTRIBUTE_ARCHIVE) at += L"Arşiv";
        row(L"Öznitelik", at.empty() ? std::wstring(L"—") : at);
        Txt(L"Konum", Rect{pane.x + 18, y, 96, 22}, 11.5f, 500, Mix(muted, kWhite, 0.1f));
        y += 22;
        TxtWrap(e.parent.empty() ? m_path : e.parent, Rect{pane.x + 14, y, pane.width - 28, 60}, 11.5f, 400, Mix(muted, kWhite, 0.4f));
    } else if (picked.size() > 1) {
        uint64_t sz = 0; int dirs = 0;
        for (int i : picked) { const FileEntry& e = m_items[m_view[i]]; if (e.dir) ++dirs; else sz += e.size; }
        Glyph(bui::kGlyphFolder, Rect{cx - 40, y + 10, 80, 80}, 54.0f, WithA(vl, 0.8f));
        y += 110;
        Txt(std::to_wstring(picked.size()) + L" öğe seçildi", Rect{pane.x + 14, y, pane.width - 28, 28}, 15, 600, kWhite, 1);
        y += 40;
        row(L"Klasör", std::to_wstring(dirs));
        row(L"Dosya", std::to_wstring(picked.size() - dirs));
        row(L"Toplam boyut", FormatSize(sz));
    } else {
        Icon(15, Rect{cx - 44, y + 8, 88, 80});
        y += 104;
        std::wstring nm = m_path.empty() ? L"Bu Bilgisayar" : m_path;
        const size_t sl = nm.find_last_of(L'\\');
        if (nm.size() > 3 && sl != std::wstring::npos) nm = nm.substr(sl + 1);
        TxtWrap(nm, Rect{pane.x + 14, y, pane.width - 28, 44}, 15, 600, kWhite);
        y += 52;
        row(L"Öğe sayısı", std::to_wstring(m_view.size()));
        if (m_path.size() >= 2 && m_path[1] == L':') {
            ULARGE_INTEGER fr{}, tot{}, tf{};
            if (GetDiskFreeSpaceExW(m_path.substr(0, 3).c_str(), &fr, &tot, &tf)) {
                row(L"Boş alan", FormatGB(fr.QuadPart));
                row(L"Toplam", FormatGB(tot.QuadPart));
            }
        }
        Txt(L"Ayrıntı için bir öğe seç", Rect{pane.x + 14, pane.Bottom() - 40, pane.width - 28, 24}, 11.5f, 400, WithA(muted, 0.8f), 1);
    }
    m_ctx->PopAxisAlignedClip();
}

// ---- folder view --------------------------------------------------------------------------------------------------
void Explorer::DrawFolder(float W, float H) {
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight, text = render::glass::kText, muted = render::glass::kMuted;
    const float top = kTitleH + kToolbarH;
    const float prevW = (m_preview && (W - kSideW) > 880) ? 316.0f : 0.0f;
    const float x0 = kSideW + 20, pw = W - kSideW - 40 - prevW;
    const bool wide = (W - kSideW) > 1000;

    // command bar
    {
        m_clip = Rect{kSideW, top, W - kSideW, kCmdH};
        struct C { int id; wchar_t glyph; const wchar_t* label; bool en; };
        const bool any = std::any_of(m_sel.begin(), m_sel.end(), [](char c) { return c != 0; });
        const bool one = std::count(m_sel.begin(), m_sel.end(), 1) == 1;
        const bool canPaste = IsClipboardFormatAvailable(CF_HDROP) != 0 && !m_deep;
        const C cmds[] = {{H_CmdNew, bui::kGlyphNewFolder, L"Yeni", !m_deep}, {H_CmdCut, bui::kGlyphCut, L"Kes", any}, {H_CmdCopy, bui::kGlyphCopy, L"Kopyala", any},
                          {H_CmdPaste, bui::kGlyphPaste, L"Yapıştır", canPaste}, {H_CmdRename, bui::kGlyphRename, L"Ad değiştir", one}, {H_CmdDelete, bui::kGlyphDelete, L"Sil", any}};
        float x = x0;
        for (const auto& c : cmds) {
            const float bw = wide ? (c.id == H_CmdRename ? 108.0f : 92.0f) : 40.0f;
            const Rect r{x, top + 2, bw, 36};
            const float hv = c.en ? Hover(c.id) : 0.0f;
            FillRR(r, 11, WithA(c.id == H_CmdDelete && c.en ? Color{1.0f, 0.35f, 0.4f, 1.0f} : vl, 0.17f * hv));
            const Color col = !c.en ? WithA(muted, 0.35f) : (c.id == H_CmdDelete ? Color{1.0f, 0.55f, 0.58f, 1.0f} : text);
            if (wide) {
                Glyph(c.glyph, Rect{r.x + 6, r.y, 28, r.height}, 14.0f, col);
                Txt(c.label, Rect{r.x + 34, r.y, r.width - 38, r.height}, 12.5f, 400, col);
            } else Glyph(c.glyph, r, 14.0f, col);
            if (c.en) Hit(r, c.id);
            x += bw + 4;
        }
        // right group: sort | details | icons | preview
        float rx = W - 20;
        auto rbtn = [&](int id, wchar_t g, bool on) {
            const Rect r{rx - 36, top + 2, 36, 36};
            rx -= 38;
            FillRR(r, 11, WithA(vl, on ? 0.28f : 0.17f * Hover(id)));
            if (on) StrokeRR(r, 11, WithA(vl, 0.4f));
            Glyph(g, r, 14.0f, on ? kWhite : text);
            Hit(r, id);
        };
        rbtn(H_CmdPreview, 0xE8A1, prevW > 0);
        rbtn(H_CmdGrid, 0xE80A, m_grid);
        rbtn(H_CmdDetails, bui::kGlyphView, !m_grid);
        const float sw = wide ? 128.0f : 40.0f;
        const Rect sr{rx - sw - 4, top + 2, sw, 36};
        FillRR(sr, 11, WithA(vl, 0.17f * Hover(H_CmdSort)));
        Glyph(bui::kGlyphSort, Rect{sr.x + (wide ? 6.0f : 0.0f), sr.y, wide ? 28.0f : sr.width, sr.height}, 14.0f, text);
        if (wide) {
            const wchar_t* names[] = {L"Ad", L"Tarih", L"Tür", L"Boyut"};
            Txt(std::wstring(L"Sırala: ") + names[static_cast<int>(m_sortKey)], Rect{sr.x + 34, sr.y, sr.width - 36, sr.height}, 12.5f, 400, text);
        }
        Hit(sr, H_CmdSort);
    }

    const float listTop = top + kCmdH;
    const float colSize = 100, colDate = 160, colType = pw > 760 ? (m_deep ? 260.0f : 150.0f) : 0.0f;
    const float nameW = pw - colSize - colDate - colType - 24;
    if (!m_grid) {
        m_clip = Rect{kSideW, listTop, W - kSideW, kHeadH};
        const Rect hr{x0, listTop, pw, kHeadH - 4};
        FillRR(hr, 11, Color{0, 0, 0, 0.20f});
        struct Cc { SortKey k; const wchar_t* t; float x, w; int al; };
        std::vector<Cc> cols = {{SortKey::Name, L"Ad", x0 + 56, nameW - 40, 0}, {SortKey::Date, L"Değiştirme tarihi", x0 + 16 + nameW, colDate, 0}};
        if (colType > 0) cols.push_back({SortKey::Type, m_deep ? L"Konum" : L"Tür", x0 + 16 + nameW + colDate, colType, 0});
        cols.push_back({SortKey::Size, L"Boyut", x0 + pw - colSize - 12, colSize, 2});
        for (const auto& c : cols) {
            const int id = H_ColHead + static_cast<int>(c.k);
            const float hv = Hover(id);
            const bool act = m_sortKey == c.k;
            Rect r{c.x - 8, hr.y, c.w + 16, hr.height};
            FillRR(r, 9, WithA(vl, 0.14f * hv));
            Txt(c.t, Rect{c.x, hr.y, c.w - 16, hr.height}, 12, act ? 600 : 500, act ? kWhite : muted, c.al);
            if (act) Glyph(m_sortAsc ? 0xE70E : 0xE70D, Rect{c.al == 2 ? c.x - 4 : c.x + c.w - 18, hr.y, 16, hr.height}, 8.0f, vl);
            Hit(r, id);
        }
    }

    // list geometry
    const float rowsTop = m_grid ? listTop + 4 : listTop + kHeadH;
    const Rect view{kSideW, rowsTop, W - kSideW - prevW, H - rowsTop - kStatusH};
    m_lx = x0; m_lw = pw; m_ly = rowsTop;
    if (m_grid) { m_cellW = m_iconSize + 36; m_cellH = m_iconSize + 64; m_cols = std::max(1, static_cast<int>((pw - 14) / m_cellW)); }
    else { m_cols = 1; m_cellH = kRowH; }
    const int n = static_cast<int>(m_view.size());
    const int rowsN = (n + m_cols - 1) / m_cols;
    m_viewH = view.height;
    m_contentH = rowsN * m_cellH + 8;
    m_scrollTarget = std::clamp(m_scrollTarget, 0.0f, std::max(0.0f, m_contentH - m_viewH));
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, m_contentH - m_viewH));
    m_listRect = view;
    m_clip = view;
    m_ctx->PushAxisAlignedClip(D(view), D2D1_ANTIALIAS_MODE_ALIASED);

    if (n == 0) {
        const wchar_t* msg = m_loading ? (m_deep || !m_search.empty() ? L"Aranıyor…" : L"Yükleniyor…") : (m_loadFailed ? L"Bu konuma erişilemedi" : (m_search.empty() ? L"Bu klasör boş" : L"Eşleşen öğe yok"));
        Glyph(m_loadFailed ? bui::kGlyphLock : bui::kGlyphFolder, Rect{view.x, view.y + 50, view.width - prevW * 0, 60}, 40.0f, WithA(vl, 0.5f));
        Txt(msg, Rect{view.x, view.y + 118, view.width, 28}, 15, 500, muted, 1);
    }
    const int firstRow = std::max(0, static_cast<int>(std::floor(m_scroll / m_cellH)));
    const int lastRow = static_cast<int>(std::ceil((m_scroll + m_viewH) / m_cellH));
    const int first = firstRow * m_cols, last = std::min(n - 1, (lastRow + 1) * m_cols - 1);
    for (int i = first; i <= last; ++i) {
        const FileEntry& e = m_items[m_view[i]];
        const Rect r = CellRect(i);
        const int id = H_Row + i;
        const float hv = Hover(id);
        const bool sel = i < static_cast<int>(m_sel.size()) && m_sel[i];
        const bool dropT = m_dropRow == i;
        const float rad = m_grid ? 14.0f : 11.0f;
        if (sel) {
            if (auto g = m_dcomp->CreateLinearGradientBrush(WithA(vl, 0.30f), WithA(render::glass::kVioletDeep, 0.17f), D2D1::Point2F(r.x, r.y), D2D1::Point2F(m_grid ? r.x : r.Right(), m_grid ? r.Bottom() : r.y)))
                m_ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), rad, rad), g.Get());
            StrokeRR(r, rad, WithA(vl, 0.40f));
            if (!m_grid) FillRR(Rect{r.x + 1, r.y + 8, 3, r.height - 16}, 1.5f, vl);
        } else FillRR(r, rad, WithA(vl, 0.12f * hv));
        if (dropT) { FillRR(r, rad, WithA(vl, 0.22f)); StrokeRR(r, rad, vl, 2.0f); }
        const std::wstring full = FullPath(e);
        const bool cutRow = m_clipCut && std::find(m_clipPaths.begin(), m_clipPaths.end(), full) != m_clipPaths.end();
        if (cutRow) m_ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.45f), nullptr);

        if (m_grid) {
            const float S = m_iconSize;
            const Rect ic{r.x + (r.width - S) * 0.5f, r.y + 8, S, S};
            bool drew = false;
            if (!e.dir && IsThumbExt(e.ext)) {
                if (auto* bmp = Thumb(full, static_cast<int>(S * 1.5f))) {
                    const D2D1_SIZE_F sz = bmp->GetSize();
                    const float sc = std::min(S / sz.width, S / sz.height);
                    const Rect tr{ic.x + (S - sz.width * sc) * 0.5f, ic.y + (S - sz.height * sc) * 0.5f, sz.width * sc, sz.height * sc};
                    m_ctx->DrawBitmap(bmp, D(tr), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    StrokeRR(tr, 4, WithA(vl, 0.28f), 1.0f);
                    drew = true;
                }
            }
            if (!drew) DrawFileGlyph(e, full, Rect{ic.x + S * 0.08f, ic.y + S * 0.08f, S * 0.84f, S * 0.84f});
            if (m_edit == E_Rename && m_renameIndex == i) {
                const Rect er{r.x + 2, ic.Bottom() + 6, r.width - 4, 24};
                FillRR(er, 8, Color{0, 0, 0, 0.5f}); StrokeRR(er, 8, vl, 1.5f);
                if (m_editAll && !m_editText.empty()) FillRR(Rect{er.x + 4, er.y + 4, std::min(EditW(12.0f) + 4, er.width - 8), er.height - 8}, 3, WithA(vl, 0.45f));
                Txt(m_editText, Rect{er.x + 6, er.y, er.width - 12, er.height}, 12, 400, kWhite);
            } else TxtWrap(e.name, Rect{r.x + 4, ic.Bottom() + 6, r.width - 8, 36}, 12, sel ? 600 : 400, sel ? kWhite : text);
        } else {
            DrawFileGlyph(e, full, Rect{r.x + 16, r.y + 3, 28, 28});
            if (m_edit == E_Rename && m_renameIndex == i) {
                const Rect er{r.x + 52, r.y + 3, nameW - 52, r.height - 6};
                FillRR(er, 8, Color{0, 0, 0, 0.45f}); StrokeRR(er, 8, vl, 1.5f);
                if (m_editAll && !m_editText.empty()) FillRR(Rect{er.x + 6, er.y + 4, std::min(EditW(13.5f) + 4, er.width - 12), er.height - 8}, 3, WithA(vl, 0.45f));
                Txt(m_editText, Rect{er.x + 8, er.y, er.width - 16, er.height}, 13.5f, 400, kWhite);
                if ((GetTickCount64() / 500) % 2 == 0) {
                    ComPtr<IDWriteTextLayout> lay; float tw = 0;
                    if (SUCCEEDED(m_dcomp->GetDWriteFactory()->CreateTextLayout(m_editText.c_str(), static_cast<UINT32>(m_editText.size()), Fmt(13.5f, 400, 0, false), 4000, 30, &lay)) && lay) {
                        DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm); tw = tm.widthIncludingTrailingWhitespace;
                    }
                    FillRR(Rect{er.x + 8 + std::min(tw, er.width - 18), er.y + 6, 1.5f, er.height - 12}, 0.5f, vl);
                }
            } else Txt(e.name, Rect{r.x + 56, r.y, nameW - 56, r.height}, 13.5f, sel ? 600 : 400, sel ? kWhite : text);
            Txt(FormatDate(e.mtime), Rect{x0 + 16 + nameW, r.y, colDate - 8, r.height}, 12.5f, 400, muted);
            if (colType > 0) Txt(m_deep ? e.parent : TypeName(e), Rect{x0 + 16 + nameW + colDate, r.y, colType - 8, r.height}, 12.5f, 400, muted);
            if (!e.dir) Txt(FormatSize(e.size), Rect{x0 + pw - colSize - 12, r.y, colSize, r.height}, 12.5f, 400, muted, 2);
        }
        if (cutRow) m_ctx->PopLayer();
        Hit(r, id);
    }
    if (m_dropRow == -2) StrokeRR(Rect{view.x + 6, view.y + 2, view.width - 12, view.height - 4}, 14, WithA(vl, 0.7f), 2.0f);
    if (m_marquee) {
        const Rect mq{std::min(m_mqX, m_mx), std::min(m_mqY - m_scroll, m_my), std::fabs(m_mx - m_mqX), std::fabs(m_my - (m_mqY - m_scroll))};
        FillRR(mq, 4, WithA(vl, 0.14f));
        StrokeRR(mq, 4, WithA(vl, 0.65f), 1.0f);
    }
    m_ctx->PopAxisAlignedClip();
    m_clip = Rect{0, 0, W, H};
    DrawScrollbar(Rect{view.Right() - 10, view.y + 2, 10, view.height - 4}, m_contentH, m_viewH);
    if (prevW > 0) DrawPreview(Rect{W - prevW, listTop + 2, prevW - 16, H - listTop - kStatusH - 2});

    // status bar
    {
        const int selN = static_cast<int>(std::count(m_sel.begin(), m_sel.end(), 1));
        std::wstring st = (m_deep ? L"Arama sonuçları: " : L"") + std::to_wstring(m_view.size()) + L" öğe";
        if (selN) {
            uint64_t sz = 0; bool anyFile = false;
            for (size_t i = 0; i < m_view.size() && i < m_sel.size(); ++i) if (m_sel[i] && !m_items[m_view[i]].dir) { sz += m_items[m_view[i]].size; anyFile = true; }
            st += L"   ·   " + std::to_wstring(selN) + L" seçili" + (anyFile ? L"   ·   " + FormatSize(sz) : L"");
        }
        if (!m_clipPaths.empty()) st += std::wstring(L"   ·   ") + (m_clipCut ? L"Kesildi: " : L"Kopyalandı: ") + std::to_wstring(m_clipPaths.size());
        if (!m_xfer.empty()) st += L"   ·   " + m_xfer;
        Txt(st, Rect{x0 + 6, H - kStatusH, pw, kStatusH}, 12, 400, muted);
        if (m_grid) Txt(L"Ctrl + fare tekerleği: simge boyutu", Rect{x0, H - kStatusH, pw, kStatusH}, 11, 400, WithA(muted, 0.6f), 2);
    }
    (void)v;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetCurrentProcessExplicitAppUserModelID(L"Aether.Explorer");     // own taskbar identity + icon
    OleInitialize(nullptr);
    render::glass::SyncTheme();

    // AetherExplorer.exe [path] : open a folder directly
    std::wstring start;
    if (cmdLine && *cmdLine) {
        start = cmdLine;
        if (start.size() > 1 && start.front() == L'"') { start.erase(0, 1); const size_t q = start.find(L'"'); if (q != std::wstring::npos) start.erase(q); }
        if (!aether::explorer::IsVirtualPath(start) && GetFileAttributesW(start.c_str()) == INVALID_FILE_ATTRIBUTES) start.clear();
    }

    Explorer app;
    if (!app.Create(inst, start)) {
        MessageBoxW(nullptr, L"Aether Explorer penceresi oluşturulamadı.", L"Aether Explorer", MB_ICONERROR);
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
        if (app.NeedsFrame()) app.Frame(); else WaitMessage();
    }
    OleUninitialize();
    return 0;
}
