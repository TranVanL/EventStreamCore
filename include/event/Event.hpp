#pragma once  
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

    struct EventHeader {
        EventSourceType sourceType;
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

        template <typename Alloc>
        Event(const EventHeader& header,
            std::string t,
            std::vector<uint8_t, Alloc> b,
            std::unordered_map<std::string, std::string> metadata)
            : header(header),
            topic(std::move(t)),
            body(b.begin(), b.end()), 
            metadata(std::move(metadata))
            {}
    };

   

}