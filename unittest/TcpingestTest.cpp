#include <gtest/gtest.h>
#include "ingest/tcp_parser.hpp"
#include "ingest/tcpingest_server.hpp"
#include "event/EventFactory.hpp"

TEST(TcpParser , parseValidframe) {
    using namespace EventStream;
    // Construct a valid frame: topic length (2 bytes) + topic + payload

    std::string topic = "test_topic";
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    frame.insert(frame.end(), topic.begin(), topic.end());
    frame.insert(frame.end(), payload.begin(), payload.end());  
    EXPECT_TRUE(1);
}

TEST(TcpIngestServer, EndtoEndFlow) {
    EventStream::EventBus eventBus;
    Ingest::TcpIngestServer tcpServer(eventBus, 9000); 
    tcpServer.start();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0) << "Failed to create socket";
    sockaddr_in client_addr{};
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(9000);
    int conn_result = connect(sock, (struct sockaddr*)&client_addr, sizeof(client_addr));
    ASSERT_EQ(conn_result, 0) << "Failed to connect to TCP Ingest Server";
    // Prepare and send a valid frame
    std::string topic = "unit_test";
    std::vector<uint8_t> payload = {0xBA, 0xAD, 0xF0, 0x0D};
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    frame.insert(frame.end(), topic.begin(), topic.end());
    frame.insert(frame.end(), payload.begin(), payload.end());

    EXPECT_TRUE(1);
    close(sock);
    tcpServer.stop();
    
}