#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#include <winrt/base.h>

#pragma comment(lib, "winmm.lib")
#include "aether/core/log.hpp"
#include "aether/core/timer.hpp"
#include "aether/core/events.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/system/metrics_collector.hpp"
#include "aether/system/gaming_mode.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/media/media_service.hpp"
#include "aether/island/island_window.hpp"
#include "aether/widgets/widget_window.hpp"
#include "aether/ui/settings_window.hpp"

using namespace aether;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    (void)hInstance;
    // Enable 1ms multimedia timer resolution for high refresh rates (144Hz / 240Hz)
    timeBeginPeriod(1);

    // Enable Per-Monitor V2 DPI Awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    core::Logger::Instance().SetMinLevel(core::LogLevel::Debug);
    AETHER_LOG_INFO("=================================================");
    AETHER_LOG_INFO("     PROJECT AETHER - DESKTOP CUSTOMIZATION      ");
    AETHER_LOG_INFO("=================================================");

    try {
        // Initialize WinRT & COM
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        AETHER_LOG_DEBUG("COM / WinRT apartment initialized");

        // 1. Initialize Configuration Manager
        config::ConfigManager::Instance().Initialize();
        const auto& appConfig = config::ConfigManager::Instance().Get();
        AETHER_LOG_DEBUG("ConfigManager initialized");

        // 2. Initialize Hardware Services
        system::TaskbarService::Instance().Initialize();
        if (appConfig.taskbar.enabled) {
            system::TaskbarService::Instance().ApplyStyle(appConfig.taskbar.style, appConfig.taskbar.tintColor);
        }
        AETHER_LOG_DEBUG("TaskbarService initialized");

        system::MetricsCollector::Instance().Start(1000);
        AETHER_LOG_DEBUG("MetricsCollector initialized");

        system::GamingModeDetector::Instance().Start(1500);
        AETHER_LOG_DEBUG("GamingModeDetector initialized");

        media::MediaService::Instance().Initialize();
        AETHER_LOG_DEBUG("MediaService initialized");

        // 3. Create Dynamic Island Window
        auto island = std::make_unique<island::IslandWindow>();
        if (!island->Create()) {
            AETHER_LOG_ERROR("Could not create IslandWindow");
        }
        AETHER_LOG_DEBUG("IslandWindow initialized");

        // 4. Create Desktop Widgets
        if (appConfig.widgets.enabled) {
            widgets::WidgetManager::Instance().Initialize();
            AETHER_LOG_DEBUG("WidgetManager initialized");
        }

        // 5. Create Settings Center Window
        if (!ui::SettingsWindow::Instance().Create()) {
            AETHER_LOG_ERROR("Could not create SettingsWindow");
        }
        AETHER_LOG_DEBUG("SettingsWindow initialized");

    // Subscribe to Gaming Mode state changes
    core::EventBus::Instance().Subscribe<system::GamingModeChangedEvent>([&](const system::GamingModeChangedEvent& evt) {
        if (evt.isGamingActive) {
            AETHER_LOG_INFO("Game detected! Gracefully sliding out non-essential overlays...");
            const auto& gameCfg = config::ConfigManager::Instance().Get().gaming;
            if (gameCfg.action == config::GamingModeAction::HideCompletely) {
                island->SetGamingSuspended(true);
            }
            if (gameCfg.pauseWidgets) {
                widgets::WidgetManager::Instance().SetVisible(false);
            }
        } else {
            AETHER_LOG_INFO("Exited game! Gracefully sliding in desktop customization...");
            island->SetGamingSuspended(false);
            widgets::WidgetManager::Instance().SetVisible(true);
        }
    });

    // Register Global Hotkey (F9) to toggle Island
    RegisterHotKey(nullptr, 101, MOD_NOREPEAT, VK_F9);

    // Check command line arguments for --settings
    int numArgs = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    if (argList) {
        for (int i = 0; i < numArgs; ++i) {
            if (wcscmp(argList[i], L"--settings") == 0) {
                ui::SettingsWindow::Instance().Show(true);
            }
        }
        LocalFree(argList);
    }

    AETHER_LOG_INFO("Entering high-efficiency 240Hz message loop...");
    timeBeginPeriod(1);
    core::Timer timer;
    bool running = true;
    double lastMetricsTick = 0.0;
    double lastClockTick = 0.0;

    while (running) {
        MSG msg;
        // Process all pending Windows messages
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }

            // Global Hotkey (F9)
            if (msg.message == WM_HOTKEY && msg.wParam == 101) {
                if (island) {
                    if (island->GetMode() == island::IslandMode::Idle) {
                        island->SetMode(island::IslandMode::MediaExpanded);
                    } else if (island->GetMode() == island::IslandMode::MediaExpanded) {
                        island->SetMode(island::IslandMode::SystemExpanded);
                    } else {
                        island->SetMode(island::IslandMode::Idle);
                    }
                }
            }

            // Message from context menu: Open Settings
            if (msg.message == WM_USER + 100) {
                ui::SettingsWindow::Instance().Show(true);
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!running) break;

        float dt = static_cast<float>(timer.Tick());
        double now = timer.Now();

        // Advance media track timeline
        media::MediaService::Instance().Update(dt);

        bool isAnimating = (island && !island->IsResting()) || ui::SettingsWindow::Instance().IsVisible();

        // Update & Render Island:
        // If animating (springs in motion, mouse hovering, music playing), update and render every frame!
        // If resting, update at 1Hz on clock tick to update HH:MM.
        if (island) {
            if (isAnimating || (now - lastClockTick >= 1.0)) {
                if (!isAnimating) lastClockTick = now;
                island->Update(dt);
                island->Render();
            }
        }

        // Update widgets at adaptive 1Hz or on demand
        if (now - lastMetricsTick >= 1.0) {
            lastMetricsTick = now;
            widgets::WidgetManager::Instance().Update(dt);
            widgets::WidgetManager::Instance().Render();
        }

        // Render Settings Window if open
        if (ui::SettingsWindow::Instance().IsVisible()) {
            ui::SettingsWindow::Instance().Render();
        }

        // 240Hz Fluid Pacing:
        // While animating: DO NOT SLEEP! The composition swap chain's Present(1, 0)
        // synchronizes directly to the monitor's hardware V-Blank (~4.16ms on 240Hz).
        // Only sleep when 100% idle and resting to yield 0% CPU.
        if (!isAnimating) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else if (island && !island->IsSpringDriven() && !ui::SettingsWindow::Instance().IsVisible()) {
            // Only ambient motion (audio bars, scrolling title): 60 fps is plenty and saves GPU/CPU.
            MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    AETHER_LOG_INFO("Shutting down Aether gracefully...");

    // Clean shutdown
    timeEndPeriod(1);
    ui::SettingsWindow::Instance().Destroy();
    widgets::WidgetManager::Instance().Shutdown();
    island.reset();

    media::MediaService::Instance().Shutdown();
    system::GamingModeDetector::Instance().Stop();
    system::MetricsCollector::Instance().Stop();
    system::TaskbarService::Instance().RestoreDefaults();

    UnregisterHotKey(nullptr, 101);
    winrt::uninit_apartment();
    AETHER_LOG_INFO("Aether shutdown complete.");
    return 0;
    } catch (const std::exception& e) {
        AETHER_LOG_ERROR("Unhandled exception in wWinMain: " << e.what());
        return 1;
    } catch (const winrt::hresult_error& he) {
        AETHER_LOG_ERROR("WinRT hresult error: " << winrt::to_string(he.message()));
        return 1;
    } catch (...) {
        AETHER_LOG_ERROR("Unknown fatal crash in wWinMain");
        return 1;
    }
}
