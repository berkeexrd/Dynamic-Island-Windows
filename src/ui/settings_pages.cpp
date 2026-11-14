#include "aether/ui/settings_pages.hpp"
#include "aether/ui/components.hpp"
#include "aether/ui/theme.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include "aether/system/metrics_collector.hpp"
#include "aether/system/gaming_mode.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/media/media_service.hpp"
#include "aether/system/autostart.hpp"
#include "aether/system/wallpaper.hpp"
#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <cstdio>

#define L10N(en, tr) (aether::i18n::Turkish() ? (tr) : (en))

namespace aether::ui {

namespace {
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void Save() { config::ConfigManager::Instance().Save(); }
ImU32 U32(const core::Color& c, float a = -1.0f) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a < 0 ? c.a : a)); }

// A small coloured bar with a label and a percentage - used by the live telemetry page.
void MeterRow(const char* label, const char* sub, float pct, const char* valueText, const ImVec4& color) {
    const auto& theme = Theme::Instance().Colors();
    UI::SettingRow(label, sub, [&](float w) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bw = w - Sc(70.0f);
        dl->AddRectFilled(ImVec2(p.x, p.y + Sc(11.0f)), ImVec2(p.x + bw, p.y + Sc(19.0f)), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)), Sc(4.0f));
        dl->AddRectFilled(ImVec2(p.x, p.y + Sc(11.0f)), ImVec2(p.x + std::max(Sc(8.0f), bw * std::clamp(pct / 100.0f, 0.0f, 1.0f)), p.y + Sc(19.0f)), ImGui::GetColorU32(color), Sc(4.0f));
        dl->AddText(ImVec2(p.x + bw + Sc(10.0f), p.y + Sc(5.0f)), ImGui::GetColorU32(theme.TextPrimary), valueText);
        ImGui::Dummy(ImVec2(w, Sc(30.0f)));
    });
}
} // namespace

void SettingsPages::Render(SettingsTab activeTab) {
    switch (activeTab) {
        case SettingsTab::General:          RenderGeneral(); break;
        case SettingsTab::DynamicIsland:    RenderDynamicIsland(); break;
        case SettingsTab::Widgets:          RenderWidgets(); break;
        case SettingsTab::Taskbar:          RenderTaskbar(); break;
        case SettingsTab::Desktop:          RenderDesktop(); break;
        case SettingsTab::Appearance:       RenderAppearance(); break;
        case SettingsTab::Animations:       RenderAnimations(); break;
        case SettingsTab::Media:            RenderMedia(); break;
        case SettingsTab::SystemMonitoring: RenderSystemMonitoring(); break;
        case SettingsTab::GamingMode:       RenderGamingMode(); break;
        case SettingsTab::Startup:          RenderStartup(); break;
        case SettingsTab::Performance:      RenderPerformance(); break;
        case SettingsTab::About:            RenderAbout(); break;
    }
}

bool SettingsPages::WelcomeNeeded() { return !config::ConfigManager::Instance().Get().firstRunDone; }

void SettingsPages::RenderWelcome(float W, float H) {
    auto& root = config::ConfigManager::Instance().GetMutable();
    const auto& theme = Theme::Instance().Colors();
    static bool s_init = false, s_auto = true, s_wall = true;
    if (!s_init) { s_init = true; s_auto = true; s_wall = root.autoWallpaper; }

    const float cw = std::min(Sc(680.0f), W - Sc(48.0f)), ch = std::min(Sc(640.0f), H - Sc(48.0f));
    ImGui::SetCursorPos(ImVec2((W - cw) * 0.5f, std::max(Sc(16.0f), (H - ch) * 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Background);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderSubtle);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Sc(24.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(34.0f), Sc(26.0f)));
    ImGui::BeginChild("Welcome", ImVec2(cw, ch), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        if (FontTitle) ImGui::PushFont(FontTitle);
        ImGui::TextColored(theme.TextPrimary, "%s", L10N("Welcome to Aether", "Aether'e hoş geldin"));
        if (FontTitle) ImGui::PopFont();
        ImGui::TextColored(theme.TextMuted, "%s", L10N("A few quick choices to make it yours. You can change all of them later.", "Sana göre ayarlamak için birkaç hızlı seçim. Hepsini sonra değiştirebilirsin."));
        ImGui::Dummy(ImVec2(0, Sc(14.0f)));

        const float inner = ImGui::GetContentRegionAvail().x;
        auto label = [&](const char* s) { ImGui::TextColored(theme.TextSecondary, "%s", s); ImGui::Dummy(ImVec2(0, Sc(4.0f))); };

        // language
        label(L10N("Language", "Dil"));
        {
            const char* names[3] = {L10N("Follow Windows", "Windows'u izle"), "English", "T\xC3\xBCrk\xC3\xA7" "e"};
            const float gap = Sc(10.0f), bw = (inner - gap * 2.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine(0.0f, gap);
                ImGui::PushID(i);
                if (UI::Button(names[i], ImVec2(bw, Sc(38.0f)), root.language == i)) { root.language = i; aether::i18n::SaveLanguage(i); Save(); }
                ImGui::PopID();
            }
        }
        ImGui::Dummy(ImVec2(0, Sc(16.0f)));

        // theme
        label(L10N("Theme", "Tema"));
        {
            int count = 0;
            const render::glass::ThemeDef* themes = render::glass::Themes(&count);
            const int pick[4] = {6, 0, 7, 1};   // Classic (default), Violet, Lavender, OLED Black
            const float gap = Sc(10.0f), tw = (inner - gap * 3.0f) / 4.0f, th = Sc(92.0f);
            const ImVec2 o = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int k = 0; k < 4; ++k) {
                const int i = pick[k];
                if (i >= count) continue;
                const auto& t = themes[i];
                const ImVec2 p0(o.x + k * (tw + gap), o.y), p1(p0.x + tw, p0.y + th);
                ImGui::SetCursorScreenPos(p0);
                char id[16]; snprintf(id, sizeof(id), "##wt%d", i);
                const bool clicked = ImGui::InvisibleButton(id, ImVec2(tw, th));
                const bool hov = ImGui::IsItemHovered();
                dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.03f, 0.03f, 0.04f, 1.0f)), Sc(12.0f));
                const ImVec2 b0(p0.x + Sc(10.0f), p0.y + Sc(10.0f)), b1(p1.x - Sc(10.0f), p1.y - Sc(30.0f));
                dl->AddRectFilledMultiColor(b0, b1, U32(t.bodyTop, 1.0f), U32(t.bodyTop, 1.0f), U32(t.bodyBot, 1.0f), U32(t.bodyBot, 1.0f));
                dl->AddRect(b0, b1, U32(t.light, 0.55f), Sc(8.0f), 0, 1.2f);
                dl->AddRectFilled(ImVec2(b0.x + 8, b0.y + 9), ImVec2(b0.x + 30, b0.y + 14), U32(t.light, 0.9f), Sc(3.0f));
                dl->AddRectFilled(ImVec2(b0.x + 8, b0.y + 22), ImVec2(b0.x + (b1.x - b0.x) * 0.65f, b0.y + 27), U32(t.accent, 1.0f), Sc(3.0f));
                dl->AddText(ImVec2(p0.x + Sc(10.0f), p1.y - Sc(24.0f)), ImGui::GetColorU32(theme.TextPrimary), WideToUtf8(L10N(t.nameEn, t.nameTr)).c_str());
                if (render::glass::ThemeIndex() == i) dl->AddRect(p0, p1, U32(t.accent, 1.0f), Sc(12.0f), 0, Sc(2.0f));
                else if (hov) dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.30f)), Sc(12.0f), 0, 1.5f);
                if (clicked) { Theme::Instance().UseGlassTheme(i); render::glass::SaveThemeFile(i); }
            }
            ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + th));
            ImGui::Dummy(ImVec2(1.0f, Sc(14.0f)));
        }

        UI::SettingRow(L10N("Launch with Windows", "Windows ile başlat"), L10N("Aether and the bar start when you sign in (one admin prompt)", "Aether ve bar oturum açınca başlar (bir kez yönetici izni ister)"), [&](float) { UI::Toggle("##wauto", &s_auto); });
        UI::SettingRow(L10N("Match wallpaper to theme", "Duvar kağıdını temaya uydur"), L10N("Purple artwork for purple themes, black for dark ones", "Mor temalara mor, koyu temalara siyah duvar kağıdı"), [&](float) { UI::Toggle("##wwall", &s_wall); });

        ImGui::Dummy(ImVec2(0, Sc(14.0f)));
        if (UI::Button(L10N("Get started", "Başla"), ImVec2(inner, Sc(46.0f)), true)) {
            root.launchOnStartup = s_auto;
            root.autoWallpaper = s_wall;
            root.firstRunDone = true;
            if (s_auto) aether::system::autostart::Apply(true);
            if (s_wall) aether::system::wallpaper::ApplyForTheme(render::glass::ThemeIndex());
            Save();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void SettingsPages::RenderGeneral() {
    auto& cfg = config::ConfigManager::Instance().GetMutable();

    UI::Section(L10N("Application", "Uygulama"), L10N("Startup, tray and display preferences", "Başlangıç, tepsi ve ekran tercihleri"), [&]() {
        UI::SettingRow(L10N("Launch with Windows", "Windows ile başlat"), L10N("Start Aether automatically when you sign in", "Oturum açınca Aether otomatik başlasın"), [&](float) {
            if (UI::Toggle("##launchOnStartup", &cfg.launchOnStartup)) {
                Save();
                aether::system::autostart::Apply(cfg.launchOnStartup);
                UI::AddToast(UI::ToastType::Success, L10N("Startup updated", "Başlangıç güncellendi"), L10N("The startup setting was saved.", "Başlangıç ayarı kaydedildi."));
            }
        });
        UI::SettingRow(L10N("Keep running in the tray", "Tepside çalışmaya devam et"), L10N("Closing this window does not quit Aether", "Bu pencereyi kapatmak Aether'i kapatmaz"), [&](float) {
            if (UI::Toggle("##minimizeToTray", &cfg.minimizeToTray)) Save();
        });
        UI::SettingRow(L10N("Language", "Dil"), L10N("Language of Aether's own interface", "Aether arayüzünün dili"), [&](float w) {
            const char* langs[] = {L10N("Follow Windows", "Windows'u izle"), "English", "T\xC3\xBCrk\xC3\xA7" "e"};
            if (UI::Combo("##language", &cfg.language, langs, 3, w)) { aether::i18n::SaveLanguage(cfg.language); Save(); }
        });
        UI::SettingRow(L10N("Target display", "Hedef ekran"), L10N("Display that holds the island and the widgets", "Island ve widget'ların bulunduğu ekran"), [&](float w) {
            const char* monitors[] = {L10N("Primary display", "Ana ekran"), L10N("Display 2", "Ekran 2"), L10N("Display 3", "Ekran 3")};
            if (UI::Combo("##monitorIndex", &cfg.monitorIndex, monitors, 3, w)) Save();
        });
        UI::SettingRow(L10N("Interface scale", "Arayüz ölçeği"), L10N("Scales every Aether surface", "Tüm Aether yüzeylerini ölçekler"), [&](float) {
            if (UI::SliderFloat("##uiScale", &cfg.uiScale, 0.8f, 2.0f, "%.2fx")) Save();
        });
    });

    UI::Section(L10N("Shortcuts", "Kısayollar"), L10N("Global hotkeys", "Genel kısayol tuşları"), [&]() {
        static int s_toggleKey = ImGuiKey_F9;
        static bool s_capturing = false;
        UI::SettingRow(L10N("Toggle the island", "Island'ı aç / kapat"), L10N("Click, then press the key you want", "Tıkla, ardından istediğin tuşa bas"), [&](float) {
            UI::Keybind("toggle_island_hotkey", &s_toggleKey, &s_capturing);
        });
    });
}

void SettingsPages::RenderDynamicIsland() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().island;

    UI::Section(L10N("Island", "Island"), L10N("Position and size of the floating pill", "Yüzen hapın konumu ve boyutu"), [&]() {
        UI::SettingRow(L10N("Show the island", "Island'ı göster"), L10N("The floating pill at the top of the screen", "Ekranın üstündeki yüzen hap"), [&](float) { if (UI::Toggle("##islandEnabled", &cfg.enabled)) Save(); });
        UI::SettingRow(L10N("Top margin", "Üst boşluk"), L10N("Distance from the top edge of the display", "Ekranın üst kenarına uzaklık"), [&](float) { if (UI::SliderFloat("##topOffset", &cfg.topOffset, 4.0f, 60.0f, "%.0f px")) Save(); });
        UI::SettingRow(L10N("Resting width", "Dinlenme genişliği"), L10N("Width of the compact pill", "Kompakt hapın genişliği"), [&](float) { if (UI::SliderFloat("##idleWidth", &cfg.idleWidth, 100.0f, 260.0f, "%.0f px")) Save(); });
        UI::SettingRow(L10N("Resting height", "Dinlenme yüksekliği"), L10N("Height of the compact pill", "Kompakt hapın yüksekliği"), [&](float) { if (UI::SliderFloat("##idleHeight", &cfg.idleHeight, 28.0f, 48.0f, "%.0f px")) Save(); });
    });

    UI::Section(L10N("Behaviour", "Davranış"), L10N("How the island opens and what it shows", "Island nasıl açılır ve neyi gösterir"), [&]() {
        UI::SettingRow(L10N("Expand on hover", "Üzerine gelince genişlet"), L10N("Show a preview when the pointer rests on it", "İmleç üzerinde durunca önizleme göster"), [&](float) { if (UI::Toggle("##expandOnHover", &cfg.expandOnHover)) Save(); });
        UI::SettingRow(L10N("Hover delay", "Bekleme süresi"), L10N("Delay before the preview opens", "Önizlemenin açılmasından önceki gecikme"), [&](float) { if (UI::SliderFloat("##hoverDelayMs", &cfg.hoverDelayMs, 0.0f, 400.0f, "%.0f ms")) Save(); });
        UI::SettingRow(L10N("Pin on click", "Tıklayınca sabitle"), L10N("Stay open until you click again", "Tekrar tıklayana kadar açık kalsın"), [&](float) { if (UI::Toggle("##pinOnClick", &cfg.pinOnClick)) Save(); });
        UI::SettingRow(L10N("Clock in the pill", "Hapta saat"), L10N("Show the time in the resting pill", "Dinlenme halinde saati göster"), [&](float) { if (UI::Toggle("##showClockIdle", &cfg.showClockIdle)) Save(); });
        UI::SettingRow(L10N("Audio visualizer", "Ses görselleştirici"), L10N("Animated bars while music plays", "Müzik çalarken hareketli çubuklar"), [&](float) { if (UI::Toggle("##showAudioVisualizer", &cfg.showAudioVisualizer)) Save(); });
        UI::SettingRow(L10N("Hide from screenshots", "Ekran görüntülerinde gizle"), L10N("The island will not appear in captures or recordings", "Island ekran görüntülerinde ve kayıtlarda görünmez"), [&](float) { if (UI::Toggle("##hideFromScreenshots", &cfg.hideFromScreenshots)) Save(); });
    });
}

void SettingsPages::RenderWidgets() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().widgets;

    UI::Section(L10N("Desktop widgets", "Masaüstü widget'ları"), L10N("Cards that live on your desktop", "Masaüstünde duran kartlar"), [&]() {
        UI::SettingRow(L10N("Show widgets", "Widget'ları göster"), L10N("Turns every desktop widget on or off", "Tüm masaüstü widget'larını açar veya kapatır"), [&](float) { if (UI::Toggle("##widgetsEnabled", &cfg.enabled)) Save(); });
        UI::SettingRow(L10N("Snap to grid", "Izgaraya yasla"), L10N("Align widgets while dragging", "Sürüklerken widget'ları hizala"), [&](float) { if (UI::Toggle("##snapToGrid", &cfg.snapToGrid)) Save(); });
        UI::SettingRow(L10N("Grid size", "Izgara boyutu"), L10N("Spacing of the snap grid", "Hizalama ızgarasının aralığı"), [&](float) { if (UI::SliderFloat("##gridSize", &cfg.gridSize, 8.0f, 32.0f, "%.0f px")) Save(); });
    });

    UI::Section(L10N("Cards", "Kartlar"), L10N("Choose which widgets are shown", "Hangi widget'ların görüneceğini seç"), [&]() {
        UI::SettingRow(L10N("System performance", "Sistem performansı"), L10N("CPU, RAM and GPU rings", "CPU, RAM ve GPU halkaları"), [&](float) { if (UI::Toggle("##sysMonEnabled", &cfg.sysMonWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Network activity", "Ağ etkinliği"), L10N("Live download and upload speed", "Anlık indirme ve yükleme hızı"), [&](float) { if (UI::Toggle("##netEnabled", &cfg.netWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Storage", "Depolama"), L10N("Used space of every drive", "Tüm sürücülerin kullanımı"), [&](float) { if (UI::Toggle("##storageEnabled", &cfg.storageWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Weather", "Hava durumu"), L10N("Current weather and the next hours", "Anlık hava ve sonraki saatler"), [&](float) { if (UI::Toggle("##weatherEnabled", &cfg.weatherWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Now playing", "Çalan müzik"), L10N("Track, artist and playback controls", "Parça, sanatçı ve oynatma düğmeleri"), [&](float) { if (UI::Toggle("##nowEnabled", &cfg.nowWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Clock and date", "Saat ve tarih"), L10N("Large clock with the long date", "Büyük saat ve uzun tarih"), [&](float) { if (UI::Toggle("##clockEnabled", &cfg.clockWidget.enabled)) Save(); });
    });
}

void SettingsPages::RenderTaskbar() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().taskbar;

    UI::Section(L10N("Windows taskbar", "Windows görev çubuğu"), L10N("Material applied to the stock taskbar", "Standart görev çubuğuna uygulanan malzeme"), [&]() {
        UI::SettingRow(L10N("Customise the taskbar", "Görev çubuğunu özelleştir"), L10N("Apply a modern material to the Windows taskbar", "Windows görev çubuğuna modern bir malzeme uygula"), [&](float) {
            if (UI::Toggle("##taskbarEnabled", &cfg.enabled)) {
                if (cfg.enabled) system::TaskbarService::Instance().ApplyStyle(cfg.style, cfg.tintColor);
                else system::TaskbarService::Instance().RestoreDefaults();
                Save();
            }
        });
        UI::SettingRow(L10N("Material", "Malzeme"), L10N("Composition effect on the taskbar", "Görev çubuğundaki kompozisyon efekti"), [&](float w) {
            const char* styles[] = {L10N("Default", "Varsayılan"), L10N("Clear", "Şeffaf"), L10N("Blur", "Bulanık"), L10N("Acrylic", "Akrilik"), L10N("Tinted", "Renkli")};
            int cur = static_cast<int>(cfg.style);
            if (UI::Combo("##tbStyle", &cur, styles, 5, w)) {
                cfg.style = static_cast<config::TaskbarStyle>(cur);
                system::TaskbarService::Instance().ApplyStyle(cfg.style, cfg.tintColor);
                Save();
            }
        });
        UI::SettingRow(L10N("Tint colour", "Renk tonu"), L10N("Used by the tinted and acrylic materials", "Renkli ve akrilik malzemelerde kullanılır"), [&](float) {
            if (UI::ColorPicker("##tbTintColor", &cfg.tintColor)) {
                system::TaskbarService::Instance().ApplyStyle(cfg.style, cfg.tintColor);
                Save();
            }
        });
    });
}

void SettingsPages::RenderDesktop() { RenderWidgets(); }

void SettingsPages::RenderAppearance() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().appearance;
    const auto& theme = Theme::Instance().Colors();

    UI::Section(L10N("Theme", "Tema"), L10N("Applies to the island, the bar, the Start menu, the panels, the widgets and this window", "Island, bar, Başlat menüsü, paneller, widget'lar ve bu pencere için geçerli"), [&]() {
        int count = 0;
        const render::glass::ThemeDef* themes = render::glass::Themes(&count);
        const int current = render::glass::ThemeIndex();
        const float availW = ImGui::GetContentRegionAvail().x;
        const float gap = Sc(14.0f), cardW = std::floor((availW - gap * 3.0f) / 4.0f), cardH = Sc(104.0f);
        ImGui::Dummy(ImVec2(0.0f, Sc(12.0f)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int i = 0; i < count; ++i) {
            const int col = i % 4, row = i / 4;
            const ImVec2 p0(origin.x + col * (cardW + gap), origin.y + row * (cardH + gap));
            const ImVec2 p1(p0.x + cardW, p0.y + cardH);
            ImGui::SetCursorScreenPos(p0);
            char id[24]; snprintf(id, sizeof(id), "##theme%d", i);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(cardW, cardH));
            const bool hovered = ImGui::IsItemHovered();
            const auto& t = themes[i];
            const bool sel = i == current;
            // preview: the panel body with its glow, a pill and a slider in the accent colour
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.03f, 0.03f, 0.04f, 1.0f)), Sc(12.0f));
            const ImVec2 b0(p0.x + Sc(12.0f), p0.y + Sc(12.0f)), b1(p1.x - Sc(12.0f), p1.y - Sc(34.0f));
            dl->AddRectFilled(ImVec2(b0.x - 3, b0.y - 3), ImVec2(b1.x + 3, b1.y + 3), U32(t.accent, 0.22f * t.glow), Sc(12.0f));
            dl->AddRectFilledMultiColor(b0, b1, U32(t.bodyTop, 1.0f), U32(t.bodyTop, 1.0f), U32(t.bodyBot, 1.0f), U32(t.bodyBot, 1.0f));
            dl->AddRect(b0, b1, U32(t.light, 0.55f), Sc(9.0f), 0, 1.2f);
            dl->AddRectFilled(ImVec2(b0.x + 10, b0.y + 12), ImVec2(b0.x + 36, b0.y + 18), U32(t.light, 0.9f), Sc(3.0f));
            dl->AddRectFilled(ImVec2(b0.x + 10, b0.y + 26), ImVec2(b1.x - 10, b0.y + 31), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.14f)), Sc(3.0f));
            dl->AddRectFilled(ImVec2(b0.x + 10, b0.y + 26), ImVec2(b0.x + (b1.x - b0.x) * 0.6f, b0.y + 31), U32(t.accent, 1.0f), Sc(3.0f));
            dl->AddText(ImVec2(p0.x + Sc(12.0f), p1.y - Sc(26.0f)), ImGui::GetColorU32(theme.TextPrimary), WideToUtf8(L10N(t.nameEn, t.nameTr)).c_str());
            if (sel) {
                dl->AddRect(p0, p1, U32(t.accent, 1.0f), Sc(12.0f), 0, Sc(2.0f));
                dl->AddCircleFilled(ImVec2(p1.x - Sc(18.0f), p1.y - Sc(17.0f)), Sc(7.0f), U32(t.accent, 1.0f));
                dl->AddLine(ImVec2(p1.x - Sc(21.5f), p1.y - Sc(17.0f)), ImVec2(p1.x - Sc(18.5f), p1.y - Sc(14.0f)), IM_COL32(255, 255, 255, 255), 1.8f);
                dl->AddLine(ImVec2(p1.x - Sc(18.5f), p1.y - Sc(14.0f)), ImVec2(p1.x - Sc(14.0f), p1.y - Sc(20.0f)), IM_COL32(255, 255, 255, 255), 1.8f);
            } else if (hovered) {
                dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.30f)), Sc(12.0f), 0, 1.5f);
            }
            if (clicked) {
                Theme::Instance().UseGlassTheme(i);
                render::glass::SaveThemeFile(i);
                cfg.preset = static_cast<config::ThemePreset>(std::min(i, 4));
                if (config::ConfigManager::Instance().Get().autoWallpaper) aether::system::wallpaper::ApplyForTheme(i);
                Save();
                UI::AddToast(UI::ToastType::Success, L10N("Theme applied", "Tema uygulandı"), WideToUtf8(L10N(t.nameEn, t.nameTr)));
            }
        }
        const int rows = (count + 3) / 4;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (cardH + gap)));
        ImGui::Dummy(ImVec2(1.0f, Sc(6.0f)));
    });

    UI::Section(L10N("Wallpaper", "Duvar kağıdı"), L10N("Purple themes get the purple artwork, dark themes the black one", "Mor temalar mor, koyu temalar siyah duvar kağıdı alır"), [&]() {
        auto& root = config::ConfigManager::Instance().GetMutable();
        UI::SettingRow(L10N("Match wallpaper to theme", "Duvar kağıdını temaya uydur"), L10N("Changes the desktop wallpaper whenever you pick a theme", "Tema seçtiğinde masaüstü duvar kağıdını değiştirir"), [&](float) {
            if (UI::Toggle("##autoWallpaper", &root.autoWallpaper)) {
                Save();
                if (root.autoWallpaper) aether::system::wallpaper::ApplyForTheme(render::glass::ThemeIndex());
            }
        });
        UI::SettingRow(L10N("My original wallpaper", "Kendi duvar kağıdım"), L10N("Bring back the wallpaper you had before Aether", "Aether'den önceki duvar kağıdını geri getir"), [&](float) {
            if (UI::Button(L10N("Restore", "Geri yükle"), ImVec2(Sc(110.0f), Sc(32.0f)), false)) aether::system::wallpaper::RestoreOriginal();
        });
    });

    UI::Section(L10N("Surfaces", "Yüzeyler"), L10N("Glass strength and depth", "Cam yoğunluğu ve derinlik"), [&]() {
        UI::SettingRow(L10N("Glow", "Parlama"), L10N("Coloured halo around panels", "Panellerin etrafındaki renkli ışıma"), [&](float) { if (UI::Toggle("##enableGlow", &cfg.enableGlow)) Save(); });
        UI::SettingRow(L10N("Shadows", "Gölgeler"), L10N("Soft depth under surfaces", "Yüzeylerin altında yumuşak derinlik"), [&](float) { if (UI::Toggle("##enableShadow", &cfg.enableShadow)) Save(); });
        UI::SettingRow(L10N("Corner radius", "Köşe yuvarlaklığı"), L10N("Roundness of cards and panels", "Kart ve panellerin yuvarlaklığı"), [&](float) { if (UI::SliderFloat("##cornerRadius", &cfg.cornerRadius, 4.0f, 24.0f, "%.0f px")) Save(); });
    });

    RenderAnimations();
}

void SettingsPages::RenderAnimations() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().animation;

    UI::Section(L10N("Motion", "Hareket"), L10N("Spring physics used by the island and the panels", "Island ve panellerin kullandığı yay fiziği"), [&]() {
        UI::SettingRow(L10N("Stiffness", "Sertlik"), L10N("How quickly shapes snap into place", "Şekillerin yerine oturma hızı"), [&](float) { if (UI::SliderFloat("##springK", &cfg.springStiffness, 100.0f, 600.0f, "%.0f")) Save(); });
        UI::SettingRow(L10N("Damping", "Sönümleme"), L10N("Higher values remove the bounce", "Yüksek değerler zıplamayı azaltır"), [&](float) { if (UI::SliderFloat("##springDamp", &cfg.springDamping, 10.0f, 60.0f, "%.0f")) Save(); });
        UI::SettingRow(L10N("Animation speed", "Animasyon hızı"), L10N("Global speed of every transition", "Tüm geçişlerin genel hızı"), [&](float) { if (UI::SliderFloat("##timeScale", &cfg.timeScale, 0.5f, 2.0f, "%.2fx")) Save(); });
        UI::SettingRow(L10N("Reduce motion", "Hareketi azalt"), L10N("Use instant state changes instead of springs", "Yaylar yerine anlık geçişler kullan"), [&](float) { if (UI::Toggle("##reducedMotion", &cfg.reducedMotion)) Save(); });
    });
}

void SettingsPages::RenderMedia() {
    auto track = media::MediaService::Instance().GetTrackLite();
    const auto& theme = Theme::Instance().Colors();

    UI::Section(L10N("Now playing", "Şu an çalan"), L10N("Read from the Windows media session", "Windows medya oturumundan okunur"), [&]() {
        UI::SettingRow(L10N("Session", "Oturum"), L10N("Active player reported by Windows", "Windows'un bildirdiği etkin oynatıcı"), [&](float) {
            if (track.hasMedia) UI::StatusBadge(L10N("ACTIVE", "AKTİF"), theme.Success);
            else UI::StatusBadge(L10N("NO SOURCE", "KAYNAK YOK"), theme.TextMuted);
        });
        UI::SettingRow(L10N("Track", "Parça"), L10N("Title and artist", "Başlık ve sanatçı"), [&](float) {
            const std::string t = WideToUtf8(track.title);
            ImGui::TextColored(theme.TextPrimary, "%s", t.c_str());
        });
        UI::SettingRow(L10N("Controls", "Kontroller"), L10N("Transport commands", "Oynatma komutları"), [&](float) {
            if (UI::Button("<<", ImVec2(Sc(44.0f), Sc(Sizing::Standard)))) media::MediaService::Instance().PreviousTrack();
            ImGui::SameLine();
            if (UI::Button(L10N("Play / Pause", "Oynat / Duraklat"), ImVec2(Sc(110.0f), Sc(Sizing::Standard)), true)) media::MediaService::Instance().PlayPause();
            ImGui::SameLine();
            if (UI::Button(">>", ImVec2(Sc(44.0f), Sc(Sizing::Standard)))) media::MediaService::Instance().NextTrack();
        });
    });
}

void SettingsPages::RenderSystemMonitoring() {
    auto m = system::MetricsCollector::Instance().GetMetrics();
    const auto& theme = Theme::Instance().Colors();
    const double gb = 1024.0 * 1024.0 * 1024.0;

    UI::Section(L10N("Live hardware", "Canlı donanım"), L10N("Measured natively, no extra drivers", "Yerel olarak ölçülür, ek sürücü gerekmez"), [&]() {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f%%", m.cpuUsagePercent);
        MeterRow(L10N("Processor", "İşlemci"), L10N("Total CPU load", "Toplam CPU yükü"), m.cpuUsagePercent, buf, theme.Accent);
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB", m.ramUsedBytes / gb, m.ramTotalBytes / gb);
        MeterRow(L10N("Memory", "Bellek"), L10N("Used / total physical memory", "Kullanılan / toplam fiziksel bellek"), m.ramUsagePercent, buf, theme.Success);
        snprintf(buf, sizeof(buf), "%.0f%%", m.gpuUsagePercent);
        MeterRow(L10N("Graphics", "Ekran kartı"), L10N("3D engine load", "3D motor yükü"), m.gpuUsagePercent, buf, theme.Info);
        UI::SettingRow(L10N("Network", "Ağ"), L10N("Download and upload", "İndirme ve yükleme"), [&](float) {
            ImGui::TextColored(theme.TextPrimary, "%.1f KB/s  /  %.1f KB/s", m.netDownloadBytesPerSec / 1024.0, m.netUploadBytesPerSec / 1024.0);
        });
    });
}

void SettingsPages::RenderGamingMode() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().gaming;
    const bool isGaming = system::GamingModeDetector::Instance().IsGaming();
    const auto& theme = Theme::Instance().Colors();

    UI::Section(L10N("Gaming mode", "Oyun modu"), L10N("Stays out of the way of full-screen games", "Tam ekran oyunların yoluna çıkmaz"), [&]() {
        UI::SettingRow(L10N("Detect full-screen apps", "Tam ekran uygulamaları algıla"), L10N("Automatic full-screen and game detection", "Otomatik tam ekran ve oyun algılama"), [&](float) { if (UI::Toggle("##gamingEnabled", &cfg.enabled)) Save(); });
        UI::SettingRow(L10N("Current state", "Şu anki durum"), L10N("What the detector sees right now", "Algılayıcının şu an gördüğü"), [&](float) {
            if (isGaming) UI::StatusBadge(L10N("GAMING MODE", "OYUN MODU"), theme.Warning);
            else UI::StatusBadge(L10N("DESKTOP", "MASAÜSTÜ"), theme.Info);
        });
        UI::SettingRow(L10N("When a game starts", "Oyun başlayınca"), L10N("What Aether does in full-screen", "Tam ekranda Aether ne yapsın"), [&](float w) {
            const char* actions[] = {L10N("Hide everything", "Hepsini gizle"), L10N("Compact pill only", "Sadece kompakt hap"), L10N("Pause animations", "Animasyonları durdur")};
            int cur = static_cast<int>(cfg.action);
            if (UI::Combo("##gameAction", &cur, actions, 3, w)) { cfg.action = static_cast<config::GamingModeAction>(cur); Save(); }
        });
        UI::SettingRow(L10N("Pause widgets", "Widget'ları duraklat"), L10N("Stop widget updates while gaming", "Oyun sırasında widget güncellemelerini durdur"), [&](float) { if (UI::Toggle("##pauseWidgets", &cfg.pauseWidgets)) Save(); });
        UI::SettingRow(L10N("Mute notifications", "Bildirimleri sustur"), L10N("No island popups while gaming", "Oyun sırasında island bildirimi çıkmaz"), [&](float) { if (UI::Toggle("##suppressNotifs", &cfg.suppressNotifications)) Save(); });
    });
}

void SettingsPages::RenderStartup() { RenderGeneral(); }

void SettingsPages::RenderPerformance() {
    const auto& theme = Theme::Instance().Colors();
    const float fps = ImGui::GetIO().Framerate;
    UI::Section(L10N("Diagnostics", "Tanılama"), L10N("Rendering pipeline status", "Çizim hattı durumu"), [&]() {
        UI::SettingRow(L10N("Settings frame rate", "Ayarlar kare hızı"), L10N("Refresh rate of this window", "Bu pencerenin yenileme hızı"), [&](float) { ImGui::TextColored(theme.Success, "%.0f FPS", fps); });
        UI::SettingRow(L10N("Compositor", "Kompozitör"), L10N("Hardware composition", "Donanım kompozisyonu"), [&](float) { UI::StatusBadge("DIRECTCOMPOSITION", theme.Info); });
    });
}

void SettingsPages::RenderAbout() {
    const auto& theme = Theme::Instance().Colors();

    UI::Section("Aether", L10N("Dynamic Island, taskbar and desktop customisation for Windows", "Windows için Dynamic Island, görev çubuğu ve masaüstü özelleştirme"), [&]() {
        UI::SettingRow(L10N("Version", "Sürüm"), L10N("Native C++20, Direct2D and DirectComposition", "Yerel C++20, Direct2D ve DirectComposition"), [&](float) { ImGui::TextColored(theme.Accent, "Aether 1.0"); });
        UI::SettingRow(L10N("Reset settings", "Ayarları sıfırla"), L10N("Restore every option to its default", "Tüm seçenekleri varsayılana döndür"), [&](float) {
            if (UI::Button(L10N("Reset", "Sıfırla"), ImVec2(Sc(120.0f), Sc(Sizing::Standard)))) {
                config::ConfigManager::Instance().ResetToDefaults();
                Save();
                UI::AddToast(UI::ToastType::Info, L10N("Settings reset", "Ayarlar sıfırlandı"), L10N("Defaults were restored.", "Varsayılanlar geri yüklendi."));
            }
        });
    });
}

} // namespace aether::ui
