#pragma once
// Picks a bundled wallpaper that matches the active Aether theme: purple artwork for purple-toned themes,
// the black metallic artwork for everything dark/neutral. The user's own wallpaper path is remembered once so it can be restored.
#include <windows.h>
#include <string>

namespace aether::system::wallpaper {

inline bool IsPurpleTheme(int i) { return i == 0 || i == 5 || i == 7; }   // Violet, Rose, Lavender

inline const wchar_t* FileFor(int i) {
    if (i == 0) return L"purple_cosmic.png";
    if (i == 5 || i == 7) return L"purple_waves.png";
    return L"black_metal.jpg";
}

inline std::wstring AppDataDir() {
    wchar_t b[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(b) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

inline std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

inline std::wstring Current() {
    wchar_t p[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, p, 0);
    return p;
}

inline void SetFile(const std::wstring& path) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        const wchar_t* style = L"10"; const wchar_t* tile = L"0";   // "Fill", not tiled
        RegSetValueExW(k, L"WallpaperStyle", 0, REG_SZ, reinterpret_cast<const BYTE*>(style), 6);
        RegSetValueExW(k, L"TileWallpaper", 0, REG_SZ, reinterpret_cast<const BYTE*>(tile), 4);
        RegCloseKey(k);
    }
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(path.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

inline void ApplyForTheme(int theme) {
    const std::wstring dir = ExeDir() + L"\\wallpapers\\";
    const std::wstring path = dir + FileFor(theme);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const std::wstring cur = Current();
    if (_wcsicmp(cur.c_str(), path.c_str()) == 0) return;
    // remember the user's own wallpaper the first time we replace it
    const std::wstring store = AppDataDir() + L"\\orig_wallpaper.txt";
    if (!cur.empty() && cur.find(dir) == std::wstring::npos && GetFileAttributesW(store.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(store.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, cur.c_str(), static_cast<DWORD>(cur.size() * sizeof(wchar_t)), &w, nullptr); CloseHandle(h); }
    }
    SetFile(path);
}

inline void RestoreOriginal() {
    const std::wstring store = AppDataDir() + L"\\orig_wallpaper.txt";
    HANDLE h = CreateFileW(store.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t b[MAX_PATH] = {}; DWORD r = 0; ReadFile(h, b, (MAX_PATH - 1) * sizeof(wchar_t), &r, nullptr); CloseHandle(h);
    if (b[0]) SetFile(b);
}

} // namespace aether::system::wallpaper
