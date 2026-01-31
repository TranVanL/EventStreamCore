#pragma once
#include <eventstream/core/ingest/ingest_server.hpp>


#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <unistd.h>
#include <vector>


class UdpIngestServer : public IngestServer {
public:
    UdpIngestServer(Dispatcher& dispatcher, int port, size_t buffer_size = 65536);
    ~UdpIngestServer() noexcept;
    void start() override;
    void stop() override;

private:
    void acceptConnections() override;  // For UDP: redirect to receiveLoop
    void receiveLoop();
    
    int serverPort;
    int server_fd;
    size_t bufferSize;
    std::atomic<bool> isRunning{false};
    std::thread receiveThread;
    
    // Receive buffer (reused to avoid allocation per datagram)
    std::vector<uint8_t> recvBuffer;
    
    // Statistics
    std::atomic<uint64_t> totalDatagramsReceived_{0};
    std::atomic<uint64_t> totalEventsProcessed_{0};
    std::atomic<uint64_t> totalParseErrors_{0};
    std::atomic<uint64_t> totalBackpressureDrops_{0};
    
    // UDP max datagram size
    static constexpr size_t MAX_UDP_DATAGRAM = 65507;
};
