// epoll_server.cpp — Linux only
#ifndef _WIN32

#include <eventstream/core/ingest/epoll_server.hpp>
#include <eventstream/core/events/event_factory.hpp>
#include <eventstream/core/ingest/frame_parser.hpp>
#include <eventstream/core/ingest/ingest_pool.hpp>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <thread>

#include <spdlog/spdlog.h>

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t   kReadChunk      = 4086;         // bytes per recv() call
static constexpr uint32_t kMaxFrameSize   = 10 * 1024 * 1024; // 10 MB frame limit
static constexpr int      kMaxEmptyFrames = 10;           // DoS guard threshold

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor / Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

EpollIngestServer::EpollIngestServer(Dispatcher& dispatcher,
                                     int port,
                                     int num_workers,
                                     int max_events)
    : IngestServer(dispatcher)
    , serverPort_(port)
    , numWorkers_(num_workers > 0 ? num_workers
                                  : static_cast<int>(std::thread::hardware_concurrency()))
    , maxEvents_(max_events)
{
    // Clamp workers to a sane range
    if (numWorkers_ < 1)  numWorkers_ = 1;
    if (numWorkers_ > 16) numWorkers_ = 16;
}


EpollIngestServer::~EpollIngestServer() noexcept {
    spdlog::info("[DESTRUCTOR] EpollIngestServer being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] EpollIngestServer destroyed successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
// start()
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Flow:
 *  1. Create server socket (non-blocking).
 *  2. Bind + listen.
 *  3. Create one epoll fd per worker thread.
 *  4. Add server_fd to ALL epoll fds so any worker can call accept().
 *     (Alternative: dedicated accept thread - chosen here for simplicity.)
 *  5. Spawn worker threads.
 *  6. Spawn accept thread.
 */
void EpollIngestServer::start() {
    isRunning_.store(true, std::memory_order_release);

    // ── 1. Create server socket ───────────────────────────────────────────────
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        spdlog::error("[EpollServer] Failed to create socket: {}", strerror(errno));
        return;
    }

    // Non-blocking is required for edge-triggered (EPOLLET) epoll
    if (!setNonBlocking(server_fd_)) {
        spdlog::error("[EpollServer] Failed to set server_fd non-blocking");
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    // Allow immediate rebind after restart
    int opt = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // ── 2. Bind + listen ──────────────────────────────────────────────────────
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(serverPort_);

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("[EpollServer] Bind failed on port {}: {}", serverPort_, strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (::listen(server_fd_, SOMAXCONN) < 0) {
        spdlog::error("[EpollServer] Listen failed: {}", strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    // ── 3. Create epoll fds (one per worker) ─────────────────────────────────
    epollFds_.resize(numWorkers_);
    for (int i = 0; i < numWorkers_; ++i) {
        epollFds_[i] = ::epoll_create1(EPOLL_CLOEXEC);
        if (epollFds_[i] < 0) {
            spdlog::error("[EpollServer] epoll_create1 failed: {}", strerror(errno));
            // Cleanup already created ones
            for (int j = 0; j < i; ++j) ::close(epollFds_[j]);
            ::close(server_fd_);
            server_fd_ = -1;
            return;
        }
    }

    // ── 4. Spawn worker threads ───────────────────────────────────────────────
    workerThreads_.reserve(numWorkers_);
    for (int i = 0; i < numWorkers_; ++i) {
        int epoll_fd = epollFds_[i];
        workerThreads_.emplace_back([this, epoll_fd]() {
            EventStream::IngestEventPool::bindToNUMA(-1);
            workerLoop(epoll_fd);
        });
    }

    // ── 5. Spawn accept thread ────────────────────────────────────────────────
    acceptThread_ = std::thread(&EpollIngestServer::acceptConnections, this);

    spdlog::info("[EpollServer] Started on port {} with {} worker threads",
                 serverPort_, numWorkers_);
}

// ─────────────────────────────────────────────────────────────────────────────
// stop()
// ─────────────────────────────────────────────────────────────────────────────

void EpollIngestServer::stop() {
    isRunning_.store(false, std::memory_order_release);

    // Close server socket → unblocks accept() in acceptThread_
    if (server_fd_ != -1) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }

    // Close all epoll fds → unblocks epoll_wait() in all worker threads
    for (int efd : epollFds_) {
        if (efd != -1) ::close(efd);
    }
    epollFds_.clear();

    // Join accept thread
    if (acceptThread_.joinable()) acceptThread_.join();

    // Join all worker threads
    for (auto& t : workerThreads_) {
        if (t.joinable()) t.join();
    }
    workerThreads_.clear();

    // Close all remaining client connections
    {
        std::lock_guard<std::mutex> lk(connectionsMutex_);
        for (auto& [fd, state] : connections_) {
            ::close(fd);
        }
        connections_.clear();
    }

    spdlog::info("[EpollServer] Stopped. Stats: connections={}, events={}, drops={}",
                 totalConnectionsAccepted_.load(),
                 totalEventsProcessed_.load(),
                 totalBackpressureDrops_.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// acceptConnections()  — dedicated accept thread
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Why a dedicated accept thread instead of letting workers call accept()?
 *
 * If multiple workers all watch server_fd with EPOLLIN:
 *  - Multiple workers wake up simultaneously ("thundering herd").
 *  - Only one wins accept(), others return EAGAIN, wasteful.
 *
 * One accept thread + round-robin assignment to worker epoll fds is cleaner.
 */
void EpollIngestServer::acceptConnections() {
    int nextWorker = 0;  // Round-robin index

    while (isRunning_.load(std::memory_order_acquire)) {
        sockaddr_in clientAddr{};
        socklen_t   clientLen = sizeof(clientAddr);

        // accept() blocks here (server_fd_ is blocking, unlike client fds)
        int clientFd = ::accept(server_fd_,
                                reinterpret_cast<sockaddr*>(&clientAddr),
                                &clientLen);
        if (clientFd < 0) {
            if (isRunning_.load(std::memory_order_acquire)) {
                // EINTR: signal interrupted → retry; others: real error
                if (errno != EINTR) {
                    spdlog::error("[EpollServer] accept() failed: {}", strerror(errno));
                }
            }
            continue;
        }

        // Client fd MUST be non-blocking for EPOLLET
        if (!setNonBlocking(clientFd)) {
            spdlog::error("[EpollServer] Failed to set client fd {} non-blocking", clientFd);
            ::close(clientFd);
            continue;
        }

        // Build human-readable address
        std::string address = inet_ntoa(clientAddr.sin_addr);
        address += ":" + std::to_string(ntohs(clientAddr.sin_port));

        // Register connection state BEFORE adding to epoll
        // (worker could see EPOLLIN before we insert into map otherwise)
        {
            std::lock_guard<std::mutex> lk(connectionsMutex_);
            connections_.emplace(clientFd, ConnectionState{});
            connections_[clientFd].clientAddress = address;
        }

        // Add fd to one worker's epoll fd (round-robin load balancing)
        int epoll_fd = epollFds_[nextWorker % numWorkers_];
        if (!epollAdd(epoll_fd, clientFd)) {
            spdlog::error("[EpollServer] epoll_ctl ADD failed for fd {}", clientFd);
            std::lock_guard<std::mutex> lk(connectionsMutex_);
            connections_.erase(clientFd);
            ::close(clientFd);
            continue;
        }

        nextWorker = (nextWorker + 1) % numWorkers_;
        activeConnections_.fetch_add(1, std::memory_order_relaxed);
        totalConnectionsAccepted_.fetch_add(1, std::memory_order_relaxed);
        spdlog::info("[EpollServer] Accepted connection from {} (fd={})", address, clientFd);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// workerLoop()  — runs in each worker thread
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Each worker owns one epoll fd.
 * epoll_wait() blocks until at least one event is ready, then processes them.
 *
 * Event types handled:
 *  EPOLLIN    → data available to read → handleRead()
 *  EPOLLRDHUP → peer closed send side → close connection
 *  EPOLLERR / EPOLLHUP → error → close connection
 */
void EpollIngestServer::workerLoop(int epoll_fd) {
    std::vector<epoll_event> events(maxEvents_);

    spdlog::info("[EpollServer] Worker thread started (epoll_fd={})", epoll_fd);

    while (isRunning_.load(std::memory_order_acquire)) {
        // Block until event(s) arrive, or 100ms timeout (to re-check isRunning_)
        int nEvents = ::epoll_wait(epoll_fd,
                                   events.data(),
                                   maxEvents_,
                                   /*timeout_ms=*/100);

        if (nEvents < 0) {
            if (errno == EINTR) continue;  // Signal interrupted → retry
            if (isRunning_.load(std::memory_order_acquire)) {
                spdlog::error("[EpollServer] epoll_wait error: {}", strerror(errno));
            }
            break;
        }

        // Process each ready event
        for (int i = 0; i < nEvents; ++i) {
            int fd              = events[i].data.fd;
            uint32_t evtFlags   = events[i].events;

            // ── Error or hangup ───────────────────────────────────────────────
            if (evtFlags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                spdlog::info("[EpollServer] Connection closed/error on fd={}", fd);
                closeConnection(fd, epoll_fd);
                continue;
            }

            // ── Data available ────────────────────────────────────────────────
            if (evtFlags & EPOLLIN) {
                bool keep = handleRead(fd, epoll_fd);
                if (!keep) {
                    closeConnection(fd, epoll_fd);
                }
            }
        }
    }

    spdlog::info("[EpollServer] Worker thread stopped (epoll_fd={})", epoll_fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// handleRead()
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Called when epoll signals EPOLLIN (data ready to read).
 *
 * With EPOLLET (edge-triggered), we MUST read ALL available data in a loop
 * until recv() returns EAGAIN/EWOULDBLOCK. Otherwise we miss events.
 *
 * With EPOLLLT (level-triggered, default), a single recv() per EPOLLIN is fine,
 * but edge-triggered is more efficient (fewer epoll_wait wakeups).
 *
 * Returns false if the connection should be closed.
 */
bool EpollIngestServer::handleRead(int client_fd, int epoll_fd) {
    // Retrieve per-connection state under lock
    ConnectionState* state = nullptr;
    {
        std::lock_guard<std::mutex> lk(connectionsMutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) {
            spdlog::warn("[EpollServer] handleRead: unknown fd={}", client_fd);
            return false;
        }
        state = &it->second;
    }

    char temp[kReadChunk];

    // Edge-triggered: drain ALL available data in a loop
    while (true) {
        ssize_t n = ::recv(client_fd, temp, kReadChunk, 0);

        if (n > 0) {
            // Append to accumulation buffer
            state->buffer.insert(state->buffer.end(), temp, temp + n);

            // Parse all complete frames out of the buffer
            processBuffer(client_fd, *state);
        }
        else if (n == 0) {
            // Peer closed connection gracefully
            spdlog::info("[EpollServer] Client {} closed connection (fd={})",
                         state->clientAddress, client_fd);
            return false;
        }
        else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // All available data has been read — normal exit for EPOLLET
                break;
            }
            // Real error
            spdlog::error("[EpollServer] recv() error on fd={}: {}", client_fd, strerror(errno));
            return false;
        }
    }

    return true;  // Keep connection open
}

// ─────────────────────────────────────────────────────────────────────────────
// processBuffer()
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Same length-prefix framing logic as TcpIngestServer::handleClient(),
 * but operates on per-connection state instead of a local variable.
 *
 * Frame format:
 *   [4 bytes big-endian length][<length> bytes payload]
 */
void EpollIngestServer::processBuffer(int client_fd, ConnectionState& state) {
    auto& buf = state.buffer;
    size_t offset = 0;

    while (offset < buf.size()) {
        // Need at least 4 bytes for the length header
        size_t remaining = buf.size() - offset;
        if (remaining < 4) break;

        // Decode big-endian frame length
        uint32_t frameLen =
            (static_cast<uint32_t>(buf[offset    ]) << 24) |
            (static_cast<uint32_t>(buf[offset + 1]) << 16) |
            (static_cast<uint32_t>(buf[offset + 2]) <<  8) |
            (static_cast<uint32_t>(buf[offset + 3])       );

        // ── DoS guard: empty frame ────────────────────────────────────────────
        if (frameLen == 0) {
            state.emptyFrameCount++;
            if (state.emptyFrameCount > kMaxEmptyFrames) {
                spdlog::error("[EpollServer] Too many empty frames from {} - closing (DoS protection)",
                              state.clientAddress);
                // Signal caller to close by clearing buffer and returning
                // closeConnection() will be called by handleRead() returning false
                buf.clear();
                return;
            }
            spdlog::warn("[EpollServer] Zero-length frame from {} ({}/{})",
                         state.clientAddress, state.emptyFrameCount, kMaxEmptyFrames);
            offset += 4;
            continue;
        }
        state.emptyFrameCount = 0;  // Reset on valid frame

        // ── Frame size guard ─────────────────────────────────────────────────
        if (frameLen > kMaxFrameSize) {
            spdlog::error("[EpollServer] Frame from {} exceeds max size ({} bytes) - closing",
                          state.clientAddress, frameLen);
            buf.clear();
            return;
        }

        // ── Wait for full frame ───────────────────────────────────────────────
        if (remaining < 4 + frameLen) break;

        // ── Extract frame ────────────────────────────────────────────────────
        std::vector<uint8_t> fullFrame(buf.begin() + offset,
                                       buf.begin() + offset + 4 + frameLen);
        offset += 4 + frameLen;

        // ── Parse + dispatch ─────────────────────────────────────────────────
        try {
            auto parsed = parseFullFrame(fullFrame);

            std::unordered_map<std::string, std::string> metadata;
            metadata.reserve(2);
            metadata["client_address"] = state.clientAddress;

            auto event = EventStream::IngestEventPool::acquireEvent();
            *event = EventStream::EventFactory::createEvent(
                EventStream::EventSourceType::TCP,
                parsed.priority,
                std::move(parsed.payload),
                std::move(parsed.topic),
                std::move(metadata)
            );

            if (!dispatcher_.tryPush(event)) {
                spdlog::warn("[EpollServer][BACKPRESSURE] Queue full, dropped event {} from {}",
                             event->header.id, state.clientAddress);
                totalBackpressureDrops_.fetch_add(1, std::memory_order_relaxed);
            } else {
                totalEventsProcessed_.fetch_add(1, std::memory_order_relaxed);
                spdlog::debug("[EpollServer] Dispatched frame: {} bytes from {} topic='{}' id={}",
                              4 + frameLen, state.clientAddress, event->topic, event->header.id);
            }
        }
        catch (const std::exception& e) {
            spdlog::warn("[EpollServer] Failed to parse frame from {}: {}",
                         state.clientAddress, e.what());
        }
    }

    // Compact buffer: remove consumed bytes in one shot
    if (offset >= buf.size()) {
        buf.clear();
    } else if (offset > 0) {
        buf.erase(buf.begin(), buf.begin() + offset);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// closeConnection()
// ─────────────────────────────────────────────────────────────────────────────

void EpollIngestServer::closeConnection(int client_fd, int epoll_fd) {
    // Remove from epoll before closing (good practice, not strictly necessary)
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);

    {
        std::lock_guard<std::mutex> lk(connectionsMutex_);
        auto it = connections_.find(client_fd);
        if (it != connections_.end()) {
            spdlog::info("[EpollServer] Closed connection: {} (fd={})",
                         it->second.clientAddress, client_fd);
            connections_.erase(it);
        }
    }

    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
    activeConnections_.fetch_sub(1, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// epollAdd()
// ─────────────────────────────────────────────────────────────────────────────

bool EpollIngestServer::epollAdd(int epoll_fd, int fd) {
    epoll_event ev{};
    ev.events  = EPOLLIN       // Data available to read
               | EPOLLET       // Edge-triggered (more efficient than level-triggered)
               | EPOLLRDHUP;   // Detect peer shutdown without polling
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        spdlog::error("[EpollServer] epoll_ctl ADD failed for fd {}: {}", fd, strerror(errno));
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// setNonBlocking()
// ─────────────────────────────────────────────────────────────────────────────

bool EpollIngestServer::setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

#endif // _WIN32
