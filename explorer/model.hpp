#pragma once
// Data layer of Aether Explorer: everything here runs on worker threads (no UI, no Direct2D).
// Directory listing uses FindFirstFileEx(FindExInfoBasic, LARGE_FETCH) = the cheapest enumeration Windows offers.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <dxgi.h>
#include <lmcons.h>
#define SECURITY_WIN32
#include <security.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace aether::explorer {

struct FileEntry {
    std::wstring name;
    std::wstring ext;          // lower case, with dot
    bool dir{false};
    bool hidden{false};
    uint64_t size{0};
    FILETIME mtime{}, ctime{};
    DWORD attr{0};
    std::wstring parent;       // only set for deep-search results
};

struct DriveInfo {
    std::wstring root;         // "C:\"
    std::wstring label;        // "Windows (C:)"
    UINT type{0};
    uint64_t total{0}, freeBytes{0};
};

struct QuickFolder {
    std::wstring name, path;
    wchar_t glyph{0};
    int count{-1};             // filled in by the home worker
};

struct RecentItem {
    std::wstring name, dir, path, ext;
    FILETIME t{};
};

struct HomeData {
    std::wstring userName;
    std::vector<DriveInfo> drives;
    std::vector<int> quickCounts;
    std::vector<RecentItem> recent;
    std::wstring cpu, gpu, ram;
    uint64_t ramTotal{0};
};

struct FolderResult {
    uint32_t gen{0};
    std::wstring path;
    bool ok{false};
    bool deep{false};
    std::vector<FileEntry> items;
};

inline std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

inline std::wstring ExtOf(const std::wstring& name) {
    const size_t p = name.find_last_of(L'.');
    if (p == std::wstring::npos || p == 0 || p + 1 >= name.size()) return L"";
    return Lower(name.substr(p));
}

inline bool IsFtp(const std::wstring& p) { return p.size() >= 6 && _wcsnicmp(p.c_str(), L"ftp://", 6) == 0; }
inline bool IsNetRoot(const std::wstring& p) { return p == L"net:"; }
// \\server (no share part): the shares of that computer are listed through WNet
inline bool IsUncServer(const std::wstring& p) {
    return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p.find(L'\\', 2) == std::wstring::npos;
}
// paths that are not plain local folders (no shell clipboard / drag, no change watching, no thumbnails)
inline bool IsVirtualPath(const std::wstring& p) { return IsFtp(p) || IsNetRoot(p) || IsUncServer(p); }

inline std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (IsNetRoot(dir)) return L"\\\\" + name;                    // net: + SERVER -> \\SERVER
    if (IsFtp(dir)) return dir.back() == L'/' ? dir + name : dir + L"/" + name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

// Parent folder; L"" = "This PC" (home)
inline std::wstring ParentOf(std::wstring p) {
    if (IsNetRoot(p)) return L"";
    if (IsFtp(p)) {
        while (p.size() > 6 && p.back() == L'/') p.pop_back();
        const size_t root = p.find(L'/', 6);                           // start of the path part
        if (root == std::wstring::npos) return L"";
        const size_t i = p.find_last_of(L'/');
        return i == root ? p.substr(0, root + 1) : p.substr(0, i);
    }
    if (IsUncServer(p)) return L"net:";
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() <= 3) return L"";                 // "C:\" -> home
    const size_t i = p.find_last_of(L'\\');
    if (i == std::wstring::npos) return L"";
    if (i <= 2) return p.substr(0, i + 1);        // "C:\x" -> "C:\"
    if (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p.find(L'\\', 2) == i) return p.substr(0, i);   // \\server\share -> \\server
    return p.substr(0, i);
}

inline bool FtpList(const std::wstring& url, struct FolderResult& out);
inline bool NetComputers(struct FolderResult& out);
inline bool NetShares(const std::wstring& server, struct FolderResult& out);

inline bool EnumerateFolder(const std::wstring& path, FolderResult& out) {
    if (IsFtp(path)) return FtpList(path, out);
    if (IsNetRoot(path)) return NetComputers(out);
    WIN32_FIND_DATAW fd;
    const std::wstring pat = JoinPath(path, L"*");
    HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return IsUncServer(path) ? NetShares(path, out) : false;
    out.items.reserve(256);
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        FileEntry e;
        e.name = fd.cFileName;
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.hidden = (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime; e.ctime = fd.ftCreationTime; e.attr = fd.dwFileAttributes;
        if (!e.dir) e.ext = ExtOf(e.name);
        out.items.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// Recursive name search (iterative, skips junctions, cancellable through the generation counter, time-boxed)
inline void DeepSearch(const std::wstring& root, const std::wstring& query, const std::atomic<uint32_t>& gen, uint32_t mine,
                       bool showHidden, FolderResult& out, size_t cap = 3000, DWORD budgetMs = 12000) {
    const ULONGLONG t0 = GetTickCount64();
    std::vector<std::wstring> stack{root};
    while (!stack.empty() && out.items.size() < cap) {
        if (gen.load() != mine || GetTickCount64() - t0 > budgetMs) break;
        const std::wstring dir = std::move(stack.back()); stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            const bool hid = (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
            if (hid && !showHidden) continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (StrStrIW(fd.cFileName, query.c_str())) {
                FileEntry e;
                e.name = fd.cFileName; e.dir = isDir; e.hidden = hid; e.parent = dir; e.attr = fd.dwFileAttributes;
                e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                e.mtime = fd.ftLastWriteTime; e.ctime = fd.ftCreationTime;
                if (!isDir) e.ext = ExtOf(e.name);
                out.items.push_back(std::move(e));
                if (out.items.size() >= cap) break;
            }
            if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(JoinPath(dir, fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    out.ok = true;
}

// folder size / item count for the preview pane
inline void FolderStats(const std::wstring& root, const std::atomic<uint32_t>& gen, uint32_t mine, int& count, uint64_t& size, DWORD budgetMs = 8000) {
    const ULONGLONG t0 = GetTickCount64();
    count = 0; size = 0;
    std::vector<std::wstring> stack{root};
    while (!stack.empty()) {
        if (gen.load() != mine || GetTickCount64() - t0 > budgetMs) break;
        const std::wstring dir = std::move(stack.back()); stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            ++count;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(JoinPath(dir, fd.cFileName)); }
            else size += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

inline int CountItems(const std::wstring& path, int cap = 9999) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(JoinPath(path, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int n = 0;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (++n >= cap) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

inline std::vector<DriveInfo> QueryDrives() {
    std::vector<DriveInfo> v;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        DriveInfo d;
        d.root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        d.type = GetDriveTypeW(d.root.c_str());
        if (d.type == DRIVE_NO_ROOT_DIR || d.type == DRIVE_UNKNOWN) continue;
        wchar_t label[MAX_PATH + 1] = {};
        UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
        const bool ok = GetVolumeInformationW(d.root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) != 0;
        ULARGE_INTEGER freeB{}, total{}, tf{};
        if (ok && GetDiskFreeSpaceExW(d.root.c_str(), &freeB, &total, &tf)) { d.total = total.QuadPart; d.freeBytes = freeB.QuadPart; }
        SetErrorMode(oldMode);
        if (d.type == DRIVE_CDROM && !ok) continue;      // empty optical drive
        std::wstring name = label;
        if (name.empty()) name = d.type == DRIVE_CDROM ? L"CD Sürücüsü" : d.type == DRIVE_REMOVABLE ? L"Çıkarılabilir Disk"
                                  : d.type == DRIVE_REMOTE ? L"Ağ Sürücüsü" : L"Yerel Disk";
        d.label = name + L" (" + d.root.substr(0, 2) + L")";
        v.push_back(std::move(d));
    }
    return v;
}

inline std::wstring ResolveLink(const std::wstring& lnk) {
    Microsoft::WRL::ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return L"";
    Microsoft::WRL::ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf)) || FAILED(pf->Load(lnk.c_str(), STGM_READ))) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    if (FAILED(sl->GetPath(buf, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) return L"";
    return buf;
}

inline std::vector<RecentItem> QueryRecent(size_t want) {
    std::vector<RecentItem> out;
    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &dir)) || !dir) return out;
    const std::wstring d = dir;
    CoTaskMemFree(dir);
    struct L { std::wstring file; FILETIME t; };
    std::vector<L> links;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(JoinPath(d, L"*.lnk").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do { links.push_back({fd.cFileName, fd.ftLastWriteTime}); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(links.begin(), links.end(), [](const L& a, const L& b) { return CompareFileTime(&a.t, &b.t) > 0; });
    int tries = 0;
    for (const auto& l : links) {
        if (out.size() >= want || ++tries > 40) break;
        const std::wstring target = ResolveLink(JoinPath(d, l.file));
        if (target.empty()) continue;
        const DWORD attr = GetFileAttributesW(target.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        RecentItem r;
        r.path = target;
        const size_t s = target.find_last_of(L'\\');
        r.name = s == std::wstring::npos ? target : target.substr(s + 1);
        r.dir = s == std::wstring::npos ? L"" : target.substr(0, s);
        r.ext = ExtOf(r.name);
        r.t = l.t;
        out.push_back(std::move(r));
    }
    return out;
}

inline std::wstring CpuName() {
    wchar_t buf[256] = {}; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"—";
    std::wstring s = buf, r;
    bool sp = false;
    for (wchar_t c : s) { if (c == L' ') { if (!sp && !r.empty()) r += c; sp = true; } else { r += c; sp = false; } }
    while (!r.empty() && r.back() == L' ') r.pop_back();
    for (const wchar_t* junk : {L"(R)", L"(TM)", L"(tm)", L"CPU "}) {
        size_t p;
        while ((p = r.find(junk)) != std::wstring::npos) r.erase(p, wcslen(junk));
    }
    return r;
}

inline std::wstring GpuName() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return L"—";
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        return d.Description;
    }
    return L"—";
}

inline std::wstring UserDisplayName() {
    wchar_t buf[256] = {}; ULONG n = 256;
    if (GetUserNameExW(NameDisplay, buf, &n) && buf[0]) return buf;
    wchar_t u[UNLEN + 1] = {}; DWORD un = UNLEN + 1;
    if (GetUserNameW(u, &un) && u[0]) return u;
    return L"Kullanıcı";
}

} // namespace aether::explorer
