#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/animation/spring.hpp"
#include "aether/render/dcomp_layer.hpp"

namespace aether::bar {

using Microsoft::WRL::ComPtr;

// Aether Start: left rail (profile, folders, settings, power), search box, pinned apps (6 x 2),
// recommended files, "All apps" list and unified search (apps, settings, files, web).
class StartMenu {
public:
    StartMenu();
    ~StartMenu();

    bool Create();
    void PreloadAsync();                        // enumerate apps / recent files / user files on worker threads
    void Toggle(HMONITOR monitor, int reservedPx);
    void Show(HMONITOR monitor, int reservedPx);
    void Hide();
    [[nodiscard]] bool IsOpen() const { return m_wantOpen; }
    [[nodiscard]] bool JustDismissed() const { return GetTickCount64() - m_dismissTick < 600; }
    bool Frame(float dt);                       // returns true while it needs frames

private:
    enum class View { Home, Apps, Search };
    enum Kind { kAppTile, kRecentRow, kAppRow, kSettingRow, kFileRow, kWebRow, kLinkPinned, kLinkRecent };

    struct App {
        std::wstring name, fold, parse;
        ComPtr<ID2D1Bitmap1> icon;
        bool iconTried{false};
    };
    struct FileItem {
        std::wstring name, fold, path;
        FILETIME time{};
        ComPtr<ID2D1Bitmap1> icon;
        bool iconTried{false};
    };
    struct Item { int id; core::Rect r; Kind kind; int ref; };           // layout units (logical px)
    struct Label { std::wstring text; core::Rect r; int style; };        // 0 header, 1 letter, 2 empty text, 3 dim text

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    // data
    void AdoptLoaded();
    void LoadPins();
    void SavePins();
    bool IsPinned(const std::wstring& parse) const;
    void TogglePin(const std::wstring& parse);
    void EnsureAppIcon(App& app);
    void EnsureFileIcon(FileItem& f);
    void LoadAvatar();

    // layout
    void SetView(View v);
    void SetQuery(const std::wstring& q);
    void BuildLayout();
    void BuildHome();
    void BuildApps();
    void BuildSearch();
    [[nodiscard]] float ViewTop() const;
    [[nodiscard]] float ViewBottom() const;
    [[nodiscard]] float MaxScroll() const;
    [[nodiscard]] int HitTest(const core::Vec2& px) const;     // returns an id (see Render) or 0
    [[nodiscard]] const Item* FindItem(int id) const;
    void MoveSelection(int dx, int dy);
    void EnsureVisible(const Item& it);

    // actions
    void Activate(int id, bool rightClick);
    void LaunchItem(const Item& it);
    void ContextMenu(const Item& it);
    void ShowPowerAction(int which);
    void OpenFolder(int knownFolder);
    void PasteClipboard();

    // drawing
    void CreateFonts();
    void Render();
    void DrawText(ID2D1DeviceContext*, const std::wstring&, IDWriteTextFormat*, const D2D1_RECT_F&, const core::Color&);
    void DrawGlyph(ID2D1DeviceContext*, wchar_t, const D2D1_RECT_F&, const core::Color&, IDWriteTextFormat* f = nullptr);
    void Fill(ID2D1DeviceContext*, const core::Rect&, float radius, const core::Color&);
    void Stroke(ID2D1DeviceContext*, const core::Rect&, float radius, const core::Color&, float w);
    [[nodiscard]] core::Rect Px(const core::Rect& logical) const;
    [[nodiscard]] float Hover(int id) const;

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ComPtr<ID2D1SolidColorBrush> m_brush;

    // data
    std::vector<App> m_apps;
    std::vector<std::wstring> m_pins;
    std::vector<int> m_pinnedIdx;               // pinned apps (indices into m_apps), in pin order
    std::vector<FileItem> m_recents;            // newest first
    std::vector<FileItem> m_files;              // searchable index of the user's folders
    std::wstring m_query, m_userName, m_userSub, m_avatarPath;
    ComPtr<ID2D1Bitmap1> m_avatar;
    bool m_avatarTried{false};

    std::atomic<int> m_loadState{0};            // 0 idle, 1 loading, 2 loaded (not yet adopted)
    std::atomic<int> m_fileState{0};
    std::mutex m_loadMutex;
    std::vector<std::pair<std::wstring, std::wstring>> m_loadedRaw;    // name, parsing name
    std::vector<FileItem> m_loadedRecents, m_loadedFiles;
    std::thread m_loader, m_recentLoader, m_fileLoader;
    bool m_adopted{false}, m_recentsAdopted{false}, m_filesAdopted{false};

    // view state
    View m_view{View::Home};
    std::vector<Item> m_items;
    std::vector<Label> m_labels;
    float m_contentH{0.0f};
    int m_selId{-1};
    int m_hoverId{0};
    core::Vec2 m_mouse{-1.0f, -1.0f};
    std::unordered_map<int, float> m_hoverAmt;
    bool m_powerOpen{false};
    bool m_stayOpen{false};                     // pin button: keep the menu open when it loses focus
    bool m_menuOpen{false};
    bool m_wantOpen{false};
    bool m_prevDown{false};
    ULONGLONG m_dismissTick{0};
    float m_time{0.0f};
    animation::Spring<float> m_open;
    animation::Spring<float> m_scroll;          // logical px
    animation::Spring<float> m_viewT;

    // geometry (pixels)
    float m_scale{1.0f};
    int m_winW{0}, m_winH{0};
    float m_pad{0.0f};

    ComPtr<IDWriteTextFormat> m_fName, m_fSub, m_fNav, m_fSearch, m_fHead, m_fLink, m_fTile, m_fRecName, m_fRecSub,
                              m_fLetter, m_fCenter, m_fIcon, m_fIconSm, m_fRowName;
    bool m_hasIconFont{false};
};

} // namespace aether::bar
