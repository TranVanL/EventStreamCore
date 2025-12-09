#pragma once    
#include "Event.hpp"
#include <atomic>
#include <chrono>


namespace EventStream {

    class EventFactory {
    public:
        
        static Event createEvent(EventSourceType sourceType, 
                                 EventPriority priority,
                                 std::vector<uint8_t> payload, 
                                 std::string  topic,
                                 std::unordered_map<std::string, std::string> metadata);
     
    private: 
        static std::atomic<uint64_t> global_event_id;
    };
   

} // namespace EventStream
