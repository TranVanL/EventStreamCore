# 06 — Networking & Ingest Layer

> File này cover TCP/UDP ingest, epoll, frame parser, và roadmap networking nâng cao (io_uring, SocketCAN, raw sockets).

---

## Q1: "How does TCP ingest work?"

**Answer:**

> Có 2 implementations:
>
> **1. TcpIngestServer (thread-per-client):**
> - Mỗi connection một thread.
> - Đọc frame từ socket, parse, push vào dispatcher MPSC queue.
> - Đơn giản nhưng không scale với 100k+ connections.
>
> **2. EpollIngestServer (production scale):**
> - 1 accept thread + N worker threads.
> - Mỗi worker chạy `epoll_wait()`.
> - Edge-triggered epoll (`EPOLLET`) với non-blocking sockets.
> - Per-connection buffer trong `connections_` map.
>
> **Code reference:** `src/core/ingest/tcp.cpp`, `src/core/ingest/epoll.cpp`

---

## Q2: "Why epoll instead of thread-per-client?"

**Answer:**

| Aspect | Thread-per-client | epoll |
|--------|-------------------|-------|
| Thread count | O(clients) | O(workers), fixed |
| Memory | Per-client stack (~8MB) | Per-connection buffer (~8KB) |
| Context switch | Nhiều | Ít |
| Scalability | ~10k connections | 100k+ connections |
| Complexity | Thấp | Cao |

> **EventStreamCore:** Có cả 2. `TcpIngestServer` cho simple use case, `EpollIngestServer` cho production scale.

---

## Q3: "Explain the frame parser."

**Answer:**

> Frame parser chuyển byte stream thành `Event` objects.
>
> **Header format:**
> - `type` (1B)
> - `priority` (1B)
> - `topic_len` (2B)
> - `body_len` (4B)
> - `crc32` (4B)
>
> **State machine:** HEADER → PAYLOAD → CHECKSUM.
> 1. Accumulate bytes until đủ header.
> 2. Parse header, validate CRC.
> 3. Đọc topic + body.
> 4. Tạo `Event`, push dispatcher.
>
> **Error handling:** Corrupted frame → drop + log + continue parsing.

**Code reference:** `include/eventstream/core/ingest/frame_parser.hpp`, `src/core/ingest/frame_parser.cpp`

---

## Q4: "How do you handle backpressure from network ingest?"

**Answer:**

> 1. **Dispatcher MPSC queue full:** `tryPush()` return false, ingest thread log warning và drop event.
> 2. **EventBus full:** Dispatcher retry với exponential backoff (max 3 lần), rồi push vào DLQ.
> 3. **Realtime queue full:** Drop oldest/newest vào DLQ.
> 4. **Transactional queue full:** Block producer 100ms, rồi trả false.
>
> **Evidence:**
> - `Dispatcher::tryPush()`: `"[BACKPRESSURE] Dispatcher MPSC queue full, dropping event"`
> - `Dispatcher::dispatchLoop()`: retry với backoff `10 * (1 << retry)` µs.

---

## Q5: "What is edge-triggered epoll and why use it?"

**Answer:**

> **Edge-triggered (EPOLLET):** Chỉ notify khi có thay đổi trạng thái (edge), không notify liên tục khi data còn trong buffer.
>
> **Pros:**
> - Ít epoll events hơn → ít context switch hơn.
> - Bắt buộc non-blocking socket + read until EAGAIN.
>
> **Cons:**
> - Code phức tạp hơn. Nếu không read hết, sẽ miss events.
>
> **EventStreamCore:** `EpollIngestServer` dùng `EPOLLIN | EPOLLET | EPOLLRDHUP`.

---

## Q6: "How does UDP ingest differ from TCP?"

**Answer:**

> **TCP:**
> - Connection-oriented, reliable, ordered.
> - Phù hợp event quan trọng, cần guaranteed delivery.
> - Overhead: connection tracking, kernel buffer.
>
> **UDP:**
> - Connectionless, unreliable, unordered.
> - Dùng `recvmmsg()` để receive batch 16 messages một lần syscall.
> - Phù hợp telemetry, high-frequency sensor data.
> - EventStreamCore UDP server nhận batch rồi parse từng message.

---

## Q7: "What is io_uring and how would you integrate it?"

**Answer:**

> **io_uring** là async I/O API của Linux (kernel 5.1+). Giảm syscall overhead bằng cách submit/receive operations qua shared ring buffer.
>
> **Integration idea:**
> ```cpp
> struct io_uring ring;
> io_uring_setup(1024, &ring);
> // submit accept
> // wait completion
> // submit read for new connection
> // parse frame → push dispatcher
> ```
>
> **Benefits:**
> - Giảm số syscall (accept/read/write trong một ring).
> - Tốt hơn epoll cho high-throughput I/O.
>
> **Trade-off:** Chỉ Linux 5.1+, cần feature-detect và fallback epoll.
>
> **Code reference:** `include/eventstream/net/io_uring_socket.hpp` (roadmap 2.0).

---

## Q8: "What is SocketCAN and why is it relevant?"

**Answer:**

> **SocketCAN** là CAN bus interface trong Linux kernel, dùng socket API thông thường.
>
> **Use case trong automotive:**
> - Nhận CAN frames từ vehicle sensors.
> - Decode signals thành events.
> - Topic = `can/<frame_id>`.
>
> **Code:**
> ```cpp
> int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
> struct canfd_frame frame;
> read(s, &frame, sizeof(frame));
> ```
>
> **EventStreamCore:** `CanIngestServer` (roadmap 2.0) đọc CAN frame và chuyển thành `Event`.

---

## Q9: "How do you prevent DoS via many small frames?"

**Answer:**

> Trong `EpollIngestServer`, mỗi connection có `emptyFrameCount`. Nếu nhận quá nhiều empty frame liên tiếp, đóng connection.
>
> **Other mitigations:**
> - `maxConnections` trong config.
> - Per-connection buffer limit.
> - Non-blocking sockets + timeout.
> - Backpressure drop khi dispatcher full.

---

## Q10: "How would you add TLS to TCP ingest?"

**Answer:**

> **Option 1 — Terminate TLS ở reverse proxy:**
> - Dùng nginx/Envoy terminate TLS, forward plain TCP đến EventStreamCore.
> - Pros: đơn giản, không ảnh hưởng engine.
> - Cons: thêm hop.
>
> **Option 2 — Embed OpenSSL trong TcpIngestServer:**
> - Mỗi connection có SSL context.
> - Read encrypted bytes → SSL_read → plaintext → frame parser.
> - Pros: end-to-end encryption.
> - Cons: CPU overhead, phức tạp hơn, ảnh hưởng latency.
>
> **Recommendation:** Option 1 cho most cases. Option 2 chỉ khi cần end-to-end và có dedicated crypto hardware.

---

## Q11: "Explain TCP congestion control and how it affects ingest."

**Answer:**

> **TCP congestion control:** Throttle sending rate dựa trên network capacity.
>
> **Algorithms:** Reno, CUBIC (Linux default), BBR.
> - **CUBIC:** Window growth dựa trên cubic function, tốt cho high BDP.
> - **BBR:** Model-based, tốt hơn trong lossy networks.
>
> **Impact on EventStreamCore:**
> - Nếu producer gửi nhanh hơn consumer, TCP buffer đầy → producer block.
> - Đây là natural backpressure.
> - Nhưng nếu buffer quá lớn, latency tăng (bufferbloat).
>
> **Tuning:** `SO_RCVBUF`, `TCP_NODELAY` (disable Nagle), `TCP_QUICKACK`.

---

## Q12: "Compare epoll, poll, and select."

**Answer:**

| Aspect | select | poll | epoll |
|--------|--------|------|-------|
| Scalability | O(n) per call | O(n) per call | O(1) per call |
| FD limit | 1024 | Unlimited | Unlimited |
| Event delivery | Copy entire fd set | Copy entire array | Only ready fds |
| Use case | Legacy | Simple | High-scale |

> **epoll internals:** Kernel maintains red-black tree of monitored fds. `epoll_wait` returns only ready fds.
>
> **EventStreamCore:** Dùng epoll cho production scale ingest.

---

## Q13: "What is zero-copy and where can you use it?"

**Answer:**

> **Zero-copy:** Tránh copy data giữa user space và kernel space.
>
> **Techniques:**
> - `sendfile()`: File → socket mà không qua user space.
> - `mmap()`: Map file vào process address space.
> - `splice()`: Move data between pipes/sockets trong kernel.
>
> **EventStreamCore opportunities:**
> - Storage read → network send (nếu có query API).
> - UDP ingest với `recvmmsg` giảm syscall overhead (không hoàn toàn zero-copy nhưng hiệu quả).
>
> **Trade-off:** Zero-copy phức tạp hơn và không phù hợp khi cần transform data.

---

## Q14: "How would you design the frame protocol from scratch?"

**Answer:**

> **Requirements:**
> - Delimit messages trong byte stream.
> - Versioning.
> - Checksum/CRC.
> - Extensibility.
>
> **Current design:**
> ```
> [magic:2][version:1][type:1][priority:1][topic_len:2][body_len:4][topic:N][body:M][crc32:4]
> ```
>
> **Improvements:**
> - Magic bytes để detect framing errors.
> - Version field cho backward compatibility.
> - Length-prefixed để parse nhanh.
> - CRC32 hoặc xxhash.
>
> **Trade-off:** Header lớn hơn → overhead cho small messages. Có thể dùng variable-length integer (varint) để giảm overhead.

---

## Q15: "What is the connection state machine in EpollIngestServer?"

**Answer:**

> **States:**
> 1. **ACCEPT:** Server fd ready, accept new connection.
> 2. **REGISTER:** Add client fd to epoll with EPOLLIN | EPOLLET | EPOLLRDHUP.
> 3. **READ:** Data available, read until EAGAIN.
> 4. **PARSE:** Extract complete frames from buffer.
> 5. **DISPATCH:** Push events to MPSC queue.
> 6. **CLOSE:** EPOLLRDHUP/EAGAIN error/empty frame limit → close fd.
>
> **Edge-triggered nuance:** Must read all available data in one notification, otherwise miss events.

---

## Q16: "How do you handle partial reads and framing?"

**Answer:**

> **Per-connection buffer:** `ConnectionState::buffer` accumulates bytes.
>
> **Process:**
> 1. Append new bytes to buffer.
> 2. Check if buffer has complete header.
> 3. Parse header, get total frame size.
> 4. If buffer has complete frame, extract and parse.
> 5. Repeat until no complete frame.
> 6. Keep remaining bytes for next read.
>
> **Code reference:** `EpollIngestServer::processBuffer()`.

---

## Q17: "What is the difference between level-triggered and edge-triggered epoll?"

**Answer:**

| Aspect | Level-Triggered (default) | Edge-Triggered |
|--------|--------------------------|----------------|
| Notification | While data available | Only on state change |
| Read behavior | Can read partial | Must read until EAGAIN |
| Miss risk | Low | High if not read all |
| CPU usage | Higher (more notifications) | Lower |
| Complexity | Lower | Higher |

> **EventStreamCore:** Dùng edge-triggered để giảm số epoll events.

---

## Q18: "What are raw sockets and when to use them?"

**Answer:**

> **Raw sockets:** Nhận/tạo packets ở link layer hoặc network layer.
>
> ```cpp
> int rs = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
> ```
>
> **Use cases:**
> - Protocol analysis.
> - Custom protocol ingest.
> - Automotive Ethernet (SOME/IP, DDS).
>
> **Trade-off:** Cần root privileges, phức tạp hơn TCP/UDP, platform-dependent.
>
> **EventStreamCore roadmap:** `RawSocket` + `ProtocolParser` trong Phase 7.

---

## Q19: "How do you handle backpressure in UDP?"

**Answer:**

> UDP không có built-in flow control như TCP.
>
> **Strategies:**
> 1. **Application-level rate limit:** Producer gửi với tốc độ controlled.
> 2. **Drop on full:** Nếu dispatcher queue full, drop UDP packets.
> 3. **Sequence numbers:** Detect packet loss, producer có thể adjust rate.
> 4. **Token bucket:** Giới hạn số packets processed per second.
>
> **EventStreamCore:** UDP packets được parse và push dispatcher. Nếu `tryPush` fail, packet bị drop.

---

## Q20: "What is SO_REUSEPORT and why use it?"

**Answer:**

> **SO_REUSEPORT:** Cho phép nhiều sockets bind cùng một port, kernel load balance connections giữa chúng.
>
> **Benefits:**
> - Better CPU utilization across cores.
> - Kernel-level load balancing.
> - No acceptor bottleneck.
>
> **Trade-off:** Connections có thể không evenly distributed nếu một số socket bận.
>
> **EventStreamCore:** Có thể dùng SO_REUSEPORT với multiple accept threads trong future scale-out.

---

## ✅ Enhanced Networking Checklist

- [ ] Giải thích TCP congestion control.
- [ ] So sánh select/poll/epoll.
- [ ] Giải thích zero-copy.
> [ ] Thiết kế frame protocol.
- [ ] Mô tả connection state machine.
- [ ] Xử lý partial reads.
- [ ] So sánh LT vs ET epoll.
- [ ] Biết raw sockets use case.
- [ ] Xử lý UDP backpressure.
- [ ] Giải thích SO_REUSEPORT.
