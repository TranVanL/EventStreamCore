#include "event/Event.hpp"
#include <string> 
#include <vector>

namespace EventStream {

    struct ParsedResult {
    std::string topic;
    std::vector<uint8_t> payload;
    };

ParsedResult parseFrame(const std::vector<uint8_t>& raw);
Event parseTCP(const std::vector<uint8_t>& raw);

} // namespace EventStream

