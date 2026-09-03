#pragma once
#include <eventstream/core/ingest/server_base.hpp>


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
    // Accept incoming TCP connections and spawn a new worker thread for each client (temporary solution; consider using a thread pool for better scalability)
    void acceptConnections() override;
    // Function call in a separate thread to handle each client connection 
    void handleClient(int client_fd, std::string client_address);

    // Cleanup thread to return finished client threads to the pool and free resources
    void cleanupFinishedThreads();  // Periodically cleanup finished client threads
    
    // Port of the TCP server to listen on
    int serverPort;
    // Number file descriptor that OS return to follow the socket if any TCP connection is established 
    int server_fd;
    std::atomic<bool> isRunning{false};

    // Thread to listen for incoming connections and spawn worker threads for each client
    std::thread acceptThread;
    
    // Client thread management with cleanup support
    struct ClientThread {
        std::thread thread;
        std::atomic<bool> finished{false};
    };
    std::list<std::unique_ptr<ClientThread>> clientThreads_;  // Use list for efficient removal
    std::mutex clientThreadsMutex_;  // Protect clientThreads_ access
    
    // Use atomic counter for metrics to avoid locking overhead in high-throughput scenarios
    std::atomic<uint64_t> totalConnectionsAccepted_{0};
    std::atomic<uint64_t> activeConnections_{0};
    std::atomic<uint64_t> totalEventsProcessed_{0};
    std::atomic<uint64_t> totalBackpressureDrops_{0};
};
