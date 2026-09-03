#pragma once  
#include <memory>
#include <string>
#include <cstdint>
#include <vector>   
#include <unordered_map>    
#include <chrono>    

namespace EventStream {

    // Source type of event appear to Header struct 
    enum struct EventSourceType {
        TCP,
        UDP,
        FILE,
        INTERNAL,
        PLUGIN,
        PYTHON,
    };
    
    // Priority of event appear to Header struct
    enum struct EventPriority {
        BATCH = 0,
        LOW = 1,
        MEDIUM = 2,
        HIGH = 3,
        CRITICAL = 4
    };
    
    // Header of Event , construct after event is received from source and before push to EventBus
    struct EventHeader {

        // Construct head with srouce type input , priority , Id of event , timestamp , body and topic length , crc32 for ensure data integrity
        EventSourceType sourceType;
        EventPriority priority;
        uint32_t id;
        uint64_t timestamp;
        uint32_t body_len;
        uint16_t topic_len;
        uint32_t crc32;
    };

    // Struct of Event , contain header , topic , body(payload) and metadata 
    struct Event {

        EventHeader header;
        std::string topic;
        std::vector<uint8_t> body;
        std::unordered_map<std::string, std::string> metadata;
        
        /// Timestamp (ns) when the event was dequeued — used for latency measurement.
        uint64_t dequeue_time_ns{0};
        
        Event() = default;
        Event(const EventHeader& header , std::string t, std::vector<uint8_t> b , std::unordered_map<std::string, std::string> metadata) 
            : header(header) , topic(std::move(t)) , body(std::move(b)) , metadata(std::move(metadata)) {}

       
    };

    // Set for convenience when create shared_ptr of Event , and avoid memory leak
    using EventPtr = std::shared_ptr<Event>;
    
    /// Return the current time in nanoseconds (monotonic).
    inline uint64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

} // namespace EventStream
