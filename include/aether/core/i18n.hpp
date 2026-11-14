#pragma once
// Tiny UI localisation helper (English / Turkish). The language follows the Windows display language;
// "--lang=en" / "--lang=tr" on the command line (see i18n::SetFromCommandLine) overrides it.
#include <windows.h>
#include <cstring>
#include <cwchar>
#include <string>

namespace aether::i18n {

inline int& Mode() { static int m = 0; return m; }   // 0 = follow Windows, 1 = English, 2 = Turkish

inline void SetFromCommandLine(const wchar_t* cmdLine) {
    if (!cmdLine) return;
    if (wcsstr(cmdLine, L"--lang=en")) Mode() = 1;
    else if (wcsstr(cmdLine, L"--lang=tr")) Mode() = 2;
}

// The chosen language lives in %APPDATA%\Aether\lang.txt ("en" / "tr" / "auto") so every Aether process follows it.
inline std::wstring LangFile() {
    wchar_t b[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(b) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\lang.txt";
}
inline int& FileMode() { static int m = 0; return m; }
inline void SaveLanguage(int mode) {   // 0 auto, 1 English, 2 Turkish
    const std::wstring p = LangFile(); if (p.empty()) return;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const char* s = mode == 1 ? "en" : (mode == 2 ? "tr" : "auto"); DWORD w; WriteFile(h, s, static_cast<DWORD>(strlen(s)), &w, nullptr); CloseHandle(h);
    FileMode() = mode;
}
inline void PollLanguageFile() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (last && now - last < 1500) return;
    last = now;
    const std::wstring p = LangFile(); if (p.empty()) return;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char b[8] = {}; DWORD r = 0; ReadFile(h, b, 7, &r, nullptr); CloseHandle(h);
    FileMode() = !strncmp(b, "en", 2) ? 1 : (!strncmp(b, "tr", 2) ? 2 : 0);
}

// An explicit English choice (command line, language file, recording) must also give English dates, whatever Windows uses.
inline bool ExplicitEnglish();
inline const wchar_t* Loc() { return ExplicitEnglish() ? L"en-US" : LOCALE_NAME_USER_DEFAULT; }

inline bool Turkish() {
    if (Mode() == 1) return false;
    if (Mode() == 2) return true;
    PollLanguageFile();
    if (FileMode() == 1) return false;
    if (FileMode() == 2) return true;
    static const bool system = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH;
    return system;
}

inline bool ExplicitEnglish() { return Mode() == 1 || (Mode() == 0 && FileMode() == 1); }

inline const wchar_t* Pick(const wchar_t* en, const wchar_t* tr) { return Turkish() ? tr : en; }

// Month / weekday names come straight from Windows (the user's regional language), so every language works.
inline const wchar_t* Month(int m /*1..12*/) {
    if (m < 1 || m > 12) return L"";
    static std::wstring cache[12]; static const wchar_t* seen = nullptr;
    if (seen != Loc()) { for (auto& x : cache) x.clear(); seen = Loc(); }
    std::wstring& c = cache[m - 1];
    if (c.empty()) {
        wchar_t b[64] = {};
        if (GetLocaleInfoEx(Loc(), LOCALE_SMONTHNAME1 + (m - 1), b, 64) > 0) c = b; else c = L"?";
    }
    return c.c_str();
}

inline const wchar_t* Weekday(int dow /*0 = Sunday*/) {
    if (dow < 0 || dow > 6) return L"";
    static std::wstring cache[7]; static const wchar_t* seen = nullptr;
    if (seen != Loc()) { for (auto& x : cache) x.clear(); seen = Loc(); }
    std::wstring& c = cache[dow];
    if (c.empty()) {
        wchar_t b[64] = {};
        if (GetLocaleInfoEx(Loc(), LOCALE_SDAYNAME1 + ((dow + 6) % 7), b, 64) > 0) c = b; else c = L"?";
    }
    return c.c_str();
}

// "Cumartesi, 14 Kasım" / "Saturday, November 14": Windows' own pattern for the user's language
inline std::wstring LongDate(const SYSTEMTIME& st) {
    wchar_t md[48] = {};
    std::wstring fmt = L"dddd, ";
    if (GetLocaleInfoEx(Loc(), LOCALE_SMONTHDAY, md, 48) > 0) fmt += md; else fmt += L"d MMMM";
    wchar_t buf[96] = {};
    if (GetDateFormatEx(Loc(), 0, &st, fmt.c_str(), buf, 96, nullptr) > 0) return buf;
    swprintf(buf, 96, L"%ls, %d %ls", Weekday(st.wDayOfWeek), st.wDay, Month(st.wMonth));
    return buf;
}

// "13.11.2026" (Turkish) / locale short date otherwise
inline std::wstring ShortDate(const SYSTEMTIME& st) {
    wchar_t buf[48];
    if (Turkish()) { swprintf(buf, 48, L"%02d.%02d.%04d", st.wDay, st.wMonth, st.wYear); return buf; }
    if (GetDateFormatEx(Loc(), DATE_SHORTDATE, &st, nullptr, buf, 48, nullptr) > 0) return buf;
    swprintf(buf, 48, L"%d/%d/%d", st.wMonth, st.wDay, st.wYear);
    return buf;
}

} // namespace aether::i18n
