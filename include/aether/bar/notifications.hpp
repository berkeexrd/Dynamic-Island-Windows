#pragma once
// Reads the toast notifications Windows keeps for the user (wpndatabase.db) so Aether can show its own
// notification panel. The database is copied first (Windows keeps it open) and opened through the
// system's winsqlite3.dll, so no extra library is needed.
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace aether::bar {

struct NotifItem {
    long long id{0};
    std::wstring app, appId, title, body;
    long long arrivalUnix{0};   // seconds since 1970
};

// Icon + display name of the app behind a toast (resolved from the Start-menu app entry on a worker thread).
struct AppVisual {
    std::wstring name;
    int size{0};
    std::vector<uint32_t> px;   // premultiplied BGRA, size x size
    bool ready{false};
};

class NotificationFeed {
public:
    static NotificationFeed& Instance() { static NotificationFeed f; return f; }
    void RequestRefresh();                       // asynchronous; cheap to call often
    [[nodiscard]] std::vector<NotifItem> Items() const { std::lock_guard<std::mutex> l(m_mutex); return m_items; }
    [[nodiscard]] bool Readable() const { return m_readable; }
    [[nodiscard]] int Count() const { std::lock_guard<std::mutex> l(m_mutex); return static_cast<int>(m_items.size()); }
    // Items the user dismissed in Aether's panel are hidden for the rest of the session.
    void Dismiss(long long id);
    void DismissAll();
    // Returns true once the icon is ready; starts resolving it the first time an app id is asked for.
    bool AppVisualFor(const std::wstring& appId, AppVisual& out);

private:
    void Work();
    mutable std::mutex m_mutex;
    std::vector<NotifItem> m_items;
    std::vector<long long> m_dismissed;
    long long m_clearedBefore{0};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_readable{true};
};

} // namespace aether::bar
