#pragma once    
#include "Event.hpp"
#include <atomic>
#include <chrono>
#include "memory/event_memorypool.hpp"

namespace EventStream {

    class EventFactory {
    public:
        static MemoryPool * framePool;
        static Event createEvent(EventSourceType sourceType, 
                                 const std::vector<uint8_t>& payload, 
                                 const std::string  topic,
                                 const std::unordered_map<std::string, std::string> metadata);
     
    private: 
        static std::atomic<uint64_t> global_event_id;
    };
    inline MemoryPool* EventFactory::framePool = nullptr;

} // namespace EventStream
