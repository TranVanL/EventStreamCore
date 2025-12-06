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

    struct Event {
        EventSourceType sourceType;
        uint64_t id;
        uint64_t timestamp;
        std::string topic;
        std::vector<uint8_t> payload;
        std::unordered_map<std::string, std::string> metadata;
        bool is_binary;    
    };

}