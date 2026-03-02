# 07 — Networking & Wire Protocol

> **Goal:** Understand the TCP/UDP ingest servers, the binary frame format, CRC-32 computation, and socket lifecycle management.

---

## 1. Network Architecture

```
            Client A ─────TCP────►┌─────────────────────┐
            Client B ─────TCP────►│  TcpIngestServer     │──► Dispatcher::tryPush()
            Client C ─────TCP────►│  (accept + N client  │
                                  │   threads)           │
                                  └─────────────────────┘

            Sensors  ─────UDP────►┌─────────────────────┐
                                  │  UdpIngestServer     │──► Dispatcher::tryPush()
                                  │  (single recvfrom    │
                                  │   thread)            │
                                  └─────────────────────┘
```

---

## 2. Wire Protocol — Binary Frame Format

### 2.1 Frame Layout

```
 ┌──────────┬──────────┬───────────┬────────────┬─────────┐
 │ Length    │ Priority │ Topic Len │ Topic      │ Payload │
 │ (4 bytes)│ (1 byte) │ (2 bytes) │ (variable) │ (rest)  │
 │ big-end. │ 0-4      │ big-end.  │ UTF-8      │ binary  │
 └──────────┴──────────┴───────────┴────────────┴─────────┘
 ◄─── length prefix ──►◄────────── frame body ────────────►
```

| Field | Size | Encoding | Description |
|-------|------|----------|-------------|
| Length | 4 bytes | Big-endian `uint32_t` | Size of the frame body (excludes this field) |
| Priority | 1 byte | `uint8_t` (0–4) | BATCH=0, LOW=1, MEDIUM=2, HIGH=3, CRITICAL=4 |
| Topic Len | 2 bytes | Big-endian `uint16_t` | Length of the topic string |
| Topic | variable | UTF-8 string | Topic name (e.g., `"sensor/temperature"`) |
| Payload | remaining bytes | Raw binary | Event body |

### 2.2 Example Frame (Hex)

```
00 00 00 14                   ← Length: 20 bytes (frame body)
03                            ← Priority: HIGH (3)
00 12                         ← Topic Len: 18 bytes
73 65 6E 73 6F 72 2F          ← "sensor/"
74 65 6D 70 65 72 61 74        ← "temperat"
75 72 65                       ← "ure"
42                            ← Payload: 1 byte (temperature = 66°C)
```

### 2.3 Frame Parser

**File:** `src/core/ingest/frame_parser.cpp`

```cpp
namespace {
inline uint32_t readUint32BE(const uint8_t* data) {
    uint32_t v;
    std::memcpy(&v, data, sizeof(v));
    return ntohl(v);  // network byte order → host byte order
}
inline uint16_t readUint16BE(const uint8_t* data) {
    uint16_t v;
    std::memcpy(&v, data, sizeof(v));
    return ntohs(v);
}
}

ParsedFrame parseFrameBody(const uint8_t* data, size_t len) {
    if (len < 3) throw std::runtime_error("Frame too small");

    uint8_t priorityVal = data[0];
    if (priorityVal > 4) throw std::runtime_error("Invalid priority");

    uint16_t topicLen = readUint16BE(data + 1);
    if (len < 3 + topicLen) throw std::runtime_error("Truncated frame");
    if (topicLen == 0) throw std::runtime_error("Empty topic");

    ParsedFrame result;
    result.priority = static_cast<EventPriority>(priorityVal);
    result.topic = std::string(reinterpret_cast<const char*>(data + 3), topicLen);

    size_t payloadOffset = 3 + topicLen;
    if (payloadOffset < len)
        result.payload.assign(data + payloadOffset, data + len);

    return result;
}

ParsedFrame parseFullFrame(const std::vector<uint8_t>& fullFrame) {
    if (fullFrame.size() < 4) throw std::runtime_error("Missing length prefix");

    uint32_t frameLen = readUint32BE(fullFrame.data());
    if (frameLen != fullFrame.size() - 4)
        throw std::runtime_error("Frame length mismatch");

    return parseFrameBody(fullFrame.data() + 4, frameLen);
}
```

**Why `memcpy` + `ntohl` instead of casting?**  
Casting `(uint32_t*)(data)` is **undefined behavior** on most architectures due to alignment requirements. `memcpy` is guaranteed safe and modern compilers optimize it to a single instruction.

**Why big-endian (network byte order)?**  
Network protocols traditionally use big-endian. `ntohl()` / `ntohs()` handle the conversion to host byte order (little-endian on x86).

---

## 3. TCP Ingest Server

**File:** `src/core/ingest/tcp_server.cpp`

### 3.1 Socket Lifecycle

```
 Constructor:
   socket(AF_INET, SOCK_STREAM) → server_fd
   setsockopt(SO_REUSEADDR | SO_REUSEPORT)
   bind(0.0.0.0:port)
   listen(backlog=128)

 start():
   acceptThread = thread(acceptConnections)

 acceptConnections():
   while (isRunning):
     client_fd = accept(server_fd)     ← blocks
     spawn thread: handleClient(client_fd)
     cleanupFinishedThreads()

 handleClient(client_fd):
   while (isRunning):
     recv(4 bytes) → frame length
     recv(frame_length bytes) → frame body
     parseFrameBody() → ParsedFrame
     IngestEventPool::acquireEvent() → shared_ptr<Event>
     EventFactory::createEvent()
     dispatcher_.tryPush(event)
   close(client_fd)

 stop():
   isRunning = false
   shutdown(server_fd, SHUT_RDWR)      ← wakes accept()
   close(server_fd)
   join acceptThread
   join all clientThreads
```

### 3.2 Per-Client Thread Management

```cpp
struct ClientThread {
    std::thread thread;
    std::atomic<bool> finished{false};
};
std::list<std::unique_ptr<ClientThread>> clientThreads_;
std::mutex clientThreadsMutex_;
```

When a client disconnects, its thread sets `finished = true`. The accept loop periodically calls `cleanupFinishedThreads()` which joins and removes finished client threads.

**Why `std::list` instead of `std::vector`?**  
Inserting/removing from a list doesn't invalidate iterators to other elements. With `std::vector`, removing a thread would shift all subsequent elements, requiring extra bookkeeping.

### 3.3 Frame Reading Loop

```cpp
void handleClient(int client_fd, std::string client_address) {
    uint8_t lenBuf[4];
    while (isRunning.load()) {
        // Read 4-byte length prefix
        ssize_t n = recv(client_fd, lenBuf, 4, MSG_WAITALL);
        if (n <= 0) break;  // disconnect or error

        uint32_t frameLen = ntohl(*(uint32_t*)lenBuf);
        if (frameLen > 1048576) break;  // 1MB sanity limit

        // Read frame body
        std::vector<uint8_t> frameBuf(frameLen);
        n = recv(client_fd, frameBuf.data(), frameLen, MSG_WAITALL);
        if (n <= 0) break;

        // Parse and dispatch
        auto parsed = parseFrameBody(frameBuf.data(), frameLen);
        auto evt = IngestEventPool::acquireEvent();
        *evt = EventFactory::createEvent(...);
        dispatcher_.tryPush(evt);
    }
    close(client_fd);
}
```

**`MSG_WAITALL`** — tells `recv()` to block until exactly N bytes are received (or connection closes). This simplifies the frame-reading logic by avoiding partial reads.

### 3.4 Graceful Shutdown

```cpp
void TcpIngestServer::stop() {
    isRunning.store(false);
    if (server_fd >= 0) {
        shutdown(server_fd, SHUT_RDWR);  // unblocks accept()
        close(server_fd);
        server_fd = -1;
    }
    if (acceptThread.joinable()) acceptThread.join();
    // Join all client threads
    std::lock_guard<std::mutex> lock(clientThreadsMutex_);
    for (auto& ct : clientThreads_) {
        if (ct->thread.joinable()) ct->thread.join();
    }
}
```

**`shutdown(SHUT_RDWR)`** closes the socket for both reading and writing, which causes any blocked `accept()` or `recv()` to return immediately with an error — the cleanest way to unblock I/O threads.

---

## 4. UDP Ingest Server

**File:** `src/core/ingest/udp_server.cpp`

### 4.1 Differences from TCP

| Aspect | TCP | UDP |
|--------|-----|-----|
| Connection | Connection-oriented (accept + per-client thread) | Connectionless (single recvfrom thread) |
| Frame boundary | Length-prefixed (application layer) | Datagram boundary (kernel provides) |
| Reliability | Guaranteed delivery | Best-effort |
| Thread model | N client threads | 1 receive thread |
| Max message | Unlimited (stream) | 65507 bytes (UDP max) |

### 4.2 Receive Loop

```cpp
void UdpIngestServer::receiveLoop() {
    sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (isRunning.load()) {
        ssize_t n = recvfrom(server_fd, recvBuffer.data(), bufferSize,
                             0, (sockaddr*)&client_addr, &addr_len);
        if (n <= 0) continue;

        // Each datagram IS a frame (no length prefix needed)
        auto parsed = parseFrameBody(recvBuffer.data(), n);
        auto evt = IngestEventPool::acquireEvent();
        *evt = EventFactory::createEvent(EventSourceType::UDP, ...);
        dispatcher_.tryPush(evt);
    }
}
```

**Why no length prefix?** UDP preserves datagram boundaries — each `recvfrom()` returns exactly one complete datagram as sent by the client. The length is known from the return value.

---

## 5. CRC-32 Computation

**File:** `src/core/events/event_factory.cpp`

### 5.1 Table-Based Algorithm

```cpp
uint32_t EventFactory::calculateCRC32(const std::vector<uint8_t>& data) {
    static uint32_t CRC32_TABLE[256];
    static std::once_flag init_flag;

    std::call_once(init_flag, []() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                if (crc & 1)
                    crc = (crc >> 1) ^ 0xEDB88320;  // reversed polynomial
                else
                    crc >>= 1;
            }
            CRC32_TABLE[i] = crc;
        }
    });

    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : data) {
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}
```

### 5.2 How CRC-32 Works

1. **Polynomial:** `0xEDB88320` is the bit-reversed form of `0x04C11DB7` (the CRC-32 polynomial used in Ethernet, PKZIP, PNG).

2. **Table generation:** Pre-computes the CRC for each possible byte value (0–255). This turns an 8-iteration loop into a single table lookup.

3. **Processing:** For each input byte:
   - XOR with the low byte of the current CRC
   - Look up the result in the table
   - XOR with the shifted CRC

4. **Final XOR:** Invert all bits (standard CRC-32 convention).

**Performance:** ~3 GB/s on modern CPUs (1 clock cycle per byte with pipelining). Hardware CRC-32C (SSE 4.2) would be faster but uses a different polynomial.

### 5.3 Thread Safety

`std::call_once` ensures the table is initialized exactly once. After initialization, the table is read-only → no synchronization needed.

---

## 6. Socket Options Explained

### 6.1 `SO_REUSEADDR`

```cpp
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

Allows rebinding to a port that's in `TIME_WAIT` state (common after crash/restart). Without this, you'd get "Address already in use" for ~60 seconds after a restart.

### 6.2 `SO_REUSEPORT`

```cpp
setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
```

Allows multiple processes to bind to the same port. The kernel load-balances incoming connections across them. Useful for multi-process servers (not currently used in this project, but set for future extensibility).

### 6.3 `listen()` Backlog

```cpp
listen(server_fd, 128);
```

The backlog is the maximum number of pending connections in the kernel's accept queue. 128 is a common default. If the queue is full, new SYN packets are dropped (client sees timeout).

---

## 7. Error Handling Philosophy

| Error Type | Handling |
|-----------|---------|
| `recv()` returns 0 | Clean disconnect → close client, log |
| `recv()` returns -1 | Error → close client, log `errno` |
| Frame too small | `throw runtime_error` → caught in client handler, close connection |
| Invalid priority | `throw runtime_error` → same |
| Frame length mismatch | `throw runtime_error` → same |
| Dispatcher full | `tryPush()` returns false → log backpressure, continue |

**Design choice:** Invalid frames close the connection. This prevents malformed data from corrupting the pipeline. Legitimate clients must implement the protocol correctly.

---

## 8. Comparison with Production Approaches

| Feature | This Project | Production Alternative |
|---------|-------------|----------------------|
| I/O model | Blocking (thread-per-client) | `epoll` / `io_uring` (event-driven) |
| Thread pool | Unbounded client threads | Fixed thread pool with connection queue |
| TLS | Not implemented | OpenSSL / BoringSSL |
| Framing | Custom binary | Protocol Buffers / FlatBuffers |
| Discovery | Manual IP:port | Service mesh (Consul, Envoy) |
| Backpressure | Application-level drop | TCP window / gRPC flow control |

**Interview point:** The blocking model is simpler but doesn't scale beyond ~10K concurrent connections (C10K problem). For production, you'd use `epoll` (Linux), `kqueue` (macOS), or `io_uring` (modern Linux) to handle thousands of connections per thread.

---

## 9. Interview Questions

**Q1: Why use `ntohl()` / `ntohs()` instead of just reading bytes directly?**  
A: Network byte order is big-endian; most modern CPUs are little-endian. `ntohl()` swaps the bytes. On a big-endian machine, it's a no-op.

**Q2: What is the maximum UDP datagram size?**  
A: 65507 bytes (65535 IP payload − 8 UDP header = 65527, but in practice the constant `MAX_UDP_DATAGRAM = 65507` accounts for the IP header too). Larger messages must be fragmented at the application layer.

**Q3: What happens if a client sends a partial TCP frame?**  
A: `MSG_WAITALL` on `recv()` blocks until the full requested length is received. If the client disconnects mid-frame, `recv()` returns 0 or -1, and the handler closes the connection. No partial frames enter the pipeline.

**Q4: Why is the CRC-32 table `static` inside the function?**  
A: Lazy initialization — the table is created on first use. Combined with `call_once`, this is thread-safe and avoids static initialization order issues.

**Q5: How would you evolve this to handle 100K concurrent connections?**  
A: Replace per-client threads with `epoll`: one thread monitors all client file descriptors, dispatches readable events to a thread pool. Add connection-level timeouts and rate limiting. Consider `io_uring` for zero-copy receive on Linux 5.1+.

---

## 10. Deep Dive: TCP State Machine and Connection Lifecycle

### 10.1 Three-Way Handshake

```
Client                    Server
  │── SYN ──────────────────►│   Client: SYN_SENT
  │                          │   Server: SYN_RECEIVED
  │◄──── SYN+ACK ───────────│
  │                          │
  │── ACK ──────────────────►│   Both: ESTABLISHED
```

The `accept()` call returns a new file descriptor only after the handshake completes. The `listen(128)` backlog determines how many completed handshakes can queue before `accept()` is called.

### 10.2 Connection Teardown (Four-Way)

```
Client                    Server
  │── FIN ──────────────────►│   Client: FIN_WAIT_1
  │◄──── ACK ────────────────│   Client: FIN_WAIT_2
  │                          │   Server: CLOSE_WAIT
  │◄──── FIN ────────────────│   Server: LAST_ACK
  │── ACK ──────────────────►│   Both: CLOSED (client enters TIME_WAIT)
```

**TIME_WAIT lasts 2×MSL (60 seconds on Linux).** During this period, the port is "in use." This is why `SO_REUSEADDR` is essential — without it, restarting the server within 60 seconds would fail with "Address already in use."

### 10.3 Half-Open Connections

If a client crashes without sending FIN (network failure, `kill -9`), the server sees no activity. Without TCP keepalive, the server holds the connection forever, wasting a thread.

**Detection:** The project relies on `recv()` eventually returning 0 or -1 when the kernel detects the dead peer (via keepalive or reset). A production system would add `SO_KEEPALIVE` with short intervals:

```cpp
int keepalive = 1;
int idle = 10;      // Start keepalive after 10s idle
int interval = 5;   // Probe every 5s
int count = 3;      // Drop after 3 failed probes
setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
```

---

## 11. Deep Dive: Nagle's Algorithm and TCP_NODELAY

### 11.1 What Nagle Does

Nagle's algorithm batches small writes: if a previous segment is unacknowledged, buffer new data until either (a) the previous ACK arrives or (b) enough data accumulates for a full MSS (Maximum Segment Size, ~1460 bytes).

```
Without TCP_NODELAY (Nagle enabled):
  write(7 bytes)  → buffered (waiting for ACK of previous data)
  ... 200ms later: ACK arrives → sends the 7 bytes

With TCP_NODELAY:
  write(7 bytes)  → sent immediately as a small TCP segment
```

### 11.2 Nagle + Delayed ACK = Latency Disaster

**Delayed ACK:** TCP receivers wait up to 200ms before sending an ACK, hoping to piggyback it on response data.

**Combined effect:**
1. Client sends 7-byte event frame → Nagle buffers it.
2. Server's TCP delays ACK for 200ms.
3. After 200ms, server sends ACK → client's Nagle releases the buffer.
4. **Result:** Every event has 200ms of artificial latency.

`TCP_NODELAY` disables Nagle, eliminating this interaction. The project sets it because event frames are typically small (< 1 KB) and latency matters.

---

## 12. Deep Dive: `epoll` vs Thread-Per-Connection

### 12.1 Thread-Per-Connection (Current Design)

```
accept() → spawn thread → recv() loop → close()
         → spawn thread → recv() loop → close()
         → spawn thread → recv() loop → close()
```

**Pros:** Simple, each thread has its own stack and context.  
**Cons:**
- 10K connections = 10K threads × 8MB stack = 80GB virtual memory.
- Context switch cost: ~2μs per switch × 10K threads = significant overhead.
- Cache pollution: each context switch flushes L1/L2 caches.

### 12.2 epoll Event Loop

```cpp
int epfd = epoll_create1(0);
struct epoll_event ev;
ev.events = EPOLLIN;
ev.data.fd = client_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

while (true) {
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);
    for (int i = 0; i < nfds; i++) {
        if (events[i].data.fd == server_fd)
            accept_new_client();
        else
            handle_client_data(events[i].data.fd);
    }
}
```

**One thread handles all connections.** `epoll_wait` returns only the file descriptors that have data ready. No blocked threads, no stack waste, no unnecessary context switches.

| Aspect | Thread-per-client | epoll |
|--------|------------------|-------|
| Connections | ~1K (stack limits) | 100K+ |
| Memory per conn | 8 MB (stack) | ~200 bytes (epoll entry) |
| Context switches | O(N) per second | O(active) per second |
| Code complexity | Simple | Moderate (state machine per conn) |

### 12.3 io_uring (Modern Linux 5.1+)

Even more efficient than `epoll`: submissions and completions are exchanged via shared ring buffers between user space and kernel — zero syscalls in the hot path. The kernel processes I/O asynchronously. This is the state-of-the-art for high-performance networking on Linux.

---

## 13. Deep Dive: Kernel TCP Buffer Mechanics

### 13.1 Buffer Flow

```
Application write() → Socket send buffer (SO_SNDBUF)
  → TCP segmentation → IP fragmentation → NIC TX queue → wire

wire → NIC RX queue → IP reassembly → TCP reassembly
  → Socket receive buffer (SO_RCVBUF) → Application recv()
```

### 13.2 Backpressure via TCP Window

```
Sender                           Receiver
  │                                │
  │── DATA (1000 bytes) ─────────►│  rcv_buf: 1000/65536 used
  │◄──── ACK, window=64536 ──────│  "I can accept 64536 more bytes"
  │                                │
  │── DATA (64000 bytes) ────────►│  rcv_buf: 65000/65536 used
  │◄──── ACK, window=536 ────────│  "Almost full!"
  │                                │
  │── DATA (536 bytes) ──────────►│  rcv_buf: 65536/65536 FULL
  │◄──── ACK, window=0 ──────────│  "STOP! I'm full!"
  │                                │
  │   (sender BLOCKS on write())  │  Application hasn't read data
```

**This is how backpressure propagates from the application to the network.** When the EventStreamCore pipeline is overloaded:
1. Dispatcher stops reading MPSC queue (PAUSED state).
2. Ingest threads stop reading from sockets.
3. Kernel receive buffer fills.
4. TCP advertises window=0.
5. Remote sender blocks on `write()`.
6. End-to-end backpressure without a single byte lost.

### 13.3 SO_RCVBUF Sizing

```cpp
int buf_size = 1048576;  // 1 MB
setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
```

Default receive buffer is typically 87KB. For burst absorption, the project sets 1MB. With 1MB buffer and 200-byte events, the kernel can buffer ~5000 events before backpressure kicks in. At 1M events/sec, this is 5ms of buffer — enough to absorb micro-bursts.

**Note:** Linux doubles the value set by `setsockopt` (for bookkeeping overhead), so setting 1MB actually gives ~2MB.

---

## 14. Extended Interview Questions

**Q6: What is the difference between `SO_REUSEADDR` and `SO_REUSEPORT`?**  
A: `SO_REUSEADDR` allows binding to a port in TIME_WAIT state (one process). `SO_REUSEPORT` allows multiple processes/threads to bind to the same port simultaneously — the kernel load-balances incoming connections across them. The project sets `SO_REUSEADDR` for fast restart; `SO_REUSEPORT` is set for future multi-process scaling.

**Q7: What is the "C10K problem" and how does it relate to this project?**  
A: The C10K problem (1999, Dan Kegel): how to handle 10,000 concurrent connections on one server. Thread-per-connection fails due to stack memory and context switching overhead. Solutions: `select` (O(N) per call), `poll` (same), `epoll` (O(active)), `io_uring` (zero-syscall). The project's thread-per-connection model handles hundreds of connections but would need `epoll` for thousands.

**Q8: Explain the difference between `read()` and `recv()` on a TCP socket.**  
A: `read()` is a generic file descriptor operation. `recv()` is socket-specific and supports flags like `MSG_WAITALL`, `MSG_PEEK`, `MSG_DONTWAIT`. The project uses `recv()` with `MSG_WAITALL` to ensure complete frame reads without manual reassembly.

**Q9: What is TCP head-of-line blocking?**  
A: TCP delivers data in order. If segment 3 is lost, segments 4, 5, 6 are buffered but not delivered until 3 is retransmitted and received. This adds latency to all events after the loss. UDP avoids this (each datagram is independent) at the cost of no delivery guarantee. For latency-critical events, the UDP path avoids head-of-line blocking.

**Q10: Why does CRC-32 use the reflected polynomial `0xEDB88320` instead of `0x04C11DB7`?**  
A: `0x04C11DB7` is the "normal" (MSB-first) polynomial. `0xEDB88320` is the "reflected" (LSB-first) version — bit-reversed. The reflected algorithm processes data LSB-first, which matches how bytes are stored in memory on little-endian CPUs (x86). This avoids explicit bit-reversal of input/output, making the implementation simpler and faster.
