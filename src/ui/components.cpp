#include "aether/ui/components.hpp"
#include "aether/ui/animation.hpp"
#include <imgui_internal.h>
#include <vector>
#include <algorithm>

namespace aether::ui {

struct ToastItem {
    UI::ToastType type;
    std::string title;
    std::string message;
    float durationRemaining{4.0f};
    float totalDuration{4.0f};
};

static std::vector<ToastItem> s_toasts;

namespace UI {

bool Button(const char* label, const ImVec2& size, bool primary) {
    const auto& theme = Theme::Instance().Colors();

    ImVec4 baseBg = primary ? theme.Accent : theme.SurfaceElevated;
    ImVec4 hoverBg = primary ? theme.AccentHover : theme.SurfaceHover;
    ImVec4 activeBg = primary ? theme.AccentActive : theme.SurfaceActive;

    ImGui::PushStyleColor(ImGuiCol_Button, baseBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeBg);
    ImGui::PushStyleColor(ImGuiCol_Text, primary ? ImVec4(1, 1, 1, 1) : theme.TextPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::SM);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, primary ? theme.AccentGlow : theme.Border);

    bool clicked = ImGui::Button(label, size);

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    return clicked;
}

bool Toggle(const char* id, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID widgetId = window->GetID(id);

    const float width = Sc(42.0f);
    const float height = Sc(22.0f);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));

    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, widgetId)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, widgetId, &hovered, &held);
    if (pressed) {
        *value = !(*value);
        ImGui::MarkItemEdited(widgetId);
    }

    // Micro-animation for toggle knob (0.0 = OFF, 1.0 = ON)
    float targetAnim = *value ? 1.0f : 0.0f;
    float animT = AnimationEngine::Instance().AnimateFloat(widgetId, targetAnim, AnimationSpeed::Fast);

    const auto& theme = Theme::Instance().Colors();
    ImVec4 offBg = hovered ? theme.SurfaceHover : theme.SurfaceElevated;
    ImVec4 onBg = hovered ? theme.AccentHover : theme.Accent;
    ImVec4 currentBg = ImVec4(
        offBg.x + (onBg.x - offBg.x) * animT,
        offBg.y + (onBg.y - offBg.y) * animT,
        offBg.z + (onBg.z - offBg.z) * animT,
        1.0f
    );

    // Track
    window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(currentBg), height * 0.5f);
    window->DrawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(hovered ? theme.BorderHover : theme.Border), height * 0.5f, 0, 1.0f);

    // Knob
    const float knobRadius = (height - Sc(6.0f)) * 0.5f;
    const float knobMinX = bb.Min.x + Sc(3.0f) + knobRadius;
    const float knobMaxX = bb.Max.x - Sc(3.0f) - knobRadius;
    const float knobX = knobMinX + (knobMaxX - knobMinX) * animT;
    const float knobY = bb.Min.y + height * 0.5f;

    window->DrawList->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, ImColor(255, 255, 255, 255));

    return pressed;
}

bool Checkbox(const char* label, bool* value) {
    const auto& theme = Theme::Instance().Colors();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.SurfaceElevated);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme.SurfaceHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme.SurfaceActive);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.Border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::XS);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool changed = ImGui::Checkbox(label, value);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return changed;
}

bool SliderFloat(const char* id, float* value, float minVal, float maxVal, const char* format) {
    const auto& theme = Theme::Instance().Colors();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.SurfaceElevated);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme.SurfaceHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme.SurfaceActive);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, theme.AccentHover);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.Border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::SM);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, Radius::XS);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool changed = ImGui::SliderFloat(id, value, minVal, maxVal, format);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
    return changed;
}

bool SliderInt(const char* id, int* value, int minVal, int maxVal) {
    const auto& theme = Theme::Instance().Colors();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.SurfaceElevated);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme.SurfaceHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme.SurfaceActive);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, theme.AccentHover);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.Border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::SM);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, Radius::XS);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool changed = ImGui::SliderInt(id, value, minVal, maxVal);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
    return changed;
}

bool Combo(const char* id, int* currentItem, const char* const items[], int itemsCount, float width) {
    const auto& theme = Theme::Instance().Colors();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.SurfaceElevated);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme.SurfaceHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme.SurfaceActive);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.Border);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.SurfaceOverlay);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::SM);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Radius::MD);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool changed = ImGui::Combo(id, currentItem, items, itemsCount);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return changed;
}

bool ColorPicker(const char* id, uint32_t* colorHexRgba) {
    float r = static_cast<float>((*colorHexRgba >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((*colorHexRgba >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(*colorHexRgba & 0xFF) / 255.0f;
    float col[3] = {r, g, b};

    const auto& theme = Theme::Instance().Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::SM);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.Border);

    bool changed = ImGui::ColorEdit3(id, col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);

    if (changed) {
        uint8_t ir = static_cast<uint8_t>(col[0] * 255.0f + 0.5f);
        uint8_t ig = static_cast<uint8_t>(col[1] * 255.0f + 0.5f);
        uint8_t ib = static_cast<uint8_t>(col[2] * 255.0f + 0.5f);
        *colorHexRgba = (0xFF << 24) | (ir << 16) | (ig << 8) | ib;
    }
    return changed;
}

bool Keybind(const char* id, int* currentKey, bool* isCapturing) {
    char buf[64];

    if (*isCapturing) {
        snprintf(buf, sizeof(buf), "[ Press key... ]##%s", id);
        // Check input
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            *isCapturing = false;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            *currentKey = 0;
            *isCapturing = false;
            return true;
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) {
                    *currentKey = k;
                    *isCapturing = false;
                    return true;
                }
            }
        }
    } else {
        if (*currentKey > 0) {
            snprintf(buf, sizeof(buf), "[ %s ]##%s", ImGui::GetKeyName(static_cast<ImGuiKey>(*currentKey)), id);
        } else {
            snprintf(buf, sizeof(buf), "[ NONE ]##%s", id);
        }
    }

    if (Button(buf, ImVec2(120.0f, Sizing::Standard), *isCapturing)) {
        *isCapturing = !(*isCapturing);
    }
    return false;
}

static bool s_firstRow = true;

void Section(const char* title, const char* description, const std::function<void()>& content) {
    const auto& theme = Theme::Instance().Colors();

    ImGui::Dummy(ImVec2(0.0f, Sc(6.0f)));
    if (FontBold) ImGui::PushFont(FontBold);
    ImGui::TextColored(theme.TextPrimary, "%s", title);
    if (FontBold) ImGui::PopFont();
    if (description && description[0]) {
        if (FontSmall) ImGui::PushFont(FontSmall);
        ImGui::TextColored(theme.TextMuted, "%s", description);
        if (FontSmall) ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0.0f, Sc(4.0f)));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Surface);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderSubtle);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Sc(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(22.0f), Sc(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Sc(12.0f), 0.0f));

    char childId[96];
    snprintf(childId, sizeof(childId), "sec_%s", title);
    ImGui::BeginChild(childId, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    s_firstRow = true;
    content();
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0.0f, Sc(10.0f)));
}

void SettingRow(const char* label, const char* description, const std::function<void(float controlWidth)>& renderControl) {
    const auto& theme = Theme::Instance().Colors();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float controlW = Sc(210.0f);
    const float labelW = availW - controlW - Sc(28.0f);
    const float padY = Sc(13.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 screen = ImGui::GetCursorScreenPos();
    if (!s_firstRow) dl->AddLine(ImVec2(screen.x, screen.y), ImVec2(screen.x + availW, screen.y), ImGui::GetColorU32(theme.BorderSubtle), 1.0f);
    s_firstRow = false;

    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x, start.y + padY));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(start.x + labelW);
    ImGui::TextColored(theme.TextPrimary, "%s", label);
    if (description && description[0]) {
        if (FontSmall) ImGui::PushFont(FontSmall);
        ImGui::TextColored(theme.TextMuted, "%s", description);
        if (FontSmall) ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float labelH = ImGui::GetItemRectSize().y;
    const float rowH = std::max(labelH + padY + padY, Sc(58.0f));

    ImGui::SetCursorPos(ImVec2(start.x + availW - controlW, start.y + (rowH - Sc(30.0f)) * 0.5f));
    ImGui::SetNextItemWidth(controlW);
    renderControl(controlW);

    ImGui::SetCursorPos(ImVec2(start.x, start.y + rowH));
}

bool SidebarItem(const char* id, const char* label, bool isSelected, float height, const char* iconUtf8) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID widgetId = window->GetID(id);

    const float availX = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + availX, pos.y + height));

    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, widgetId)) return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, widgetId, &hovered, &held);
    const float sel = AnimationEngine::Instance().AnimateFloat(widgetId, isSelected ? 1.0f : 0.0f, AnimationSpeed::Normal);
    const auto& theme = Theme::Instance().Colors();

    if (sel > 0.01f) {
        ImVec4 bg = theme.Accent; bg.w = 0.16f * sel;
        window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(bg), Sc(10.0f));
        ImVec4 edge = theme.Accent; edge.w = 0.30f * sel;
        window->DrawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(edge), Sc(10.0f), 0, 1.0f);
        const float barH = height * 0.42f;
        window->DrawList->AddRectFilled(ImVec2(bb.Min.x + 1.0f, bb.Min.y + (height - barH) * 0.5f), ImVec2(bb.Min.x + Sc(4.0f), bb.Min.y + (height + barH) * 0.5f), ImGui::GetColorU32(theme.Accent), Sc(2.0f));
    } else if (hovered) {
        window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f)), Sc(10.0f));
    }
    ImVec4 textCol = isSelected ? theme.TextPrimary : (hovered ? theme.TextSecondary : theme.TextMuted);
    float textX = bb.Min.x + Sc(18.0f);
    if (iconUtf8 && FontIcons) {
        ImGui::PushFont(FontIcons);
        const ImVec2 isz = ImGui::CalcTextSize(iconUtf8);
        ImVec4 ic = isSelected ? theme.AccentHover : textCol;
        window->DrawList->AddText(ImVec2(bb.Min.x + Sc(16.0f), bb.Min.y + (height - isz.y) * 0.5f), ImGui::GetColorU32(ic), iconUtf8);
        ImGui::PopFont();
        textX = bb.Min.x + Sc(46.0f);
    }
    const ImVec2 textPos(textX, bb.Min.y + (height - ImGui::GetFontSize()) * 0.5f);
    window->DrawList->AddText(textPos, ImGui::GetColorU32(textCol), label);
    return pressed;
}

void StatusBadge(const char* label, const ImVec4& color) {
    ImVec2 pad(Spacing::SM, Spacing::XS);
    ImVec2 textSz = ImGui::CalcTextSize(label);
    ImVec2 sz(textSz.x + pad.x * 2.0f, textSz.y + pad.y * 2.0f);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Dim background + subtle border
    ImVec4 bgCol = ImVec4(color.x, color.y, color.z, 0.16f);
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ImGui::GetColorU32(bgCol), Radius::XL);
    dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ImGui::GetColorU32(color), Radius::XL, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), ImGui::GetColorU32(color), label);

    ImGui::Dummy(sz);
}

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered()) {
        const auto& theme = Theme::Instance().Colors();
        ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.SurfaceOverlay);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderHover);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Radius::SM);
        ImGui::SetTooltip("%s", text);
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(2);
    }
}

void AddToast(ToastType type, const std::string& title, const std::string& message) {
    s_toasts.push_back({type, title, message, 4.0f, 4.0f});
}

void RenderToasts() {
    if (s_toasts.empty()) return;

    float dt = ImGui::GetIO().DeltaTime;
    for (auto it = s_toasts.begin(); it != s_toasts.end();) {
        it->durationRemaining -= dt;
        if (it->durationRemaining <= 0.0f) {
            it = s_toasts.erase(it);
        } else {
            ++it;
        }
    }

    if (s_toasts.empty()) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float toastW = 300.0f;
    float startX = vp->Pos.x + vp->Size.x - toastW - Spacing::XL;
    float currentY = vp->Pos.y + vp->Size.y - Spacing::XL;

    const auto& theme = Theme::Instance().Colors();

    for (int i = static_cast<int>(s_toasts.size()) - 1; i >= 0; --i) {
        auto& t = s_toasts[i];
        float toastH = 68.0f;
        currentY -= (toastH + Spacing::SM);

        ImGui::SetNextWindowPos(ImVec2(startX, currentY));
        ImGui::SetNextWindowSize(ImVec2(toastW, toastH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.SurfaceOverlay);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderHover);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::MD);

        char toastWinId[64];
        snprintf(toastWinId, sizeof(toastWinId), "##toast_%d", i);

        if (ImGui::Begin(toastWinId, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav)) {
            ImVec4 typeCol = theme.Info;
            if (t.type == ToastType::Success) typeCol = theme.Success;
            else if (t.type == ToastType::Warning) typeCol = theme.Warning;
            else if (t.type == ToastType::Error) typeCol = theme.Error;

            ImGui::TextColored(typeCol, "%s", t.title.c_str());
            ImGui::TextColored(theme.TextSecondary, "%s", t.message.c_str());

            // Progress bar
            float prog = t.durationRemaining / t.totalDuration;
            ImVec2 barP1 = ImVec2(startX + Spacing::MD, currentY + toastH - 4.0f);
            ImVec2 barP2 = ImVec2(startX + Spacing::MD + (toastW - Spacing::MD * 2.0f) * prog, currentY + toastH - 2.0f);
            ImGui::GetForegroundDrawList()->AddRectFilled(barP1, barP2, ImGui::GetColorU32(typeCol), 1.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
}

} // namespace UI

} // namespace aether::ui
