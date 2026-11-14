#pragma once
// Start-with-Windows support: a per-user Run entry for Aether.exe, and a highest-privilege logon task for
// AetherBar.exe (it needs administrator rights, which a plain Run entry cannot give without a UAC prompt each time).
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace aether::system::autostart {

inline std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

inline void SetRunKey(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
        const std::wstring v = std::wstring(L"\"") + p + L"\"";
        RegSetValueExW(k, L"Aether", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Aether");
    }
    RegCloseKey(k);
}

// Creates / removes the elevated logon task for the bar. Shows one UAC prompt.
inline void SetBarTask(bool on) {
    const std::wstring bar = ExeDir() + L"\\AetherBar.exe";
    std::wstring args;
    if (on) {
        if (GetFileAttributesW(bar.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        args = L"/Create /TN \"AetherBar\" /TR \"\\\"" + bar + L"\\\"\" /SC ONLOGON /RL HIGHEST /F";
    } else {
        args = L"/Delete /TN \"AetherBar\" /F";
    }
    ShellExecuteW(nullptr, L"runas", L"schtasks.exe", args.c_str(), nullptr, SW_HIDE);
}

inline void Apply(bool on) { SetRunKey(on); SetBarTask(on); }

} // namespace aether::system::autostart
