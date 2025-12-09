#include "event/EventBus.hpp"
#include <vector>

namespace EventStream {

    void EventBus::publishEvent (const Event& event) {
        // Copy subscribers under the lock, then invoke outside the lock
        std::vector<EventHandlerFunction> handlers;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            handlers = subscribers;
        }

        for (const auto &handler : handlers) {
            try {
                handler(event);
            } catch (...) {
                // swallow exceptions from subscribers to avoid crashing the publisher
            }
        }
    }

    void EventBus::subscribe(EventHandlerFunction handler) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        subscribers.push_back(std::move(handler));
    }

} // namespace EventStream