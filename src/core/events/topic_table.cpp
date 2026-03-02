#include <eventstream/core/events/topic_table.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>

namespace EventStream {

bool TopicTable::loadFromFile(const std::string& path) {
    std::unique_lock lock(mutex_);
    std::ifstream ifs(path);
    if (!ifs) return false;

    std::string line;
    while (std::getline(ifs, line)) {
        // Strip comments
        auto posc = line.find('#');
        if (posc != std::string::npos) line = line.substr(0, posc);

        auto pos = line.find(':');
        if (pos == std::string::npos) continue;

        std::string topic = line.substr(0, pos);
        std::string pr    = line.substr(pos + 1);

        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
        };
        trim(topic);
        trim(pr);
        if (topic.empty() || pr.empty()) continue;

        if      (pr == "BATCH")    table_[topic] = EventPriority::BATCH;
        else if (pr == "LOW")      table_[topic] = EventPriority::LOW;
        else if (pr == "MEDIUM")   table_[topic] = EventPriority::MEDIUM;
        else if (pr == "HIGH")     table_[topic] = EventPriority::HIGH;
        else if (pr == "CRITICAL") table_[topic] = EventPriority::CRITICAL;
        else continue;
    }

    spdlog::info("Loaded {} topics from {}", table_.size(), path);
    return true;
}

bool TopicTable::findTopic(const std::string& topic, EventPriority& priority) const {
    std::shared_lock lock(mutex_);
    auto it = table_.find(topic);
    if (it != table_.end()) {
        priority = it->second;
        return true;
    }
    return false;
}

} // namespace EventStream


