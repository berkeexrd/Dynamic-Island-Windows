#pragma once

#include <functional>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <mutex>
#include <atomic>

namespace aether::core {

using SubscriptionToken = uint64_t;

class EventBus {
public:
    static EventBus& Instance() {
        static EventBus instance;
        return instance;
    }

    template<typename EventType>
    SubscriptionToken Subscribe(std::function<void(const EventType&)> handler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        SubscriptionToken token = ++m_nextToken;
        auto typeIdx = std::type_index(typeid(EventType));

        auto wrapper = [handler](const void* evt) {
            handler(*static_cast<const EventType*>(evt));
        };

        m_handlers[typeIdx].push_back({token, wrapper});
        return token;
    }

    void Unsubscribe(SubscriptionToken token) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [typeIdx, list] : m_handlers) {
            auto it = std::remove_if(list.begin(), list.end(), [token](const HandlerEntry& entry) {
                return entry.token == token;
            });
            if (it != list.end()) {
                list.erase(it, list.end());
                break;
            }
        }
    }

    template<typename EventType>
    void Publish(const EventType& event) {
        std::vector<std::function<void(const void*)>> callbacksToInvoke;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_handlers.find(std::type_index(typeid(EventType)));
            if (it != m_handlers.end()) {
                callbacksToInvoke.reserve(it->second.size());
                for (const auto& entry : it->second) {
                    callbacksToInvoke.push_back(entry.fn);
                }
            }
        }

        for (auto& fn : callbacksToInvoke) {
            fn(&event);
        }
    }

private:
    EventBus() = default;

    struct HandlerEntry {
        SubscriptionToken token;
        std::function<void(const void*)> fn;
    };

    std::mutex m_mutex;
    std::atomic<SubscriptionToken> m_nextToken{0};
    std::unordered_map<std::type_index, std::vector<HandlerEntry>> m_handlers;
};

} // namespace aether::core
