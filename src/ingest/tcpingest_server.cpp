#include "ingest/tcpingest_server.hpp"
#include "event/EventFactory.hpp"
#include "ingest/tcp_parser.hpp"

namespace Ingest {
  
    TcpIngestServer::TcpIngestServer(EventStream::EventBus& bus, int port)
        : IngestServer(bus), serverPort(port), server_fd(-1), isRunning(false), eventBus(bus) {
    }

    TcpIngestServer::~TcpIngestServer() {
        stop();
    }

    void TcpIngestServer::start() {

        // WINDOW 
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
            close(server_fd);
            return;
        }

        if (listen(server_fd, 5) < 0) {
            throw std::runtime_error("Failed to listen on socket");
        }

        acceptThread = std::thread(&TcpIngestServer::acceptConnections, this);
        spdlog::info("TCP Ingest Server started on port {}", serverPort);
    }

    void TcpIngestServer::stop() {
        isRunning.store(false , std::memory_order_release);
        if (server_fd != -1) {

            #ifdef _WIN32
            shutdown(server_fd, SD_BOTH);
            #else 
            shutdown(server_fd, SHUT_RDWR);
            #endif
            server_fd = -1;
        }
        if (acceptThread.joinable()){
            acceptThread.join();
        }

        for (auto& t : clientThreads) {
            if (t.joinable()) {
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

            client_buf.insert(client_buf.end() ,temp ,temp + bytes_received);
            while(1) {
                if (client_buf.size() < 4) break;

                uint32_t frame_len = (static_cast<uint32_t>(client_buf[0]) << 24) |
                                 (static_cast<uint32_t>(client_buf[1]) << 16) |
                                 (static_cast<uint32_t>(client_buf[2]) << 8) |
                                 (static_cast<uint32_t>(client_buf[3])); 

                if (frame_len == 0) {
                    spdlog::warn("Zero length frame from {} -- skipping 4 bytes",client_address);
                    client_buf.erase(client_buf.begin(), client_buf.begin() +4);
                    continue;
                }

                if (frame_len > MAX_BUFFER_SIZE) {
                    spdlog::error("Frame from {} out of Buffer Size . Closing connection " , client_address);
                    close(client_fd);
                    return;
                }

                if (client_buf.size() < 4 + frame_len) {
                    break;
                    // Wait more data 
                }

                std::vector<uint8_t> full_frame;
                full_frame.reserve(4 + frame_len);
                full_frame.insert(full_frame.end(), client_buf.begin()  , client_buf.begin() + 4 + frame_len);
                client_buf.erase(client_buf.begin(), client_buf.begin() + 4 + frame_len);

                try {
                    auto parsed = parseTCPFrame(full_frame);
                    auto event= EventStream::EventFactory::createEvent(
                        EventStream::EventSourceType::TCP,
                        parsed.payload,
                        parsed.topic,
                        {{"client_address",client_address}}
                    );
                    eventBus.publishEvent(event);
                    spdlog::info("Receive frame : {} byte from {} with topic '{}' and eventID {}" ,4 + frame_len , client_address , event.topic , event.header.id);
                    
                } catch (const std::exception &e) {
                    spdlog::warn("Fail parse frame from {} : {}",client_address , e.what() );
                    continue;
                }
                

            }
            
        }

        close(client_fd);
        spdlog::info("Closed connection with client {}", client_address);
    }

}
