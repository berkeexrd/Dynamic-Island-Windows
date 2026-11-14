#pragma once
// Small shell helpers shared by the desktop (and usable by any Aether window): system clipboard for files, shell data objects,
// OLE drop source and IFileOperation based copy / move / delete / rename. Header-only; every function here is `inline`.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <ole2.h>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include "model.hpp"

namespace aether::shellutil {

using Microsoft::WRL::ComPtr;
using aether::explorer::JoinPath;

inline bool IEquals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
inline std::wstring UniqueName(const std::wstring& dir, const std::wstring& base, const std::wstring& ext) {
    std::wstring n = base + ext;
    for (int i = 2; GetFileAttributesW(JoinPath(dir, n).c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; ++i)
        n = base + L" (" + std::to_wstring(i) + L")" + ext;
    return n;
}

// ---- file operations (IFileOperation = the shell's own engine: progress UI, undo, recycle bin, conflict dialogs) ----
enum class Op { Copy, Move, Delete, DeletePerm, Rename };

// Runs on a worker thread; posts `doneMsg` (lParam = malloc'ed wchar_t* name to select, or null) to `owner` when finished.
inline void RunFileOp(HWND owner, UINT doneMsg, Op op, std::vector<std::wstring> src, std::wstring dest, std::wstring newName, std::wstring selectAfter) {
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
            if (!PostMessageW(owner, doneMsg, 0, reinterpret_cast<LPARAM>(sel))) free(sel);
        }
    }).detach();
}

inline bool SetClipFiles(HWND hwnd, const std::vector<std::wstring>& paths, bool cut) {
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
inline bool GetClipFiles(HWND hwnd, std::vector<std::wstring>& out, bool& cut) {
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
inline void SetClipText(HWND hwnd, const std::wstring& t) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (!h) return;
    wcscpy(static_cast<wchar_t*>(GlobalLock(h)), t.c_str()); GlobalUnlock(h);
    if (!OpenClipboard(hwnd)) { GlobalFree(h); return; }
    EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, h); CloseClipboard();
}
inline ComPtr<IDataObject> MakeDataObject(const std::vector<std::wstring>& paths) {
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
inline std::vector<std::wstring> PathsFromData(IDataObject* obj) {
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

class ShellDropSource : public IDropSource {
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


} // namespace aether::shellutil
