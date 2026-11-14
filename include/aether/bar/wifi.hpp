#pragma once
// Wi-Fi control through the native WLAN API: scan, list, connect (known profile or password), disconnect, radio on/off.
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace aether::bar {

struct WifiNet {
    std::wstring ssid;
    int signal{0};          // 0..100
    bool secure{false};
    bool known{false};      // a saved profile exists
    bool connected{false};
    unsigned authAlgo{0};   // DOT11_AUTH_ALGORITHM
};

struct WifiState {
    bool adapter{false};    // a Wi-Fi adapter exists
    bool radioOn{true};
    bool scanning{false};
    bool busy{false};       // connecting / toggling
    std::wstring status;    // last result, e.g. "Could not connect"
    std::vector<WifiNet> nets;
};

class WifiService {
public:
    static WifiService& Instance() { static WifiService s; return s; }
    [[nodiscard]] WifiState Get() const { std::lock_guard<std::mutex> l(m_mutex); return m_state; }
    void Refresh(bool scan);                                           // asynchronous
    void Connect(const std::wstring& ssid, const std::wstring& password, bool known, bool secure, unsigned authAlgo);
    void Disconnect();
    void SetRadio(bool on);
    void ClearStatus() { std::lock_guard<std::mutex> l(m_mutex); m_state.status.clear(); }

private:
    void Run(void (WifiService::*fn)());
    void DoRefresh(bool scan);
    mutable std::mutex m_mutex;
    std::mutex m_op;
    WifiState m_state;
};

} // namespace aether::bar
