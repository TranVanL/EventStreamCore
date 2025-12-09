#pragma once
#include "Event.hpp"
#include <functional>
#include <mutex>
// simple synchronous dispatch: publishEvent calls subscribers directly
#include <vector>

namespace EventStream {

    class EventBus {
    public:
    using EventHandlerFunction = std::function<void(const Event&)>;
    void publishEvent(const Event& event);
    // register a subscriber; handler is called synchronously when publishEvent is invoked
    void subscribe(EventHandlerFunction handler);

    private:
        std::mutex queue_mutex;
        std::vector<EventHandlerFunction> subscribers;
    };

} // namespace EventStream
    