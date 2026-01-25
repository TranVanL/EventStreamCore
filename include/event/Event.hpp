#pragma once  
#include <memory>
#include <string>
#include <cstdint>
#include <vector>   
#include <unordered_map>    
#include <chrono>    

namespace EventStream {

    enum struct EventSourceType {
        TCP,
        UDP,
        FILE,
        INTERNAL,
        PLUGIN,
        PYTHON,
    };
    
    enum struct EventPriority {
        BATCH = 0,
        LOW = 1,
        MEDIUM = 2,
        HIGH = 3,
        CRITICAL = 4
    };
    
    struct EventHeader {
        EventSourceType sourceType;
        EventPriority priority;
        uint32_t id;
        uint64_t timestamp;
        uint32_t body_len;
        uint16_t topic_len;
        uint32_t crc32;
    };

    struct Event {
        EventHeader header;
        std::string topic;
        std::vector<uint8_t> body;
        std::unordered_map<std::string, std::string> metadata;
        
        // Day 37: Track when event was dequeued for latency measurement
        uint64_t dequeue_time_ns{0};
        
        Event() = default;
        Event(const EventHeader& header , std::string t, std::vector<uint8_t> b , std::unordered_map<std::string, std::string> metadata) 
            : header(header) , topic(std::move(t)) , body(std::move(b)) , metadata(std::move(metadata)) {}

       
    };

    using EventPtr = std::shared_ptr<Event>;
    
    /**
     * @brief Get current time in nanoseconds (Day 37)
     */
    inline uint64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

} // namespace EventStream
