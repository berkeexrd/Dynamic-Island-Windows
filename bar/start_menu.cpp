#include "aether/bar/start_menu.hpp"
#include "aether/render/context_menu.hpp"
#include "aether/bar/ui_common.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include "aether/core/monitors.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <sddl.h>
#include <powrprof.h>
#define SECURITY_WIN32
#include <security.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <unordered_set>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;
namespace i18n = aether::i18n;

namespace {

// ---- layout constants (logical px, 100 % DPI) ----------------------------------------------------
constexpr float kPanelW = 760.0f, kPanelH = 560.0f;
constexpr float kRailW = 222.0f;
constexpr float kPaneX = 238.0f, kPaneW = 492.0f;
constexpr float kSearchY = 33.0f, kSearchH = 42.0f;
constexpr float kContentTop = 100.0f;
constexpr float kTileW = kPaneW / 6.0f, kTileH = 84.0f;
constexpr float kRowH = 46.0f;
constexpr float kPadOuter = 46.0f;                   // transparent margin around the panel for shadow / glow

// ids that are not content items
enum : int {
    kIdNavHome = 1, kIdNavApps, kIdNavDocs, kIdNavDownloads, kIdNavMusic, kIdNavPictures, kIdNavVideos,
    kIdSettings = 8, kIdPower = 9, kIdSearch = 10, kIdPin = 11, kIdAvatar = 12,
    kIdPowerBase = 21,                               // 21..25 = lock, sign out, sleep, restart, shut down
    kIdItemBase = 100,
};

inline D2D1_RECT_F RF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline Color A(const Color& c, float a) { return Color{c.r, c.g, c.b, a}; }
inline Color W(float a) { return Color{1.0f, 1.0f, 1.0f, a}; }

// Lower-case + fold Turkish letters so "fotograflar" finds "Fotoğraflar"
std::wstring Fold(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring low(s.size() * 2 + 2, L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()), low.data(), static_cast<int>(low.size()), nullptr, nullptr, 0);
    if (n <= 0) low = s; else low.resize(static_cast<size_t>(n));
    std::wstring out;
    out.reserve(low.size());
    for (wchar_t c : low) {
        switch (c) {
            case L'\u0131': out += L'i'; break;   // ı
            case L'\u0307': break;                // combining dot (from İ)
            case L'\u011F': out += L'g'; break;   // ğ
            case L'\u015F': out += L's'; break;   // ş
            case L'\u00F6': out += L'o'; break;   // ö
            case L'\u00FC': out += L'u'; break;   // ü
            case L'\u00E7': out += L'c'; break;   // ç
            case L'\u00E2': out += L'a'; break;   // â
            default: out += c;
        }
    }
    return out;
}

// ---- vector file-type icons (the concept uses coloured badges, not the generic shell icons) ------------------
enum class FileKind { None, Image, Video, Archive, Text, Code, Audio };

FileKind KindOfFile(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return FileKind::None;
    std::wstring e = name.substr(dot + 1);
    for (auto& ch : e) ch = static_cast<wchar_t>(towlower(ch));
    if (dot == 0) return FileKind::Text;   // .gitignore, .env ...
    static const wchar_t* img[] = {L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"tif", L"tiff", L"heic", L"ico"};
    static const wchar_t* vid[] = {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"m4v"};
    static const wchar_t* arc[] = {L"zip", L"rar", L"7z", L"tar", L"gz", L"cab"};
    static const wchar_t* txt[] = {L"txt", L"md", L"log", L"diff", L"patch", L"ini", L"cfg", L"csv", L"rtf"};
    static const wchar_t* cod[] = {L"sln", L"cpp", L"c", L"h", L"hpp", L"cs", L"js", L"ts", L"py", L"json", L"xml", L"html", L"css", L"cmake", L"bat", L"ps1", L"vcxproj"};
    static const wchar_t* aud[] = {L"mp3", L"wav", L"flac", L"ogg", L"m4a", L"aac"};
    auto in = [&](const wchar_t* const* list, size_t n) { for (size_t i = 0; i < n; ++i) if (e == list[i]) return true; return false; };
    if (in(img, std::size(img))) return FileKind::Image;
    if (in(vid, std::size(vid))) return FileKind::Video;
    if (in(arc, std::size(arc))) return FileKind::Archive;
    if (in(txt, std::size(txt))) return FileKind::Text;
    if (in(cod, std::size(cod))) return FileKind::Code;
    if (in(aud, std::size(aud))) return FileKind::Audio;
    return FileKind::None;
}

ComPtr<ID2D1LinearGradientBrush> VGrad(ID2D1DeviceContext* ctx, const Color& a, const Color& b, float x0, float y0, float x1, float y1) {
    D2D1_GRADIENT_STOP st[2] = {{0.0f, D2D1::ColorF(a.r, a.g, a.b, a.a)}, {1.0f, D2D1::ColorF(b.r, b.g, b.b, b.a)}};
    ComPtr<ID2D1GradientStopCollection> gc;
    ComPtr<ID2D1LinearGradientBrush> br;
    if (SUCCEEDED(ctx->CreateGradientStopCollection(st, 2, &gc)) && gc)
        ctx->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)), gc.Get(), &br);
    return br;
}

// Draws the badge inside `r` (a square). Returns false for unknown types so the caller falls back to the shell icon.
bool DrawFileBadge(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* br, FileKind kind, const Rect& r) {
    if (kind == FileKind::None) return false;
    ComPtr<ID2D1Factory> fac;
    ctx->GetFactory(&fac);
    const float u = r.width;
    auto P = [&](float fx, float fy) { return D2D1::Point2F(r.x + fx * u, r.y + fy * u); };
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts, ID2D1Brush* b) {
        if (!fac) return;
        ComPtr<ID2D1PathGeometry> g; ComPtr<ID2D1GeometrySink> sk;
        if (FAILED(fac->CreatePathGeometry(&g)) || FAILED(g->Open(&sk))) return;
        bool first = true;
        for (const auto& pt : pts) { if (first) { sk->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED); first = false; } else sk->AddLine(pt); }
        sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close();
        ctx->FillGeometry(g.Get(), b);
    };
    auto tile = [&](const Color& top, const Color& bot) {
        auto g = VGrad(ctx, top, bot, r.x, r.y, r.x + u * 0.4f, r.Bottom());
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(RF(r.x, r.y, r.Right(), r.Bottom()), u * 0.24f, u * 0.24f);
        if (g) ctx->FillRoundedRectangle(rr, g.Get());
    };
    auto white = [&](float a) { br->SetColor(D2D1::ColorF(1, 1, 1, a)); };
    switch (kind) {
        case FileKind::Image: {
            tile(Color{0.38f, 0.80f, 1.0f, 1.0f}, Color{0.15f, 0.38f, 0.86f, 1.0f});
            white(0.95f);
            ctx->FillEllipse(D2D1::Ellipse(P(0.70f, 0.30f), u * 0.085f, u * 0.085f), br);
            poly({P(0.10f, 0.86f), P(0.40f, 0.42f), P(0.62f, 0.72f), P(0.72f, 0.60f), P(0.92f, 0.86f)}, br);
            break;
        }
        case FileKind::Video: {
            tile(Color{0.70f, 0.42f, 1.0f, 1.0f}, Color{0.44f, 0.20f, 0.86f, 1.0f});
            white(0.97f);
            poly({P(0.38f, 0.26f), P(0.38f, 0.74f), P(0.76f, 0.50f)}, br);
            break;
        }
        case FileKind::Archive: {
            tile(Color{1.0f, 0.80f, 0.24f, 1.0f}, Color{0.94f, 0.56f, 0.08f, 1.0f});
            br->SetColor(D2D1::ColorF(0.45f, 0.27f, 0.04f, 0.55f));
            for (int i = 0; i < 4; ++i)
                ctx->FillRectangle(RF(r.x + u * (i % 2 ? 0.50f : 0.43f), r.y + u * (0.10f + i * 0.11f), r.x + u * (i % 2 ? 0.57f : 0.50f), r.y + u * (0.17f + i * 0.11f)), br);
            white(0.92f);
            ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(r.x + u * 0.41f, r.y + u * 0.56f, r.x + u * 0.59f, r.y + u * 0.78f), u * 0.05f, u * 0.05f), br);
            break;
        }
        case FileKind::Text: {
            auto g = VGrad(ctx, Color{0.98f, 0.98f, 1.0f, 1.0f}, Color{0.78f, 0.80f, 0.90f, 1.0f}, r.x, r.y, r.x, r.Bottom());
            const float x0 = r.x + u * 0.20f, x1 = r.x + u * 0.80f, y0 = r.y + u * 0.06f, y1 = r.y + u * 0.94f;
            if (g) {
                ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(x0, y0, x1, y1), u * 0.08f, u * 0.08f), g.Get());
            }
            br->SetColor(D2D1::ColorF(0.50f, 0.52f, 0.64f, 0.85f));
            for (int i = 0; i < 4; ++i)
                ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(x0 + u * 0.10f, y0 + u * (0.30f + i * 0.14f), x1 - u * (i == 3 ? 0.26f : 0.10f), y0 + u * (0.34f + i * 0.14f)), u * 0.02f, u * 0.02f), br);
            br->SetColor(D2D1::ColorF(0.62f, 0.64f, 0.78f, 1.0f));
            poly({D2D1::Point2F(x1 - u * 0.22f, y0), D2D1::Point2F(x1, y0 + u * 0.22f), D2D1::Point2F(x1 - u * 0.22f, y0 + u * 0.22f)}, br);
            break;
        }
        case FileKind::Code: {
            tile(Color{0.62f, 0.38f, 0.96f, 1.0f}, Color{0.38f, 0.18f, 0.74f, 1.0f});
            white(0.96f);
            const float w = u * 0.075f;
            ctx->DrawLine(P(0.40f, 0.30f), P(0.24f, 0.50f), br, w, nullptr);
            ctx->DrawLine(P(0.24f, 0.50f), P(0.40f, 0.70f), br, w, nullptr);
            ctx->DrawLine(P(0.60f, 0.30f), P(0.76f, 0.50f), br, w, nullptr);
            ctx->DrawLine(P(0.76f, 0.50f), P(0.60f, 0.70f), br, w, nullptr);
            white(0.7f);
            ctx->DrawLine(P(0.54f, 0.26f), P(0.46f, 0.74f), br, w * 0.8f, nullptr);
            break;
        }
        case FileKind::Audio: {
            tile(Color{1.0f, 0.46f, 0.70f, 1.0f}, Color{0.82f, 0.20f, 0.52f, 1.0f});
            white(0.96f);
            ctx->FillEllipse(D2D1::Ellipse(P(0.40f, 0.68f), u * 0.12f, u * 0.10f), br);
            ctx->FillRectangle(RF(r.x + u * 0.48f, r.y + u * 0.24f, r.x + u * 0.55f, r.y + u * 0.68f), br);
            poly({P(0.55f, 0.24f), P(0.74f, 0.32f), P(0.74f, 0.44f), P(0.55f, 0.36f)}, br);
            break;
        }
        default: return false;
    }
    return true;
}

// "Microsoft Edge" -> "Edge" etc.: the concept uses the short product names
std::wstring ShortAppName(const std::wstring& n) {
    static const wchar_t* pre[] = {L"Microsoft ", L"Windows "};
    for (const wchar_t* p : pre) {
        const size_t len = wcslen(p);
        if (n.size() > len + 2 && n.compare(0, len, p) == 0) return n.substr(len);
    }
    return n;
}

std::wstring DataPath(const wchar_t* file) {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
    std::wstring dir = std::wstring(appdata) + L"\\Aether";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + file;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
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

ULONGLONG FtU64(const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

const wchar_t* TrSuffixForTime(int minute) {
    // Turkish locative suffix of the spoken minute: "21:14'te", "21:15'te", "21:16'da" ...
    static const wchar_t* last[] = {L"da", L"de", L"de", L"te", L"te", L"te", L"da", L"de", L"de", L"da"};
    if (minute % 10 != 0) return last[minute % 10];
    switch (minute) { case 0: case 10: case 30: return L"da"; case 40: return L"ta"; default: return L"de"; }
}

std::wstring Ago(const FILETIME& ft) {
    FILETIME nowFt; SYSTEMTIME nowSt; GetSystemTimeAsFileTime(&nowFt); GetLocalTime(&nowSt);
    const ULONGLONG nowT = FtU64(nowFt), t = FtU64(ft);
    const long long sec = t > nowT ? 0 : static_cast<long long>((nowT - t) / 10000000ULL);
    FILETIME lf; SYSTEMTIME ls; FileTimeToLocalFileTime(&ft, &lf); FileTimeToSystemTime(&lf, &ls);
    const bool tr = i18n::Turkish();
    wchar_t b[96];
    if (sec < 60) return i18n::Pick(L"Opened just now", L"Az önce açıldı");
    if (sec < 3600) {
        const int m = static_cast<int>(sec / 60);
        if (tr) swprintf(b, 96, L"%d dakika önce açıldı", m);
        else swprintf(b, 96, L"Opened %d minute%ls ago", m, m == 1 ? L"" : L"s");
        return b;
    }
    if (sec < 6 * 3600 || (ls.wDay == nowSt.wDay && ls.wMonth == nowSt.wMonth && ls.wYear == nowSt.wYear)) {
        const int h = static_cast<int>(sec / 3600);
        if (tr) swprintf(b, 96, L"%d saat önce açıldı", h);
        else swprintf(b, 96, L"Opened %d hour%ls ago", h, h == 1 ? L"" : L"s");
        return b;
    }
    if (sec < 48 * 3600 && ls.wDay != nowSt.wDay) {
        if (tr) swprintf(b, 96, L"Dün %02d:%02d'%ls açıldı", ls.wHour, ls.wMinute, TrSuffixForTime(ls.wMinute));
        else swprintf(b, 96, L"Opened yesterday at %02d:%02d", ls.wHour, ls.wMinute);
        return b;
    }
    if (tr) swprintf(b, 96, L"%02d.%02d.%04d tarihinde açıldı", ls.wDay, ls.wMonth, ls.wYear);
    else swprintf(b, 96, L"Opened %ls %d", i18n::Month(ls.wMonth), ls.wDay);
    return b;
}

struct SettingDef { const wchar_t* en; const wchar_t* tr; const wchar_t* uri; };
const SettingDef kSettings[] = {
    {L"Wi-Fi", L"Wi-Fi", L"ms-settings:network-wifi"},
    {L"Network & internet", L"Ağ ve internet", L"ms-settings:network"},
    {L"Bluetooth & devices", L"Bluetooth ve cihazlar", L"ms-settings:bluetooth"},
    {L"Display", L"Ekran", L"ms-settings:display"},
    {L"Sound", L"Ses", L"ms-settings:sound"},
    {L"Notifications", L"Bildirimler", L"ms-settings:notifications"},
    {L"Power & battery", L"Güç ve pil", L"ms-settings:powersleep"},
    {L"Storage", L"Depolama", L"ms-settings:storagesense"},
    {L"Personalization", L"Kişiselleştirme", L"ms-settings:personalization"},
    {L"Background", L"Arka plan", L"ms-settings:personalization-background"},
    {L"Colors", L"Renkler", L"ms-settings:colors"},
    {L"Apps & features", L"Uygulamalar ve özellikler", L"ms-settings:appsfeatures"},
    {L"Default apps", L"Varsayılan uygulamalar", L"ms-settings:defaultapps"},
    {L"Startup apps", L"Başlangıç uygulamaları", L"ms-settings:startupapps"},
    {L"Accounts", L"Hesaplar", L"ms-settings:yourinfo"},
    {L"Date & time", L"Tarih ve saat", L"ms-settings:dateandtime"},
    {L"Language & region", L"Dil ve bölge", L"ms-settings:regionlanguage"},
    {L"Gaming", L"Oyun", L"ms-settings:gaming-gamebar"},
    {L"Privacy & security", L"Gizlilik ve güvenlik", L"ms-settings:privacy"},
    {L"Windows Update", L"Windows Update", L"ms-settings:windowsupdate"},
    {L"Night light", L"Gece ışığı", L"ms-settings:nightlight"},
    {L"Mouse", L"Fare", L"ms-settings:mousetouchpad"},
    {L"Keyboard", L"Klavye", L"ms-settings:easeofaccess-keyboard"},
    {L"Printers & scanners", L"Yazıcılar ve tarayıcılar", L"ms-settings:printers"},
    {L"VPN", L"VPN", L"ms-settings:network-vpn"},
    {L"Accessibility", L"Erişilebilirlik", L"ms-settings:easeofaccess"},
    {L"About", L"Hakkında", L"ms-settings:about"},
};

} // namespace

// =============================================================================================
//  Lifetime
// =============================================================================================

StartMenu::StartMenu()
    : m_open(0.0f, animation::SpringConfig{520.0f, 36.0f, 1.0f, 0.002f})
    , m_scroll(0.0f, animation::SpringConfig{380.0f, 34.0f, 1.0f, 0.05f})
    , m_viewT(1.0f, animation::SpringConfig{420.0f, 34.0f, 1.0f, 0.002f}) {
    wchar_t name[256];
    DWORD len = ARRAYSIZE(name);
    m_userName = GetUserNameW(name, &len) ? name : L"User";
    wchar_t disp[256]; ULONG dl = ARRAYSIZE(disp);
    if (GetUserNameExW(NameDisplay, disp, &dl) && disp[0]) m_userName = disp;
    wchar_t upn[256]; ULONG ul = ARRAYSIZE(upn);
    if (GetUserNameExW(NameUserPrincipal, upn, &ul) && wcschr(upn, L'@')) m_userSub = upn;
    else m_userSub = i18n::Pick(L"Local account", L"Yerel Hesap");
}

StartMenu::~StartMenu() {
    if (m_loader.joinable()) m_loader.join();
    if (m_recentLoader.joinable()) m_recentLoader.join();
    if (m_fileLoader.joinable()) m_fileLoader.join();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

// =============================================================================================
//  Data: apps (shell:AppsFolder), recent files, user file index
// =============================================================================================

void StartMenu::PreloadAsync() {
    if (m_loadState.load() != 0) return;
    m_loadState = 1;
    m_fileState = 1;

    m_loader = std::thread([this] {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        std::vector<std::pair<std::wstring, std::wstring>> raw;
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
        { std::lock_guard<std::mutex> lock(m_loadMutex); m_loadedRaw = std::move(raw); }
        m_loadState = 2;
        if (com) CoUninitialize();
    });

    // recently opened files: resolve the shortcuts in the Recent folder
    m_recentLoader = std::thread([this] {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        std::vector<FileItem> out;
        const std::wstring dir = KnownFolder(FOLDERID_Recent);
        struct Lnk { std::wstring path; FILETIME t; };
        std::vector<Lnk> lnks;
        WIN32_FIND_DATAW fd;
        if (HANDLE h = FindFirstFileExW((dir + L"\\*.lnk").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0); h != INVALID_HANDLE_VALUE) {
            do { lnks.push_back({dir + L"\\" + fd.cFileName, fd.ftLastWriteTime}); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(lnks.begin(), lnks.end(), [](const Lnk& a, const Lnk& b) { return FtU64(a.t) > FtU64(b.t); });
        std::unordered_set<std::wstring> seen;
        ComPtr<IShellLinkW> link;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
            ComPtr<IPersistFile> pf;
            link.As(&pf);
            for (const Lnk& l : lnks) {
                if (out.size() >= 24) break;
                if (!pf || FAILED(pf->Load(l.path.c_str(), STGM_READ))) continue;
                wchar_t target[MAX_PATH] = {};
                if (FAILED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) || !target[0]) continue;
                const DWORD attr = GetFileAttributesW(target);
                if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
                FileItem f;
                f.path = target;
                f.name = PathFindFileNameW(target);
                f.fold = Fold(f.name);
                f.time = l.t;
                if (!seen.insert(f.fold + L"|" + Fold(f.path)).second) continue;
                out.push_back(std::move(f));
            }
        }
        { std::lock_guard<std::mutex> lock(m_loadMutex); m_loadedRecents = std::move(out); }
        if (com) CoUninitialize();
    });

    // searchable index of the user's own folders (depth limited)
    m_fileLoader = std::thread([this] {
        std::vector<FileItem> out;
        const KNOWNFOLDERID* roots[] = {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos};
        struct Walk {
            std::vector<FileItem>& out;
            void Dir(const std::wstring& dir, int depth) {
                if (out.size() > 40000) return;
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (h == INVALID_HANDLE_VALUE) return;
                do {
                    if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (depth < 4 && _wcsicmp(fd.cFileName, L"node_modules") != 0 && _wcsicmp(fd.cFileName, L"AppData") != 0)
                            Dir(dir + L"\\" + fd.cFileName, depth + 1);
                        continue;
                    }
                    FileItem f;
                    f.name = fd.cFileName;
                    f.fold = Fold(f.name);
                    f.path = dir + L"\\" + fd.cFileName;
                    f.time = fd.ftLastWriteTime;
                    out.push_back(std::move(f));
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        } walk{out};
        for (const auto& r : roots) {
            const std::wstring p = KnownFolder(*r);
            if (!p.empty()) walk.Dir(p, 0);
        }
        { std::lock_guard<std::mutex> lock(m_loadMutex); m_loadedFiles = std::move(out); }
        m_fileState = 2;
    });
}

void StartMenu::AdoptLoaded() {
    if (!m_adopted && m_loadState.load() == 2) {
        std::vector<std::pair<std::wstring, std::wstring>> raw;
        { std::lock_guard<std::mutex> lock(m_loadMutex); raw = std::move(m_loadedRaw); }
        for (auto& [name, parse] : raw) {
            const std::wstring fold = Fold(name);
            if (fold.find(L"uninstall") != std::wstring::npos || fold.find(L"kaldir") != std::wstring::npos) continue;
            App a;
            a.name = name; a.fold = fold; a.parse = parse;
            m_apps.push_back(std::move(a));
        }
        std::sort(m_apps.begin(), m_apps.end(), [](const App& a, const App& b) { return a.fold < b.fold; });
        m_adopted = true;
        LoadPins();
    }
    if (!m_recentsAdopted) {
        std::lock_guard<std::mutex> lock(m_loadMutex);
        if (!m_loadedRecents.empty() || m_loadState.load() == 2) {
            m_recents = std::move(m_loadedRecents);
            m_loadedRecents.clear();
            if (m_adopted) m_recentsAdopted = true;
        }
    }
    if (!m_filesAdopted && m_fileState.load() == 2) {
        std::lock_guard<std::mutex> lock(m_loadMutex);
        m_files = std::move(m_loadedFiles);
        m_filesAdopted = true;
    }
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
    if (!fromFile) {
        // first run: twelve well-known apps (English or Turkish display names)
        const std::vector<std::vector<const wchar_t*>> prefer = {
            {L"edge"}, {L"spotify"}, {L"discord"}, {L"steam"}, {L"visual studio"}, {L"github"},
            {L"file explorer", L"dosya gezgini"}, {L"settings", L"ayarlar"}, {L"photos", L"fotograflar"},
            {L"calculator", L"hesap makinesi"}, {L"notepad", L"not defteri"}, {L"obs"},
            {L"terminal"}, {L"store", L"magaza"}, {L"paint"}, {L"chrome"}, {L"firefox"}, {L"snipping", L"ekran alintisi"}};
        for (const auto& alts : prefer) {
            if (m_pins.size() >= 12) break;
            bool found = false;
            for (const wchar_t* key : alts) {
                for (const auto& a : m_apps) {
                    if (a.fold.find(key) != std::wstring::npos && std::find(m_pins.begin(), m_pins.end(), a.parse) == m_pins.end()) {
                        m_pins.push_back(a.parse);
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }
    }
    m_pinnedIdx.clear();
    for (const auto& p : m_pins)
        for (int i = 0; i < static_cast<int>(m_apps.size()); ++i)
            if (m_apps[i].parse == p) { m_pinnedIdx.push_back(i); break; }
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
    if (it != m_pins.end()) m_pins.erase(it); else m_pins.push_back(parse);
    SavePins();
    m_pinnedIdx.clear();
    for (const auto& p : m_pins)
        for (int i = 0; i < static_cast<int>(m_apps.size()); ++i)
            if (m_apps[i].parse == p) { m_pinnedIdx.push_back(i); break; }
    BuildLayout();
}

void StartMenu::EnsureAppIcon(App& app) {
    if (app.icon || app.iconTried) return;
    app.iconTried = true;
    ComPtr<IShellItem> item;
    const std::wstring path = L"shell:AppsFolder\\" + app.parse;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item.As(&factory))) return;
    const int px = static_cast<int>(64.0f * std::max(1.0f, m_scale));
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

void StartMenu::EnsureFileIcon(FileItem& f) {
    if (f.icon || f.iconTried) return;
    f.iconTried = true;
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(f.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon) return;
    f.icon = ui::IconToBitmap(m_dcomp->GetContext(), sfi.hIcon, static_cast<int>(48 * std::max(1.0f, m_scale)));
    DestroyIcon(sfi.hIcon);
}

// Account picture: Windows keeps resized copies in Public\AccountPictures\<SID> (folder).
void StartMenu::LoadAvatar() {
    if (m_avatarTried) return;
    m_avatarTried = true;
    HANDLE token = nullptr;
    std::wstring sidStr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD need = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &need);
        std::vector<BYTE> buf(need);
        if (need && GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s) && s) { sidStr = s; LocalFree(s); }
        }
        CloseHandle(token);
    }
    if (sidStr.empty()) return;
    const std::wstring dir = KnownFolder(FOLDERID_Public) + L"\\AccountPictures\\" + sidStr;
    std::wstring best; ULONGLONG bestSize = 0;
    WIN32_FIND_DATAW fd;
    if (HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring n = fd.cFileName;
            const std::wstring ext = n.size() > 4 ? Fold(n.substr(n.size() - 4)) : L"";
            if (ext != L".jpg" && ext != L".png") continue;
            const ULONGLONG sz = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const bool is192 = n.find(L"Image192") != std::wstring::npos;
            if (is192 || (best.find(L"Image192") == std::wstring::npos && sz > bestSize)) { best = dir + L"\\" + n; bestSize = sz; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (best.empty() || !m_dcomp) return;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(best.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return;
    ComPtr<ID2D1Bitmap> bmp;
    if (auto ctx = m_dcomp->GetContext()) {
        if (SUCCEEDED(ctx->CreateBitmapFromWicBitmap(conv.Get(), &bmp))) bmp.As(&m_avatar);
    }
}

// Grabs the desktop behind the panel (the menu window is still hidden / not yet shown), shrinks it 4x,
// blurs it on the CPU (tiny image, ~1 ms) and uploads it. Render() paints it scaled back up under the
// glass body, which is what gives the panel its real frosted-glass look - Direct2D cannot blur what is
// behind a window by itself. Failure is harmless: the panel just stays plain translucent.
void StartMenu::CaptureBackdrop() {
    if (!m_dcomp || !m_hwnd) return;
    if (IsWindowVisible(m_hwnd) && m_backdrop) return;   // still fading out from the previous open: keep that one
    RECT wr; GetWindowRect(m_hwnd, &wr);
    render::CaptureBackdrop(m_dcomp->GetContext(), wr.left + static_cast<int>(m_pad), wr.top + static_cast<int>(m_pad),
                            static_cast<int>(kPanelW * m_scale), static_cast<int>(kPanelH * m_scale), 5, m_backdrop);
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
    m_pad = kPadOuter;
    m_winW = static_cast<int>(kPanelW + m_pad * 2);
    m_winH = static_cast<int>(kPanelH + m_pad * 2);

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
    const float s = m_scale;

    auto make = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT w, float size, DWRITE_TEXT_ALIGNMENT a, bool ellipsis) {
        f.Reset();
        if (!fam) return;
        dw->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * s, L"en-us", &f);
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
    const auto L_ = DWRITE_TEXT_ALIGNMENT_LEADING, C_ = DWRITE_TEXT_ALIGNMENT_CENTER, T_ = DWRITE_TEXT_ALIGNMENT_TRAILING;
    make(m_fName,    display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f, L_, true);
    make(m_fSub,     text,    DWRITE_FONT_WEIGHT_NORMAL,    11.0f, L_, true);
    make(m_fNav,     text,    DWRITE_FONT_WEIGHT_NORMAL,    13.5f, L_, true);
    make(m_fSearch,  text,    DWRITE_FONT_WEIGHT_NORMAL,    13.5f, L_, true);
    make(m_fHead,    display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.5f, L_, false);
    make(m_fLink,    text,    DWRITE_FONT_WEIGHT_NORMAL,    12.0f, T_, false);
    make(m_fTile,    text,    DWRITE_FONT_WEIGHT_NORMAL,    11.0f, C_, true);
    make(m_fRecName, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, L_, true);
    make(m_fRecSub,  text,    DWRITE_FONT_WEIGHT_NORMAL,    11.0f, L_, true);
    make(m_fRowName, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, L_, true);
    make(m_fLetter,  display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, L_, false);
    make(m_fCenter,  text,    DWRITE_FONT_WEIGHT_NORMAL,    13.0f, C_, true);
    make(m_fIcon,    icons,   DWRITE_FONT_WEIGHT_NORMAL,    17.0f, C_, false);
    make(m_fIconSm,  icons,   DWRITE_FONT_WEIGHT_NORMAL,    14.0f, C_, false);
}

void StartMenu::Toggle(HMONITOR monitor, int reservedPx) {
    if (!m_wantOpen && JustDismissed()) return;   // the click that closed it must not reopen it
    if (m_wantOpen) Hide(); else Show(monitor, reservedPx);
}

void StartMenu::PrepareFor(HMONITOR monitor, int reservedPx) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    float s = core::MonitorScale(monitor);
    // shrink on small screens so the whole panel always fits above the bar
    const float availH = static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top - reservedPx) - 12.0f * s;
    const float availW = static_cast<float>(mi.rcMonitor.right - mi.rcMonitor.left) - 16.0f * s;
    s *= std::min({1.0f, availH / ((kPanelH + kPadOuter) * s), availW / ((kPanelW + kPadOuter * 2) * s)});

    const float oldScale = m_scale;
    m_scale = s;
    m_pad = kPadOuter * s;
    m_winW = static_cast<int>(kPanelW * s + m_pad * 2);
    m_winH = static_cast<int>(kPanelH * s + m_pad * 2);
    SetWindowPos(m_hwnd, nullptr, 0, 0, m_winW, m_winH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    m_dcomp->Resize(m_winW, m_winH);
    if (oldScale != m_scale) {
        CreateFonts();
        for (auto& a : m_apps) { a.icon.Reset(); a.iconTried = false; }
        for (auto& f : m_recents) { f.icon.Reset(); f.iconTried = false; }
        for (auto& f : m_files) { f.icon.Reset(); f.iconTried = false; }
        m_warmed = false;
    }
    LoadAvatar();
}

// Runs while the menu is hidden: loads the avatar + the icons of the first screen and renders one invisible frame,
// which builds the glow / gradient caches. Without it all of that happened on the first click (visible hitch).
bool StartMenu::Warm() {
    if (m_warmed || !m_hwnd || m_wantOpen || IsWindowVisible(m_hwnd)) return false;
    AdoptLoaded();
    if (!m_adopted) return m_loadState.load() != 0;   // app list still loading: try again later
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    PrepareFor(mon, static_cast<int>(82.0f * core::MonitorScale(mon)));
    m_query.clear(); m_view = View::Home; m_scroll.SnapTo(0.0f);
    BuildLayout();
    int budget = 4;
    for (int idx : m_pinnedIdx) {
        if (idx < 0 || idx >= static_cast<int>(m_apps.size())) continue;
        App& a = m_apps[idx];
        if (a.icon || a.iconTried) continue;
        if (budget-- <= 0) return true;
        EnsureAppIcon(a);
    }
    for (size_t i = 0; i < m_recents.size() && i < 6; ++i) {
        FileItem& f = m_recents[i];
        if (f.icon || f.iconTried) continue;
        if (budget-- <= 0) return true;
        EnsureFileIcon(f);
    }
    m_open.SnapTo(1.0f);
    Render();
    m_open.SnapTo(0.0f); m_open.SetTarget(0.0f);
    m_warmed = true;
    return false;
}

void StartMenu::Show(HMONITOR monitor, int reservedPx) {
    if (!m_hwnd) return;
    AdoptLoaded();
    if (m_loadState.load() == 0) PreloadAsync();

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    PrepareFor(monitor, reservedPx);
    const float s = m_scale;

    const int cx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    const int bottom = mi.rcMonitor.bottom - reservedPx - static_cast<int>(6.0f * s);
    SetWindowPos(m_hwnd, HWND_TOPMOST, cx - m_winW / 2, bottom - m_winH + static_cast<int>(m_pad), m_winW, m_winH, SWP_NOACTIVATE);

    m_query.clear();
    m_powerOpen = false;
    m_selId = -1; m_hoverId = 0; m_hoverAmt.clear();
    m_view = View::Home;
    m_scroll.SnapTo(0.0f);
    BuildLayout();

    m_prevDown = true; m_dismissTick = 0;
    m_wantOpen = true;
    m_open.SnapTo(0.0f);
    m_open.SetTarget(1.0f);
    m_viewT.SnapTo(1.0f);
    CaptureBackdrop();
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
    if (m_hwnd) { LONG ex = GetWindowLongW(m_hwnd, GWL_EXSTYLE); if (ex & WS_EX_TRANSPARENT) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT); }
    if (!m_wantOpen) return;
    m_wantOpen = false;
    m_powerOpen = false;
    m_open.SetTarget(0.0f);
}

bool StartMenu::Frame(float dt) {
    render::glass::SyncTheme();
    if (!m_hwnd) return false;
    AdoptLoaded();
    m_time += dt;
    m_open.Update(dt);
    m_scroll.Update(dt);
    m_viewT.Update(dt);
    // Outside-click dismissal + click-through: only the panel itself takes the mouse, the transparent glow
    // margin lets clicks fall through to whatever is below (dock icons, windows). A press outside closes it.
    if (m_wantOpen && IsWindowVisible(m_hwnd)) {
        RECT wr; GetWindowRect(m_hwnd, &wr);
        POINT cp; GetCursorPos(&cp);
        const int pad = static_cast<int>(m_pad);
        const bool inside = cp.x >= wr.left + pad && cp.x < wr.right - pad && cp.y >= wr.top + pad && cp.y < wr.bottom - pad;
        LONG ex = GetWindowLongW(m_hwnd, GWL_EXSTYLE);
        const bool transparent = (ex & WS_EX_TRANSPARENT) != 0;
        if (!inside && !transparent) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
        else if (inside && transparent) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (down && !m_prevDown && !inside && !(m_stayOpen || m_menuOpen)) { m_dismissTick = GetTickCount64(); Hide(); }
        m_prevDown = down;
    }
    if (!m_wantOpen && m_open.IsResting() && IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
        return false;
    }
    if (!IsWindowVisible(m_hwnd)) return Warm();
    Render();
    return true;
}

// =============================================================================================
//  Layout (logical px; the content area scrolls vertically)
// =============================================================================================

float StartMenu::ViewTop() const { return 92.0f; }
float StartMenu::ViewBottom() const { return kPanelH - 8.0f; }
float StartMenu::MaxScroll() const { return std::max(0.0f, m_contentH - (ViewBottom() - kContentTop)); }

Rect StartMenu::Px(const Rect& r) const {
    return Rect{m_pad + r.x * m_scale, m_pad + r.y * m_scale, r.width * m_scale, r.height * m_scale};
}

float StartMenu::Hover(int id) const {
    auto it = m_hoverAmt.find(id);
    return it == m_hoverAmt.end() ? 0.0f : it->second;
}

void StartMenu::SetView(View v) {
    if (m_view == v) return;
    m_view = v;
    m_scroll.SnapTo(0.0f);
    m_viewT.SnapTo(0.0f);
    m_viewT.SetTarget(1.0f);
    BuildLayout();
    m_selId = -1;
}

void StartMenu::SetQuery(const std::wstring& q) {
    m_query = q;
    if (m_query.empty()) { SetView(View::Home); return; }
    const bool wasSearch = m_view == View::Search;
    m_view = View::Search;
    m_scroll.SnapTo(0.0f);
    if (!wasSearch) { m_viewT.SnapTo(0.0f); m_viewT.SetTarget(1.0f); }
    BuildLayout();
    m_selId = -1;
    for (const Item& it : m_items) if (it.kind != kLinkPinned && it.kind != kLinkRecent) { m_selId = it.id; break; }
}

void StartMenu::BuildLayout() {
    m_items.clear();
    m_labels.clear();
    switch (m_view) {
        case View::Home: BuildHome(); break;
        case View::Apps: BuildApps(); break;
        case View::Search: BuildSearch(); break;
    }
    m_scroll.SnapTo(std::clamp(m_scroll.GetValue(), 0.0f, MaxScroll()));
}

void StartMenu::BuildHome() {
    auto add = [&](Kind k, int ref, const Rect& r) {
        m_items.push_back(Item{kIdItemBase + static_cast<int>(m_items.size()), r, k, ref});
    };
    m_labels.push_back({i18n::Pick(L"Pinned apps", L"Sabitlenen Uygulamalar"), Rect{kPaneX, 100, 300, 24}, 0});
    add(kLinkPinned, 0, Rect{kPaneX + kPaneW - 170, 100, 170, 24});
    if (!m_adopted) {
        m_labels.push_back({i18n::Pick(L"Loading apps...", L"Uygulamalar yükleniyor..."), Rect{kPaneX, 150, kPaneW, 40}, 2});
    } else if (m_pinnedIdx.empty()) {
        m_labels.push_back({i18n::Pick(L"Nothing is pinned yet. Right-click an app to pin it.", L"Henüz sabitlenen bir şey yok. Sabitlemek için bir uygulamaya sağ tıklayın."),
                            Rect{kPaneX, 150, kPaneW, 40}, 2});
    }
    for (int k = 0; k < std::min<int>(12, static_cast<int>(m_pinnedIdx.size())); ++k)
        add(kAppTile, m_pinnedIdx[k], Rect{kPaneX + (k % 6) * kTileW, 134.0f + (k / 6) * kTileH, kTileW, kTileH});

    m_labels.push_back({L"", Rect{kPaneX + 4, 322, kPaneW - 8, 1}, 4});   // divider
    m_labels.push_back({i18n::Pick(L"Recommended", L"Önerilen"), Rect{kPaneX, 337, 300, 24}, 0});
    add(kLinkRecent, 0, Rect{kPaneX + kPaneW - 170, 337, 170, 24});
    if (m_recents.empty()) {
        m_labels.push_back({i18n::Pick(L"Files you open will show up here.", L"Açtığınız dosyalar burada görünür."), Rect{kPaneX, 390, kPaneW, 40}, 2});
    }
    for (int k = 0; k < std::min<int>(6, static_cast<int>(m_recents.size())); ++k)
        add(kRecentRow, k, Rect{kPaneX + (k % 2) * 249.0f, 367.0f + (k / 2) * 54.0f, 243.0f, 52.0f});
    m_contentH = kPanelH;
}

void StartMenu::BuildApps() {
    auto add = [&](Kind k, int ref, const Rect& r) {
        m_items.push_back(Item{kIdItemBase + static_cast<int>(m_items.size()), r, k, ref});
    };
    float y = kContentTop;
    m_labels.push_back({i18n::Pick(L"All apps", L"Tüm Uygulamalar"), Rect{kPaneX, y, 300, 24}, 0});
    y += 34;
    if (!m_adopted) {
        m_labels.push_back({i18n::Pick(L"Loading apps...", L"Uygulamalar yükleniyor..."), Rect{kPaneX, y, kPaneW, 40}, 2});
        y += 44;
    }
    wchar_t cur = 0;
    int col = 0;
    for (int i = 0; i < static_cast<int>(m_apps.size()); ++i) {
        wchar_t c = static_cast<wchar_t>(towupper(m_apps[i].fold.empty() ? L'#' : m_apps[i].fold[0]));
        if (!iswalpha(c)) c = L'#';
        if (c != cur) {
            if (col != 0) { y += kTileH; col = 0; }
            cur = c;
            m_labels.push_back({std::wstring(1, cur), Rect{kPaneX + 6, y, 100, 28}, 1});
            y += 30;
        }
        add(kAppTile, i, Rect{kPaneX + col * kTileW, y, kTileW, kTileH});
        if (++col == 6) { col = 0; y += kTileH; }
    }
    if (col != 0) y += kTileH;
    m_contentH = y + 8;
}

void StartMenu::BuildSearch() {
    auto add = [&](Kind k, int ref, const Rect& r) {
        m_items.push_back(Item{kIdItemBase + static_cast<int>(m_items.size()), r, k, ref});
    };
    const std::wstring q = Fold(m_query);
    float y = kContentTop;

    // --- apps
    std::vector<std::pair<int, int>> apps;   // score, index
    for (int i = 0; i < static_cast<int>(m_apps.size()); ++i) {
        const std::wstring& n = m_apps[i].fold;
        const size_t pos = n.find(q);
        if (pos == std::wstring::npos) continue;
        int score = 2;
        if (pos == 0) score = 0;
        else if (n[pos - 1] == L' ' || n[pos - 1] == L'-' || n[pos - 1] == L'.') score = 1;
        apps.emplace_back(score, i);
    }
    std::stable_sort(apps.begin(), apps.end(), [](auto& a, auto& b) { return a.first < b.first; });

    // --- settings
    std::vector<int> settings;
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kSettings)); ++i) {
        if (Fold(kSettings[i].en).find(q) != std::wstring::npos || Fold(kSettings[i].tr).find(q) != std::wstring::npos) settings.push_back(i);
    }

    // --- files (recent first, then the folder index)
    std::vector<std::pair<int, std::pair<int, int>>> files;   // score, (source 0 recents / 1 index, index)
    auto scan = [&](const std::vector<FileItem>& list, int source) {
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const size_t pos = list[i].fold.find(q);
            if (pos == std::wstring::npos) continue;
            files.push_back({pos == 0 ? 0 : 1, {source, i}});
        }
    };
    scan(m_recents, 0);
    scan(m_files, 1);
    std::stable_sort(files.begin(), files.end(), [&](auto& a, auto& b) {
        if (a.first != b.first) return a.first < b.first;
        const FileItem& fa = a.second.first == 0 ? m_recents[a.second.second] : m_files[a.second.second];
        const FileItem& fb = b.second.first == 0 ? m_recents[b.second.second] : m_files[b.second.second];
        return FtU64(fa.time) > FtU64(fb.time);
    });
    {   // drop duplicates (a recent file is also in the index)
        std::unordered_set<std::wstring> seen;
        std::vector<std::pair<int, std::pair<int, int>>> uniq;
        for (auto& f : files) {
            const FileItem& fi = f.second.first == 0 ? m_recents[f.second.second] : m_files[f.second.second];
            if (seen.insert(Fold(fi.path)).second) uniq.push_back(f);
        }
        files.swap(uniq);
    }

    auto section = [&](const wchar_t* en, const wchar_t* tr) {
        m_labels.push_back({i18n::Pick(en, tr), Rect{kPaneX + 4, y, 300, 24}, 0});
        y += 28;
    };
    if (!apps.empty()) {
        section(L"Apps", L"Uygulamalar");
        for (size_t i = 0; i < std::min<size_t>(6, apps.size()); ++i) { add(kAppRow, apps[i].second, Rect{kPaneX, y, kPaneW, kRowH}); y += kRowH + 2; }
        y += 8;
    }
    if (!settings.empty()) {
        section(L"Settings", L"Ayarlar");
        for (size_t i = 0; i < std::min<size_t>(4, settings.size()); ++i) { add(kSettingRow, settings[i], Rect{kPaneX, y, kPaneW, kRowH}); y += kRowH + 2; }
        y += 8;
    }
    if (!files.empty()) {
        section(L"Files", L"Dosyalar");
        for (size_t i = 0; i < std::min<size_t>(7, files.size()); ++i) {
            // encode the source in ref: recents are stored as -(index + 1)
            const auto& f = files[i].second;
            add(kFileRow, f.first == 0 ? -(f.second + 1) : f.second, Rect{kPaneX, y, kPaneW, kRowH});
            y += kRowH + 2;
        }
        y += 8;
    }
    if (apps.empty() && settings.empty() && files.empty()) {
        m_labels.push_back({i18n::Pick(L"No results found", L"Sonuç bulunamadı"), Rect{kPaneX, y + 6, kPaneW, 28}, 2});
        y += 40;
    }
    section(L"Search the web", L"Web'de ara");
    add(kWebRow, 0, Rect{kPaneX, y, kPaneW, kRowH});
    y += kRowH + 8;
    m_contentH = y + 8;
}

const StartMenu::Item* StartMenu::FindItem(int id) const {
    const int idx = id - kIdItemBase;
    return (idx >= 0 && idx < static_cast<int>(m_items.size())) ? &m_items[idx] : nullptr;
}

// Returns the id under a pixel position (0 = nothing / panel background)
int StartMenu::HitTest(const Vec2& px) const {
    const float s = m_scale;
    const Vec2 p{(px.x - m_pad) / s, (px.y - m_pad) / s};
    if (m_powerOpen) {
        const Rect card{20, 488 - 5 * 38 - 20, 188, 5 * 38 + 12};
        if (card.Contains(p)) {
            const int k = static_cast<int>((p.y - card.y - 6) / 38.0f);
            return (k >= 0 && k < 5) ? kIdPowerBase + k : -1;
        }
    }
    if (p.x < 0 || p.y < 0 || p.x > kPanelW || p.y > kPanelH) return 0;
    if (p.x < kRailW) {
        if (Rect{20, 36, 184, 54}.Contains(p)) return kIdAvatar;
        for (int i = 0; i < 7; ++i) if (Rect{21, 124.0f + i * 44.0f, 184, 40}.Contains(p)) return kIdNavHome + i;
        if (Rect{21, 445, 184, 40}.Contains(p)) return kIdSettings;
        if (Rect{21, 488, 184, 40}.Contains(p)) return kIdPower;
        return 0;
    }
    if (Rect{kPaneX + kPaneW - 40, kSearchY, 40, kSearchH}.Contains(p)) return kIdPin;
    if (Rect{kPaneX, kSearchY, kPaneW, kSearchH}.Contains(p)) return kIdSearch;
    if (p.y < ViewTop()) return 0;
    const float sc = m_view == View::Home ? 0.0f : m_scroll.GetValue();
    if (m_view != View::Home && p.y > ViewBottom()) return 0;
    const Vec2 c{p.x, p.y + sc};
    for (const Item& it : m_items) if (it.r.Contains(c)) return it.id;
    return 0;
}

void StartMenu::EnsureVisible(const Item& it) {
    if (m_view == View::Home) return;
    const float viewH = ViewBottom() - kContentTop;
    float target = m_scroll.GetTarget();
    if (it.r.y - 40 < target + 0) target = it.r.y - kContentTop - 36;
    else if (it.r.Bottom() > target + kContentTop + viewH) target = it.r.Bottom() - kContentTop - viewH + 8;
    m_scroll.SetTarget(std::clamp(target, 0.0f, MaxScroll()));
}

// Moves the selection to the nearest selectable item in a direction (works for grids, 2-column lists and rows)
void StartMenu::MoveSelection(int dx, int dy) {
    auto selectable = [](const Item& it) { return it.kind != kLinkPinned && it.kind != kLinkRecent; };
    const Item* cur = FindItem(m_selId);
    if (!cur || !selectable(*cur)) {
        for (const Item& it : m_items) if (selectable(it)) { m_selId = it.id; EnsureVisible(it); return; }
        return;
    }
    const Vec2 c = cur->r.Center();
    const Item* best = nullptr;
    float bestScore = 1e9f;
    for (const Item& it : m_items) {
        if (!selectable(it) || it.id == cur->id) continue;
        const Vec2 o = it.r.Center();
        const float ddx = o.x - c.x, ddy = o.y - c.y;
        float along, across;
        if (dx != 0) { along = ddx * dx; across = std::fabs(ddy); }
        else { along = ddy * dy; across = std::fabs(ddx); }
        if (along < 8.0f) continue;
        const float score = along + across * 3.0f;
        if (score < bestScore) { bestScore = score; best = &it; }
    }
    if (best) { m_selId = best->id; EnsureVisible(*best); }
}

// =============================================================================================
//  Actions
// =============================================================================================

void StartMenu::OpenFolder(int which) {
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Music, &FOLDERID_Pictures, &FOLDERID_Videos};
    const std::wstring p = KnownFolder(*ids[std::clamp(which, 0, 4)]);
    if (!p.empty()) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Hide();
}

void StartMenu::ShowPowerAction(int which) {
    Hide();
    EnableShutdownPrivilege();
    switch (which) {
        case 0: LockWorkStation(); break;
        case 1: ExitWindowsEx(EWX_LOGOFF, 0); break;
        case 2: SetSuspendState(FALSE, FALSE, FALSE); break;
        case 3: ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER); break;
        case 4: ExitWindowsEx(EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER); break;
    }
}

void StartMenu::LaunchItem(const Item& it) {
    auto openFile = [&](const FileItem& f) { ShellExecuteW(nullptr, L"open", f.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    switch (it.kind) {
        case kAppTile:
        case kAppRow: {
            if (it.ref < 0 || it.ref >= static_cast<int>(m_apps.size())) return;
            const std::wstring target = L"shell:AppsFolder\\" + m_apps[it.ref].parse;
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kRecentRow:
            if (it.ref >= 0 && it.ref < static_cast<int>(m_recents.size())) openFile(m_recents[it.ref]);
            break;
        case kFileRow:
            if (it.ref < 0) { const int i = -(it.ref + 1); if (i < static_cast<int>(m_recents.size())) openFile(m_recents[i]); }
            else if (it.ref < static_cast<int>(m_files.size())) openFile(m_files[it.ref]);
            break;
        case kSettingRow:
            ShellExecuteW(nullptr, L"open", kSettings[it.ref].uri, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kWebRow: {
            std::string utf8;
            const int n = WideCharToMultiByte(CP_UTF8, 0, m_query.c_str(), static_cast<int>(m_query.size()), nullptr, 0, nullptr, nullptr);
            utf8.resize(static_cast<size_t>(std::max(0, n)));
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, m_query.c_str(), static_cast<int>(m_query.size()), utf8.data(), n, nullptr, nullptr);
            std::wstring url = L"https://www.bing.com/search?q=";
            for (unsigned char c : utf8) {
                wchar_t b[4];
                if (iswalnum(c) && c < 128) url += static_cast<wchar_t>(c);
                else { swprintf(b, 4, L"%%%02X", c); url += b; }
            }
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kLinkPinned: SetView(View::Apps); return;
        case kLinkRecent: ShellExecuteW(nullptr, L"open", L"explorer.exe", L"shell:recent", nullptr, SW_SHOWNORMAL); break;
    }
    Hide();
}

void StartMenu::ContextMenu(const Item& it) {
    if (it.kind == kLinkPinned || it.kind == kLinkRecent || it.kind == kSettingRow || it.kind == kWebRow) return;
    POINT pt; GetCursorPos(&pt);
    std::vector<aether::ui::MenuEntry> items;
    items.push_back(aether::ui::MenuItem(1, i18n::Pick(L"Open", L"Aç"), 0xE8A7));
    const bool isApp = it.kind == kAppTile || it.kind == kAppRow;
    std::wstring parse;
    if (isApp && it.ref >= 0 && it.ref < static_cast<int>(m_apps.size())) {
        parse = m_apps[it.ref].parse;
        items.push_back(aether::ui::MenuItem(2, IsPinned(parse) ? i18n::Pick(L"Unpin from Start", L"Başlangıç'tan kaldır") : i18n::Pick(L"Pin to Start", L"Başlangıç'a sabitle"), 0xE718));
    } else {
        items.push_back(aether::ui::MenuItem(3, i18n::Pick(L"Open file location", L"Dosya konumunu aç"), 0xE838));
    }
    m_menuOpen = true;
    const int cmd = aether::ui::ShowContextMenu(items, pt, false);
    m_menuOpen = false;
    if (cmd == 1) LaunchItem(it);
    else if (cmd == 2 && !parse.empty()) TogglePin(parse);
    else if (cmd == 3) {
        const FileItem* f = nullptr;
        if (it.kind == kRecentRow && it.ref >= 0 && it.ref < static_cast<int>(m_recents.size())) f = &m_recents[it.ref];
        else if (it.kind == kFileRow) {
            if (it.ref < 0) { const int i = -(it.ref + 1); if (i < static_cast<int>(m_recents.size())) f = &m_recents[i]; }
            else if (it.ref < static_cast<int>(m_files.size())) f = &m_files[it.ref];
        }
        if (f) {
            const std::wstring args = L"/select,\"" + f->path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            Hide();
        }
    }
}

void StartMenu::Activate(int id, bool rightClick) {
    if (id >= kIdItemBase) {
        if (const Item* it = FindItem(id)) { if (rightClick) ContextMenu(*it); else LaunchItem(*it); }
        return;
    }
    if (rightClick) return;
    if (id >= kIdPowerBase && id < kIdPowerBase + 5) { ShowPowerAction(id - kIdPowerBase); return; }
    switch (id) {
        case kIdNavHome:      m_query.clear(); SetView(View::Home); break;
        case kIdNavApps:      m_query.clear(); SetView(View::Apps); break;
        case kIdNavDocs:      OpenFolder(0); break;
        case kIdNavDownloads: OpenFolder(1); break;
        case kIdNavMusic:     OpenFolder(2); break;
        case kIdNavPictures:  OpenFolder(3); break;
        case kIdNavVideos:    OpenFolder(4); break;
        case kIdSettings:     ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL); Hide(); break;
        case kIdAvatar:       ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL); Hide(); break;
        case kIdPower:        m_powerOpen = !m_powerOpen; break;
        case kIdPin:          m_stayOpen = !m_stayOpen; break;
        default: break;
    }
}

void StartMenu::PasteClipboard() {
    if (!OpenClipboard(m_hwnd)) return;
    std::wstring text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            for (; *p && text.size() < 200; ++p) if (*p >= 32) text += *p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (!text.empty()) SetQuery(m_query + text);
}

// =============================================================================================
//  Rendering
// =============================================================================================

void StartMenu::DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f, const D2D1_RECT_F& r, const Color& c) {
    if (!f || t.empty()) return;
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, &r, m_brush.Get());
}

void StartMenu::DrawGlyph(ID2D1DeviceContext* ctx, wchar_t g, const D2D1_RECT_F& r, const Color& c, IDWriteTextFormat* f) {
    IDWriteTextFormat* fmt = f ? f : m_fIcon.Get();
    if (!m_hasIconFont || !fmt) return;
    wchar_t buf[2] = {g, 0};
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(buf, 1, fmt, &r, m_brush.Get());
}

void StartMenu::Fill(ID2D1DeviceContext* ctx, const Rect& r, float radius, const Color& c) {
    if (auto g = m_dcomp->CreateSquircleGeometry(r, radius)) {
        m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
        ctx->FillGeometry(g.Get(), m_brush.Get());
    }
}

void StartMenu::Stroke(ID2D1DeviceContext* ctx, const Rect& r, float radius, const Color& c, float w) {
    if (auto g = m_dcomp->CreateSquircleGeometry(r, radius)) {
        m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
        ctx->DrawGeometry(g.Get(), m_brush.Get(), w);
    }
}

void StartMenu::Render() {
    const float s = m_scale;
    const float o = Clamp01(m_open.GetValue());
    const Rect panel = Px(Rect{0, 0, kPanelW, kPanelH});
    const float R = 32.0f * s;
    auto body = m_dcomp->CreateSquircleGeometry(panel, R);
    auto silhouette = render::RecordSilhouette(m_dcomp.get(), {body.Get()}, s, 1.0f);   // must happen before BeginDraw
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    // ---- hover bookkeeping -------------------------------------------------------------------
    m_hoverId = (m_mouse.x >= 0.0f && m_wantOpen) ? HitTest(m_mouse) : 0;
    for (auto it = m_hoverAmt.begin(); it != m_hoverAmt.end();) {
        const float target = it->first == m_hoverId ? 1.0f : 0.0f;
        it->second += (target - it->second) * 0.35f;
        if (target == 0.0f && it->second < 0.01f) it = m_hoverAmt.erase(it); else ++it;
    }
    if (m_hoverId > 0 && !m_hoverAmt.count(m_hoverId)) m_hoverAmt[m_hoverId] = 0.2f;

    const D2D1_MATRIX_3X2_F outer = D2D1::Matrix3x2F::Scale(0.95f + 0.05f * o, 0.95f + 0.05f * o, D2D1::Point2F(panel.Center().x, panel.Bottom())) *
                                    D2D1::Matrix3x2F::Translation(0.0f, (1.0f - o) * 20.0f * s);
    ctx->SetTransform(outer);
    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                       D2D1::IdentityMatrix(), o, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    ctx->PushLayer(&lp, nullptr);

    // ---- body ---------------------------------------------------------------------------------
    render::DrawGlassGlow(ctx, silhouette.Get(), s, 1.0f);
    if (body && m_backdrop) render::FillBackdrop(ctx, m_backdrop, body.Get(), panel, 1.0f);   // frosted backdrop under the glass body
    if (body) {
        render::DrawGlassBody(ctx, m_dcomp.get(), m_brush.Get(), body.Get(), panel, s, 1.0f, m_backdrop ? 0.46f : 1.0f);
        {   // the rail is a darker, slightly deeper pane than the content area
            D2D1_LAYER_PARAMETERS1 rl = D2D1::LayerParameters1(D2D1::InfiniteRect(), body.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                               D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
            ctx->PushLayer(&rl, nullptr);
            if (auto rg = m_dcomp->CreateLinearGradientBrush(Color{0.0f, 0.0f, 0.0f, 0.34f}, Color{0.0f, 0.0f, 0.0f, 0.20f},
                                                             D2D1::Point2F(panel.x, panel.y), D2D1::Point2F(panel.x + kRailW * s, panel.y)))
                ctx->FillRectangle(RF(panel.x, panel.y, panel.x + kRailW * s, panel.Bottom()), rg.Get());
            ctx->PopLayer();
        }
        {   // a band of light that slowly travels around the rim (reflection), plus a soft bloom of it
            const float ang = m_time * 0.45f;
            const Vec2 c = panel.Center();
            const float L = std::max(panel.width, panel.height) * 0.62f;
            const D2D1_POINT_2F p0 = D2D1::Point2F(c.x + std::cos(ang) * L, c.y + std::sin(ang) * L * 0.8f);
            const D2D1_POINT_2F p1 = D2D1::Point2F(c.x - std::cos(ang) * L, c.y - std::sin(ang) * L * 0.8f);
            const Color hot = Color::Lerp(Color{1.0f, 1.0f, 1.0f, 1.0f}, render::glass::kVioletLight, 0.35f);
            if (auto rim = m_dcomp->CreateLinearGradientBrush({{0.00f, A(render::glass::kAccent2, 0.0f)}, {0.36f, A(render::glass::kVioletLight, 0.0f)},
                                                               {0.50f, A(hot, 0.80f)}, {0.64f, A(render::glass::kAccent2, 0.0f)}, {1.00f, A(render::glass::kAccent2, 0.0f)}}, p0, p1))
                ctx->DrawGeometry(body.Get(), rim.Get(), 1.5f * s);
            if (auto bloom = m_dcomp->CreateLinearGradientBrush({{0.00f, A(render::glass::kAccent2, 0.0f)}, {0.30f, A(render::glass::kViolet, 0.0f)},
                                                                 {0.50f, A(render::glass::kViolet, 0.30f)}, {0.70f, A(render::glass::kAccent2, 0.0f)}, {1.00f, A(render::glass::kAccent2, 0.0f)}}, p0, p1))
                ctx->DrawGeometry(body.Get(), bloom.Get(), 7.0f * s);
        }
        // divider between the rail and the content pane
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.07f));
        ctx->FillRectangle(RF(panel.x + kRailW * s, panel.y + 1.0f * s, panel.x + kRailW * s + 1.0f * s, panel.Bottom() - 1.0f * s), m_brush.Get());
    }

    // ---- left rail ----------------------------------------------------------------------------
    {
        const Rect av = Px(Rect{24, 37, 50, 50});
        const Vec2 ac = av.Center();
        const float ar = av.width / 2;
        if (m_avatar) {
            ComPtr<ID2D1BitmapBrush> bb;
            if (SUCCEEDED(ctx->CreateBitmapBrush(m_avatar.Get(), &bb)) && bb) {
                const D2D1_SIZE_F sz = m_avatar->GetSize();
                const float k = av.width / std::min(sz.width, sz.height);
                bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(av.x - (sz.width * k - av.width) / 2, av.y - (sz.height * k - av.height) / 2));
                bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ac.x, ac.y), ar, ar), bb.Get());
            }
        } else {
            auto g = m_dcomp->CreateLinearGradientBrush(ui::kPurpleLight, ui::kPurpleDeep, D2D1::Point2F(av.x, av.y), D2D1::Point2F(av.Right(), av.Bottom()));
            if (g) ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ac.x, ac.y), ar, ar), g.Get());
            const std::wstring initial(1, static_cast<wchar_t>(towupper(m_userName.empty() ? L'U' : m_userName[0])));
            DrawText(ctx, initial, m_fCenter.Get(), RF(av), W(1.0f));
        }
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.16f + 0.2f * Hover(kIdAvatar)));
        ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ac.x, ac.y), ar, ar), m_brush.Get(), 1.2f * s);
        DrawText(ctx, m_userName, m_fName.Get(), RF(Px(Rect{88, 44, 124, 20})), W(0.96f));
        DrawText(ctx, m_userSub, m_fSub.Get(), RF(Px(Rect{88, 63, 124, 16})), W(0.52f));
    }

    struct Nav { int id; float y; wchar_t glyph; int special; const wchar_t* en; const wchar_t* tr; };
    const Nav nav[] = {
        {kIdNavHome, 124, 0, 1, L"Start", L"Başlat"},
        {kIdNavApps, 168, 0, 2, L"All apps", L"Tüm Uygulamalar"},
        {kIdNavDocs, 212, ui::kGlyphDocument, 0, L"Documents", L"Belgeler"},
        {kIdNavDownloads, 256, ui::kGlyphDownload, 0, L"Downloads", L"İndirilenler"},
        {kIdNavMusic, 300, ui::kGlyphMusic, 0, L"Music", L"Müzik"},
        {kIdNavPictures, 344, ui::kGlyphPicture, 0, L"Pictures", L"Resimler"},
        {kIdNavVideos, 388, ui::kGlyphVideo, 0, L"Videos", L"Videolar"},
        {kIdSettings, 445, ui::kGlyphSettings, 0, L"Settings", L"Ayarlar"},
        {kIdPower, 488, ui::kGlyphPower, 0, L"Power", L"Güç"},
    };
    const int activeNav = m_view == View::Apps ? kIdNavApps : kIdNavHome;
    for (const Nav& n : nav) {
        const Rect r = Px(Rect{21, n.y, 184, 40});
        const float h = Hover(n.id);
        const bool active = n.id == activeNav || (n.id == kIdPower && m_powerOpen);
        Color ink = W(0.86f + 0.14f * h);
        Color iconInk = ink;
        if (active) {
            // concept look: colour fades from a saturated violet on the left into the panel on the right, a thin
            // lit accent bar on the left edge, soft bloom - not a uniform purple block
            if (auto bloom = m_dcomp->CreateLinearGradientBrush({{0.0f, A(ui::kPurple, 0.20f)}, {1.0f, A(ui::kPurple, 0.0f)}},
                                                                D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.Right(), r.y)))
                if (auto geo = m_dcomp->CreateSquircleGeometry(Rect{r.x - 4.0f * s, r.y - 4.0f * s, r.width + 8.0f * s, r.height + 8.0f * s}, 16.0f * s))
                    ctx->FillGeometry(geo.Get(), bloom.Get());
            if (auto geo = m_dcomp->CreateSquircleGeometry(r, 12.0f * s)) {
                if (auto g = m_dcomp->CreateLinearGradientBrush({{0.00f, A(ui::kPurple, 0.88f)}, {0.45f, A(ui::kPurpleDeep, 0.52f)}, {1.00f, A(ui::kPurpleDeep, 0.10f)}},
                                                                D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.Right(), r.y)))
                    ctx->FillGeometry(geo.Get(), g.Get());
                if (auto sheen = m_dcomp->CreateLinearGradientBrush(W(0.10f), W(0.0f), D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.y + r.height * 0.55f)))
                    ctx->FillGeometry(geo.Get(), sheen.Get());
                if (auto rim = m_dcomp->CreateLinearGradientBrush({{0.0f, A(ui::kPurpleLight, 0.50f)}, {0.6f, A(ui::kPurpleLight, 0.10f)}, {1.0f, A(ui::kPurpleLight, 0.0f)}},
                                                                  D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.Right(), r.y)))
                    ctx->DrawGeometry(geo.Get(), rim.Get(), 1.0f * s);
            }
            {   // left accent bar with a little glow
                const float bh = r.height * 0.56f, bx = r.x + 1.5f * s, by = r.Center().y - bh / 2;
                m_brush->SetColor(D2D1::ColorF(ui::kPurpleLight.r, ui::kPurpleLight.g, ui::kPurpleLight.b, 0.28f));
                ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(bx - 2.0f * s, by - 2.0f * s, bx + 5.0f * s, by + bh + 2.0f * s), 3.5f * s, 3.5f * s), m_brush.Get());
                m_brush->SetColor(D2D1::ColorF(ui::kPurpleLight.r, ui::kPurpleLight.g, ui::kPurpleLight.b, 0.95f));
                ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(bx, by, bx + 2.4f * s, by + bh), 1.2f * s, 1.2f * s), m_brush.Get());
            }
            ink = W(1.0f);
            iconInk = Color::Lerp(W(1.0f), A(ui::kPurpleLight, 1.0f), 0.40f);   // lavender-tinted glyph like the concept
        } else if (h > 0.01f) {
            Fill(ctx, r, 12.0f * s, W(0.065f * h));
        }
        const float cx = r.x + 27.0f * s, cy = r.Center().y;
        if (n.special == 1) {            // Start: filled 2x2
            const float cell = 7.0f * s, gap = 2.4f * s;
            for (int iy = 0; iy < 2; ++iy) for (int ix = 0; ix < 2; ++ix) {
                const float x = cx - cell - gap / 2 + ix * (cell + gap), y = cy - cell - gap / 2 + iy * (cell + gap);
                m_brush->SetColor(D2D1::ColorF(iconInk.r, iconInk.g, iconInk.b, iconInk.a));
                ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(x, y, x + cell, y + cell), 1.8f * s, 1.8f * s), m_brush.Get());
            }
        } else if (n.special == 2) {     // All apps: outlined 2x2
            const float cell = 7.0f * s, gap = 2.4f * s;
            for (int iy = 0; iy < 2; ++iy) for (int ix = 0; ix < 2; ++ix) {
                const float x = cx - cell - gap / 2 + ix * (cell + gap), y = cy - cell - gap / 2 + iy * (cell + gap);
                m_brush->SetColor(D2D1::ColorF(iconInk.r, iconInk.g, iconInk.b, iconInk.a));
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(RF(x + 0.5f * s, y + 0.5f * s, x + cell - 0.5f * s, y + cell - 0.5f * s), 1.8f * s, 1.8f * s), m_brush.Get(), 1.2f * s);
            }
        } else {
            DrawGlyph(ctx, n.glyph, RF(cx - 14.0f * s, r.y, cx + 14.0f * s, r.Bottom()), iconInk);
        }
        DrawText(ctx, i18n::Pick(n.en, n.tr), m_fNav.Get(), RF(r.x + 52.0f * s, r.y, r.Right() - 8.0f * s, r.Bottom()), ink);
    }

    {   // hairline above Settings / Power
        const Rect dv = Px(Rect{24, 431, 178, 1});
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.075f));
        ctx->FillRectangle(RF(dv), m_brush.Get());
    }

    // ---- search box -----------------------------------------------------------------------------
    {
        const Rect box = Px(Rect{kPaneX, kSearchY, kPaneW, kSearchH});
        Fill(ctx, box, box.height / 2, W(0.060f));
        Stroke(ctx, box, box.height / 2, m_query.empty() ? W(0.15f) : A(ui::kPurple, 0.65f), 1.0f * s);
        DrawGlyph(ctx, ui::kGlyphSearch, RF(box.x + 8.0f * s, box.y, box.x + 42.0f * s, box.Bottom()), W(0.88f), m_fIcon.Get());
        const float tx = box.x + 50.0f * s;
        const float tr = box.Right() - 46.0f * s;
        if (m_query.empty()) {
            DrawText(ctx, i18n::Pick(L"Search apps, settings and files...", L"Uygulama, ayar veya dosya arayın..."), m_fSearch.Get(), RF(tx, box.y, tr, box.Bottom()), W(0.46f));
        } else {
            DrawText(ctx, m_query, m_fSearch.Get(), RF(tx, box.y, tr, box.Bottom()), W(0.97f));
        }
        if (std::fmod(m_time, 1.1f) < 0.6f && m_wantOpen) {
            float cw = 0;
            if (!m_query.empty() && m_fSearch) {
                ComPtr<IDWriteTextLayout> tl;
                if (SUCCEEDED(m_dcomp->GetDWriteFactory()->CreateTextLayout(m_query.c_str(), static_cast<UINT32>(m_query.size()), m_fSearch.Get(), 4096, 40, &tl))) {
                    DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm); cw = std::min(tm.widthIncludingTrailingWhitespace, tr - tx - 4.0f * s);
                }
            }
            m_brush->SetColor(D2D1::ColorF(ui::kPurpleLight.r, ui::kPurpleLight.g, ui::kPurpleLight.b, 0.95f));
            ctx->FillRectangle(RF(tx + cw + 1.0f * s, box.y + 11.0f * s, tx + cw + 2.4f * s, box.Bottom() - 11.0f * s), m_brush.Get());
        }
        const Rect pin = Px(Rect{kPaneX + kPaneW - 40, kSearchY + 5, 32, 32});
        if (Hover(kIdPin) > 0.01f) Fill(ctx, pin, 16.0f * s, W(0.08f * Hover(kIdPin)));
        DrawGlyph(ctx, ui::kGlyphPin, RF(pin), m_stayOpen ? ui::kPurpleLight : W(0.60f), m_fIconSm.Get());
    }

    // ---- content ----------------------------------------------------------------------------------
    {
        const bool scrolls = m_view != View::Home;
        const float sc = scrolls ? m_scroll.GetValue() : 0.0f;
        const float t = Clamp01(m_viewT.GetValue());
        ctx->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, (1.0f - t) * 10.0f * s) * outer);
        D2D1_LAYER_PARAMETERS1 fade = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                             D2D1::IdentityMatrix(), t, nullptr, D2D1_LAYER_OPTIONS1_NONE);
        ctx->PushLayer(&fade, nullptr);
        if (scrolls) ctx->PushAxisAlignedClip(RF(Px(Rect{kPaneX - 8, ViewTop(), kPaneW + 16, ViewBottom() - ViewTop()})), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        auto at = [&](const Rect& r) { return Px(Rect{r.x, r.y - sc, r.width, r.height}); };
        const float vTop = Px(Rect{0, ViewTop(), 1, 1}).y, vBot = Px(Rect{0, ViewBottom(), 1, 1}).y;

        for (const Label& l : m_labels) {
            const Rect r = at(l.r);
            if (scrolls && (r.Bottom() < vTop || r.y > vBot)) continue;
            if (l.style == 4) { m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.10f)); ctx->FillRectangle(RF(r.x, r.y, r.Right(), r.y + std::max(1.0f, s)), m_brush.Get()); continue; }
            switch (l.style) {
                case 0: DrawText(ctx, l.text, m_fHead.Get(), RF(r), W(0.95f)); break;
                case 1: DrawText(ctx, l.text, m_fLetter.Get(), RF(r), A(ui::kPurpleLight, 0.85f)); break;
                case 2: DrawText(ctx, l.text, m_fNav.Get(), RF(r), W(0.46f)); break;
                default: DrawText(ctx, l.text, m_fSub.Get(), RF(r), W(0.5f)); break;
            }
        }

        int iconBudget = 5;
        for (const Item& it : m_items) {
            const Rect r = at(it.r);
            if (scrolls && (r.Bottom() < vTop || r.y > vBot)) continue;
            const float h = Hover(it.id);
            const bool sel = it.id == m_selId;
            auto rowBg = [&](const Rect& rr, float radius) {
                if (sel) { Fill(ctx, rr, radius, A(ui::kPurple, 0.16f)); Stroke(ctx, rr, radius, A(ui::kPurpleLight, 0.70f), 1.2f * s); }
                else if (h > 0.01f) Fill(ctx, rr, radius, W(0.075f * h));
            };

            switch (it.kind) {
                case kLinkPinned:
                case kLinkRecent:
                    DrawText(ctx, i18n::Pick(L"Show all", L"Tümünü Göster"), m_fLink.Get(), RF(r), W(0.52f + 0.40f * h));
                    break;

                case kAppTile: {
                    App& app = m_apps[it.ref];
                    if (!app.icon && !app.iconTried && iconBudget > 0) { EnsureAppIcon(app); --iconBudget; }
                    rowBg(Rect{r.x + 3 * s, r.y + 2 * s, r.width - 6 * s, r.height - 4 * s}, 14.0f * s);
                    const float isz = 30.0f * s, lift = 2.0f * s * h;
                    const float cx = r.Center().x;
                    {   // dark glass icon tile: top-lit rim, deep fill
                        const Rect tile{cx - 25.0f * s, r.y + 4.0f * s - lift, 50.0f * s, 50.0f * s};
                        if (auto g = m_dcomp->CreateSquircleGeometry(tile, 15.0f * s)) {
                            if (auto f = VGrad(ctx, Color{0.150f, 0.115f, 0.230f, 0.50f + 0.14f * h}, Color{0.070f, 0.052f, 0.120f, 0.58f}, tile.x, tile.y, tile.x, tile.Bottom()))
                                ctx->FillGeometry(g.Get(), f.Get());
                            if (auto rm = VGrad(ctx, A(ui::kPurpleLight, 0.30f + 0.35f * h), A(ui::kPurple, 0.07f + 0.15f * h), tile.x, tile.y, tile.Right(), tile.Bottom()))
                                ctx->DrawGeometry(g.Get(), rm.Get(), 1.0f * s);
                        }
                    }
                    const float ix = cx - isz / 2, iy = r.y + 4.0f * s + 25.0f * s - isz / 2 - lift;
                    if (app.icon) {
                        const D2D1_RECT_F dst = RF(ix, iy, ix + isz, iy + isz);
                        ctx->DrawBitmap(app.icon.Get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    } else Fill(ctx, Rect{ix, iy, isz, isz}, 10.0f * s, W(0.08f));
                    DrawText(ctx, ShortAppName(app.name), m_fTile.Get(), RF(r.x + 2 * s, r.y + 57.0f * s, r.Right() - 2 * s, r.y + 77.0f * s), W(0.88f));
                    break;
                }

                case kRecentRow: {
                    FileItem& f = m_recents[it.ref];
                    if (!f.icon && !f.iconTried && iconBudget > 0) { EnsureFileIcon(f); --iconBudget; }
                    rowBg(r, 14.0f * s);
                    const float isz = 28.0f * s;
                    const Rect tile{r.x + 10.0f * s, r.Center().y - 23.0f * s, 46.0f * s, 46.0f * s};
                    const float ix = tile.x + (tile.width - isz) / 2, iy = tile.y + (tile.height - isz) / 2;
                    if (auto g = m_dcomp->CreateSquircleGeometry(tile, 13.0f * s)) {
                        if (auto fl = VGrad(ctx, Color{0.150f, 0.115f, 0.230f, 0.50f + 0.14f * h}, Color{0.070f, 0.052f, 0.120f, 0.58f}, tile.x, tile.y, tile.x, tile.Bottom()))
                            ctx->FillGeometry(g.Get(), fl.Get());
                        if (auto rm = VGrad(ctx, A(ui::kPurpleLight, 0.26f + 0.35f * h), A(ui::kPurple, 0.06f + 0.15f * h), tile.x, tile.y, tile.Right(), tile.Bottom()))
                            ctx->DrawGeometry(g.Get(), rm.Get(), 1.0f * s);
                    }
                    if (!DrawFileBadge(ctx, m_brush.Get(), KindOfFile(f.name), Rect{ix, iy, isz, isz})) {
                        if (f.icon) { const D2D1_RECT_F dst = RF(ix - 2 * s, iy - 2 * s, ix + isz + 2 * s, iy + isz + 2 * s); ctx->DrawBitmap(f.icon.Get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC); }
                        else Fill(ctx, Rect{ix, iy, isz, isz}, 9.0f * s, W(0.08f));
                    }
                    DrawText(ctx, f.name, m_fRecName.Get(), RF(r.x + 66.0f * s, r.y + 7.0f * s, r.Right() - 8.0f * s, r.y + 28.0f * s), W(0.95f));
                    DrawText(ctx, Ago(f.time), m_fRecSub.Get(), RF(r.x + 66.0f * s, r.y + 27.0f * s, r.Right() - 8.0f * s, r.y + 46.0f * s), W(0.50f));
                    break;
                }

                case kAppRow:
                case kSettingRow:
                case kFileRow:
                case kWebRow: {
                    rowBg(r, 14.0f * s);
                    const float isz = 32.0f * s;
                    const Rect ib{r.x + 14.0f * s, r.Center().y - isz / 2, isz, isz};
                    std::wstring title, sub;
                    if (it.kind == kAppRow) {
                        App& app = m_apps[it.ref];
                        if (!app.icon && !app.iconTried && iconBudget > 0) { EnsureAppIcon(app); --iconBudget; }
                        if (app.icon) { const D2D1_RECT_F dst = RF(ib); ctx->DrawBitmap(app.icon.Get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC); }
                        else Fill(ctx, ib, 9.0f * s, W(0.08f));
                        title = app.name; sub = i18n::Pick(L"App", L"Uygulama");
                    } else if (it.kind == kSettingRow) {
                        Fill(ctx, ib, 9.0f * s, A(ui::kPurple, 0.28f));
                        DrawGlyph(ctx, ui::kGlyphSettings, RF(ib), ui::kPurpleLight);
                        title = i18n::Pick(kSettings[it.ref].en, kSettings[it.ref].tr); sub = i18n::Pick(L"Settings", L"Ayarlar");
                    } else if (it.kind == kFileRow) {
                        FileItem& f = it.ref < 0 ? m_recents[-(it.ref + 1)] : m_files[it.ref];
                        if (!f.icon && !f.iconTried && iconBudget > 0) { EnsureFileIcon(f); --iconBudget; }
                        if (!DrawFileBadge(ctx, m_brush.Get(), KindOfFile(f.name), ib)) {
                            if (f.icon) { const D2D1_RECT_F dst = RF(ib); ctx->DrawBitmap(f.icon.Get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC); }
                            else Fill(ctx, ib, 9.0f * s, W(0.08f));
                        }
                        title = f.name;
                        sub = f.path.substr(0, f.path.find_last_of(L'\\'));
                    } else {
                        Fill(ctx, ib, 9.0f * s, A(ui::kPurple, 0.28f));
                        DrawGlyph(ctx, ui::kGlyphGlobe, RF(ib), ui::kPurpleLight);
                        title = std::wstring(i18n::Pick(L"Search the web for ", L"Web'de ara: ")) + L"\"" + m_query + L"\"";
                        sub = L"Bing";
                    }
                    DrawText(ctx, title, m_fRowName.Get(), RF(r.x + 58.0f * s, r.y + 5.0f * s, r.Right() - 40.0f * s, r.y + 26.0f * s), W(0.96f));
                    DrawText(ctx, sub, m_fRecSub.Get(), RF(r.x + 58.0f * s, r.y + 25.0f * s, r.Right() - 40.0f * s, r.y + 43.0f * s), W(0.48f));
                    if (sel) DrawGlyph(ctx, ui::kGlyphEnter, RF(r.Right() - 38.0f * s, r.y, r.Right() - 8.0f * s, r.Bottom()), ui::kPurpleLight, m_fIconSm.Get());
                    break;
                }
            }
        }

        if (scrolls) {
            ctx->PopAxisAlignedClip();
            if (MaxScroll() > 1.0f) {   // scroll indicator
                const float viewH = vBot - vTop;
                const float thumbH = std::max(36.0f * s, viewH * viewH / (m_contentH * s));
                const float frac = sc / MaxScroll();
                const float ty = vTop + (viewH - thumbH) * frac;
                m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.20f));
                ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(panel.Right() - 14.0f * s, ty, panel.Right() - 10.0f * s, ty + thumbH), 2.0f * s, 2.0f * s), m_brush.Get());
            }
        }
        ctx->PopLayer();
        ctx->SetTransform(outer);
    }

    // ---- power pop-up -------------------------------------------------------------------------------
    if (m_powerOpen) {
        const Rect card = Px(Rect{20, 488 - 5 * 38 - 20, 188, 5 * 38 + 12});
        for (int i = 3; i >= 1; --i) Fill(ctx, Rect{card.x - i * 2 * s, card.y - i * 1 * s + 4 * s, card.width + i * 4 * s, card.height + i * 3 * s}, 18.0f * s + i * 2 * s, Color{0, 0, 0, 0.10f});
        Fill(ctx, card, 18.0f * s, Color{0.105f, 0.090f, 0.150f, 0.99f});
        Stroke(ctx, card, 18.0f * s, A(ui::kPurple, 0.40f), 1.0f * s);
        struct P { wchar_t g; const wchar_t* en; const wchar_t* tr; };
        const P pw[] = {{ui::kGlyphLock, L"Lock", L"Kilitle"}, {ui::kGlyphSignOut, L"Sign out", L"Oturumu kapat"}, {ui::kGlyphSleep, L"Sleep", L"Uyku"},
                        {ui::kGlyphRestart, L"Restart", L"Yeniden başlat"}, {ui::kGlyphPower, L"Shut down", L"Kapat"}};
        for (int k = 0; k < 5; ++k) {
            const Rect r{card.x + 6 * s, card.y + (6 + k * 38) * s, card.width - 12 * s, 36 * s};
            const float h = Hover(kIdPowerBase + k);
            if (h > 0.01f) Fill(ctx, r, 11.0f * s, W(0.09f * h));
            DrawGlyph(ctx, pw[k].g, RF(r.x + 6 * s, r.y, r.x + 38 * s, r.Bottom()), k == 4 ? Color{1.0f, 0.55f, 0.55f, 1.0f} : W(0.9f), m_fIconSm.Get());
            DrawText(ctx, i18n::Pick(pw[k].en, pw[k].tr), m_fNav.Get(), RF(r.x + 44 * s, r.y, r.Right() - 6 * s, r.Bottom()), W(0.94f));
        }
    }

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
            if (LOWORD(wp) == WA_INACTIVE && !m_menuOpen && !m_stayOpen) Hide();
            return 0;
        case WM_MOUSEMOVE: {
            m_mouse = Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            m_mouse = Vec2{-1.0f, -1.0f};
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                ui::ApplyCursor(m_hoverId > 0 && m_hoverId != kIdSearch);
                return TRUE;
            }
            break;
        case WM_MOUSEWHEEL: {
            if (m_view == View::Home) return 0;
            const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            m_scroll.SetTarget(std::clamp(m_scroll.GetTarget() - notches * 84.0f, 0.0f, MaxScroll()));
            return 0;
        }
        case WM_LBUTTONUP: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            const int id = HitTest(p);
            if (m_powerOpen && id != kIdPower && !(id >= kIdPowerBase && id < kIdPowerBase + 5)) { m_powerOpen = false; if (id == -1) return 0; }
            if (id > 0) Activate(id, false);
            return 0;
        }
        case WM_RBUTTONUP: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            const int id = HitTest(p);
            if (id >= kIdItemBase) Activate(id, true);
            return 0;
        }
        case WM_CHAR: {
            const wchar_t c = static_cast<wchar_t>(wp);
            if (c == 8) { if (!m_query.empty()) { std::wstring q = m_query; q.pop_back(); SetQuery(q); } }
            else if (c >= 32 && c != 127 && m_query.size() < 120) SetQuery(m_query + c);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE:
                    if (m_powerOpen) m_powerOpen = false;
                    else if (!m_query.empty()) SetQuery(L"");
                    else if (m_view != View::Home) SetView(View::Home);
                    else Hide();
                    return 0;
                case VK_RETURN: {
                    const Item* it = FindItem(m_selId);
                    if (!it) for (const Item& i : m_items) if (i.kind != kLinkPinned && i.kind != kLinkRecent) { it = &i; break; }
                    if (it) LaunchItem(*it);
                    return 0;
                }
                case VK_LEFT:  MoveSelection(-1, 0); return 0;
                case VK_RIGHT: MoveSelection(1, 0); return 0;
                case VK_UP:    MoveSelection(0, -1); return 0;
                case VK_DOWN:  MoveSelection(0, 1); return 0;
                case VK_PRIOR: m_scroll.SetTarget(std::max(0.0f, m_scroll.GetTarget() - 400.0f)); return 0;
                case VK_NEXT:  m_scroll.SetTarget(std::min(MaxScroll(), m_scroll.GetTarget() + 400.0f)); return 0;
                case 'V': if (GetKeyState(VK_CONTROL) & 0x8000) { PasteClipboard(); return 0; } break;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace aether::bar
