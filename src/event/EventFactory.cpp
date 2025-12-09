#include "event/EventFactory.hpp"
#include "memory/event_memorypool.hpp"
#include "memory/memory_allocator.hpp"

namespace EventStream {

    std::atomic<uint64_t> EventFactory::global_event_id{0};
    
    Event EventFactory::createEvent(EventSourceType sourceType,
                                    const std::vector<uint8_t>& payload,
                                    const std::string  topic,
                                    const std::unordered_map<std::string,std::string> metadata
                                    ) {
        EventHeader h;
        h.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        h.sourceType = sourceType;
        h.id = global_event_id.fetch_add(1, std::memory_order_relaxed);
        h.topic_len = topic.size();
        h.body_len = payload.size();
        h.crc32 = 0; //TODO 

        auto body = std::vector<uint8_t, PoolAllocator<uint8_t>>(
            payload.begin(), payload.end(), PoolAllocator<uint8_t>(framePool)
        );

        body.assign(payload.begin(),payload.end());

        return Event(h,std::move(topic),std::move(body),std::move(metadata));
    }

} // namespace EventStream
