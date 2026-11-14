#pragma once
// One place that answers "is this PC online, and over what?" for the bar icon and the quick panel.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace aether::bar {

struct NetState {
    bool connected{false};   // some adapter is up and has an address
    bool internet{false};    // Windows says the internet is reachable
    bool wifi{false};
    bool ethernet{false};
    int signal{0};           // Wi-Fi quality 0..100
    std::wstring name;       // SSID / adapter name
    std::wstring ip;
};

inline NetState QueryNetState() {
    NetState st;
    // 1. Windows' own connectivity verdict (what the stock tray icon uses)
    {
        Microsoft::WRL::ComPtr<INetworkListManager> nlm;
        if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm))) && nlm) {
            NLM_CONNECTIVITY c = NLM_CONNECTIVITY_DISCONNECTED;
            if (SUCCEEDED(nlm->GetConnectivity(&c))) {
                st.internet = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
                if (c & (NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK | NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV6_SUBNET)) st.connected = true;
                if (st.internet) st.connected = true;
            }
        }
    }
    // 2. adapters: pick the best one that is up and has a usable address
    ULONG len = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &len);
    std::vector<char> buf(len ? len : 1);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const IP_ADAPTER_ADDRESSES* best = nullptr; int bestScore = -1;
    if (len && GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &len) == NO_ERROR) {
        for (auto* a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
            bool usable = false;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family == AF_INET) {
                    auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                    if (sin->sin_addr.S_un.S_un_b.s_b1 != 169) usable = true;   // skip 169.254.x.x (no DHCP)
                } else if (u->Address.lpSockaddr->sa_family == AF_INET6) usable = true;
            }
            if (!usable) continue;
            int score = 1 + (a->FirstGatewayAddress ? 4 : 0) + (a->IfType == IF_TYPE_IEEE80211 || a->IfType == IF_TYPE_ETHERNET_CSMACD ? 2 : 0);
            if (score > bestScore) { bestScore = score; best = a; }
        }
    }
    if (best) {
        st.connected = true;
        st.wifi = best->IfType == IF_TYPE_IEEE80211;
        st.ethernet = !st.wifi;
        if (best->FriendlyName) st.name = best->FriendlyName;
        for (auto* u = best->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            wchar_t ip[32]; swprintf_s(ip, L"%d.%d.%d.%d", sin->sin_addr.S_un.S_un_b.s_b1, sin->sin_addr.S_un.S_un_b.s_b2, sin->sin_addr.S_un.S_un_b.s_b3, sin->sin_addr.S_un.S_un_b.s_b4);
            st.ip = ip; break;
        }
        if (best->FirstGatewayAddress) st.internet = st.internet || true;   // a gateway means we are routed even if NLM is slow
    }
    // 3. Wi-Fi details (SSID, signal)
    if (st.wifi) {
        HANDLE h = nullptr; DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &h) == ERROR_SUCCESS) {
            PWLAN_INTERFACE_INFO_LIST list = nullptr;
            if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
                for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                    if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
                    PWLAN_CONNECTION_ATTRIBUTES at = nullptr; DWORD sz = 0; WLAN_OPCODE_VALUE_TYPE t;
                    if (WlanQueryInterface(h, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &sz, reinterpret_cast<PVOID*>(&at), &t) == ERROR_SUCCESS && at) {
                        st.signal = static_cast<int>(at->wlanAssociationAttributes.wlanSignalQuality);
                        std::string ssid(reinterpret_cast<const char*>(at->wlanAssociationAttributes.dot11Ssid.ucSSID), at->wlanAssociationAttributes.dot11Ssid.uSSIDLength);
                        if (!ssid.empty()) {
                            const int n = MultiByteToWideChar(CP_UTF8, 0, ssid.c_str(), static_cast<int>(ssid.size()), nullptr, 0);
                            std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, ssid.c_str(), static_cast<int>(ssid.size()), w.data(), n);
                            st.name = w;
                        }
                        WlanFreeMemory(at);
                    }
                }
                WlanFreeMemory(list);
            }
            WlanCloseHandle(h, nullptr);
        }
    }
    return st;
}

} // namespace aether::bar
