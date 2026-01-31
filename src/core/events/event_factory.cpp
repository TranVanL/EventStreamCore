#include <eventstream/core/events/event_factory.hpp>
#include <cstdint>
#include <mutex>

namespace EventStream {

    uint32_t EventFactory::calculateCRC32(const std::vector<uint8_t>& data) {
        static uint32_t CRC32_TABLE[256];
        static std::once_flag init_flag;

        std::call_once(init_flag, []() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t crc = i;
                for (int j = 0; j < 8; j++) {
                    if (crc & 1)
                        crc = (crc >> 1) ^ 0xEDB88320;
                    else
                        crc >>= 1;
                }
                CRC32_TABLE[i] = crc;
            }
        });

        uint32_t crc = 0xFFFFFFFF;
        for (uint8_t byte : data) {
            crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF];
        }
        return crc ^ 0xFFFFFFFF;
    }

    std::atomic<uint64_t> EventFactory::global_event_id{0};
    
    Event EventFactory::createEvent(EventSourceType sourceType,
                                    EventPriority priority,
                                    std::vector<uint8_t>&& payload,
                                    std::string&& topic,
                                    std::unordered_map<std::string,std::string>&& metadata
                                    ) {
        EventHeader h;
        h.priority = priority;
        h.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        h.sourceType = sourceType;
        h.id = global_event_id.fetch_add(1, std::memory_order_relaxed);
        h.topic_len = topic.size();
        h.body_len = payload.size();
        h.crc32 = EventFactory::calculateCRC32(payload);

        return Event(h,std::move(topic),std::move(payload),std::move(metadata));
    }

} // namespace EventStream
