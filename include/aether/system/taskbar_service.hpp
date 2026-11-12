#pragma once

#include "aether/config/config_manager.hpp"
#include <windows.h>
#include <vector>

namespace aether::system {

class TaskbarService {
public:
    static TaskbarService& Instance() {
        static TaskbarService instance;
        return instance;
    }

    void Initialize();
    void ApplyStyle(config::TaskbarStyle style, uint32_t tintColor);
    void RestoreDefaults();

    static bool SetWindowAccent(HWND hwnd, int accentState, uint32_t color);

private:
    TaskbarService() = default;
    ~TaskbarService() { RestoreDefaults(); }

    std::vector<HWND> FindTaskbarWindows();

    bool m_initialized{false};
    config::TaskbarStyle m_currentStyle{config::TaskbarStyle::Default};
};

} // namespace aether::system
