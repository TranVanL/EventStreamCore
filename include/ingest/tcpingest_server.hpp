#pragma once
#include "ingest_server.hpp"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <unistd.h>
#include <cctype>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace Ingest {

    class TcpIngestServer : public IngestServer {
    public:
        TcpIngestServer(EventStream::EventBus& bus, int port);
        ~TcpIngestServer();
        void start() override;
        void stop() override;
    
    private:
        void acceptConnections() override;
        void handleClient(int client_fd , std::string client_address);
        
        int serverPort;
        int server_fd;
        std::atomic<bool> isRunning;
        std::thread acceptThread;
        EventStream::EventBus& eventBus;
        std::vector<std::thread> clientThreads;
    };
}