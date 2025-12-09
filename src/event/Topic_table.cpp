#include "event/Topic_table.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>

using namespace EventStream;

static PriorityOverride strToPrio(const std::string& s) {
    std::string t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    if (t == "LOW") return PriorityOverride::LOW;
    if (t == "MEDIUM") return PriorityOverride::MEDIUM;
    if (t == "HIGH") return PriorityOverride::HIGH;
    if (t == "CRITICAL") return PriorityOverride::CRITICAL;
    return PriorityOverride::NONE;
}

bool TopicTable::loadFromFile(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::string line;
    std::unordered_map<std::string, PriorityOverride> tmp;
    while (std::getline(ifs, line)) {
        // strip comments
        auto posc = line.find('#');
        if (posc != std::string::npos) line = line.substr(0, posc);
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string topic = line.substr(0, pos);
        std::string pr = line.substr(pos + 1);
        // trim
        auto trim = [](std::string &s){
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a==std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
        };
        trim(topic); trim(pr);
        if (topic.empty() || pr.empty()) continue;
        tmp[topic] = strToPrio(pr);
    }
    {
        std::unique_lock lk(mtx_);
        map_.swap(tmp);
    }
    return true;
}

bool TopicTable::lookup(const std::string& topic, PriorityOverride &out) const {
    std::shared_lock lk(mtx_);
    auto it = map_.find(topic);
    if (it == map_.end()) return false;
    out = it->second;
    return true;
}
