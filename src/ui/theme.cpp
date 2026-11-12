#include "aether/ui/theme.hpp"
#include <algorithm>

namespace aether::ui {

Theme::Theme() {
    Apply();
}

void Theme::SetAccentColor(uint32_t hexRgba) {
    float r = static_cast<float>((hexRgba >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((hexRgba >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(hexRgba & 0xFF) / 255.0f;
    float a = static_cast<float>((hexRgba >> 24) & 0xFF) / 255.0f;
    if (a <= 0.0f) a = 1.0f;

    m_colors.Accent = ImVec4(r, g, b, a);
    m_colors.AccentHover = ImVec4(
        std::min(r * 1.15f, 1.0f),
        std::min(g * 1.15f, 1.0f),
        std::min(b * 1.15f, 1.0f),
        a
    );
    m_colors.AccentActive = ImVec4(r * 0.85f, g * 0.85f, b * 0.85f, a);
    m_colors.AccentMuted = ImVec4(r, g, b, 0.20f);
    m_colors.AccentGlow = ImVec4(r, g, b, 0.35f);
    m_colors.BorderFocus = m_colors.Accent;

    Apply();
}

void Theme::Apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Sizing & Spacing
    style.WindowPadding     = ImVec2(Spacing::LG, Spacing::LG);
    style.FramePadding      = ImVec2(Spacing::MD, Spacing::SM);
    style.CellPadding       = ImVec2(Spacing::SM, Spacing::SM);
    style.ItemSpacing       = ImVec2(Spacing::MD, Spacing::MD);
    style.ItemInnerSpacing  = ImVec2(Spacing::SM, Spacing::SM);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing     = Spacing::XL;
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 12.0f;

    // Radius hierarchy: Window > Popup/Card > Control > Badge
    style.WindowRounding    = Radius::LG;
    style.ChildRounding     = Radius::MD;
    style.FrameRounding     = Radius::SM;
    style.PopupRounding     = Radius::MD;
    style.ScrollbarRounding = Radius::SM;
    style.GrabRounding      = Radius::XS;
    style.TabRounding       = Radius::SM;

    // Borders
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;

    // Colors mapping to tokens
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = m_colors.TextPrimary;
    colors[ImGuiCol_TextDisabled]          = m_colors.TextDisabled;
    colors[ImGuiCol_WindowBg]              = m_colors.Background;
    colors[ImGuiCol_ChildBg]               = m_colors.Surface;
    colors[ImGuiCol_PopupBg]               = m_colors.SurfaceOverlay;
    colors[ImGuiCol_Border]                = m_colors.Border;
    colors[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg]               = m_colors.SurfaceElevated;
    colors[ImGuiCol_FrameBgHovered]        = m_colors.SurfaceHover;
    colors[ImGuiCol_FrameBgActive]         = m_colors.SurfaceActive;
    colors[ImGuiCol_TitleBg]               = m_colors.Shell;
    colors[ImGuiCol_TitleBgActive]         = m_colors.Shell;
    colors[ImGuiCol_TitleBgCollapsed]      = m_colors.Shell;
    colors[ImGuiCol_MenuBarBg]             = m_colors.Shell;
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab]         = m_colors.SurfaceHover;
    colors[ImGuiCol_ScrollbarGrabHovered]  = m_colors.SurfaceActive;
    colors[ImGuiCol_ScrollbarGrabActive]   = m_colors.Accent;
    colors[ImGuiCol_CheckMark]             = m_colors.Accent;
    colors[ImGuiCol_SliderGrab]            = m_colors.Accent;
    colors[ImGuiCol_SliderGrabActive]      = m_colors.AccentHover;
    colors[ImGuiCol_Button]                = m_colors.SurfaceElevated;
    colors[ImGuiCol_ButtonHovered]         = m_colors.SurfaceHover;
    colors[ImGuiCol_ButtonActive]          = m_colors.SurfaceActive;
    colors[ImGuiCol_Header]                = m_colors.SurfaceElevated;
    colors[ImGuiCol_HeaderHovered]         = m_colors.SurfaceHover;
    colors[ImGuiCol_HeaderActive]          = m_colors.SurfaceActive;
    colors[ImGuiCol_Separator]             = m_colors.BorderSubtle;
    colors[ImGuiCol_SeparatorHovered]      = m_colors.BorderHover;
    colors[ImGuiCol_SeparatorActive]       = m_colors.Accent;
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered]     = m_colors.AccentMuted;
    colors[ImGuiCol_ResizeGripActive]      = m_colors.Accent;
    colors[ImGuiCol_Tab]                   = m_colors.SurfaceElevated;
    colors[ImGuiCol_TabHovered]            = m_colors.SurfaceHover;
    colors[ImGuiCol_TabActive]             = m_colors.SurfaceActive;
    colors[ImGuiCol_TabUnfocused]          = m_colors.SurfaceElevated;
    colors[ImGuiCol_TabUnfocusedActive]    = m_colors.SurfaceActive;
    colors[ImGuiCol_ModalWindowDimBg]      = m_colors.BackdropDim;
}

} // namespace aether::ui
