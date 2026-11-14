#pragma once
// Network side of Aether Explorer: FTP (WinINet), SMB neighbourhood (WNet), saved connections (DPAPI-protected passwords),
// remote<->local transfers and the small native input dialog used by the connection manager.
// Everything except the dialog helpers is blocking and meant for worker threads.
#include <windows.h>
#include <wininet.h>
#include <winnetwk.h>
#include <wincrypt.h>
#include <shobjidl.h>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "model.hpp"

namespace aether::explorer {

// ---- FTP URLs:  ftp://[user@]host[:port]/path   (passwords never live in the path, only in the session registry) ----
struct FtpUrl {
    std::wstring user, host, path{L"/"};
    unsigned port{21};
    bool ok{false};
};

inline FtpUrl ParseFtp(const std::wstring& u, std::wstring* passOut = nullptr) {
    FtpUrl r;
    if (!IsFtp(u)) return r;
    const std::wstring rest = u.substr(6);
    const size_t sl = rest.find(L'/');
    const std::wstring auth = sl == std::wstring::npos ? rest : rest.substr(0, sl);
    r.path = sl == std::wstring::npos ? L"/" : rest.substr(sl);
    std::wstring hostport = auth;
    const size_t at = auth.find_last_of(L'@');
    if (at != std::wstring::npos) {
        const std::wstring ui = auth.substr(0, at);
        hostport = auth.substr(at + 1);
        const size_t c = ui.find(L':');
        if (c != std::wstring::npos) { r.user = ui.substr(0, c); if (passOut) *passOut = ui.substr(c + 1); }
        else r.user = ui;
    }
    const size_t pc = hostport.find_last_of(L':');
    if (pc != std::wstring::npos) { r.port = static_cast<unsigned>(_wtoi(hostport.substr(pc + 1).c_str())); hostport = hostport.substr(0, pc); }
    r.host = hostport;
    if (r.port == 0) r.port = 21;
    r.ok = !r.host.empty();
    return r;
}

inline std::wstring FtpKey(const FtpUrl& u) { return Lower(u.user + L"@" + u.host + L":" + std::to_wstring(u.port)); }

struct FtpPasswords {
    std::mutex mu;
    std::map<std::wstring, std::wstring> pass;
};
inline FtpPasswords& Passwords() { static FtpPasswords* p = new FtpPasswords(); return *p; }
inline void SetFtpPass(const FtpUrl& u, const std::wstring& pw) { auto& P = Passwords(); std::lock_guard<std::mutex> l(P.mu); P.pass[FtpKey(u)] = pw; }
inline bool GetFtpPass(const FtpUrl& u, std::wstring& pw) {
    auto& P = Passwords(); std::lock_guard<std::mutex> l(P.mu);
    auto it = P.pass.find(FtpKey(u));
    if (it == P.pass.end()) return false;
    pw = it->second; return true;
}

inline std::wstring BuildFtp(const std::wstring& user, const std::wstring& host, unsigned port, std::wstring path) {
    if (path.empty() || path[0] != L'/') path = L"/" + path;
    return L"ftp://" + (user.empty() ? L"" : user + L"@") + host + (port != 21 && port != 0 ? L":" + std::to_wstring(port) : L"") + path;
}

// Canonical form; a password typed into the URL is moved into the registry.
inline std::wstring NormalizeFtp(const std::wstring& url) {
    std::wstring pw;
    const FtpUrl u = ParseFtp(url, &pw);
    if (!u.ok) return url;
    if (!pw.empty()) SetFtpPass(u, pw);
    std::wstring p = u.path;
    while (p.size() > 1 && p.back() == L'/') p.pop_back();
    return BuildFtp(u.user, u.host, u.port, p);
}

// ---- WinINet client --------------------------------------------------------------------------------------------------
class FtpClient {
public:
    ~FtpClient() { Close(); }
    bool Open(const std::wstring& url) {
        const FtpUrl u = ParseFtp(url);
        if (!u.ok) return false;
        std::wstring pw;
        if (!u.user.empty()) GetFtpPass(u, pw);
        m_i = InternetOpenW(L"AetherExplorer", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (!m_i) return false;
        DWORD t = 10000;
        InternetSetOptionW(m_i, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
        t = 25000;
        InternetSetOptionW(m_i, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
        InternetSetOptionW(m_i, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));
        m_c = InternetConnectW(m_i, u.host.c_str(), static_cast<INTERNET_PORT>(u.port), u.user.empty() ? nullptr : u.user.c_str(),
                               u.user.empty() ? nullptr : pw.c_str(), INTERNET_SERVICE_FTP, INTERNET_FLAG_PASSIVE, 0);
        return m_c != nullptr;
    }
    void Close() {
        if (m_c) InternetCloseHandle(m_c);
        if (m_i) InternetCloseHandle(m_i);
        m_c = m_i = nullptr;
    }
    bool List(const std::wstring& rpath, std::vector<FileEntry>& out) {
        if (!FtpSetCurrentDirectoryW(m_c, rpath.c_str())) return false;
        WIN32_FIND_DATAW fd{};
        HINTERNET h = FtpFindFirstFileW(m_c, L"*", &fd, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NEED_FILE, 0);
        if (!h) return GetLastError() == ERROR_NO_MORE_FILES;      // empty folder
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            FileEntry e;
            e.name = fd.cFileName;
            e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.hidden = e.name[0] == L'.';
            e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            e.mtime = fd.ftLastWriteTime; e.ctime = fd.ftCreationTime; e.attr = fd.dwFileAttributes;
            if (!e.dir) e.ext = ExtOf(e.name);
            out.push_back(std::move(e));
        } while (InternetFindNextFileW(h, &fd));
        InternetCloseHandle(h);
        return true;
    }
    bool IsDir(const std::wstring& rpath) { return FtpSetCurrentDirectoryW(m_c, rpath.c_str()) != FALSE; }
    bool Get(const std::wstring& r, const std::wstring& local) {
        return FtpGetFileW(m_c, r.c_str(), local.c_str(), FALSE, FILE_ATTRIBUTE_NORMAL, FTP_TRANSFER_TYPE_BINARY | INTERNET_FLAG_RELOAD, 0) != FALSE;
    }
    bool Put(const std::wstring& local, const std::wstring& r) {
        return FtpPutFileW(m_c, local.c_str(), r.c_str(), FTP_TRANSFER_TYPE_BINARY, 0) != FALSE;
    }
    bool Del(const std::wstring& r) { return FtpDeleteFileW(m_c, r.c_str()) != FALSE; }
    bool Rmd(const std::wstring& r) { return FtpRemoveDirectoryW(m_c, r.c_str()) != FALSE; }
    bool Mkd(const std::wstring& r) { return FtpCreateDirectoryW(m_c, r.c_str()) != FALSE; }
    bool Ren(const std::wstring& a, const std::wstring& b) { return FtpRenameFileW(m_c, a.c_str(), b.c_str()) != FALSE; }
private:
    HINTERNET m_i{nullptr}, m_c{nullptr};
};

inline std::wstring RJoin(const std::wstring& a, const std::wstring& b) { return (a.empty() || a.back() != L'/') ? a + L"/" + b : a + b; }
using NoteFn = std::function<void(const std::wstring&)>;

inline bool FtpGetTree(FtpClient& c, const std::wstring& r, const std::wstring& local, const NoteFn& note) {
    if (c.IsDir(r)) {
        CreateDirectoryW(local.c_str(), nullptr);
        std::vector<FileEntry> items;
        if (!c.List(r, items)) return false;
        bool ok = true;
        for (const auto& e : items) ok &= FtpGetTree(c, RJoin(r, e.name), JoinPath(local, e.name), note);
        return ok;
    }
    if (note) note(L"İndiriliyor: " + local.substr(local.find_last_of(L'\\') + 1));
    return c.Get(r, local);
}

inline bool FtpPutTree(FtpClient& c, const std::wstring& local, const std::wstring& r, const NoteFn& note) {
    const DWORD a = GetFileAttributesW(local.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        c.Mkd(r);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(JoinPath(local, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        bool ok = true;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
                ok &= FtpPutTree(c, JoinPath(local, fd.cFileName), RJoin(r, fd.cFileName), note);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return ok;
    }
    if (note) note(L"Yükleniyor: " + local.substr(local.find_last_of(L'\\') + 1));
    return c.Put(local, r);
}

inline bool FtpDelTree(FtpClient& c, const std::wstring& r, const NoteFn& note) {
    if (c.IsDir(r)) {
        std::vector<FileEntry> items;
        if (!c.List(r, items)) return false;
        bool ok = true;
        for (const auto& e : items) ok &= FtpDelTree(c, RJoin(r, e.name), note);
        return ok & c.Rmd(r);
    }
    if (note) note(L"Siliniyor: " + r.substr(r.find_last_of(L'/') + 1));
    return c.Del(r);
}

inline std::wstring LeafOf(const std::wstring& p) {
    std::wstring q = p;
    while (q.size() > 1 && (q.back() == L'/' || q.back() == L'\\')) q.pop_back();
    const size_t i = q.find_last_of(L"/\\");
    return i == std::wstring::npos ? q : q.substr(i + 1);
}

inline bool DeleteLocalToBin(const std::wstring& p) {
    std::wstring d = p; d.push_back(0); d.push_back(0);
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE; op.pFrom = d.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0;
}

// Copies/moves any mix of local and ftp:// sources into `dest` (a local folder or an ftp:// folder).
inline bool TransferItems(const std::vector<std::wstring>& srcs, const std::wstring& dest, bool move, const NoteFn& note) {
    bool all = true;
    for (const auto& src : srcs) {
        const std::wstring leaf = LeafOf(src);
        bool ok = false;
        if (IsFtp(src)) {
            FtpClient sc;
            if (sc.Open(src)) {
                std::wstring rs = ParseFtp(src).path;
                while (rs.size() > 1 && rs.back() == L'/') rs.pop_back();
                if (!IsFtp(dest)) {
                    ok = FtpGetTree(sc, rs, JoinPath(dest, leaf), note);
                } else {
                    wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
                    const std::wstring td = JoinPath(JoinPath(tmp, L"AetherXfer"), std::to_wstring(GetTickCount64()));
                    SHCreateDirectoryExW(nullptr, td.c_str(), nullptr);
                    const std::wstring tl = JoinPath(td, leaf);
                    if (FtpGetTree(sc, rs, tl, note)) {
                        FtpClient dc;
                        if (dc.Open(dest)) {
                            std::wstring rd = ParseFtp(dest).path;
                            ok = FtpPutTree(dc, tl, RJoin(rd, leaf), note);
                        }
                    }
                    std::wstring d2 = td; d2.push_back(0); d2.push_back(0);
                    SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = d2.c_str(); op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
                    SHFileOperationW(&op);
                }
                if (ok && move) FtpDelTree(sc, rs, note);
            }
        } else if (IsFtp(dest)) {
            FtpClient dc;
            if (dc.Open(dest)) {
                std::wstring rd = ParseFtp(dest).path;
                ok = FtpPutTree(dc, src, RJoin(rd, leaf), note);
                if (ok && move) DeleteLocalToBin(src);
            }
        }
        all &= ok;
    }
    return all;
}

// ---- lists for FolderResult ----------------------------------------------------------------------------------------
inline bool FtpList(const std::wstring& url, FolderResult& out) {
    FtpClient c;
    if (!c.Open(url)) return false;
    std::wstring p = ParseFtp(url).path;
    return c.List(p, out.items);
}

inline void EnumNetRec(NETRESOURCEW* parent, int depth, std::set<std::wstring>& servers, bool shares, std::vector<std::wstring>* shareOut) {
    HANDLE h = nullptr;
    if (WNetOpenEnumW(RESOURCE_GLOBALNET, shares ? RESOURCETYPE_DISK : RESOURCETYPE_ANY, 0, parent, &h) != NO_ERROR) return;
    std::vector<BYTE> buf(32768);
    for (;;) {
        DWORD cnt = static_cast<DWORD>(-1), sz = static_cast<DWORD>(buf.size());
        const DWORD rc = WNetEnumResourceW(h, &cnt, buf.data(), &sz);
        if (rc == ERROR_MORE_DATA) { buf.resize(sz + 1024); continue; }
        if (rc != NO_ERROR) break;
        auto* nr = reinterpret_cast<NETRESOURCEW*>(buf.data());
        for (DWORD i = 0; i < cnt; ++i) {
            if (!nr[i].lpRemoteName) continue;
            if (shares) { if (shareOut) shareOut->push_back(nr[i].lpRemoteName); continue; }
            if (nr[i].dwDisplayType == RESOURCEDISPLAYTYPE_SERVER) {
                std::wstring n = nr[i].lpRemoteName;
                while (!n.empty() && n[0] == L'\\') n.erase(0, 1);
                if (!n.empty()) servers.insert(n);
            } else if ((nr[i].dwUsage & RESOURCEUSAGE_CONTAINER) && depth < 3) {
                NETRESOURCEW copy = nr[i];
                EnumNetRec(&copy, depth + 1, servers, false, nullptr);
            }
        }
    }
    WNetCloseEnum(h);
}

// "net:" -> computers of the network neighbourhood (as folders named after the server)
inline bool NetComputers(FolderResult& out) {
    std::set<std::wstring> servers;
    EnumNetRec(nullptr, 0, servers, false, nullptr);
    for (const auto& s : servers) {
        FileEntry e; e.name = s; e.dir = true; e.attr = FILE_ATTRIBUTE_DIRECTORY;
        out.items.push_back(std::move(e));
    }
    return true;
}

// "\\server" -> shared folders of that server
inline bool NetShares(const std::wstring& server, FolderResult& out) {
    NETRESOURCEW nr{};
    nr.dwScope = RESOURCE_GLOBALNET; nr.dwType = RESOURCETYPE_ANY; nr.dwUsage = RESOURCEUSAGE_CONTAINER;
    std::wstring name = server;
    nr.lpRemoteName = name.data();
    std::set<std::wstring> dummy;
    std::vector<std::wstring> shares;
    EnumNetRec(&nr, 0, dummy, true, &shares);
    for (const auto& s : shares) {
        FileEntry e; e.name = s.substr(s.find_last_of(L'\\') + 1); e.dir = true; e.attr = FILE_ATTRIBUTE_DIRECTORY;
        if (!e.name.empty()) out.items.push_back(std::move(e));
    }
    return !out.items.empty();
}

// ---- saved connections ---------------------------------------------------------------------------------------------------
struct Conn {
    std::wstring name, host, user, pass, path{L"/"};
    unsigned port{21};
    std::wstring Url() const { return BuildFtp(user, host, port, path); }
};

inline std::wstring ProtectPass(const std::wstring& p) {
    if (p.empty()) return L"";
    DATA_BLOB in{static_cast<DWORD>(p.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(p.data()))}, out{};
    if (!CryptProtectData(&in, L"aether", nullptr, nullptr, nullptr, 0, &out)) return L"";
    DWORD n = 0;
    CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::wstring s(n, L'\0');
    if (n) CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &n);
    LocalFree(out.pbData);
    s.resize(wcslen(s.c_str()));
    return s;
}
inline std::wstring UnprotectPass(const std::wstring& s) {
    if (s.empty()) return L"";
    DWORD n = 0;
    if (!CryptStringToBinaryW(s.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr) || !n) return L"";
    std::vector<BYTE> b(n);
    if (!CryptStringToBinaryW(s.c_str(), 0, CRYPT_STRING_BASE64, b.data(), &n, nullptr, nullptr)) return L"";
    DATA_BLOB in{n, b.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return L"";
    std::wstring r(reinterpret_cast<wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    LocalFree(out.pbData);
    return r;
}

// ---- native folder picker ------------------------------------------------------------------------------------------------
inline std::wstring PickFolder(HWND owner, const wchar_t* title) {
    std::wstring res;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opt = 0; dlg->GetOptions(&opt); dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (title) dlg->SetTitle(title);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)) && it) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { res = p; CoTaskMemFree(p); }
                it->Release();
            }
        }
        dlg->Release();
    }
    return res;
}

// ---- tiny native input dialog (connection manager, password prompt) --------------------------------------------------
struct DlgField { std::wstring label, value; bool password{false}; };

namespace dlg_detail {
struct State {
    std::vector<DlgField>* f{nullptr};
    std::vector<HWND> edits;
    bool done{false}, ok{false};
    HBRUSH bg{nullptr}, ebg{nullptr};
};
inline LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_COMMAND:
            if (!st) break;
            if (LOWORD(w) == IDOK) {
                for (size_t i = 0; i < st->edits.size() && i < st->f->size(); ++i) {
                    wchar_t b[512] = {}; GetWindowTextW(st->edits[i], b, 512); (*st->f)[i].value = b;
                }
                st->ok = true; st->done = true;
            } else if (LOWORD(w) == IDCANCEL) st->done = true;
            return 0;
        case WM_CLOSE: if (st) st->done = true; return 0;
        case WM_ERASEBKGND: {
            RECT rc; GetClientRect(h, &rc);
            if (st) FillRect(reinterpret_cast<HDC>(w), &rc, st->bg);
            return 1;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
            SetTextColor(reinterpret_cast<HDC>(w), RGB(214, 200, 244)); SetBkColor(reinterpret_cast<HDC>(w), RGB(22, 14, 36));
            return st ? reinterpret_cast<LRESULT>(st->bg) : 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), RGB(244, 238, 255)); SetBkColor(reinterpret_cast<HDC>(w), RGB(42, 30, 66));
            return st ? reinterpret_cast<LRESULT>(st->ebg) : 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace dlg_detail

// Returns true when the user pressed the OK button; the fields hold the edited values.
inline bool InputDialog(HWND owner, const std::wstring& title, std::vector<DlgField>& fields, const std::wstring& okText) {
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = dlg_detail::Proc; wc.hInstance = inst; wc.lpszClassName = L"AetherExplorerInputDlg";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        reg = true;
    }
    const UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    const float s = dpi / 96.0f;
    auto S = [&](int v) { return static_cast<int>(v * s + 0.5f); };
    dlg_detail::State st;
    st.f = &fields;
    st.bg = CreateSolidBrush(RGB(22, 14, 36));
    st.ebg = CreateSolidBrush(RGB(42, 30, 66));
    const int cw = S(420), rowH = S(56), btnH = S(54);
    const int ch = S(14) + static_cast<int>(fields.size()) * rowH + btnH;
    RECT wr = {0, 0, cw, ch};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&wr, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT orc = {0, 0, 1280, 800};
    if (owner) GetWindowRect(owner, &orc);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    const int x = orc.left + ((orc.right - orc.left) - ww) / 2, y = orc.top + ((orc.bottom - orc.top) - wh) / 2;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AetherExplorerInputDlg", title.c_str(), style, x, y, ww, wh, owner, nullptr, inst, &st);
    if (!h) { DeleteObject(st.bg); DeleteObject(st.ebg); return false; }
    BOOL dark = TRUE;
    {
        using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (HMODULE dm = LoadLibraryW(L"dwmapi.dll"))
            if (auto fn = reinterpret_cast<Fn>(GetProcAddress(dm, "DwmSetWindowAttribute"))) fn(h, 20, &dark, sizeof(dark));
    }
    HFONT font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int yy = S(10);
    for (size_t i = 0; i < fields.size(); ++i) {
        HWND lb = CreateWindowExW(0, L"STATIC", fields[i].label.c_str(), WS_CHILD | WS_VISIBLE, S(18), yy, cw - S(36), S(18), h, nullptr, inst, nullptr);
        HWND ed = CreateWindowExW(0, L"EDIT", fields[i].value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | (fields[i].password ? ES_PASSWORD : 0),
                                  S(18), yy + S(20), cw - S(36), S(26), h, nullptr, inst, nullptr);
        SendMessageW(lb, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(ed, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        st.edits.push_back(ed);
        yy += rowH;
    }
    HWND ok = CreateWindowExW(0, L"BUTTON", okText.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, cw - S(18) - S(190), yy + S(6), S(110), S(30), h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), inst, nullptr);
    HWND cn = CreateWindowExW(0, L"BUTTON", L"İptal", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, cw - S(18) - S(70), yy + S(6), S(70), S(30), h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), inst, nullptr);
    SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(cn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    if (!st.edits.empty()) SetFocus(st.edits[0]);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    DestroyWindow(h);
    DeleteObject(font); DeleteObject(st.bg); DeleteObject(st.ebg);
    return st.ok;
}

} // namespace aether::explorer
