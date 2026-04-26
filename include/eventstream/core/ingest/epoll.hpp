#pragma once

// Epoll is Linux-only
#ifndef _WIN32

#include <eventstream/core/ingest/server_base.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>

#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

/**
 * @class EpollIngestServer
 * @brief High-performance TCP ingest server using Linux epoll.
 *
 * Architecture:
 *   - 1 accept thread:   calls accept(), registers new fds into epoll.
 *   - N worker threads:  each runs epoll_wait() and processes I/O events.
 *   - Per-connection buffer: stored in a map keyed by fd.
 *   - No thread-per-client: scales to 100k+ connections easily.
 *
 * Compared to TcpIngestServer (thread-per-client):
 *   - Much lower thread count (N workers vs N clients).
 *   - Lower memory usage (no per-client stack).
 *   - Better CPU utilization under high connection counts.
 *   - Slightly more complex code, worth it for production scale.
 *
 * MPSC queue (inbound_queue_ inside Dispatcher) is still used:
 *   - Multiple worker threads push parsed events.
 *   - Single Dispatcher thread pops and routes.
 */
class EpollIngestServer : public IngestServer {
public:
    /**
     * @param dispatcher   The dispatcher to push parsed events into.
     * @param port         TCP port to listen on.
     * @param num_workers  Number of epoll worker threads (default = hardware concurrency).
     * @param max_events   Max epoll events per epoll_wait() call.
     */
    EpollIngestServer(Dispatcher& dispatcher,
                      int port,
                      int num_workers = 0,
                      int max_events  = 64);

    ~EpollIngestServer() noexcept;

    void start() override;
    void stop()  override;

private:
    // ── Internal types ────────────────────────────────────────────────────────

    /**
     * Per-connection state stored in the connection map.
     * Each fd has its own accumulation buffer and metadata.
     */
    struct ConnectionState {
        std::vector<uint8_t> buffer;          // Accumulation buffer (like clientBuffer in TcpIngestServer)
        std::string          clientAddress;   // Human-readable "IP:port" for logging
        int                  emptyFrameCount; // DoS guard counter

        ConnectionState() : emptyFrameCount(0) {
            buffer.reserve(8192);
        }
    };

    // ── IngestServer interface ────────────────────────────────────────────────

    /**
     * Accept loop: runs in a dedicated thread.
     * Calls accept() in a loop and registers each new fd into all epoll fds.
     */
    void acceptConnections() override;

    // ── Internal helpers ──────────────────────────────────────────────────────

    /** Worker loop: runs epoll_wait() and dispatches I/O events. */
    void workerLoop(int epoll_fd);

    /**
     * Process inbound data for a single connection.
     * Reads from fd, appends to per-connection buffer, parses complete frames,
     * and pushes events to the dispatcher.
     *
     * @return false if the connection should be closed.
     */
    bool handleRead(int client_fd, int epoll_fd);

    /** Parse all complete frames out of a ConnectionState's buffer. */
    void processBuffer(int client_fd, ConnectionState& state);

    /** Register fd with epoll (EPOLLIN | EPOLLET | EPOLLRDHUP). */
    bool epollAdd(int epoll_fd, int fd);

    /** Remove fd from epoll and close it; remove from connection map. */
    void closeConnection(int client_fd, int epoll_fd);

    /** Set fd to non-blocking mode (required for edge-triggered epoll). */
    static bool setNonBlocking(int fd);

    // ── Configuration ─────────────────────────────────────────────────────────
    int serverPort_;
    int numWorkers_;
    int maxEvents_;

    // ── Runtime state ─────────────────────────────────────────────────────────
    int              server_fd_{-1};
    std::atomic<bool> isRunning_{false};

    std::thread              acceptThread_;
    std::vector<std::thread> workerThreads_;
    std::vector<int>         epollFds_;       // One epoll fd per worker thread

    // ── Per-connection state ───────────────────────────────────────────────────
    std::unordered_map<int, ConnectionState> connections_;
    std::mutex                               connectionsMutex_;

    // ── Statistics ────────────────────────────────────────────────────────────
    std::atomic<uint64_t> totalConnectionsAccepted_{0};
    std::atomic<uint64_t> totalEventsProcessed_{0};
    std::atomic<uint64_t> totalBackpressureDrops_{0};
    std::atomic<uint64_t> activeConnections_{0};
};

#endif // _WIN32
