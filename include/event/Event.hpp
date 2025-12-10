#pragma once  
#include <memory>
#include <string>
#include <cstdint>
#include <vector>   
#include <unordered_map>    

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
        LOW = 0,
        MEDIUM = 1,
        HIGH = 2,
        CRITICAL = 3
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
        
        Event() = default;
        Event(const EventHeader& header , std::string t, std::vector<uint8_t> b , std::unordered_map<std::string, std::string> metadata) 
            : header(header) , topic(std::move(t)) , body(std::move(b)) , metadata(std::move(metadata)) {}

       
    };

    using EventPtr = std::shared_ptr<Event>;
   

}