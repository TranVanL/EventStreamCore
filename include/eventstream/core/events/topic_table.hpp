#pragma once
#include <eventstream/core/events/event.hpp>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @class TopicTable
 * @brief Maps topic names to their configured EventPriority.
 *
 * Loaded from a config file ("topic : PRIORITY" format).
 * Thread-safe: uses shared_mutex for concurrent reads.
 */
class TopicTable {
public:
    TopicTable() = default;

    /// Load topic→priority mappings from a config file.
    bool loadFromFile(const std::string& path);

    /// Look up a topic's priority.  Returns true if found.
    bool findTopic(const std::string& topic, EventPriority& priority) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, EventPriority> table_;
};

} // namespace EventStream
