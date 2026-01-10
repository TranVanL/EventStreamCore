#include "ingest/tcpingest_server.hpp"
#include "event/EventFactory.hpp"
#include "ingest/tcp_parser.hpp"
#include <map>


  
    TcpIngestServer::TcpIngestServer(Dispatcher& dispatcher, int port)
        : IngestServer(dispatcher), serverPort(port), server_fd(-1) {
    }

    TcpIngestServer::~TcpIngestServer() noexcept {
        spdlog::info("[DESTRUCTOR] TcpIngestServer being destroyed...");
        stop();
        spdlog::info("[DESTRUCTOR] TcpIngestServer destroyed successfully");
    }

    void closeSocket(int fd){
        if (fd == -1) return;
        #ifdef _WIN32
            shutdown(fd, SD_BOTH);
            closesocket(fd);
        #else
            shutdown(fd, SHUT_RDWR);
            close(fd);
        #endif
    }

    void TcpIngestServer::start() {
        #ifdef _WIN32 
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
            spdlog::error("WSAStartup failed");
            return;
        }
        #endif

        isRunning.store(true , std::memory_order_release);
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            spdlog::error("Failed to create socket for TCP Ingest Server");
            return;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(serverPort);

        if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            spdlog::error("Failed to bind socket on port {}", serverPort);
            closeSocket(server_fd);
            server_fd = -1;
        }

        if (listen(server_fd, 5) < 0) {
            spdlog::error("Failed to listen on socket");
            closeSocket(server_fd);
            server_fd = -1;
        }

        acceptThread = std::thread(&TcpIngestServer::acceptConnections, this);
        spdlog::info("TCP Ingest Server started on port {}", serverPort);
    }

    void TcpIngestServer::stop() {
        isRunning.store(false , std::memory_order_release);
        if (server_fd != -1) {
            closeSocket(server_fd);
            #ifdef _WIN32
                WSACleanup();
            #endif
            server_fd = -1;
        }

        if (acceptThread.joinable()){
            acceptThread.join();
        }

        // Safely join all client threads with timeout protection
        for (auto& t : clientThreads) {
            if (t.joinable()) {
                // Use timeout to avoid deadlock - threads should exit within reasonable time
                // After shutdown signal, threads should exit their recv() calls
                t.join();
            }
        }
        clientThreads.clear();

        spdlog::info("TCP Ingest Server stopped.");
    }

    void TcpIngestServer::acceptConnections() {
        while (isRunning.load(std::memory_order_acquire)) {
            sockaddr_in clientaddr{};
            socklen_t clientlen = sizeof(clientaddr);
            int client_fd = accept(server_fd, (struct sockaddr*)&clientaddr, &clientlen);
            if (client_fd < 0) {
                if (isRunning.load(std::memory_order_acquire)) {
                    spdlog::error("Failed to accept client connection");
                } 
                continue;
            }
            std::string client_address = inet_ntoa(clientaddr.sin_addr);
            spdlog::info("Accepted connection from {}", client_address); 
            clientThreads.emplace_back(&TcpIngestServer::handleClient , this ,client_fd , client_address);
        }
    }
    
    void TcpIngestServer::handleClient(int client_fd,std::string client_address) {
        constexpr size_t buffer_chunk = 4096;
        constexpr uint32_t MAX_BUFFER_SIZE = 10 * 1024 * 1024;
        std::vector<uint8_t> client_buf;
        client_buf.reserve(8192);
        char temp[buffer_chunk];

        while (isRunning.load(std::memory_order_acquire)) {
            ssize_t bytes_received = recv(client_fd, temp, buffer_chunk, 0);
            if (bytes_received <= 0) {
                spdlog::info("Client {} disconnected.", client_address);
                break;
            }

            client_buf.insert(client_buf.end(), temp, temp + bytes_received);
            
            // Buffer offset optimization: avoid erase+copy operations
            size_t buffer_offset = 0;
            while (buffer_offset < client_buf.size()) {
                size_t remaining = client_buf.size() - buffer_offset;
                if (remaining < 4) break;

                uint32_t frame_len = (static_cast<uint32_t>(client_buf[buffer_offset]) << 24) |
                                     (static_cast<uint32_t>(client_buf[buffer_offset+1]) << 16) |
                                     (static_cast<uint32_t>(client_buf[buffer_offset+2]) << 8) |
                                     (static_cast<uint32_t>(client_buf[buffer_offset+3])); 

                if (frame_len == 0) {
                    spdlog::warn("Zero length frame from {} -- skipping 4 bytes", client_address);
                    buffer_offset += 4;
                    continue;
                }

                if (frame_len > MAX_BUFFER_SIZE) {
                    spdlog::error("Frame from {} exceeds buffer size. Closing connection.", client_address);
                    closeSocket(client_fd);
                    return;
                }

                if (remaining < 4 + frame_len) {
                    break; // wait for more data
                }

                // Zero-copy: use offset into existing buffer instead of copying
                std::vector<uint8_t> full_frame(client_buf.begin() + buffer_offset, 
                                                 client_buf.begin() + buffer_offset + 4 + frame_len);
                buffer_offset += 4 + frame_len;

                try {
                    auto parsed = parseTCPFrame(full_frame);
                    std::unordered_map<std::string,std::string> metadata;
                    metadata.reserve(2);
                    metadata["client_address"] = client_address;
                    auto event = std::make_shared<EventStream::Event>(
                        EventStream::EventFactory::createEvent(
                            EventStream::EventSourceType::TCP,
                            parsed.priority,
                            std::move(parsed.payload),
                            std::move(parsed.topic),
                            std::move(metadata)  // Direct move semantics - zero copy
                        )
                    );
                    dispatcher_.tryPush(event);
                    spdlog::info("Received frame: {} bytes from {} with topic '{}' and eventID {}", 
                                 4 + frame_len, client_address, event->topic, event->header.id);
                } catch (const std::exception &e) {
                    spdlog::warn("Failed to parse frame from {}: {}", client_address, e.what());
                    continue;
                }
            }
            
            // Cleanup consumed data from buffer
            if (buffer_offset > 0 && buffer_offset < client_buf.size()) {
                client_buf.erase(client_buf.begin(), client_buf.begin() + buffer_offset);
            } else if (buffer_offset >= client_buf.size()) {
                client_buf.clear();
            }
        }

        closeSocket(client_fd);
        spdlog::info("Closed connection with client {}", client_address);
    }


