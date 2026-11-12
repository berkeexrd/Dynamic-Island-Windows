#pragma once

#include <imgui.h>
#include <string>
#include <functional>
#include <cstdint>
#include "aether/ui/theme.hpp"

namespace aether::ui {

namespace UI {

    // Base custom controls
    bool Button(const char* label, const ImVec2& size = ImVec2(0.0f, Sizing::Standard), bool primary = false);
    bool Toggle(const char* id, bool* value);
    bool Checkbox(const char* label, bool* value);
    bool SliderFloat(const char* id, float* value, float minVal, float maxVal, const char* format = "%.2f");
    bool SliderInt(const char* id, int* value, int minVal, int maxVal);
    bool Combo(const char* id, int* currentItem, const char* const items[], int itemsCount, float width = 160.0f);
    bool ColorPicker(const char* id, uint32_t* colorHexRgba);

    // Keybind capture control
    bool Keybind(const char* id, int* currentKey, bool* isCapturing);

    // Layout containers & semantic blocks
    void Section(const char* title, const char* description, const std::function<void()>& content);
    void SettingRow(const char* label, const char* description, const std::function<void(float controlWidth)>& renderControl);

    // Navigation & Badges
    bool SidebarItem(const char* id, const char* label, bool isSelected, float height = 40.0f);
    void StatusBadge(const char* label, const ImVec4& color);
    void Tooltip(const char* text);

    // Toast notifications
    enum class ToastType { Success, Info, Warning, Error };
    void AddToast(ToastType type, const std::string& title, const std::string& message);
    void RenderToasts();

} // namespace UI

} // namespace aether::ui
