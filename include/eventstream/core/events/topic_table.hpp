#pragma once
#include <eventstream/core/events/event.hpp>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <spdlog/spdlog.h>

namespace EventStream {

class TopicTable {
public:
    TopicTable() = default;
    bool LoadFileConfig (const std::string & path);
    bool FoundTopic (const std::string & topic , EventPriority & priority) const;

private:
    mutable std::shared_mutex share_mutex;
    std::unordered_map<std::string, EventPriority> Table;
};

} // namespace EventStream
