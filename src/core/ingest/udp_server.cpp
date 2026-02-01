#include <eventstream/core/ingest/udp_server.hpp>
#include <eventstream/core/events/event_factory.hpp>
#include <eventstream/core/ingest/frame_parser.hpp>
#include <eventstream/core/ingest/ingest_pool.hpp>
#include <map>


// Helper function to close socket cross-platform
static void closeSocket(int fd) {
    if (fd == -1) return;
    #ifdef _WIN32
        closesocket(fd);
    #else
        close(fd);
    #endif
}

UdpIngestServer::UdpIngestServer(Dispatcher& dispatcher, int port, size_t buffer_size)
    : IngestServer(dispatcher), serverPort(port), server_fd(-1), 
      bufferSize(std::min(buffer_size, MAX_UDP_DATAGRAM)) {
    recvBuffer.resize(bufferSize);
}

UdpIngestServer::~UdpIngestServer() noexcept {
    spdlog::info("[DESTRUCTOR] UdpIngestServer being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] UdpIngestServer destroyed successfully");
}

void UdpIngestServer::start() {
    #ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        spdlog::error("WSAStartup failed");
        return;
    }
    #endif

    isRunning.store(true, std::memory_order_release);
    server_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_fd < 0) {
        spdlog::error("Failed to create socket for UDP Ingest Server");
        return;
    }

    // Enable SO_REUSEADDR to avoid "address already in use" errors
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // Increase receive buffer size for high throughput
    int rcvbuf = 4 * 1024 * 1024;  // 4MB receive buffer
    setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(serverPort);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        spdlog::error("Failed to bind socket on port {}", serverPort);
        closeSocket(server_fd);
        server_fd = -1;
        return;
    }

    receiveThread = std::thread([this]() {
        // Initialize thread-local event pool for UDP receiver thread
        EventStream::IngestEventPool::bindToNUMA(-1);  // TODO: get NUMA node from config
        receiveLoop();
    });
    spdlog::info("UDP Ingest Server started on port {}", serverPort);
}

void UdpIngestServer::stop() {
    isRunning.store(false, std::memory_order_release);

    // Close socket to unblock recvfrom()
    if (server_fd != -1) {
        closeSocket(server_fd);
        server_fd = -1;
    }

    // Join receive thread
    if (receiveThread.joinable()) {
        receiveThread.join();
    }

    #ifdef _WIN32
        WSACleanup();
    #endif

    spdlog::info("UDP Ingest Server stopped. Stats: datagrams={}, events={}, errors={}, drops={}",
                 totalDatagramsReceived_.load(),
                 totalEventsProcessed_.load(),
                 totalParseErrors_.load(),
                 totalBackpressureDrops_.load());
}

void UdpIngestServer::acceptConnections() {
    // For UDP, redirect to receiveLoop (connectionless protocol)
    receiveLoop();
}

void UdpIngestServer::receiveLoop() {
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);

    while (isRunning.load(std::memory_order_acquire)) {
        ssize_t bytesReceived = recvfrom(
            server_fd,
            reinterpret_cast<char*>(recvBuffer.data()),
            recvBuffer.size(),
            0,
            (struct sockaddr*)&clientAddr,
            &clientLen
        );

        if (bytesReceived < 0) {
            if (isRunning.load(std::memory_order_acquire)) {
                #ifdef _WIN32
                int err = WSAGetLastError();
                if (err != WSAEINTR && err != WSAEWOULDBLOCK) {
                    spdlog::warn("recvfrom error: {}", err);
                }
                #else
                if (errno != EINTR && errno != EAGAIN) {
                    spdlog::warn("recvfrom error: {}", strerror(errno));
                }
                #endif
            }
            continue;
        }

        if (bytesReceived == 0) {
            continue;
        }

        totalDatagramsReceived_.fetch_add(1, std::memory_order_relaxed);
        std::string clientAddress = inet_ntoa(clientAddr.sin_addr);

        // Minimum frame: 4 (length) + 1 (priority) + 2 (topic_len) + 1 (min topic) = 8 bytes
        if (bytesReceived < 8) {
            spdlog::warn("Datagram too small ({} bytes) from {}", bytesReceived, clientAddress);
            totalParseErrors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Read frame length (big-endian)
        uint32_t frameLen = (static_cast<uint32_t>(recvBuffer[0]) << 24) |
                            (static_cast<uint32_t>(recvBuffer[1]) << 16) |
                            (static_cast<uint32_t>(recvBuffer[2]) << 8) |
                            (static_cast<uint32_t>(recvBuffer[3]));

        if (frameLen == 0 || 4 + frameLen > static_cast<size_t>(bytesReceived)) {
            spdlog::warn("Invalid frame length {} (datagram size: {}) from {}",
                         frameLen, bytesReceived, clientAddress);
            totalParseErrors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        try {
            auto parsed = parseFrameBody(recvBuffer.data() + 4, frameLen);

            std::unordered_map<std::string, std::string> metadata;
            metadata.reserve(2);
            metadata["client_address"] = clientAddress;

            // Acquire event from thread-local pool (zero-copy after warmup)
            auto event = EventStream::IngestEventPool::acquireEvent();
            *event = EventStream::EventFactory::createEvent(
                EventStream::EventSourceType::UDP,
                parsed.priority,
                std::move(parsed.payload),
                std::move(parsed.topic),
                std::move(metadata)
            );

            if (!dispatcher_.tryPush(event)) {
                spdlog::warn("[BACKPRESSURE] Dispatcher queue full, dropped event {} from {}",
                             event->header.id, clientAddress);
                totalBackpressureDrops_.fetch_add(1, std::memory_order_relaxed);
            } else {
                totalEventsProcessed_.fetch_add(1, std::memory_order_relaxed);
                spdlog::info("Received frame: {} bytes from {} topic='{}' eventID={}",
                             4 + frameLen, clientAddress, event->topic, event->header.id);
            }

        } catch (const std::exception& e) {
            spdlog::warn("Failed to parse datagram from {}: {}", clientAddress, e.what());
            totalParseErrors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
