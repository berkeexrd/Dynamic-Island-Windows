#include "aether/bar/wifi.hpp"
#include "aether/core/i18n.hpp"
#include <wlanapi.h>
#include <algorithm>
#include <thread>

#pragma comment(lib, "wlanapi.lib")

namespace aether::bar {
namespace i18n = aether::i18n;

namespace {
struct Handle {
    HANDLE h{nullptr}; GUID guid{}; bool ok{false};
    Handle() {
        DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) { h = nullptr; return; }
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
            if (list->dwNumberOfItems > 0) { guid = list->InterfaceInfo[0].InterfaceGuid; ok = true; }
            WlanFreeMemory(list);
        }
    }
    ~Handle() { if (h) WlanCloseHandle(h, nullptr); }
};

std::wstring Widen(const DOT11_SSID& s) {
    std::string a(reinterpret_cast<const char*>(s.ucSSID), s.uSSIDLength);
    if (a.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a.data(), static_cast<int>(a.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(cp, 0, a.data(), static_cast<int>(a.size()), nullptr, 0); }
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, a.data(), static_cast<int>(a.size()), w.data(), n);
    return w;
}
std::wstring XmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) { case L'&': o += L"&amp;"; break; case L'<': o += L"&lt;"; break; case L'>': o += L"&gt;"; break; case L'"': o += L"&quot;"; break; case L'\'': o += L"&apos;"; break; default: o += c; }
    }
    return o;
}
} // namespace

void WifiService::Run(void (WifiService::*fn)()) { (void)fn; }

void WifiService::Refresh(bool scan) {
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (m_state.scanning) return;
        if (scan) m_state.scanning = true;
    }
    std::thread([this, scan] { std::lock_guard<std::mutex> op(m_op); DoRefresh(scan); }).detach();
}

void WifiService::DoRefresh(bool scan) {
    WifiState st;
    {
        std::lock_guard<std::mutex> l(m_mutex); st.status = m_state.status;
    }
    Handle w;
    st.adapter = w.ok;
    if (w.ok) {
        // radio state
        PWLAN_RADIO_STATE rs = nullptr; DWORD sz = 0;
        if (WlanQueryInterface(w.h, &w.guid, wlan_intf_opcode_radio_state, nullptr, &sz, reinterpret_cast<PVOID*>(&rs), nullptr) == ERROR_SUCCESS && rs) {
            for (DWORD i = 0; i < rs->dwNumberOfPhys; ++i)
                if (rs->PhyRadioState[i].dot11SoftwareRadioState == dot11_radio_state_off || rs->PhyRadioState[i].dot11HardwareRadioState == dot11_radio_state_off) st.radioOn = false;
            WlanFreeMemory(rs);
        }
        if (st.radioOn) {
            if (scan) { WlanScan(w.h, &w.guid, nullptr, nullptr, nullptr); Sleep(1800); }
            std::wstring cur;
            PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; sz = 0;
            if (WlanQueryInterface(w.h, &w.guid, wlan_intf_opcode_current_connection, nullptr, &sz, reinterpret_cast<PVOID*>(&ca), nullptr) == ERROR_SUCCESS && ca) {
                if (ca->isState == wlan_interface_state_connected) cur = Widen(ca->wlanAssociationAttributes.dot11Ssid);
                WlanFreeMemory(ca);
            }
            PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
            if (WlanGetAvailableNetworkList(w.h, &w.guid, 0, nullptr, &list) == ERROR_SUCCESS && list) {
                for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                    const auto& n = list->Network[i];
                    WifiNet net;
                    net.ssid = Widen(n.dot11Ssid);
                    if (net.ssid.empty()) continue;
                    net.signal = static_cast<int>(n.wlanSignalQuality);
                    net.secure = n.bSecurityEnabled != 0;
                    net.known = (n.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
                    net.connected = (n.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0 || net.ssid == cur;
                    net.authAlgo = static_cast<unsigned>(n.dot11DefaultAuthAlgorithm);
                    auto it = std::find_if(st.nets.begin(), st.nets.end(), [&](const WifiNet& o) { return o.ssid == net.ssid; });
                    if (it == st.nets.end()) st.nets.push_back(net);
                    else { it->signal = std::max(it->signal, net.signal); it->connected |= net.connected; it->known |= net.known; }
                }
                WlanFreeMemory(list);
            }
            std::sort(st.nets.begin(), st.nets.end(), [](const WifiNet& a, const WifiNet& b) { if (a.connected != b.connected) return a.connected; return a.signal > b.signal; });
        }
    }
    std::lock_guard<std::mutex> l(m_mutex);
    st.busy = m_state.busy; st.scanning = false;
    m_state = std::move(st);
}

void WifiService::Connect(const std::wstring& ssid, const std::wstring& password, bool known, bool secure, unsigned authAlgo) {
    { std::lock_guard<std::mutex> l(m_mutex); m_state.busy = true; m_state.status.clear(); }
    std::thread([=, this] {
        std::lock_guard<std::mutex> op(m_op);
        bool ok = false;
        {
            Handle w;
            if (w.ok) {
                if (!known) {
                    std::wstring x = L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>" + XmlEsc(ssid) +
                        L"</name><SSIDConfig><SSID><name>" + XmlEsc(ssid) + L"</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>auto</connectionMode><MSM><security><authEncryption>";
                    if (!secure) x += L"<authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX></authEncryption>";
                    else {
                        const bool sae = authAlgo == 9;
                        x += std::wstring(L"<authentication>") + (sae ? L"WPA3SAE" : L"WPA2PSK") + L"</authentication><encryption>AES</encryption><useOneX>false</useOneX>" +
                             L"</authEncryption><sharedKey><keyType>passphrase</keyType><protected>false</protected><keyMaterial>" + XmlEsc(password) + L"</keyMaterial></sharedKey>";
                        if (!sae) x += L"";
                    }
                    if (!secure) x += L"";
                    x += L"</security></MSM></WLANProfile>";
                    DWORD reason = 0;
                    if (WlanSetProfile(w.h, &w.guid, 0, x.c_str(), nullptr, TRUE, nullptr, &reason) != ERROR_SUCCESS) goto done;
                }
                WLAN_CONNECTION_PARAMETERS p{};
                p.wlanConnectionMode = wlan_connection_mode_profile;
                p.strProfile = ssid.c_str();
                p.dot11BssType = dot11_BSS_type_infrastructure;
                ok = WlanConnect(w.h, &w.guid, &p, nullptr) == ERROR_SUCCESS;
            }
        }
        done:
        if (ok) Sleep(3500);
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_state.busy = false;
            if (!ok) m_state.status = i18n::Pick(L"Could not connect. Check the password.", L"Bağlanılamadı. Şifreyi kontrol et.");
        }
        DoRefresh(false);
    }).detach();
}

void WifiService::Disconnect() {
    std::thread([this] { std::lock_guard<std::mutex> op(m_op); { Handle w; if (w.ok) WlanDisconnect(w.h, &w.guid, nullptr); } Sleep(600); DoRefresh(false); }).detach();
}

void WifiService::SetRadio(bool on) {
    { std::lock_guard<std::mutex> l(m_mutex); m_state.busy = true; }
    std::thread([this, on] {
        std::lock_guard<std::mutex> op(m_op);
        {
            Handle w;
            if (w.ok) {
                WLAN_PHY_RADIO_STATE s{}; s.dwPhyIndex = 0; s.dot11SoftwareRadioState = on ? dot11_radio_state_on : dot11_radio_state_off;
                WlanSetInterface(w.h, &w.guid, wlan_intf_opcode_radio_state, sizeof(s), &s, nullptr);
            }
        }
        Sleep(on ? 1500 : 300);
        { std::lock_guard<std::mutex> l(m_mutex); m_state.busy = false; }
        DoRefresh(on);
    }).detach();
}

} // namespace aether::bar
