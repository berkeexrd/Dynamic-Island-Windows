#include "aether/bar/notifications.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <map>
#include <set>
#include <algorithm>
#include <thread>

namespace aether::bar {

namespace {
// Minimal view of the sqlite3 C API, resolved from the Windows system copy.
struct Sqlite {
    HMODULE mod{nullptr};
    int (*open_v2)(const char*, void**, int, const char*){nullptr};
    int (*prepare_v2)(void*, const char*, int, void**, const char**){nullptr};
    int (*step)(void*){nullptr};
    const unsigned char* (*column_text)(void*, int){nullptr};
    const void* (*column_blob)(void*, int){nullptr};
    int (*column_bytes)(void*, int){nullptr};
    long long (*column_int64)(void*, int){nullptr};
    int (*finalize)(void*){nullptr};
    int (*close)(void*){nullptr};
    bool Load() {
        if (mod) return open_v2 != nullptr;
        mod = LoadLibraryW(L"winsqlite3.dll");
        if (!mod) return false;
        auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(mod, n)); };
        open_v2 = reinterpret_cast<decltype(open_v2)>(get("sqlite3_open_v2"));
        prepare_v2 = reinterpret_cast<decltype(prepare_v2)>(get("sqlite3_prepare_v2"));
        step = reinterpret_cast<decltype(step)>(get("sqlite3_step"));
        column_text = reinterpret_cast<decltype(column_text)>(get("sqlite3_column_text"));
        column_blob = reinterpret_cast<decltype(column_blob)>(get("sqlite3_column_blob"));
        column_bytes = reinterpret_cast<decltype(column_bytes)>(get("sqlite3_column_bytes"));
        column_int64 = reinterpret_cast<decltype(column_int64)>(get("sqlite3_column_int64"));
        finalize = reinterpret_cast<decltype(finalize)>(get("sqlite3_finalize"));
        close = reinterpret_cast<decltype(close)>(get("sqlite3_close"));
        return open_v2 && prepare_v2 && step && column_text && column_blob && column_bytes && column_int64 && finalize && close;
    }
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Unescape(std::string s) {
    auto rep = [&](const char* a, const char* b) { size_t p = 0; const std::string A = a; while ((p = s.find(A, p)) != std::string::npos) { s.replace(p, A.size(), b); p += std::string(b).size(); } };
    rep("&lt;", "<"); rep("&gt;", ">"); rep("&quot;", "\""); rep("&apos;", "'"); rep("&#10;", " "); rep("&#13;", ""); rep("&amp;", "&");
    return s;
}

// <text ...>inner</text> elements in document order
std::vector<std::string> TextElements(const std::string& xml) {
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = xml.find("<text", p)) != std::string::npos) {
        const char c = p + 5 < xml.size() ? xml[p + 5] : 0;
        if (c != ' ' && c != '>' && c != '/') { p += 5; continue; }
        const size_t gt = xml.find('>', p);
        if (gt == std::string::npos) break;
        if (xml[gt - 1] == '/') { p = gt; continue; }
        const size_t end = xml.find("</text>", gt);
        if (end == std::string::npos) break;
        std::string inner = Unescape(xml.substr(gt + 1, end - gt - 1));
        if (!inner.empty()) out.push_back(inner);
        p = end + 7;
    }
    return out;
}

std::mutex g_appMtx;
std::map<std::wstring, AppVisual> g_apps;
std::set<std::wstring> g_appBusy;

void ResolveApp(std::wstring id) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppVisual v;
    Microsoft::WRL::ComPtr<IShellItem> item;
    std::wstring path = L"shell:AppsFolder\\" + id;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) && (id.find(L'\\') != std::wstring::npos || id.find(L".exe") != std::wstring::npos)) {
        item.Reset();
        hr = SHCreateItemFromParsingName(id.c_str(), nullptr, IID_PPV_ARGS(&item));
    }
    if (SUCCEEDED(hr) && item) {
        PWSTR nm = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &nm)) && nm) { v.name = nm; CoTaskMemFree(nm); }
        Microsoft::WRL::ComPtr<IShellItemImageFactory> fac;
        if (SUCCEEDED(item.As(&fac))) {
            HBITMAP hb = nullptr;
            if (SUCCEEDED(fac->GetImage(SIZE{96, 96}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
                BITMAP bm{};
                if (GetObjectW(hb, sizeof(bm), &bm) && bm.bmBitsPixel == 32 && bm.bmWidth > 0 && bm.bmHeight > 0) {
                    const int w = bm.bmWidth, hh = bm.bmHeight, n = std::max(w, hh);
                    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hh;
                    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                    std::vector<uint32_t> src(static_cast<size_t>(w) * hh);
                    HDC dc = GetDC(nullptr);
                    if (GetDIBits(dc, hb, 0, hh, src.data(), &bi, DIB_RGB_COLORS)) {
                        v.px.assign(static_cast<size_t>(n) * n, 0);
                        for (int y = 0; y < hh; ++y) for (int x = 0; x < w; ++x)
                            v.px[static_cast<size_t>(y + (n - hh) / 2) * n + x + (n - w) / 2] = src[static_cast<size_t>(y) * w + x];
                        v.size = n;
                    }
                    ReleaseDC(nullptr, dc);
                }
                DeleteObject(hb);
            }
        }
    }
    v.ready = true;
    {
        std::lock_guard<std::mutex> l(g_appMtx);
        g_apps[id] = std::move(v);
        g_appBusy.erase(id);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

std::wstring AppName(const std::string& id) {
    std::string s = id;
    const size_t bang = s.find('!');
    if (bang != std::string::npos) s = s.substr(0, bang);
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.size() > 4 && _stricmp(s.c_str() + s.size() - 4, ".exe") == 0) s.resize(s.size() - 4);
    const size_t us = s.find('_');
    if (us != std::string::npos) s = s.substr(0, us);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < s.size()) s = s.substr(dot + 1);
    return Widen(s);
}
} // namespace

bool NotificationFeed::AppVisualFor(const std::wstring& appId, AppVisual& out) {
    if (appId.empty()) return false;
    std::lock_guard<std::mutex> l(g_appMtx);
    auto it = g_apps.find(appId);
    if (it != g_apps.end()) { out = it->second; return out.ready; }
    if (g_appBusy.insert(appId).second) std::thread(ResolveApp, appId).detach();
    return false;
}

void NotificationFeed::Dismiss(long long id) { std::lock_guard<std::mutex> l(m_mutex); m_dismissed.push_back(id); m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [&](const NotifItem& i) { return i.id == id; }), m_items.end()); }
void NotificationFeed::DismissAll() {
    std::lock_guard<std::mutex> l(m_mutex);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    m_clearedBefore = static_cast<long long>(u.QuadPart / 10000000ULL) - 11644473600LL + 2;
    m_items.clear();
}

void NotificationFeed::RequestRefresh() {
    if (m_busy.exchange(true)) return;
    std::thread([this] { Work(); m_busy = false; }).detach();
}

void NotificationFeed::Work() {
    static Sqlite db;
    if (!db.Load()) { m_readable = false; return; }
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) { m_readable = false; return; }
    const std::wstring src = std::wstring(local) + L"\\Microsoft\\Windows\\Notifications\\wpndatabase.db";
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"aether_wpn";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring dst = dir + L"\\w.db";
    DeleteFileW((dst + L"-wal").c_str()); DeleteFileW((dst + L"-shm").c_str());
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { m_readable = false; return; }
    CopyFileW((src + L"-wal").c_str(), (dst + L"-wal").c_str(), FALSE);
    CopyFileW((src + L"-shm").c_str(), (dst + L"-shm").c_str(), FALSE);

    // sqlite wants UTF-8 paths
    const int n = WideCharToMultiByte(CP_UTF8, 0, dst.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string path(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, dst.c_str(), -1, path.data(), n, nullptr, nullptr);
    void* h = nullptr;
    if (db.open_v2(path.c_str(), &h, 2 /*READWRITE*/, nullptr) != 0 || !h) { m_readable = false; if (h) db.close(h); return; }

    std::vector<NotifItem> items;
    void* st = nullptr;
    const char* sql = "SELECT n.Id, h.PrimaryId, n.Payload, n.ArrivalTime, n.ExpiryTime FROM Notification n JOIN NotificationHandler h ON n.HandlerId = h.RecordId "
                      "WHERE n.Type = 'toast' ORDER BY n.ArrivalTime DESC LIMIT 60";
    bool ok = false;
    if (db.prepare_v2(h, sql, -1, &st, nullptr) == 0 && st) {
        ok = true;
        FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER nu; nu.LowPart = nowFt.dwLowDateTime; nu.HighPart = nowFt.dwHighDateTime;
        const long long nowTicks = static_cast<long long>(nu.QuadPart);
        while (db.step(st) == 100 /*SQLITE_ROW*/) {
            NotifItem it;
            it.id = db.column_int64(st, 0);
            const unsigned char* pid = db.column_text(st, 1);
            it.app = AppName(pid ? reinterpret_cast<const char*>(pid) : "");
            it.appId = Widen(pid ? reinterpret_cast<const char*>(pid) : "");
            const int bytes = db.column_bytes(st, 2);
            const void* blob = db.column_blob(st, 2);
            std::string xml;
            if (blob && bytes > 0) {
                xml.assign(static_cast<const char*>(blob), static_cast<size_t>(bytes));
                if (xml.size() >= 3 && static_cast<unsigned char>(xml[0]) == 0xEF) xml.erase(0, 3);
                if (xml.size() >= 2 && static_cast<unsigned char>(xml[0]) == 0xFF && static_cast<unsigned char>(xml[1]) == 0xFE) {   // UTF-16
                    std::wstring w(reinterpret_cast<const wchar_t*>(xml.data() + 2), (xml.size() - 2) / 2);
                    const int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
                    xml.assign(static_cast<size_t>(m), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), xml.data(), m, nullptr, nullptr);
                }
            }
            const auto texts = TextElements(xml);
            if (!texts.empty()) it.title = Widen(texts[0]);
            for (size_t i = 1; i < texts.size(); ++i) { if (!it.body.empty()) it.body += L"  "; it.body += Widen(texts[i]); }
            const long long arrival = db.column_int64(st, 3), expiry = db.column_int64(st, 4);
            if (expiry > 0 && expiry < nowTicks) continue;
            it.arrivalUnix = arrival / 10000000LL - 11644473600LL;
            if (it.title.empty() && it.body.empty()) continue;
            items.push_back(std::move(it));
        }
        db.finalize(st);
    }
    db.close(h);
    m_readable = ok;
    if (!ok) return;
    std::lock_guard<std::mutex> l(m_mutex);
    items.erase(std::remove_if(items.begin(), items.end(), [&](const NotifItem& i) {
        return i.arrivalUnix <= m_clearedBefore || std::find(m_dismissed.begin(), m_dismissed.end(), i.id) != m_dismissed.end();
    }), items.end());
    if (items.size() > 40) items.resize(40);
    m_items = std::move(items);
}

} // namespace aether::bar
