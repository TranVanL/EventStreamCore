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
#include <cctype>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <list>
#include <mutex>


class TcpIngestServer : public IngestServer {
public:
    TcpIngestServer(Dispatcher& dispatcher, int port);
    ~TcpIngestServer() noexcept;
    void start() override;
    void stop() override;

private:
    void acceptConnections() override;
    void handleClient(int client_fd, std::string client_address);
    void cleanupFinishedThreads();  // Periodically cleanup finished client threads
    
    int serverPort;
    int server_fd;
    std::atomic<bool> isRunning{false};
    std::thread acceptThread;
    
    // Client thread management with cleanup support
    struct ClientThread {
        std::thread thread;
        std::atomic<bool> finished{false};
    };
    std::list<std::unique_ptr<ClientThread>> clientThreads_;  // Use list for efficient removal
    std::mutex clientThreadsMutex_;  // Protect clientThreads_ access
    
    // Statistics
    std::atomic<uint64_t> totalConnectionsAccepted_{0};
    std::atomic<uint64_t> activeConnections_{0};
    std::atomic<uint64_t> totalEventsProcessed_{0};
    std::atomic<uint64_t> totalBackpressureDrops_{0};
};
