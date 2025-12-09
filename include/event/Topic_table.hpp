#pragma once
#include <string>
#include <unordered_map>
#include <shared_mutex>

namespace EventStream {

enum class PriorityOverride {
    NONE = -1,
    LOW = 0,
    MEDIUM = 1,
    HIGH = 2,
    CRITICAL = 3
};

class TopicTable {
public:
    TopicTable() = default;

    // load simple mapping file "topic:PRIORITY" per line 
    bool loadFromFile(const std::string& path);

    // returns true if override exists and writes into out
    bool lookup(const std::string& topic, PriorityOverride &out) const;

private:
    mutable std::shared_mutex mtx_;
    std::unordered_map<std::string, PriorityOverride> map_;
};

} // namespace EventStream
