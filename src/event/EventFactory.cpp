#include "event/EventFactory.hpp"
#include <cstdint>

namespace EventStream {

// Simple CRC32 implementation
uint32_t EventFactory::calculateCRC32(const std::vector<uint8_t>& data) {
    static const uint32_t CRC32_TABLE[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
        0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
        0x1DB71642, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA44E5D6, 0x8D079D40,
        0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD108D7FD, 0xA6636D8B,
        0x29A00B5A, 0x5E119B8C, 0xC7C24336, 0xB0C9A636, 0x2EE7B697, 0x59B33D01, 0xC2CBDC9B, 0xB5BDD80D,
        0x2CD99E8B, 0x5BDEAE1D, 0xC8D8D9E7, 0xBFD91D77, 0x21B4F4D5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
        0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6E5C65, 0x58626FF3, 0xCBCF0409, 0xBCBD429F,
        0x550C367D, 0x22026A6B, 0xBB1C0A91, 0xCC0D9407, 0x5A827999, 0x2D00CEAF, 0xB4902035, 0xC3A843A3,
        0x5DB76D72, 0x2A6F2DE4, 0xB344D91E, 0xC4D68088, 0x63A36E0B, 0x1407449D, 0x8D598E67, 0xFADABF1F,
        0x6DADC7F1, 0x1ABB5B87, 0x8317D7D7, 0xF4655741, 0x65BFAED5, 0x12B7A043, 0x8B8750B9, 0xFCD5502F,
        0x68CAB56C, 0x1FD975FA, 0x86B75A00, 0xF1B6E696, 0x6E7F1AC7, 0x1955B051, 0x804DB8AB, 0xF7D6D83D
    };
    
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : data) {
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

    std::atomic<uint64_t> EventFactory::global_event_id{0};
    
    Event EventFactory::createEvent(EventSourceType sourceType,
                                    EventPriority priority,
                                    std::vector<uint8_t> payload,
                                    std::string topic,
                                    std::unordered_map<std::string,std::string> metadata
                                    ) {
        EventHeader h;
        h.priority = priority;
        h.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        h.sourceType = sourceType;
        h.id = global_event_id.fetch_add(1, std::memory_order_relaxed);
        h.topic_len = topic.size();
        h.body_len = payload.size();
        h.crc32 = calculateCRC32(payload);

        return Event(h,std::move(topic),std::move(payload),std::move(metadata));
    }

} // namespace EventStream
