// AetherBin: the Recycle Bin in the Aether style. Lists what was deleted (name, original folder, date, size), restores,
// deletes for good and empties - through the shell's own recycle-bin folder, so everything stays consistent with Windows.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <commoncontrols.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"
#include "aether/render/mini_gfx.hpp"

using namespace aether;
using aether::mg::Color;
using aether::mg::Rect;
using aether::mg::kWhite;
using aether::mg::WithA;
using aether::mg::Mix;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT_PTR kTimerPoll = 1;

struct Item {
    PITEMID_CHILD pidl{nullptr};
    std::wstring name, orig, date, size, ext;
    double dateVal{0};
    bool dir{false};
};

std::wstring Str(IShellFolder2* f, PCUITEMID_CHILD c, UINT col) {
    SHELLDETAILS sd = {};
    if (FAILED(f->GetDetailsOf(c, col, &sd))) return L"";
    wchar_t b[512] = {};
    StrRetToBufW(&sd.str, c, b, 512);
    return b;
}
std::wstring HumanSize(uint64_t v) {
    wchar_t b[64] = {};
    StrFormatByteSizeW(static_cast<LONGLONG>(v), b, 64);
    return b;
}

class Bin {
public:
    bool Create(HINSTANCE inst);
    bool NeedsNothing() const { return true; }
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    float m_scale{1.0f}, m_W{1000}, m_H{680};
    ComPtr<IShellFolder> m_bin;
    ComPtr<IShellFolder2> m_bin2;
    std::vector<Item> m_items;
    std::vector<char> m_sel;
    std::map<std::wstring, ComPtr<ID2D1Bitmap1>> m_icons;
    int m_anchor{-1}, m_hot{-1}, m_hotBtn{0};
    float m_scroll{0.0f};
    int m_confirm{0};                      // 1 = delete selection for good, 2 = empty the bin
    std::wstring m_status;
    LONGLONG m_binCount{-1}, m_binSize{0};
    std::vector<std::pair<Rect, int>> m_hits;
    static constexpr float kHeader = 130.0f, kRow = 46.0f, kColHead = 30.0f;

    void Refresh();
    void Render();
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    int RowAt(float x, float y) const;
    int HitId(float x, float y) const { for (int i = static_cast<int>(m_hits.size()) - 1; i >= 0; --i) if (m_hits[i].first.Contains(x, y)) return m_hits[i].second; return 0; }
    void Hit(const Rect& r, int id) { m_hits.push_back({r, id}); }
    ComPtr<ID2D1Bitmap1> IconFor(const Item& it);
    bool Invoke(const std::vector<int>& idx, const char* verb, bool noUi);
    std::vector<int> Selected() const { std::vector<int> v; for (size_t i = 0; i < m_sel.size(); ++i) if (m_sel[i]) v.push_back(static_cast<int>(i)); return v; }
    void Restore();
    void DeleteForGood();
    void EmptyAll();
    void Button(const Rect& r, const std::wstring& label, wchar_t glyph, int id, bool enabled, bool danger);
    float ContentH() const { return static_cast<float>(m_items.size()) * kRow; }
    float ViewH() const { return m_H - kHeader - kColHead - (m_confirm ? 64.0f : 0.0f) - 8.0f; }
    void ClampScroll() { m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, ContentH() - ViewH())); }
};

bool Bin::Create(HINSTANCE inst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"AetherBinWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Geri Dönüşüm Kutusu", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1020, 700, nullptr, nullptr, inst, this);
    if (!m_hwnd) return false;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20, &dark, sizeof(dark));
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    RECT rc; GetClientRect(m_hwnd, &rc);
    m_dc = std::make_unique<render::DCompLayer>();
    if (!m_dc->Initialize(m_hwnd, rc.right, rc.bottom)) return false;
    m_g.Init(m_dc.get());
    render::glass::SyncTheme();
    Refresh();
    SetTimer(m_hwnd, kTimerPoll, 2500, nullptr);
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    Render();
    return true;
}

ComPtr<ID2D1Bitmap1> Bin::IconFor(const Item& it) {
    const std::wstring key = it.dir ? L"<dir>" : it.ext;
    auto f = m_icons.find(key);
    if (f != m_icons.end()) return f->second;
    ComPtr<ID2D1Bitmap1> bmp;
    SHFILEINFOW sfi = {};
    const std::wstring probe = it.dir ? L"klasor" : L"dosya" + it.ext;
    if (SHGetFileInfoW(probe.c_str(), it.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) {
        ComPtr<IImageList> il;
        if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&il))) && il) {
            HICON ic = nullptr;
            if (SUCCEEDED(il->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &ic)) && ic) { bmp = mg::BitmapFromHicon(m_dc->GetContext(), ic); DestroyIcon(ic); }
        }
    }
    m_icons[key] = bmp;
    return bmp;
}

void Bin::Refresh() {
    std::set<std::wstring> keep;
    for (size_t i = 0; i < m_items.size() && i < m_sel.size(); ++i) if (m_sel[i]) keep.insert(m_items[i].name + L"\n" + m_items[i].orig);
    for (auto& it : m_items) if (it.pidl) CoTaskMemFree(it.pidl);
    m_items.clear();
    m_bin.Reset(); m_bin2.Reset();
    PIDLIST_ABSOLUTE bp = nullptr;
    ComPtr<IShellFolder> desk;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &bp)) && bp && SUCCEEDED(SHGetDesktopFolder(&desk))) {
        desk->BindToObject(bp, nullptr, IID_PPV_ARGS(&m_bin));
        CoTaskMemFree(bp);
    }
    if (m_bin) {
        m_bin.As(&m_bin2);
        ComPtr<IEnumIDList> en;
        if (SUCCEEDED(m_bin->EnumObjects(m_hwnd, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en)) && en) {
            PITEMID_CHILD c = nullptr; ULONG got = 0;
            while (en->Next(1, &c, &got) == S_OK && got == 1) {
                Item it; it.pidl = c;
                STRRET sr = {}; wchar_t b[MAX_PATH * 2] = {};
                if (SUCCEEDED(m_bin->GetDisplayNameOf(c, SHGDN_INFOLDER, &sr))) { StrRetToBufW(&sr, c, b, MAX_PATH * 2); it.name = b; }
                SFGAOF at = SFGAO_FOLDER; PCUITEMID_CHILD cc = c; m_bin->GetAttributesOf(1, &cc, &at);
                it.dir = (at & SFGAO_FOLDER) != 0;
                const size_t dot = it.name.find_last_of(L'.');
                if (!it.dir && dot != std::wstring::npos) it.ext = it.name.substr(dot);
                if (m_bin2) {
                    static const SHCOLUMNID kOrig = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 2};
                    static const SHCOLUMNID kDate = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 3};
                    VARIANT v; VariantInit(&v);
                    if (SUCCEEDED(m_bin2->GetDetailsEx(c, &kOrig, &v)) && v.vt == VT_BSTR && v.bstrVal) it.orig = v.bstrVal; else it.orig = Str(m_bin2.Get(), c, 1);
                    VariantClear(&v);
                    if (SUCCEEDED(m_bin2->GetDetailsEx(c, &kDate, &v)) && v.vt == VT_DATE) {
                        it.dateVal = v.date;
                        SYSTEMTIME st = {};
                        if (VariantTimeToSystemTime(v.date, &st)) {
                            SYSTEMTIME lt = {}; SystemTimeToTzSpecificLocalTime(nullptr, &st, &lt);
                            wchar_t d[64] = {}, t[32] = {};
                            GetDateFormatEx(L"tr-TR", 0, &lt, L"d MMM yyyy", d, 64, nullptr);
                            GetTimeFormatEx(L"tr-TR", TIME_NOSECONDS, &lt, nullptr, t, 32);
                            it.date = std::wstring(d) + L"  " + t;
                        }
                    } else it.date = Str(m_bin2.Get(), c, 2);
                    VariantClear(&v);
                }
                if (!it.dir && SUCCEEDED(m_bin->GetDisplayNameOf(c, SHGDN_FORPARSING, &sr))) {
                    StrRetToBufW(&sr, c, b, MAX_PATH * 2);
                    WIN32_FILE_ATTRIBUTE_DATA ad{};
                    if (GetFileAttributesExW(b, GetFileExInfoStandard, &ad)) it.size = HumanSize((static_cast<uint64_t>(ad.nFileSizeHigh) << 32) | ad.nFileSizeLow);
                }
                m_items.push_back(std::move(it));
            }
        }
    }
    std::stable_sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) { return a.dateVal > b.dateVal; });
    m_sel.assign(m_items.size(), 0);
    for (size_t i = 0; i < m_items.size(); ++i) if (keep.count(m_items[i].name + L"\n" + m_items[i].orig)) m_sel[i] = 1;
    SHQUERYRBINFO qi = {sizeof(qi)};
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &qi))) { m_binCount = qi.i64NumItems; m_binSize = qi.i64Size; }
    ClampScroll();
}

bool Bin::Invoke(const std::vector<int>& idx, const char* verb, bool noUi) {
    if (idx.empty() || !m_bin) return false;
    std::vector<PCUITEMID_CHILD> cs;
    for (int i : idx) cs.push_back(m_items[i].pidl);
    ComPtr<IContextMenu> cm;
    if (FAILED(m_bin->GetUIObjectOf(m_hwnd, static_cast<UINT>(cs.size()), cs.data(), IID_IContextMenu, nullptr, reinterpret_cast<void**>(cm.GetAddressOf()))) || !cm) return false;
    HMENU hm = CreatePopupMenu();
    cm->QueryContextMenu(hm, 0, 1, 0x7FFF, CMF_NORMAL);
    CMINVOKECOMMANDINFO ici = {sizeof(ici)};
    ici.fMask = noUi ? CMIC_MASK_FLAG_NO_UI : 0;
    ici.hwnd = m_hwnd; ici.lpVerb = verb; ici.nShow = SW_SHOWNORMAL;
    const HRESULT hr = cm->InvokeCommand(&ici);
    DestroyMenu(hm);
    return SUCCEEDED(hr);
}

void Bin::Restore() {
    std::vector<int> idx = Selected();
    if (idx.empty()) for (int i = 0; i < static_cast<int>(m_items.size()); ++i) idx.push_back(i);       // nothing selected: restore everything
    if (idx.empty()) return;
    const bool ok = Invoke(idx, "undelete", false);
    m_status = ok ? std::to_wstring(idx.size()) + L" öğe geri yüklendi" : L"Geri yüklenemedi";
    Refresh();
}
void Bin::DeleteForGood() {
    const auto idx = Selected();
    if (idx.empty()) return;
    const bool ok = Invoke(idx, "delete", true);
    m_status = ok ? std::to_wstring(idx.size()) + L" öğe kalıcı olarak silindi" : L"Silinemedi";
    Refresh();
}
void Bin::EmptyAll() {
    const HRESULT hr = SHEmptyRecycleBinW(m_hwnd, nullptr, SHERB_NOCONFIRMATION);
    m_status = SUCCEEDED(hr) ? L"Geri dönüşüm kutusu boşaltıldı" : L"Boşaltılamadı";
    Refresh();
}

int Bin::RowAt(float x, float y) const {
    if (x < 12 || x > m_W - 12 || y < kHeader + kColHead || y > kHeader + kColHead + ViewH()) return -1;
    const int i = static_cast<int>((y - kHeader - kColHead + m_scroll) / kRow);
    return (i >= 0 && i < static_cast<int>(m_items.size())) ? i : -1;
}

void Bin::Button(const Rect& r, const std::wstring& label, wchar_t glyph, int id, bool enabled, bool danger) {
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    const Color base = danger ? Color{0.95f, 0.35f, 0.40f, 1.0f} : v;
    const bool hot = enabled && m_hotBtn == id;
    m_g.FillRR(r, 12, enabled ? WithA(base, hot ? 0.50f : 0.28f) : WithA(kWhite, 0.06f));
    m_g.StrokeRR(r, 12, enabled ? WithA(danger ? Color{1.0f, 0.55f, 0.55f, 1.0f} : vl, hot ? 0.9f : 0.5f) : WithA(kWhite, 0.10f), 1.0f);
    const float a = enabled ? 1.0f : 0.35f;
    m_g.Glyph(glyph, Rect{r.x + 8, r.y, 28, r.height}, 14.0f, WithA(kWhite, a));
    m_g.Text(label, Rect{r.x + 38, r.y, r.width - 44, r.height}, 13.0f, 500, WithA(kWhite, a));
    if (enabled) Hit(r, id);
}

void Bin::Render() {
    if (!m_dc || IsIconic(m_hwnd)) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    if (rc.right < 64 || rc.bottom < 64) return;
    if (!m_g.Begin()) return;
    auto* ctx = m_g.ctx;
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    m_W = rc.right / m_scale; m_H = rc.bottom / m_scale;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    m_hits.clear();
    const auto& th = render::glass::CurrentTheme();
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    m_g.VGradientRR(Rect{0, 0, m_W, m_H}, 0, Color{th.bodyTop.r * 0.5f + 0.02f, th.bodyTop.g * 0.5f + 0.01f, th.bodyTop.b * 0.5f + 0.05f, 1.0f},
                    Color{th.bodyBot.r * 0.35f + 0.01f, th.bodyBot.g * 0.35f + 0.01f, th.bodyBot.b * 0.35f + 0.03f, 1.0f});

    // header
    const bool empty = m_items.empty();
    const Rect ic{28, 22, 46, 46};
    m_g.Glow(ic, 14, v, 12.0f, 0.8f);
    m_g.VGradientRR(ic, 14, Color{0.78f, 0.62f, 1.0f, 1.0f}, Color{0.50f, 0.30f, 0.90f, 1.0f});
    m_g.Glyph(0xE74D, ic, 22.0f, kWhite);
    m_g.Text(L"Geri Dönüşüm Kutusu", Rect{88, 18, m_W - 400, 30}, 21.0f, 600, kWhite);
    std::wstring sub = empty ? L"Boş" : std::to_wstring(m_items.size()) + L" öğe";
    if (!empty && m_binSize > 0) sub += L"  ·  " + HumanSize(static_cast<uint64_t>(m_binSize));
    const auto sel = Selected();
    if (!sel.empty()) sub += L"  ·  " + std::to_wstring(sel.size()) + L" seçili";
    m_g.Text(sub, Rect{88, 48, m_W - 400, 22}, 13.0f, 400, WithA(kWhite, 0.62f));
    if (!m_status.empty()) m_g.Text(m_status, Rect{m_W - 360, 22, 332, 22}, 12.5f, 400, WithA(vl, 0.95f), DWRITE_TEXT_ALIGNMENT_TRAILING);
    float bx = 28;
    const float by = 84, bh = 34;
    Button(Rect{bx, by, sel.empty() ? 190.0f : 170.0f, bh}, sel.empty() ? L"Tümünü geri yükle" : L"Geri yükle", 0xE777, 1, !empty, false); bx += (sel.empty() ? 190.0f : 170.0f) + 10;
    Button(Rect{bx, by, 150, bh}, L"Kalıcı olarak sil", 0xE74D, 2, !sel.empty(), true); bx += 160;
    Button(Rect{bx, by, 190, bh}, L"Kutuyu boşalt", 0xE74D, 3, !empty, true);

    // column headings
    const float cx0 = 28, cwDate = 170, cwSize = 90;
    const float cwName = (m_W - 56 - cwDate - cwSize) * 0.52f, cwOrig = (m_W - 56 - cwDate - cwSize) * 0.48f;
    const float y0 = kHeader;
    m_g.FillRR(Rect{12, y0, m_W - 24, kColHead}, 8, WithA(kWhite, 0.05f));
    m_g.Text(L"Ad", Rect{cx0 + 40, y0, cwName - 40, kColHead}, 12.0f, 600, WithA(kWhite, 0.6f));
    m_g.Text(L"Özgün konum", Rect{cx0 + cwName, y0, cwOrig, kColHead}, 12.0f, 600, WithA(kWhite, 0.6f));
    m_g.Text(L"Silinme tarihi", Rect{cx0 + cwName + cwOrig, y0, cwDate, kColHead}, 12.0f, 600, WithA(kWhite, 0.6f));
    m_g.Text(L"Boyut", Rect{cx0 + cwName + cwOrig + cwDate, y0, cwSize - 4, kColHead}, 12.0f, 600, WithA(kWhite, 0.6f), DWRITE_TEXT_ALIGNMENT_TRAILING);

    // rows
    const Rect view{12, y0 + kColHead, m_W - 24, ViewH()};
    if (empty) {
        m_g.Glyph(0xE74D, Rect{view.x, view.y + view.height / 2 - 70, view.width, 70}, 54.0f, WithA(vl, 0.35f));
        m_g.Text(L"Geri dönüşüm kutusu boş", Rect{view.x, view.y + view.height / 2 + 4, view.width, 30}, 16.0f, 500, WithA(kWhite, 0.7f), DWRITE_TEXT_ALIGNMENT_CENTER);
        m_g.Text(L"Sildiğiniz dosyalar burada görünür.", Rect{view.x, view.y + view.height / 2 + 34, view.width, 24}, 13.0f, 400, WithA(kWhite, 0.4f), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        ctx->PushAxisAlignedClip(mg::D(view), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const int first = std::max(0, static_cast<int>(m_scroll / kRow)), last = std::min(static_cast<int>(m_items.size()) - 1, static_cast<int>((m_scroll + view.height) / kRow));
        for (int i = first; i <= last; ++i) {
            const Item& it = m_items[i];
            const Rect row{view.x, view.y + i * kRow - m_scroll, view.width, kRow - 2};
            const bool s = m_sel[i] != 0, hot = i == m_hot;
            if (s) { m_g.FillRR(row, 10, WithA(Mix(v, vl, 0.2f), 0.30f)); m_g.StrokeRR(row, 10, WithA(vl, 0.55f), 1.0f); }
            else if (hot) m_g.FillRR(row, 10, WithA(vl, 0.09f));
            if (auto bmp = IconFor(it)) {
                const D2D1_RECT_F dr = D2D1::RectF(cx0, row.y + 7, cx0 + 30, row.y + 37);
                ctx->DrawBitmap(bmp.Get(), &dr, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            }
            m_g.Text(it.name, Rect{cx0 + 40, row.y, cwName - 48, row.height}, 13.5f, s ? 600 : 400, kWhite);
            m_g.Text(it.orig, Rect{cx0 + cwName, row.y, cwOrig - 10, row.height}, 12.5f, 400, WithA(kWhite, 0.62f));
            m_g.Text(it.date, Rect{cx0 + cwName + cwOrig, row.y, cwDate, row.height}, 12.5f, 400, WithA(kWhite, 0.62f));
            m_g.Text(it.size, Rect{cx0 + cwName + cwOrig + cwDate, row.y, cwSize - 4, row.height}, 12.5f, 400, WithA(kWhite, 0.62f), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        ctx->PopAxisAlignedClip();
        const float maxS = std::max(0.0f, ContentH() - view.height);
        if (maxS > 0) {
            const float th2 = std::max(30.0f, view.height * view.height / ContentH());
            m_g.FillRR(Rect{m_W - 10, view.y + (view.height - th2) * (m_scroll / maxS), 3, th2}, 1.5f, WithA(kWhite, 0.30f));
        }
    }

    // confirmation bar
    if (m_confirm) {
        const Rect bar{20, m_H - 76, m_W - 40, 58};
        m_g.Glow(bar, 14, Color{0.95f, 0.35f, 0.40f, 1.0f}, 10.0f, 0.8f);
        m_g.FillRR(bar, 14, Color{0.16f, 0.06f, 0.10f, 0.97f});
        m_g.StrokeRR(bar, 14, Color{1.0f, 0.5f, 0.55f, 0.6f}, 1.2f);
        const std::wstring q = m_confirm == 2 ? L"Geri dönüşüm kutusundaki tüm öğeler kalıcı olarak silinsin mi? Bu işlem geri alınamaz."
                                              : std::to_wstring(sel.size()) + L" öğe kalıcı olarak silinsin mi? Bu işlem geri alınamaz.";
        m_g.Text(q, Rect{bar.x + 20, bar.y, bar.width - 280, bar.height}, 13.5f, 500, kWhite);
        Button(Rect{bar.Right() - 250, bar.y + 12, 110, 34}, L"Sil", 0xE74D, 10, true, true);
        Button(Rect{bar.Right() - 130, bar.y + 12, 110, 34}, L"Vazgeç", 0xE711, 11, true, false);
    }
    m_g.End();
}

LRESULT CALLBACK Bin::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Bin* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Bin*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else self = reinterpret_cast<Bin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Bin::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps); Render(); return 0; }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && m_dc) { m_dc->Resize(LOWORD(l), HIWORD(l)); ClampScroll(); Render(); }
        return 0;
    case WM_GETMINMAXINFO: { auto* mm = reinterpret_cast<MINMAXINFO*>(l); mm->ptMinTrackSize.x = 760; mm->ptMinTrackSize.y = 420; return 0; }
    case WM_ACTIVATE: if (LOWORD(w) != WA_INACTIVE) { Refresh(); Render(); } return 0;
    case WM_TIMER:
        if (w == kTimerPoll) {
            if (render::glass::SyncTheme()) Render();
            SHQUERYRBINFO qi = {sizeof(qi)};
            if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &qi)) && (qi.i64NumItems != m_binCount || qi.i64Size != m_binSize)) { Refresh(); Render(); }
        }
        return 0;
    case WM_MOUSEMOVE: {
        const float x = GET_X_LPARAM(l) / m_scale, y = GET_Y_LPARAM(l) / m_scale;
        const int row = m_confirm && y > m_H - 80 ? -1 : RowAt(x, y), btn = HitId(x, y);
        if (row != m_hot || btn != m_hotBtn) { m_hot = row; m_hotBtn = btn; Render(); }
        return 0;
    }
    case WM_MOUSELEAVE: m_hot = -1; m_hotBtn = 0; Render(); return 0;
    case WM_MOUSEWHEEL:
        m_scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120.0f * kRow * 2; ClampScroll(); Render();
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(m_hwnd);
        const float x = GET_X_LPARAM(l) / m_scale, y = GET_Y_LPARAM(l) / m_scale;
        const int btn = HitId(x, y);
        m_status.clear();
        if (btn) {
            switch (btn) {
                case 1: Restore(); break;
                case 2: if (!Selected().empty()) m_confirm = 1; break;
                case 3: if (!m_items.empty()) m_confirm = 2; break;
                case 10: { const int c = m_confirm; m_confirm = 0; if (c == 1) DeleteForGood(); else if (c == 2) EmptyAll(); break; }
                case 11: m_confirm = 0; break;
            }
            Render();
            return 0;
        }
        const int row = RowAt(x, y);
        const bool ctrl = (w & MK_CONTROL) != 0, shift = (w & MK_SHIFT) != 0;
        if (row >= 0) {
            if (shift && m_anchor >= 0) { std::fill(m_sel.begin(), m_sel.end(), 0); for (int i = std::min(m_anchor, row); i <= std::max(m_anchor, row); ++i) m_sel[i] = 1; }
            else if (ctrl) { m_sel[row] = !m_sel[row]; m_anchor = row; }
            else { std::fill(m_sel.begin(), m_sel.end(), 0); m_sel[row] = 1; m_anchor = row; }
        } else if (y > kHeader + kColHead) std::fill(m_sel.begin(), m_sel.end(), 0);
        Render();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const int row = RowAt(GET_X_LPARAM(l) / m_scale, GET_Y_LPARAM(l) / m_scale);
        if (row >= 0) { std::fill(m_sel.begin(), m_sel.end(), 0); m_sel[row] = 1; Restore(); Render(); }
        return 0;
    }
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const int n = static_cast<int>(m_items.size());
        auto moveTo = [&](int i) {
            if (n == 0) return;
            i = std::clamp(i, 0, n - 1);
            std::fill(m_sel.begin(), m_sel.end(), 0); m_sel[i] = 1; m_anchor = i;
            const float top = i * kRow, bot = top + kRow;
            if (top < m_scroll) m_scroll = top; else if (bot > m_scroll + ViewH()) m_scroll = bot - ViewH();
            ClampScroll();
        };
        int cur = -1; for (int i = 0; i < n; ++i) if (m_sel[i]) { cur = i; break; }
        switch (w) {
            case VK_DOWN: moveTo(cur < 0 ? 0 : cur + 1); break;
            case VK_UP: moveTo(cur < 0 ? 0 : cur - 1); break;
            case VK_HOME: moveTo(0); break;
            case VK_END: moveTo(n - 1); break;
            case VK_RETURN: if (m_confirm) { Handle(WM_LBUTTONDOWN, 0, 0); } else Restore(); break;
            case VK_DELETE: if (!Selected().empty()) m_confirm = 1; break;
            case VK_ESCAPE: if (m_confirm) m_confirm = 0; else DestroyWindow(m_hwnd); return 0;
            case VK_F5: Refresh(); break;
            case 'A': if (ctrl) std::fill(m_sel.begin(), m_sel.end(), 1); break;
        }
        Render();
        return 0;
    }
    case WM_DESTROY:
        for (auto& it : m_items) if (it.pidl) CoTaskMemFree(it.pidl);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Aether.Bin.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {                         // already open: just bring it forward
        if (HWND h = FindWindowW(L"AetherBinWindow", nullptr)) { if (IsIconic(h)) ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
        return 0;
    }
    SetCurrentProcessExplicitAppUserModelID(L"Aether.Bin");
    OleInitialize(nullptr);
    Bin app;
    if (!app.Create(inst)) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    OleUninitialize();
    return 0;
}
