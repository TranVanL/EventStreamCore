#include <eventstream/core/ingest/tcp_server.hpp>
#include <eventstream/core/events/event_factory.hpp>
#include <eventstream/core/ingest/frame_parser.hpp>
#include <eventstream/core/ingest/ingest_pool.hpp>
#include <map>


// Helper function to close socket cross-platform
static void closeSocket(int fd) {
    if (fd == -1) return;
    #ifdef _WIN32
        shutdown(fd, SD_BOTH);
        closesocket(fd);
    #else
        shutdown(fd, SHUT_RDWR);
        close(fd);
    #endif
}

TcpIngestServer::TcpIngestServer(Dispatcher& dispatcher, int port)
    : IngestServer(dispatcher), serverPort(port), server_fd(-1) {
}

TcpIngestServer::~TcpIngestServer() noexcept {
    spdlog::info("[DESTRUCTOR] TcpIngestServer being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] TcpIngestServer destroyed successfully");
}

void TcpIngestServer::start() {
    #ifdef _WIN32 
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        spdlog::error("WSAStartup failed");
        return;
    }
    #endif

    isRunning.store(true, std::memory_order_release);
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        spdlog::error("Failed to create socket for TCP Ingest Server");
        return;
    }

    // Enable SO_REUSEADDR to avoid "address already in use" errors
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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

    if (listen(server_fd, SOMAXCONN) < 0) {
        spdlog::error("Failed to listen on socket");
        closeSocket(server_fd);
        server_fd = -1;
        return;
    }

    acceptThread = std::thread(&TcpIngestServer::acceptConnections, this);
    spdlog::info("TCP Ingest Server started on port {}", serverPort);
}

void TcpIngestServer::stop() {
    isRunning.store(false, std::memory_order_release);
    
    // Close server socket to unblock accept()
    if (server_fd != -1) {
        closeSocket(server_fd);
        server_fd = -1;
    }

    // Join accept thread
    if (acceptThread.joinable()) {
        acceptThread.join();
    }

    // Join all client threads
    {
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        for (auto& ct : clientThreads_) {
            if (ct && ct->thread.joinable()) {
                ct->thread.join();
            }
        }
        clientThreads_.clear();
    }

    #ifdef _WIN32
        WSACleanup();
    #endif

    spdlog::info("TCP Ingest Server stopped. Total connections: {}", 
                 totalConnectionsAccepted_.load());
}

void TcpIngestServer::cleanupFinishedThreads() {
    std::lock_guard<std::mutex> lock(clientThreadsMutex_);
    
    auto it = clientThreads_.begin();
    while (it != clientThreads_.end()) {
        if ((*it)->finished.load(std::memory_order_acquire)) {
            if ((*it)->thread.joinable()) {
                (*it)->thread.join();
            }
            it = clientThreads_.erase(it);
        } else {
            ++it;
        }
    }
}

void TcpIngestServer::acceptConnections() {
    constexpr int kCleanupInterval = 10;  // Cleanup every 10 new connections
    int connectionsSinceCleanup = 0;

    while (isRunning.load(std::memory_order_acquire)) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(server_fd, (struct sockaddr*)&clientAddr, &clientLen);

        if (clientFd < 0) {
            if (isRunning.load(std::memory_order_acquire)) {
                spdlog::error("Failed to accept client connection");
            }
            continue;
        }

        std::string clientAddress = inet_ntoa(clientAddr.sin_addr);
        spdlog::info("Accepted TCP connection from {}", clientAddress);

        // Create client thread with finish flag
        auto clientThread = std::make_unique<ClientThread>();
        ClientThread* ctPtr = clientThread.get();

        // Start handler thread with event pool
        clientThread->thread = std::thread([this, clientFd, clientAddress, ctPtr]() {
            // Initialize thread-local event pool for this TCP client handler
            EventStream::IngestEventPool::bindToNUMA(-1);  // TODO: get NUMA node from config
            
            activeConnections_.fetch_add(1, std::memory_order_relaxed);
            handleClient(clientFd, clientAddress);
            activeConnections_.fetch_sub(1, std::memory_order_relaxed);
            ctPtr->finished.store(true, std::memory_order_release);
        });

        {
            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            clientThreads_.push_back(std::move(clientThread));
        }

        totalConnectionsAccepted_.fetch_add(1, std::memory_order_relaxed);

        // Periodic cleanup of finished threads
        if (++connectionsSinceCleanup >= kCleanupInterval) {
            cleanupFinishedThreads();
            connectionsSinceCleanup = 0;
        }
    }
}
    
    void TcpIngestServer::handleClient(int clientFd, std::string clientAddress) {
        constexpr size_t kBufferChunk = 4096;
        constexpr uint32_t kMaxBufferSize = 10 * 1024 * 1024;
        std::vector<uint8_t> clientBuffer;
        clientBuffer.reserve(8192);
        char temp[kBufferChunk];

        while (isRunning.load(std::memory_order_acquire)) {
            ssize_t bytesReceived = recv(clientFd, temp, kBufferChunk, 0);
            if (bytesReceived <= 0) {
                spdlog::info("Client {} disconnected.", clientAddress);
                break;
            }

            clientBuffer.insert(clientBuffer.end(), temp, temp + bytesReceived);

            // Buffer offset optimization: avoid erase+copy operations
            size_t bufferOffset = 0;
            while (bufferOffset < clientBuffer.size()) {
                size_t remaining = clientBuffer.size() - bufferOffset;
                if (remaining < 4) break;

                uint32_t frameLen = (static_cast<uint32_t>(clientBuffer[bufferOffset]) << 24) |
                                    (static_cast<uint32_t>(clientBuffer[bufferOffset + 1]) << 16) |
                                    (static_cast<uint32_t>(clientBuffer[bufferOffset + 2]) << 8) |
                                    (static_cast<uint32_t>(clientBuffer[bufferOffset + 3]));

                if (frameLen == 0) {
                    spdlog::warn("Zero length frame from {} -- skipping 4 bytes", clientAddress);
                    bufferOffset += 4;
                    continue;
                }

                if (frameLen > kMaxBufferSize) {
                    spdlog::error("Frame from {} exceeds max size. Closing connection.", clientAddress);
                    closeSocket(clientFd);
                    return;
                }

                if (remaining < 4 + frameLen) {
                    break;  // Wait for more data
                }

                // Extract frame from buffer
                std::vector<uint8_t> fullFrame(clientBuffer.begin() + bufferOffset,
                                               clientBuffer.begin() + bufferOffset + 4 + frameLen);
                bufferOffset += 4 + frameLen;

                try {
                    auto parsed = parseFullFrame(fullFrame);
                    std::unordered_map<std::string, std::string> metadata;
                    metadata.reserve(2);
                    metadata["client_address"] = clientAddress;

                    // Acquire event from thread-local pool (zero-copy after warmup)
                    auto event = EventStream::IngestEventPool::acquireEvent();
                    *event = EventStream::EventFactory::createEvent(
                        EventStream::EventSourceType::TCP,
                        parsed.priority,
                        std::move(parsed.payload),
                        std::move(parsed.topic),
                        std::move(metadata)
                    );

                    if (!dispatcher_.tryPush(event)) {
                        spdlog::warn("[BACKPRESSURE] Dispatcher queue full, dropped event {} from {}",
                                     event->header.id, clientAddress);
                    } else {
                        spdlog::info("Received frame: {} bytes from {} topic='{}' eventID={}",
                                     4 + frameLen, clientAddress, event->topic, event->header.id);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to parse frame from {}: {}", clientAddress, e.what());
                    continue;
                }
            }

            // Cleanup consumed data from buffer
            if (bufferOffset > 0 && bufferOffset < clientBuffer.size()) {
                clientBuffer.erase(clientBuffer.begin(), clientBuffer.begin() + bufferOffset);
            } else if (bufferOffset >= clientBuffer.size()) {
                clientBuffer.clear();
            }
        }

        closeSocket(clientFd);
        spdlog::info("Closed connection with client {}", clientAddress);
    }


