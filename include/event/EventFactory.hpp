#pragma once    
#include "Event.hpp"
#include <atomic>
#include <chrono>

namespace EventStream {

    class EventFactory {
    public:
        static Event createEvent(EventSourceType sourceType, 
                                 const std::vector<uint8_t>& payload, 
                                 const std::string & topic,
                                 const std::unordered_map<std::string, 
                                 std::string>& metadata = {},
                                 bool is_binary = false);
     
    private: 
        static std::atomic<uint64_t> global_event_id;
    };

} // namespace EventStream
