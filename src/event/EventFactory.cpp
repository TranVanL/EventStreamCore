#include "event/EventFactory.hpp"

namespace EventStream {

    std::atomic<uint64_t> EventFactory::global_event_id = 1;

    Event EventFactory::createEvent(EventSourceType sourceType,
                                    const std::vector<uint8_t>& payload,
                                    const std::string & topic,
                                    const std::unordered_map<std::string,
                                    std::string>& metadata,
                                    bool is_binary) {
        Event event;
        event.sourceType = sourceType;
        event.id = global_event_id.fetch_add(1, std::memory_order_relaxed);
        event.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        event.topic = topic;
        event.payload = payload;
        event.metadata = metadata;
        event.is_binary = is_binary;
        return event;
    }

} // namespace EventStream
