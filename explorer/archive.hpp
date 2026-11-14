#pragma once
// Archive support of Aether Explorer. No decoder is bundled: extraction/compression is delegated to tools that are
// already on the machine, in this order of preference:
//   7-Zip (7z.exe), WinRAR (UnRAR.exe / WinRAR.exe) and Windows' own tar.exe (libarchive: zip, tar.*, 7z, and rar on current builds).
// Everything here is blocking and meant for worker threads.
#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include "model.hpp"

namespace aether::explorer {

inline bool IsArchiveExt(const std::wstring& e) {
    static const wchar_t* k[] = {L".zip", L".rar", L".7z", L".tar", L".gz", L".tgz", L".bz2", L".tbz2", L".xz", L".txz", L".zst", L".lz", L".lzma",
                                 L".cab", L".iso", L".jar", L".war", L".apk", L".cbz", L".cbr", L".wim", L".arj", L".z"};
    for (auto* x : k) if (e == x) return true;
    return false;
}

// archives that are browsed by a double click (an .iso is mounted by Windows itself, so it is not here)
inline bool IsBrowsableArchive(const std::wstring& e) {
    static const wchar_t* k[] = {L".zip", L".rar", L".7z", L".tar", L".gz", L".tgz", L".bz2", L".tbz2", L".xz", L".txz", L".zst", L".cab"};
    for (auto* x : k) if (e == x) return true;
    return false;
}

inline std::wstring ArcExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : L""; }

inline std::wstring EnvPath(const wchar_t* var, const wchar_t* rel) {
    wchar_t b[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(var, b, MAX_PATH) || !b[0]) return L"";
    return ArcExists(std::wstring(b) + rel);
}

inline std::wstring FindOnPath(const wchar_t* exe) {
    wchar_t b[MAX_PATH] = {};
    if (SearchPathW(nullptr, exe, nullptr, MAX_PATH, b, nullptr)) return b;
    return L"";
}

inline std::wstring Find7z() {
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"}) {
        auto p = EnvPath(v, L"\\7-Zip\\7z.exe"); if (!p.empty()) return p;
    }
    auto p = FindOnPath(L"7z.exe"); if (!p.empty()) return p;
    p = FindOnPath(L"7zz.exe"); if (!p.empty()) return p;
    p = FindOnPath(L"NanaZipC.exe");
    return p;
}
inline std::wstring FindUnrar() {
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"}) {
        auto p = EnvPath(v, L"\\WinRAR\\UnRAR.exe"); if (!p.empty()) return p;
    }
    return FindOnPath(L"UnRAR.exe");
}
inline std::wstring FindTar() {
    auto p = EnvPath(L"SystemRoot", L"\\System32\\tar.exe");
    return p.empty() ? FindOnPath(L"tar.exe") : p;
}

// Runs a command line hidden in `cwd`; returns the exit code, or -1 if it could not start / timed out.
inline int RunHidden(const std::wstring& cmd, const std::wstring& cwd, DWORD timeoutMs = 3600000) {
    STARTUPINFOW si = {sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) return -1;
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs), code = 0;
    if (w == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 1); code = static_cast<DWORD>(-1); }
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

inline std::wstring QuoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

// Extracts `archive` into `destDir` (created when missing). Returns true on success; `tool` gets the engine that was used / tried.
inline bool ExtractArchive(const std::wstring& archive, const std::wstring& destDir, std::wstring& tool) {
    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
    const std::wstring ext = ExtOf(archive);
    const std::wstring z7 = Find7z(), rar = FindUnrar(), tar = FindTar();
    struct Try { std::wstring name, cmd; bool sevenzip; };
    std::vector<Try> tries;
    auto add7z = [&]() { if (!z7.empty()) tries.push_back({L"7-Zip", QuoteArg(z7) + L" x -y -aoa " + QuoteArg(archive), true}); };
    auto addRar = [&]() { if (!rar.empty()) tries.push_back({L"WinRAR", QuoteArg(rar) + L" x -y -o+ -idq " + QuoteArg(archive), false}); };
    auto addTar = [&]() { if (!tar.empty()) tries.push_back({L"tar", QuoteArg(tar) + L" -xf " + QuoteArg(archive), false}); };
    if (ext == L".rar" || ext == L".cbr") { addRar(); add7z(); addTar(); }
    else if (ext == L".7z" || ext == L".wim" || ext == L".arj" || ext == L".z" || ext == L".lzma") { add7z(); addTar(); }
    else { addTar(); add7z(); addRar(); }
    for (const auto& t : tries) {
        tool = t.name;
        const int rc = RunHidden(t.cmd, destDir);
        if (rc == 0 || (t.sevenzip && rc == 1)) return true;     // 7-Zip: 1 = warnings only
    }
    if (tries.empty()) tool = L"";
    return false;
}

// kind: 0 = zip, 1 = 7z, 2 = tar.gz. `names` are file names relative to `dir`.
inline bool CompressTo(int kind, const std::wstring& dir, const std::vector<std::wstring>& names, const std::wstring& out) {
    if (kind == 1) {
        const std::wstring z7 = Find7z();
        if (z7.empty()) return false;
        std::wstring cmd = QuoteArg(z7) + L" a -y " + QuoteArg(out);
        for (const auto& n : names) cmd += L" " + QuoteArg(n);
        return RunHidden(cmd, dir) <= 1;
    }
    const std::wstring tar = FindTar();
    if (tar.empty()) return false;
    std::wstring cmd = QuoteArg(tar) + (kind == 2 ? L" -czf " : L" -a -c -f ") + QuoteArg(out);
    for (const auto& n : names) cmd += L" " + QuoteArg(n);
    return RunHidden(cmd, dir) == 0;
}

} // namespace aether::explorer
