#include "event/Topic_table.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>

using namespace EventStream;

bool TopicTable::LoadFileConfig(const std::string& path) {
    std::unique_lock lock(share_mutex);
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::string line;
    while (std::getline(ifs, line)) {
       
        auto posc = line.find('#');
        if (posc != std::string::npos) line = line.substr(0, posc);
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string topic = line.substr(0, pos);
        std::string pr = line.substr(pos + 1);
        
        auto trim = [](std::string &s){
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a==std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
        };

        trim(topic); trim(pr);
        if (topic.empty() || pr.empty()) continue;
        if (pr == "LOW")           Table[topic] = EventPriority::LOW;
        else if (pr == "MEDIUM")   Table[topic] = EventPriority::MEDIUM;
        else if (pr == "HIGH")     Table[topic] = EventPriority::HIGH;
        else if (pr == "CRITICAL") Table[topic] = EventPriority::CRITICAL;
        else  continue; 
        
    }
    spdlog::info("Loaded {} topics from {}", Table.size(), path);
    return true;
}

bool TopicTable::FoundTopic(const std::string& topic, EventPriority& priority) const {
    std::shared_lock lock(share_mutex);
    auto it = Table.find(topic);
    if (it != Table.end()) {
        priority = it->second;
        return true;
    }
    return false;
}


