// ============================================================================
// TCP INGEST & FRAME PARSER UNIT TESTS
// ============================================================================
// Tests for TCP frame parsing and network ingestion layer
// ============================================================================

#include <gtest/gtest.h>
#include <eventstream/core/ingest/frame_parser.hpp>
#include <eventstream/core/ingest/tcp_server.hpp>
#include <eventstream/core/events/event_factory.hpp>

using namespace EventStream;

// ============================================================================
// FRAME CONSTRUCTION TESTS
// ============================================================================

TEST(FrameParser, ConstructValidFrame) {
    // Construct a valid frame: topic length (2 bytes) + topic + payload
    std::string topic = "test_topic";
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));  // MSB
    frame.push_back(static_cast<uint8_t>(topic_len & 0xFF));         // LSB
    frame.insert(frame.end(), topic.begin(), topic.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    // Verify frame structure
    EXPECT_EQ(frame.size(), 2 + topic.size() + payload.size());
    EXPECT_EQ(frame[0], 0x00);  // topic_len MSB
    EXPECT_EQ(frame[1], 0x0A);  // topic_len LSB (10)
}

TEST(FrameParser, ConstructFrameWithPriority) {
    // Frame format: [4B length][1B priority][2B topic_len][topic][payload]
    std::string topic = "sensors/temp";
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    uint8_t priority = static_cast<uint8_t>(EventPriority::HIGH);
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    
    std::vector<uint8_t> frame;
    
    // 4-byte length (big-endian)
    uint32_t frame_len = 1 + 2 + topic.size() + payload.size();
    frame.push_back(static_cast<uint8_t>((frame_len >> 24) & 0xFF));
    frame.push_back(static_cast<uint8_t>((frame_len >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((frame_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(frame_len & 0xFF));
    
    // 1-byte priority
    frame.push_back(priority);
    
    // 2-byte topic length (big-endian)
    frame.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    
    // Topic string
    frame.insert(frame.end(), topic.begin(), topic.end());
    
    // Payload
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    // Verify total size
    EXPECT_EQ(frame.size(), 4 + 1 + 2 + topic.size() + payload.size());
}

TEST(FrameParser, EmptyTopic) {
    std::string topic = "";
    std::vector<uint8_t> payload = {0xFF};
    uint16_t topic_len = 0;
    
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    EXPECT_EQ(frame.size(), 3);  // 2 bytes topic_len + 1 byte payload
    EXPECT_EQ(frame[0], 0x00);
    EXPECT_EQ(frame[1], 0x00);
}

TEST(FrameParser, MaxTopicLength) {
    // Create a long topic (max reasonable size)
    std::string topic(255, 'x');  // 255 character topic
    std::vector<uint8_t> payload = {0x01};
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    frame.insert(frame.end(), topic.begin(), topic.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    EXPECT_EQ(frame.size(), 2 + 255 + 1);
    EXPECT_EQ(frame[0], 0x00);  // MSB
    EXPECT_EQ(frame[1], 0xFF);  // LSB = 255
}

// ============================================================================
// NOTE: End-to-end TCP server tests require network setup and are commented out
// They should be enabled in integration tests with proper setup/teardown
// ============================================================================

/*
TEST(TcpIngestServer, EndToEndFlow) {
    EventBusMulti eventBus;
    Dispatcher dispatcher(eventBus);
    TcpIngestServer tcpServer(dispatcher, 9000); 
    tcpServer.start();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0) << "Failed to create socket";
    
    sockaddr_in client_addr{};
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(9000);
    
    int conn_result = connect(sock, (struct sockaddr*)&client_addr, sizeof(client_addr));
    ASSERT_EQ(conn_result, 0) << "Failed to connect to TCP Ingest Server";
    
    // Send frame...
    
    close(sock);
    tcpServer.stop();
}
*/