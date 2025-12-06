#include "ingest/tcpingest_server.hpp"
#include "event/EventFactory.hpp"
#include "ingest/tcp_parser.hpp"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>


namespace Ingest {
  
    TcpIngestServer::TcpIngestServer(EventStream::EventBus& bus, int port)
        : IngestServer(bus), serverPort(port), server_fd(-1), isRunning(false), eventBus(bus) {
    }

    TcpIngestServer::~TcpIngestServer() {
        stop();
    }

    void TcpIngestServer::start() {
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
            close(server_fd);
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
        constexpr size_t buffer_size = 4096;
        char buffer[buffer_size];

        while(isRunning.load(std::memory_order_acquire)) {
            ssize_t bytes_received = recv(client_fd, buffer, buffer_size, 0);
            if (bytes_received <= 0) {
                spdlog::info("Client {} disconnected.", client_address);
                break;
            }

            std::vector<uint8_t> raw(buffer, buffer + bytes_received);
            try {
                auto event = EventStream::parseTCP(raw);
                // attach client address to metadata
                event.metadata["client_address"] = client_address;
                eventBus.publishEvent(event);
                spdlog::info("Received {} bytes from {} and published event ID {} with topic {}", bytes_received, client_address, event.id, event.topic);
                for (const auto& byte : event.payload) {
                    spdlog::info("  Payload byte: 0x{:02x}", byte);
                }
            } catch (const std::exception& e) {
                spdlog::warn("Parse error from {}: {} - publishing raw payload", client_address, e.what());
                // Publish raw payload as an event with empty topic
                auto event = EventStream::EventFactory::createEvent(
                    EventStream::EventSourceType::TCP,
                    raw,
                    std::string(),
                    {{"client_address", client_address}},
                    false
                );
                eventBus.publishEvent(event);
                spdlog::info("Received {} bytes from {} and published event ID {} (raw)", bytes_received, client_address, event.id);
            }
        }
        close(client_fd);
        spdlog::info("Closed connection with client {}", client_address);
    }

}
