// AetherSetup: the Aether installer / uninstaller, drawn with the same Direct2D / DirectComposition stack as the rest of Aether.
//
//   AetherSetup.exe                 install (welcome -> options -> install -> done)
//   AetherSetup.exe /S              silent install with the default options
//   Uninstall.exe --uninstall       (the same exe, copied into the install folder) uninstall
//
// Per-user install (no administrator rights): %LOCALAPPDATA%\Programs\Aether by default.
// The program files travel inside this exe as an RCDATA zip (resource 300, built by the AetherPayload CMake target) and are unpacked with
// the tar.exe that ships with Windows 10+. Without the resource (developer builds) the files next to the installer are copied instead.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/mini_gfx.hpp"

using namespace aether;
using aether::mg::Color;
using aether::mg::Rect;
using aether::mg::WithA;
using Microsoft::WRL::ComPtr;

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace {

constexpr wchar_t kVersion[] = L"1.0.0";
constexpr wchar_t kUninstKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Aether";
constexpr float kW = 900.0f, kH = 580.0f, kLeft = 280.0f;

bool Turkish() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH; }
std::wstring T(const wchar_t* tr, const wchar_t* en) { return Turkish() ? tr : en; }

float Clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
float Smooth(float x) { x = Clamp01(x); return x * x * (3.0f - 2.0f * x); }

const wchar_t* const kExes[] = {L"Aether.exe", L"AetherBar.exe", L"AetherDesktop.exe", L"AetherExplorer.exe", L"AetherShell.exe", L"AetherBin.exe", L"AetherIntro.exe"};

// ---- small system helpers ------------------------------------------------------------------------------------------------------
std::wstring Env(const wchar_t* n) { wchar_t b[MAX_PATH * 2]; const DWORD k = GetEnvironmentVariableW(n, b, MAX_PATH * 2); return (k && k < MAX_PATH * 2) ? std::wstring(b) : L""; }
std::wstring SelfPath() { wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH); return p; }
std::wstring DirOf(const std::wstring& p) { const size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? L"." : p.substr(0, k); }
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}
void MakeDirs(const std::wstring& p) { SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }
std::wstring Esc(const std::wstring& s) { std::wstring o; for (wchar_t c : s) { if (c == L'\\' || c == L'"') o += L'\\'; o += c; } return o; }

bool RegSetStr(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& v) {
    HKEY k; if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k); return r == ERROR_SUCCESS;
}
bool RegSetDword(HKEY root, const std::wstring& sub, const wchar_t* name, DWORD v) {
    HKEY k; if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k); return r == ERROR_SUCCESS;
}
bool RegGetStr(HKEY root, const std::wstring& sub, const wchar_t* name, std::wstring& out) {
    HKEY k; if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    wchar_t b[2048]; DWORD n = sizeof(b), t = 0;
    const LONG r = RegQueryValueExW(k, name, nullptr, &t, reinterpret_cast<BYTE*>(b), &n);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (t != REG_SZ && t != REG_EXPAND_SZ)) return false;
    out.assign(b, n / sizeof(wchar_t)); while (!out.empty() && out.back() == 0) out.pop_back();
    return true;
}

// Ends every running Aether process (the shell first, so it cannot restart the others). Returns true if the shell session was running.
bool KillAether() {
    bool shellWas = false;
    const DWORD me = GetCurrentProcessId();
    for (int pass = 0; pass < 2; ++pass) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return shellWas;
        PROCESSENTRY32W pe = {sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ProcessID == me) continue;
            const bool isShell = _wcsicmp(pe.szExeFile, L"AetherShell.exe") == 0;
            if ((pass == 0) != isShell) continue;
            for (const wchar_t* n : kExes) {
                if (_wcsicmp(pe.szExeFile, n) != 0) continue;
                if (isShell) shellWas = true;
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) { TerminateProcess(h, 0); WaitForSingleObject(h, 1500); CloseHandle(h); }
                break;
            }
        }
        CloseHandle(snap);
    }
    return shellWas;
}

bool MakeLink(const std::wstring& lnk, const std::wstring& target, const std::wstring& desc) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str()); sl->SetWorkingDirectory(DirOf(target).c_str()); sl->SetDescription(desc.c_str()); sl->SetIconLocation(target.c_str(), 0);
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf))) return false;
    return SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
}

void DeleteTree(const std::wstring& dir) {
    std::wstring from = dir; from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NO_UI | FOF_SILENT | FOF_NOCONFIRMATION;
    SHFileOperationW(&op);
}
void CopyTree(const std::wstring& from, const std::wstring& to) {
    MakeDirs(to);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyTree(from + L"\\" + fd.cFileName, to + L"\\" + fd.cFileName);
        else CopyFileW((from + L"\\" + fd.cFileName).c_str(), (to + L"\\" + fd.cFileName).c_str(), FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
uint64_t TreeSize(const std::wstring& dir) {
    uint64_t sz = 0; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sz += TreeSize(dir + L"\\" + fd.cFileName);
        else sz += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return sz;
}

// ---- options / install model ------------------------------------------------------------------------------------------------------
struct Opts {
    std::wstring dir;
    bool bar{true}, desk{true}, expl{true};
    bool deskLnk{true}, startLnk{true}, autostart{true}, replaceShell{false}, folders{false}, intro{true};
    bool launch{true};
};

struct Progress {
    std::atomic<float> value{0.0f};
    std::atomic<int> step{0};
    std::atomic<bool> done{false}, failed{false};
    std::atomic<bool> wasShellSession{false};
    std::mutex mu; std::wstring error;
    void SetError(const std::wstring& e) { { std::lock_guard<std::mutex> lk(mu); error = e; } failed = true; }
    std::wstring Error() { std::lock_guard<std::mutex> lk(mu); return error; }
};

std::wstring ShellBackupPath() { const std::wstring a = Env(L"APPDATA"); return a.empty() ? L"" : a + L"\\Aether\\shell-backup.json"; }
constexpr wchar_t kWinlogon[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
const wchar_t* const kFolderClasses[] = {L"Directory", L"Folder", L"Drive"};

bool Extract(const std::wstring& dir, Progress& pr) {
    MakeDirs(dir);
    HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(300), RT_RCDATA);
    if (!rs) {                                                       // developer build: copy what sits next to the installer
        const std::wstring src = DirOf(SelfPath());
        bool any = false;
        for (const wchar_t* n : kExes) if (Exists(src + L"\\" + n)) { CopyFileW((src + L"\\" + n).c_str(), (dir + L"\\" + n).c_str(), FALSE); any = true; }
        if (Exists(src + L"\\wallpapers")) CopyTree(src + L"\\wallpapers", dir + L"\\wallpapers");
        if (!any) { pr.SetError(T(L"Kurulum dosyaları bulunamadı (payload).", L"The installation files (payload) were not found.")); return false; }
        return true;
    }
    HGLOBAL g = LoadResource(nullptr, rs); const DWORD sz = SizeofResource(nullptr, rs); const void* data = g ? LockResource(g) : nullptr;
    if (!data || !sz) { pr.SetError(T(L"Paket okunamadı.", L"Could not read the package.")); return false; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring zip = std::wstring(tmp) + L"aether-payload.zip";
    HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) { pr.SetError(T(L"Geçici dosya yazılamadı.", L"Could not write a temporary file.")); return false; }
    DWORD wr = 0; WriteFile(f, data, sz, &wr, nullptr); CloseHandle(f);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring cl = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(tar.c_str(), cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteFileW(zip.c_str()); pr.SetError(T(L"tar.exe başlatılamadı (Windows 10 veya üzeri gerekir).", L"Could not start tar.exe (Windows 10 or newer is required).")); return false;
    }
    const ULONGLONG t0 = GetTickCount64();
    while (WaitForSingleObject(pi.hProcess, 40) == WAIT_TIMEOUT) {   // no byte-level progress from tar: ease towards 70% while it works
        const float k = 1.0f - std::exp(-static_cast<float>(GetTickCount64() - t0) / 3500.0f);
        pr.value = 0.12f + 0.58f * k;
    }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); DeleteFileW(zip.c_str());
    if (code != 0) { pr.SetError(T(L"Dosyalar çıkarılamadı. Aether çalışıyorsa kapatıp tekrar dene.", L"Could not unpack the files. If Aether is running, close it and try again.")); return false; }
    return true;
}

void DoInstall(Opts o, Progress* pr) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pr->step = 0; pr->value = 0.02f;
    pr->wasShellSession = KillAether();
    pr->step = 1; pr->value = 0.10f;
    if (!Extract(o.dir, *pr)) { CoUninitialize(); return; }
    pr->step = 2; pr->value = 0.74f;
    if (!o.bar) DeleteFileW((o.dir + L"\\AetherBar.exe").c_str());
    if (!o.desk) DeleteFileW((o.dir + L"\\AetherDesktop.exe").c_str());
    if (!o.expl) { DeleteFileW((o.dir + L"\\AetherExplorer.exe").c_str()); DeleteFileW((o.dir + L"\\AetherBin.exe").c_str()); }
    const std::wstring self = o.dir + L"\\Uninstall.exe";
    CopyFileW(SelfPath().c_str(), self.c_str(), FALSE);
    const std::wstring shellExe = o.dir + L"\\AetherShell.exe";
    const std::wstring appdata = Env(L"APPDATA");
    MakeDirs(appdata + L"\\Aether");
    if (o.intro) DeleteFileW((appdata + L"\\Aether\\no-intro").c_str());
    else { HANDLE h = CreateFileW((appdata + L"\\Aether\\no-intro").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    DeleteFileW((appdata + L"\\Aether\\use-explorer").c_str());

    pr->step = 3; pr->value = 0.82f;
    if (o.startLnk) {
        const std::wstring sm = Known(FOLDERID_Programs) + L"\\Aether";
        MakeDirs(sm);
        MakeLink(sm + L"\\Aether.lnk", shellExe, L"Aether");
        MakeLink(sm + L"\\" + T(L"Aether'i Kaldır", L"Uninstall Aether") + L".lnk", self, L"Aether Uninstall");
    }
    if (o.deskLnk) MakeLink(Known(FOLDERID_Desktop) + L"\\Aether.lnk", shellExe, L"Aether");

    pr->step = 4; pr->value = 0.92f;
    RegSetStr(HKEY_CURRENT_USER, kUninstKey, L"DisplayName", L"Aether");
    RegSetStr(HKEY_CURRENT_USER, kUninstKey, L"DisplayVersion", kVersion);
    RegSetStr(HKEY_CURRENT_USER, kUninstKey, L"Publisher", L"Aether");
    RegSetStr(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", o.dir);
    RegSetStr(HKEY_CURRENT_USER, kUninstKey, L"DisplayIcon", shellExe);
    RegSetStr(HKEY_CURRENT_USER, kUninstKey, L"UninstallString", L"\"" + self + L"\" --uninstall");
    RegSetDword(HKEY_CURRENT_USER, kUninstKey, L"NoModify", 1);
    RegSetDword(HKEY_CURRENT_USER, kUninstKey, L"NoRepair", 1);
    RegSetDword(HKEY_CURRENT_USER, kUninstKey, L"EstimatedSize", static_cast<DWORD>(TreeSize(o.dir) / 1024));
    if (o.replaceShell) {
        const std::wstring bk = ShellBackupPath();
        if (!bk.empty() && !Exists(bk)) {                            // remember what the shell was, once (same format as tools\install-shell.ps1)
            std::wstring prev; const bool has = RegGetStr(HKEY_CURRENT_USER, kWinlogon, L"Shell", prev) && prev.find(L"AetherShell") == std::wstring::npos;
            std::string js = "{\n  \"previousShell\": ";
            auto u8 = [](const std::wstring& w) { std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr); if (!s.empty()) s.pop_back(); return s; };
            js += has ? "\"" + u8(Esc(prev)) + "\"" : "null";
            js += ",\n  \"binDir\": \"" + u8(Esc(o.dir)) + "\"\n}\n";
            HANDLE h = CreateFileW(bk.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, js.data(), static_cast<DWORD>(js.size()), &w, nullptr); CloseHandle(h); }
        }
        RegSetStr(HKEY_CURRENT_USER, kWinlogon, L"Shell", L"\"" + shellExe + L"\"");
    } else if (o.autostart) {
        RegSetStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Aether", L"\"" + shellExe + L"\"");
    }
    if ((o.replaceShell || o.folders) && o.expl) {
        const std::wstring cmd = L"\"" + o.dir + L"\\AetherExplorer.exe\" \"%1\"";
        for (const wchar_t* c : kFolderClasses) {
            const std::wstring key = std::wstring(L"Software\\Classes\\") + c + L"\\shell\\open\\command";
            RegSetStr(HKEY_CURRENT_USER, key, nullptr, cmd);
            RegSetStr(HKEY_CURRENT_USER, key, L"DelegateExecute", L"");
        }
    }
    pr->step = 5; pr->value = 1.0f; Sleep(450);
    pr->done = true;
    CoUninitialize();
}

void DoUninstall(const std::wstring& dir, Progress* pr) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pr->step = 0; pr->value = 0.05f;
    pr->wasShellSession = KillAether();
    Sleep(300);
    pr->step = 1; pr->value = 0.30f;
    // Windows shell back to what it was
    std::wstring cur;
    if (RegGetStr(HKEY_CURRENT_USER, kWinlogon, L"Shell", cur) && cur.find(L"AetherShell") != std::wstring::npos) {
        std::wstring prev; const std::wstring bk = ShellBackupPath();
        HANDLE h = bk.empty() ? INVALID_HANDLE_VALUE : CreateFileW(bk.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[4096] = {}; DWORD n = 0; ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr); CloseHandle(h);
            std::string s(buf, n); const size_t k = s.find("\"previousShell\"");
            if (k != std::string::npos) {
                size_t q = s.find(':', k); q = s.find_first_not_of(" \t\r\n", q + 1);
                if (q != std::string::npos && s[q] == '"') {
                    std::string v; for (size_t i = q + 1; i < s.size() && s[i] != '"'; ++i) { if (s[i] == '\\' && i + 1 < s.size()) ++i; v += s[i]; }
                    const int wl = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, nullptr, 0);
                    if (wl > 1) { prev.assign(static_cast<size_t>(wl), L'\0'); MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, prev.data(), wl); prev.pop_back(); }
                }
            }
        }
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinlogon, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            if (!prev.empty()) RegSetValueExW(k, L"Shell", 0, REG_SZ, reinterpret_cast<const BYTE*>(prev.c_str()), static_cast<DWORD>((prev.size() + 1) * sizeof(wchar_t)));
            else RegDeleteValueW(k, L"Shell");
            RegCloseKey(k);
        }
    }
    for (const wchar_t* c : kFolderClasses) {
        const std::wstring key = std::wstring(L"Software\\Classes\\") + c + L"\\shell\\open";
        std::wstring v;
        if (RegGetStr(HKEY_CURRENT_USER, key + L"\\command", nullptr, v) && v.find(L"AetherExplorer") != std::wstring::npos) SHDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
    }
    const std::wstring bk = ShellBackupPath(); if (!bk.empty()) DeleteFileW(bk.c_str());
    DeleteFileW((Env(L"APPDATA") + L"\\Aether\\use-explorer").c_str());
    pr->step = 2; pr->value = 0.55f;
    { HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"Aether"); RegCloseKey(k); } }
    SHDeleteKeyW(HKEY_CURRENT_USER, kUninstKey);
    DeleteTree(Known(FOLDERID_Programs) + L"\\Aether");
    DeleteFileW((Known(FOLDERID_Desktop) + L"\\Aether.lnk").c_str());
    pr->step = 3; pr->value = 0.75f;
    DeleteTree(dir);
    pr->step = 4; pr->value = 1.0f; Sleep(450);
    pr->done = true;
    CoUninitialize();
}

// =============================================================================================================================
//   the window
// =============================================================================================================================
enum Page { P_WELCOME = 0, P_OPTIONS = 1, P_INSTALL = 2, P_DONE = 3 };

class Setup {
public:
    bool uninstall{false}, silent{false};
    std::wstring uninstDir;

    bool Create(HINSTANCE inst) {
        m_inst = inst;
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW; wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"AetherSetupWindow";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hCursor = nullptr;
        RegisterClassExW(&wc);
        m_scale = 1.0f;
        POINT pt = {0, 0}; GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
        m_scale = dx / 96.0f;
        MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(mon, &mi);
        const int w = static_cast<int>(kW * m_scale), h = static_cast<int>(kH * m_scale);
        const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2, y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
        m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, wc.lpszClassName, uninstall ? L"Aether - Uninstall" : L"Aether Setup", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                                 x, y, w, h, nullptr, nullptr, inst, this);
        if (!m_hwnd) return false;
        const DWORD pref = 2;                                         // DWMWCP_ROUND
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
        RECT rc; GetClientRect(m_hwnd, &rc);
        m_dc = std::make_unique<render::DCompLayer>();
        if (!m_dc->Initialize(m_hwnd, rc.right, rc.bottom)) return false;
        m_g.Init(m_dc.get());
        QueryPerformanceFrequency(&m_freq); QueryPerformanceCounter(&m_last); m_t0 = m_last;
        m_o.dir = Known(FOLDERID_LocalAppData) + L"\\Programs\\Aether";
        if (uninstall) { m_page = P_WELCOME; }
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        SetTimer(m_hwnd, 1, 15, nullptr);
        return true;
    }
    void Run() { MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    Opts& Options() { return m_o; }
    void StartInstallNow() { Begin(); }

private:
    HINSTANCE m_inst{nullptr};
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    float m_scale{1.0f};
    LARGE_INTEGER m_freq{}, m_last{}, m_t0{};
    int m_page{P_WELCOME};
    float m_pageT{1.0f};
    Opts m_o;
    Progress m_pr;
    std::thread m_worker;
    float m_prog{0.0f};
    bool m_started{false};
    struct Hit { Rect r; int id; };
    std::vector<Hit> m_hits;
    std::map<int, float> m_hv;
    int m_hover{0}, m_down{0};
    float m_mx{-1}, m_my{-1};

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Setup* self = nullptr;
        if (m == WM_NCCREATE) { self = static_cast<Setup*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        else self = reinterpret_cast<Setup*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    int HitAt(float x, float y) const { for (int i = static_cast<int>(m_hits.size()) - 1; i >= 0; --i) if (m_hits[i].r.Contains(x, y)) return m_hits[i].id; return 0; }

    LRESULT Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_NCHITTEST: {
            POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(h, &p);
            const float x = p.x / m_scale, y = p.y / m_scale;
            if (y < 46.0f && x < kW - 110.0f) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, m_hover ? IDC_HAND : IDC_ARROW)); return TRUE; }
            break;
        case WM_MOUSEMOVE: {
            m_mx = GET_X_LPARAM(l) / m_scale; m_my = GET_Y_LPARAM(l) / m_scale;
            m_hover = HitAt(m_mx, m_my);
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE: m_hover = 0; m_mx = m_my = -1; return 0;
        case WM_LBUTTONDOWN: m_down = HitAt(GET_X_LPARAM(l) / m_scale, GET_Y_LPARAM(l) / m_scale); SetCapture(h); return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int id = HitAt(GET_X_LPARAM(l) / m_scale, GET_Y_LPARAM(l) / m_scale);
            if (id && id == m_down) Click(id);
            m_down = 0;
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE && m_page != P_INSTALL) Click(1);
            return 0;
        case WM_TIMER: Frame(); return 0;
        case WM_CLOSE: Click(1); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void Go(int page) { m_page = page; m_pageT = 0.0f; m_hits.clear(); }

    void Begin() {
        if (m_started) return;
        m_started = true;
        Go(P_INSTALL);
        if (m_worker.joinable()) m_worker.join();
        if (uninstall) { const std::wstring d = uninstDir; m_worker = std::thread([this, d] { DoUninstall(d, &m_pr); }); }
        else { Opts o = m_o; m_worker = std::thread([this, o] { DoInstall(o, &m_pr); }); }
    }

    void Finish() {
        const bool launched = m_page == P_DONE && !m_pr.failed;
        if (launched && !uninstall) {
            const std::wstring shell = m_o.dir + L"\\AetherShell.exe";
            const bool relaunch = m_pr.wasShellSession || (m_o.launch && !m_o.replaceShell);
            if (relaunch && Exists(shell)) {
                if (m_o.intro && m_o.launch && Exists(m_o.dir + L"\\AetherIntro.exe")) ShellExecuteW(nullptr, L"open", (m_o.dir + L"\\AetherIntro.exe").c_str(), L"--first", m_o.dir.c_str(), SW_SHOWNORMAL);
                ShellExecuteW(nullptr, L"open", shell.c_str(), nullptr, m_o.dir.c_str(), SW_SHOWNORMAL);
            }
        } else if (launched && uninstall && GetShellWindow() == nullptr) {       // we were running as the shell: give the user Windows' own back
            wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
            ShellExecuteW(nullptr, L"open", (std::wstring(win) + L"\\explorer.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        DestroyWindow(m_hwnd);
    }

    void PickDir() {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        DWORD fl = 0; dlg->GetOptions(&fl); dlg->SetOptions(fl | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(T(L"Kurulum klasörünü seç", L"Choose the install folder").c_str());
        if (FAILED(dlg->Show(m_hwnd))) return;
        ComPtr<IShellItem> it; PWSTR p = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            std::wstring d = p; CoTaskMemFree(p);
            while (d.size() > 3 && d.back() == L'\\') d.pop_back();
            const size_t k = d.find_last_of(L'\\');
            if (_wcsicmp(k == std::wstring::npos ? d.c_str() : d.c_str() + k + 1, L"Aether") != 0) d += L"\\Aether";
            m_o.dir = d;
        }
    }

    void Click(int id) {
        switch (id) {
        case 1:                                                   // close
            if (m_page == P_INSTALL && !m_pr.done && !m_pr.failed) return;
            if (m_page == P_DONE) { Finish(); return; }
            DestroyWindow(m_hwnd); return;
        case 2: ShowWindow(m_hwnd, SW_MINIMIZE); return;
        case 10:                                                  // primary
            if (m_page == P_WELCOME) { if (uninstall) Begin(); else Go(P_OPTIONS); }
            else if (m_page == P_OPTIONS) Begin();
            else if (m_page == P_DONE) Finish();
            else if (m_page == P_INSTALL && m_pr.failed) DestroyWindow(m_hwnd);
            return;
        case 11: if (m_page == P_OPTIONS) Go(P_WELCOME); return;
        case 12: PickDir(); return;
        case 20: m_o.bar = !m_o.bar; return;
        case 21: m_o.desk = !m_o.desk; return;
        case 22: m_o.expl = !m_o.expl; return;
        case 30: m_o.deskLnk = !m_o.deskLnk; return;
        case 31: m_o.startLnk = !m_o.startLnk; return;
        case 32: if (!m_o.replaceShell) m_o.autostart = !m_o.autostart; return;
        case 33:
            m_o.replaceShell = !m_o.replaceShell;
            if (m_o.replaceShell) { m_o.bar = m_o.desk = m_o.expl = true; m_o.folders = true; }
            return;
        case 34: if (!m_o.replaceShell) m_o.folders = !m_o.folders; return;
        case 35: m_o.intro = !m_o.intro; return;
        case 40: m_o.launch = !m_o.launch; return;
        case 41: ExitWindowsEx(EWX_LOGOFF, 0); return;
        }
    }

    // ---- drawing --------------------------------------------------------------------------------------------------------------
    void Radial(float cx, float cy, float r, const Color& c0, const Color& c1) {
        if (c0.a <= 0.002f) return;
        auto* ctx = m_g.ctx;
        D2D1_GRADIENT_STOP st[2] = {{0.0f, D2D1::ColorF(c0.r, c0.g, c0.b, c0.a)}, {1.0f, D2D1::ColorF(c1.r, c1.g, c1.b, c1.a)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        if (FAILED(ctx->CreateGradientStopCollection(st, 2, &sc))) return;
        ComPtr<ID2D1RadialGradientBrush> br;
        if (FAILED(ctx->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r, r), sc.Get(), &br))) return;
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br.Get());
    }
    float Hv(int id) const { auto it = m_hv.find(id); return it == m_hv.end() ? 0.0f : it->second; }
    void Reg(const Rect& r, int id) { m_hits.push_back({r, id}); }

    const Color violet{0.55f, 0.34f, 1.0f, 1.0f}, vlight{0.76f, 0.66f, 1.0f, 1.0f}, ink{0.95f, 0.93f, 1.0f, 1.0f}, mute{0.70f, 0.66f, 0.84f, 1.0f};

    void Button(const Rect& r, const std::wstring& label, bool primary, int id, bool enabled = true) {
        const float h = enabled ? Hv(id) : 0.0f;
        if (primary) {
            if (enabled) m_g.Glow(r, 12, violet, 12.0f, 0.5f + 0.6f * h);
            m_g.VGradientRR(r, 12, enabled ? Color{0.66f + 0.06f * h, 0.46f + 0.06f * h, 1.0f, 1.0f} : Color{0.3f, 0.26f, 0.4f, 1.0f}, enabled ? Color{0.45f + 0.05f * h, 0.26f + 0.05f * h, 0.92f, 1.0f} : Color{0.24f, 0.2f, 0.34f, 1.0f});
            m_g.StrokeRR(r, 12, Color{1, 1, 1, 0.22f}, 1.0f);
        } else {
            m_g.FillRR(r, 12, Color{1, 1, 1, 0.06f + 0.07f * h});
            m_g.StrokeRR(r, 12, Color{1, 1, 1, 0.14f}, 1.0f);
        }
        m_g.Text(label, r, 14.0f, 600, Color{1, 1, 1, enabled ? 1.0f : 0.5f}, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (enabled) Reg(r, id);
    }

    // a selectable row: checkbox (kind 0) or switch (kind 1)
    void Row(const Rect& r, const std::wstring& title, const std::wstring& desc, bool on, int id, bool locked, int kind) {
        const float h = locked ? 0.0f : Hv(id);
        m_g.FillRR(r, 12, Color{1, 1, 1, 0.04f + 0.05f * h});
        m_g.StrokeRR(r, 12, on ? WithA(violet, 0.55f) : Color{1, 1, 1, 0.08f}, 1.0f);
        const float cy = r.y + r.height * 0.5f;
        if (kind == 0) {
            const Rect b{r.x + 14, cy - 10, 20, 20};
            m_g.FillRR(b, 6, on ? WithA(violet, locked ? 0.55f : 1.0f) : Color{1, 1, 1, 0.10f});
            m_g.StrokeRR(b, 6, on ? WithA(vlight, 0.8f) : Color{1, 1, 1, 0.25f}, 1.2f);
            if (on) m_g.Glyph(0xE73E, b, 12.0f, mg::kWhite);
        } else {
            const Rect b{r.x + 12, cy - 11, 40, 22};
            m_g.FillRR(b, 11, on ? WithA(violet, locked ? 0.5f : 0.95f) : Color{1, 1, 1, 0.16f});
            m_g.FillRR(Rect{b.x + (on ? 20.0f : 2.0f), b.y + 2, 18, 18}, 9, mg::kWhite);
        }
        const float tx = r.x + (kind == 0 ? 46.0f : 64.0f);
        if (desc.empty()) m_g.Text(title, Rect{tx, r.y, r.width - (tx - r.x) - 8, r.height}, 13.0f, 500, WithA(ink, locked ? 0.7f : 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING);
        else {
            m_g.Text(title, Rect{tx, r.y + 6, r.width - (tx - r.x) - 8, 20}, 13.0f, 500, WithA(ink, locked ? 0.7f : 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING);
            m_g.Text(desc, Rect{tx, r.y + 25, r.width - (tx - r.x) - 8, 16}, 10.5f, 400, WithA(mute, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        if (!locked) Reg(r, id);
    }

    void DrawLeft(float t) {
        // aurora panel
        m_g.VGradientRR(Rect{0, 0, kLeft, kH}, 0, Color{0.10f, 0.05f, 0.24f, 1.0f}, Color{0.035f, 0.02f, 0.10f, 1.0f});
        Radial(kLeft * (0.35f + 0.10f * std::sin(t * 0.6f)), 150.0f + 18.0f * std::cos(t * 0.8f), 220.0f, WithA(violet, 0.55f), WithA(violet, 0.0f));
        Radial(kLeft * (0.8f + 0.06f * std::cos(t * 0.7f)), kH - 120.0f + 14.0f * std::sin(t * 0.5f), 230.0f, Color{0.9f, 0.4f, 0.9f, 0.28f}, Color{0.9f, 0.4f, 0.9f, 0.0f});
        // orb logo
        const float cx = 78.0f, cy = 104.0f, r = 26.0f * (1.0f + 0.03f * std::sin(t * 2.2f));
        Radial(cx, cy, r * 2.8f, WithA(violet, 0.6f), WithA(violet, 0.0f));
        Radial(cx, cy, r, Color{1.0f, 0.97f, 1.0f, 1.0f}, Color{0.62f, 0.42f, 1.0f, 1.0f});
        m_g.StrokeRR(Rect{cx - r, cy - r, r * 2, r * 2}, r, Color{1, 1, 1, 0.5f}, 1.2f);
        m_g.Text(L"Aether", Rect{40, 150, 220, 44}, 30.0f, 300, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
        m_g.Text(T(L"Sürüm ", L"Version ") + kVersion, Rect{40, 192, 220, 20}, 12.0f, 400, WithA(mute, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING);
        // steps
        const wchar_t* names[4];
        if (uninstall) { names[0] = Turkish() ? L"Onay" : L"Confirm"; names[1] = Turkish() ? L"-" : L"-"; names[2] = Turkish() ? L"Kaldırma" : L"Removing"; names[3] = Turkish() ? L"Bitti" : L"Done"; }
        else { names[0] = Turkish() ? L"Hoş geldin" : L"Welcome"; names[1] = Turkish() ? L"Seçenekler" : L"Options"; names[2] = Turkish() ? L"Kurulum" : L"Installing"; names[3] = Turkish() ? L"Bitti" : L"Done"; }
        float y = 270.0f;
        for (int i = 0; i < 4; ++i) {
            if (uninstall && i == 1) continue;
            const bool cur = i == m_page, past = i < m_page;
            const Rect dot{44, y + 4, 16, 16};
            m_g.FillRR(dot, 8, cur ? WithA(violet, 1.0f) : (past ? WithA(violet, 0.55f) : Color{1, 1, 1, 0.10f}));
            if (cur) m_g.Glow(dot, 8, violet, 9.0f, 1.0f);
            if (past) m_g.Glyph(0xE73E, dot, 9.0f, mg::kWhite);
            m_g.Text(names[i], Rect{74, y, 180, 24}, 14.0f, cur ? 600 : 400, cur ? ink : WithA(mute, 0.8f), DWRITE_TEXT_ALIGNMENT_LEADING);
            if (i < 3) m_g.FillRR(Rect{51.5f, y + 22, 1.5f, 24}, 0.75f, Color{1, 1, 1, 0.10f});
            y += 46.0f;
        }
        m_g.Text(T(L"Yönetici izni gerekmez", L"No administrator rights needed"), Rect{40, kH - 46, 220, 20}, 11.0f, 400, WithA(mute, 0.6f), DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    void DrawChrome() {
        const Rect cl{kW - 52, 10, 38, 30}, mn{kW - 94, 10, 38, 30};
        const float hc = Hv(1), hm = Hv(2);
        m_g.FillRR(mn, 8, Color{1, 1, 1, 0.09f * hm}); m_g.Glyph(0xE921, mn, 11.0f, WithA(ink, 0.85f)); Reg(mn, 2);
        const bool lockClose = m_page == P_INSTALL && !m_pr.done && !m_pr.failed;
        m_g.FillRR(cl, 8, Color{0.92f, 0.25f, 0.35f, 0.85f * hc * (lockClose ? 0.0f : 1.0f)}); m_g.Glyph(0xE8BB, cl, 10.0f, WithA(ink, lockClose ? 0.3f : 0.85f)); if (!lockClose) Reg(cl, 1);
    }

    void DrawWelcome() {
        const float x = kLeft + 44, w = kW - kLeft - 88;
        if (uninstall) {
            m_g.Text(T(L"Aether'i kaldır", L"Uninstall Aether"), Rect{x, 84, w, 56}, 34.0f, 300, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
            m_g.TextBox(T(L"Aether bu bilgisayardan kaldırılacak. Windows kabuğu eski haline döner, klasörler yeniden Windows Gezgini ile açılır. Ayarların ve duvar kağıtların silinmez.",
                          L"Aether will be removed from this computer. The Windows shell is restored and folders open in Windows Explorer again. Your settings and wallpapers are kept."),
                        Rect{x, 150, w, 90}, 14.5f, 400, WithA(ink, 0.82f), DWRITE_TEXT_ALIGNMENT_LEADING);
            m_g.Text(m_o.dir.empty() ? L"" : uninstDir, Rect{x, 250, w, 24}, 12.0f, 400, WithA(mute, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING);
            Button(Rect{x, kH - 90, 200, 46}, T(L"Kaldır", L"Uninstall"), true, 10);
            Button(Rect{x + 214, kH - 90, 130, 46}, T(L"Vazgeç", L"Cancel"), false, 1);
            return;
        }
        m_g.Text(T(L"Aether'e hoş geldin", L"Welcome to Aether"), Rect{x, 76, w, 60}, 36.0f, 300, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
        m_g.TextBox(T(L"Windows masaüstünü baştan tasarlayan, tamamen kendi kendine yeten bir kabuk: cam görünümlü görev çubuğu, Dynamic Island, masaüstü, dosya yöneticisi ve daha fazlası.",
                      L"A self-contained shell that redesigns the Windows desktop: a glass taskbar, Dynamic Island, desktop, file manager and more."),
                    Rect{x, 142, w, 70}, 14.5f, 400, WithA(ink, 0.82f), DWRITE_TEXT_ALIGNMENT_LEADING);
        struct F { wchar_t g; const wchar_t* tt; const wchar_t* te; const wchar_t* dt; const wchar_t* de; };
        const F fs[4] = {
            {0xE7F4, L"Dynamic Island", L"Dynamic Island", L"Bildirim, müzik ve sistem bilgisi tek yerde", L"Notifications, music and system status in one place"},
            {0xE90E, L"Cam görev çubuğu", L"Glass taskbar", L"Başlat menüsü, tepsi ve hızlı ayarlar", L"Start menu, tray and quick settings"},
            {0xE8B9, L"Masaüstü ve widget'lar", L"Desktop and widgets", L"Duvar kağıdı, simgeler, canlı kartlar", L"Wallpaper, icons and live cards"},
            {0xE8B7, L"Aether Gezgini", L"Aether Explorer", L"Dosya yöneticisi ve Geri Dönüşüm Kutusu", L"File manager and Recycle Bin"}};
        const float cw = (w - 14) / 2, ch = 82;
        for (int i = 0; i < 4; ++i) {
            const Rect r{x + (i % 2) * (cw + 14), 236.0f + (i / 2) * (ch + 14), cw, ch};
            m_g.FillRR(r, 14, Color{1, 1, 1, 0.045f});
            m_g.StrokeRR(r, 14, Color{1, 1, 1, 0.09f}, 1.0f);
            const Rect ib{r.x + 14, r.y + 16, 36, 36};
            m_g.FillRR(ib, 10, WithA(violet, 0.30f)); m_g.Glyph(fs[i].g, ib, 16.0f, vlight);
            m_g.Text(Turkish() ? fs[i].tt : fs[i].te, Rect{r.x + 62, r.y + 14, r.width - 70, 22}, 14.0f, 600, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
            m_g.TextBox(Turkish() ? fs[i].dt : fs[i].de, Rect{r.x + 62, r.y + 38, r.width - 70, 38}, 11.5f, 400, WithA(mute, 0.95f), DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        Button(Rect{x, kH - 90, 220, 46}, T(L"Kuruluma başla", L"Get started"), true, 10);
        m_g.Text(T(L"Kullanıcı bazlı kurulum", L"Per-user install"), Rect{x + 236, kH - 90, 240, 46}, 12.0f, 400, WithA(mute, 0.7f), DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    void DrawOptions() {
        const float x = kLeft + 40, w = kW - kLeft - 80;
        m_g.Text(T(L"Kurulum seçenekleri", L"Install options"), Rect{x, 56, w, 40}, 26.0f, 300, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
        // location
        m_g.Text(T(L"KONUM", L"LOCATION"), Rect{x, 106, 200, 16}, 10.5f, 600, WithA(vlight, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING);
        const Rect fld{x, 126, w - 126, 40};
        m_g.FillRR(fld, 11, Color{1, 1, 1, 0.06f}); m_g.StrokeRR(fld, 11, Color{1, 1, 1, 0.12f}, 1.0f);
        m_g.Glyph(0xE8B7, Rect{fld.x + 8, fld.y, 28, fld.height}, 14.0f, vlight);
        m_g.Text(m_o.dir, Rect{fld.x + 40, fld.y, fld.width - 50, fld.height}, 12.5f, 400, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
        Button(Rect{x + w - 112, 126, 112, 40}, T(L"Değiştir...", L"Browse..."), false, 12);
        // two columns
        const float cw = (w - 20) / 2, y0 = 186;
        m_g.Text(T(L"BİLEŞENLER", L"COMPONENTS"), Rect{x, y0, cw, 16}, 10.5f, 600, WithA(vlight, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING);
        m_g.Text(T(L"EK SEÇENEKLER", L"EXTRAS"), Rect{x + cw + 20, y0, cw, 16}, 10.5f, 600, WithA(vlight, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING);
        const float rh = 50, gap = 8;
        const bool rs = m_o.replaceShell;
        Row(Rect{x, y0 + 22, cw, rh}, T(L"Aether Island", L"Aether Island"), T(L"Zorunlu · ada, widget'lar, oturum yöneticisi", L"Required · island, widgets, session manager"), true, 19, true, 0);
        Row(Rect{x, y0 + 22 + (rh + gap), cw, rh}, T(L"Görev çubuğu", L"Taskbar"), T(L"Başlat menüsü, tepsi, hızlı ayarlar", L"Start menu, tray, quick settings"), m_o.bar, 20, rs, 0);
        Row(Rect{x, y0 + 22 + 2 * (rh + gap), cw, rh}, T(L"Masaüstü", L"Desktop"), T(L"Duvar kağıdı ve simgeler", L"Wallpaper and icons"), m_o.desk, 21, rs, 0);
        Row(Rect{x, y0 + 22 + 3 * (rh + gap), cw, rh}, T(L"Dosya yöneticisi", L"File manager"), T(L"Aether Gezgini + Geri Dönüşüm", L"Aether Explorer + Recycle Bin"), m_o.expl, 22, rs, 0);
        const float x2 = x + cw + 20, rh2 = 40, g2 = 6;
        Row(Rect{x2, y0 + 22, cw, rh2}, T(L"Masaüstü kısayolu", L"Desktop shortcut"), L"", m_o.deskLnk, 30, false, 1);
        Row(Rect{x2, y0 + 22 + (rh2 + g2), cw, rh2}, T(L"Başlat menüsü kısayolu", L"Start menu shortcut"), L"", m_o.startLnk, 31, false, 1);
        Row(Rect{x2, y0 + 22 + 2 * (rh2 + g2), cw, rh2}, T(L"Windows ile birlikte başlat", L"Start with Windows"), L"", m_o.autostart || rs, 32, rs, 1);
        Row(Rect{x2, y0 + 22 + 3 * (rh2 + g2), cw, rh2}, T(L"Klasörleri Aether'de aç", L"Open folders in Aether"), L"", m_o.folders || rs, 34, rs, 1);
        Row(Rect{x2, y0 + 22 + 4 * (rh2 + g2), cw, rh2}, T(L"Açılış animasyonu", L"Startup animation"), L"", m_o.intro, 35, false, 1);
        Row(Rect{x2, y0 + 22 + 5 * (rh2 + g2), cw, 52}, T(L"Windows kabuğu olarak ayarla", L"Use as the Windows shell"), T(L"explorer.exe yerine açılır · geri alınabilir", L"replaces explorer.exe · reversible"), rs, 33, false, 1);
        Button(Rect{x, kH - 90, 130, 46}, T(L"Geri", L"Back"), false, 11);
        Button(Rect{x + w - 200, kH - 90, 200, 46}, T(L"Kur", L"Install"), true, 10);
    }

    void DrawProgress(float t) {
        const float x = kLeft + 44, w = kW - kLeft - 88;
        const bool failed = m_pr.failed;
        m_g.Text(failed ? T(L"Bir sorun çıktı", L"Something went wrong") : (uninstall ? T(L"Kaldırılıyor...", L"Removing...") : T(L"Kuruluyor...", L"Installing...")), Rect{x, 84, w, 50}, 30.0f, 300, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
        const wchar_t* stepsIn[6] = {L"Çalışan Aether kapatılıyor", L"Dosyalar çıkarılıyor", L"Bileşenler hazırlanıyor", L"Kısayollar oluşturuluyor", L"Sistem kaydediliyor", L"Tamamlanıyor"};
        const wchar_t* stepsInE[6] = {L"Closing running Aether", L"Unpacking files", L"Preparing components", L"Creating shortcuts", L"Registering with the system", L"Finishing"};
        const wchar_t* stepsUn[5] = {L"Çalışan Aether kapatılıyor", L"Windows kabuğu geri yükleniyor", L"Kısayollar ve kayıtlar siliniyor", L"Dosyalar siliniyor", L"Tamamlanıyor"};
        const wchar_t* stepsUnE[5] = {L"Closing running Aether", L"Restoring the Windows shell", L"Removing shortcuts and registry entries", L"Deleting files", L"Finishing"};
        const int n = uninstall ? 5 : 6;
        const int cur = m_pr.step.load();
        float y = 160;
        for (int i = 0; i < n; ++i) {
            const bool done = i < cur || m_pr.done, act = i == cur && !m_pr.done;
            const Rect ic{x, y + 2, 22, 22};
            if (done) { m_g.FillRR(ic, 11, WithA(violet, 0.9f)); m_g.Glyph(0xE73E, ic, 11.0f, mg::kWhite); }
            else if (act) {
                m_g.StrokeRR(ic, 11, WithA(violet, 0.35f), 2.0f);
                const float a = t * 6.0f;
                for (int k = 0; k < 6; ++k) { const float aa = a - k * 0.22f; m_g.FillRR(Rect{ic.x + 11 + std::cos(aa) * 8.5f - 2, ic.y + 11 + std::sin(aa) * 8.5f - 2, 4, 4}, 2, Color{vlight.r, vlight.g, vlight.b, 1.0f - k * 0.16f}); }
            } else m_g.StrokeRR(ic, 11, Color{1, 1, 1, 0.14f}, 1.5f);
            const std::wstring lab = uninstall ? (Turkish() ? stepsUn[i] : stepsUnE[i]) : (Turkish() ? stepsIn[i] : stepsInE[i]);
            m_g.Text(lab, Rect{x + 36, y, w - 40, 26}, 14.0f, act ? 600 : 400, done || act ? ink : WithA(mute, 0.6f), DWRITE_TEXT_ALIGNMENT_LEADING);
            y += 36;
        }
        const Rect bar{x, kH - 130, w, 8};
        m_g.FillRR(bar, 4, Color{1, 1, 1, 0.09f});
        if (!failed) {
            const Rect fill{bar.x, bar.y, std::max(8.0f, bar.width * m_prog), 8};
            m_g.Glow(fill, 4, violet, 10.0f, 0.9f);
            m_g.VGradientRR(fill, 4, vlight, violet);
            m_g.Text(std::to_wstring(static_cast<int>(std::lround(m_prog * 100))) + L"%", Rect{x, kH - 112, w, 24}, 12.5f, 500, WithA(mute, 0.95f), DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            m_g.TextBox(m_pr.Error(), Rect{x, kH - 196, w, 60}, 13.0f, 400, Color{1.0f, 0.62f, 0.66f, 1.0f}, DWRITE_TEXT_ALIGNMENT_LEADING);
            Button(Rect{x, kH - 90, 180, 46}, T(L"Kapat", L"Close"), true, 10);
        }
    }

    void DrawDone(float t) {
        const float x = kLeft + 44, w = kW - kLeft - 88;
        const float cx = x + 44, cy = 150;
        Radial(cx, cy, 90, WithA(violet, 0.55f), WithA(violet, 0.0f));
        const float s = 1.0f + 0.03f * std::sin(t * 2.0f);
        const Rect c{cx - 34 * s, cy - 34 * s, 68 * s, 68 * s};
        m_g.VGradientRR(c, 34 * s, vlight, violet);
        m_g.StrokeRR(c, 34 * s, Color{1, 1, 1, 0.5f}, 1.4f);
        m_g.Glyph(0xE73E, c, 30.0f, mg::kWhite);
        m_g.Text(uninstall ? T(L"Aether kaldırıldı", L"Aether was removed") : T(L"Aether hazır!", L"Aether is ready!"), Rect{x, 214, w, 50}, 32.0f, 300, ink, DWRITE_TEXT_ALIGNMENT_LEADING);
        std::wstring msg;
        if (uninstall) msg = T(L"Her şey eski haline döndü. Windows kabuğu geri yüklendi.", L"Everything is back to how it was. The Windows shell was restored.");
        else if (m_o.replaceShell) msg = T(L"Aether, Windows kabuğu olarak ayarlandı. Oturumu kapatıp yeniden açtığında Aether açılır. Acil durumda oturum açarken SHIFT tuşunu basılı tut: normal Windows masaüstü gelir.",
                                          L"Aether is set as the Windows shell. It starts the next time you sign in. In an emergency hold SHIFT while signing in to get the normal Windows desktop.");
        else msg = T(L"Kurulum tamamlandı. Aether'i şimdi başlatabilir veya Başlat menüsünden açabilirsin.", L"Installation complete. You can start Aether now or open it from the Start menu.");
        m_g.TextBox(msg, Rect{x, 272, w, 100}, 14.5f, 400, WithA(ink, 0.82f), DWRITE_TEXT_ALIGNMENT_LEADING);
        if (!uninstall && !m_o.replaceShell) Row(Rect{x, 392, w - 80, 44}, T(L"Aether'i şimdi başlat", L"Start Aether now"), L"", m_o.launch, 40, false, 1);
        if (!uninstall && m_o.replaceShell) Button(Rect{x + 216, kH - 90, 200, 46}, T(L"Oturumu kapat", L"Sign out"), false, 41);
        Button(Rect{x, kH - 90, 200, 46}, uninstall ? T(L"Kapat", L"Close") : T(L"Bitir", L"Finish"), true, 10);
    }

    void Frame() {
        if (IsIconic(m_hwnd)) return;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const float dt = std::clamp(static_cast<float>(now.QuadPart - m_last.QuadPart) / static_cast<float>(m_freq.QuadPart), 0.001f, 0.05f);
        m_last = now;
        const float t = static_cast<float>(now.QuadPart - m_t0.QuadPart) / static_cast<float>(m_freq.QuadPart);
        m_pageT = std::min(1.0f, m_pageT + dt / 0.35f);
        for (int id : {1, 2, 10, 11, 12, 19, 20, 21, 22, 30, 31, 32, 33, 34, 35, 40, 41}) {
            float& v = m_hv[id]; const float target = (id == m_hover) ? 1.0f : 0.0f; v += (target - v) * (1.0f - std::exp(-16.0f * dt));
        }
        m_prog += (m_pr.value.load() - m_prog) * (1.0f - std::exp(-6.0f * dt));
        if (m_page == P_INSTALL && m_pr.done && m_prog > 0.985f) Go(P_DONE);
        if (silent && !m_started && !uninstall) Begin();
        if (silent && m_page == P_DONE) { Finish(); return; }

        auto* ctx = m_g.Begin();
        if (!ctx) return;
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
        m_hits.clear();
        m_g.FillRect(Rect{0, 0, kW, kH}, Color{0.035f, 0.02f, 0.09f, 1.0f});
        // right side backdrop glow
        Radial(kW - 80.0f, 40.0f, 360.0f, Color{0.5f, 0.3f, 1.0f, 0.16f}, Color{0.5f, 0.3f, 1.0f, 0.0f});
        DrawLeft(t);
        const float e = 1.0f - std::pow(1.0f - Smooth(m_pageT), 3.0f);
        ctx->SetTransform(D2D1::Matrix3x2F::Translation(18.0f * (1.0f - e), 0) * D2D1::Matrix3x2F::Scale(m_scale, m_scale));
        ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), e), nullptr);
        switch (m_page) {
            case P_WELCOME: DrawWelcome(); break;
            case P_OPTIONS: DrawOptions(); break;
            case P_INSTALL: DrawProgress(t); break;
            default: DrawDone(t); break;
        }
        ctx->PopLayer();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
        DrawChrome();
        m_g.End();
        m_hover = HitAt(m_mx, m_my);
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring cl = cmd ? cmd : L"";
    const bool wantUninstall = cl.find(L"--uninstall") != std::wstring::npos;
    const bool silent = cl.find(L"/S") != std::wstring::npos || cl.find(L"/s") != std::wstring::npos;

    Setup app;
    app.silent = silent;
    if (wantUninstall) {
        // the running copy sits inside the folder it must delete: continue from a temporary copy
        const std::wstring self = SelfPath(), dir = DirOf(self);
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const size_t fromPos = cl.find(L"--from ");
        if (fromPos == std::wstring::npos) {
            const std::wstring copy = std::wstring(tmp) + L"aether-uninstall.exe";
            if (CopyFileW(self.c_str(), copy.c_str(), FALSE)) {
                const std::wstring args = L"--uninstall --from \"" + dir + L"\"";
                ShellExecuteW(nullptr, L"open", copy.c_str(), args.c_str(), tmp, SW_SHOWNORMAL);
                return 0;
            }
            app.uninstDir = dir;
        } else {
            std::wstring d = cl.substr(fromPos + 7);
            d.erase(std::remove(d.begin(), d.end(), L'"'), d.end());
            while (!d.empty() && d.back() == L' ') d.pop_back();
            app.uninstDir = d;
        }
        app.uninstall = true;
        app.Options().dir = app.uninstDir;
    }
    if (silent) app.Options().launch = false;
    if (!app.Create(inst)) return 1;
    app.Run();
    return 0;
}
