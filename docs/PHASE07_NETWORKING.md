# Phase 7 — Networking: io_uring, SocketCAN, Raw Sockets

## Mục tiêu

Nâng cấp network ingest layer với io_uring, SocketCAN, raw sockets. Chứng minh networking experience ở system level.

---

## 1. io_uring

### Tại sao io_uring?

- Async I/O không cần epoll + read/write syscall.
- Giảm system call overhead.
- Batch submit/completion.

### API cơ bản

```cpp
#include <liburing.h>

struct io_uring ring;
io_uring_queue_init(32, &ring, 0);

struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, fd, buf, len, 0);
io_uring_submit(&ring);

struct io_uring_cqe* cqe;
io_uring_wait_cqe(&ring, &cqe);
int res = cqe->res;
io_uring_cqe_seen(&ring, cqe);
```

### Multi-shot accept

```cpp
io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
```

### Buffer ring (io_uring_buf_ring)

- Pre-register buffers để kernel tự động chọn buffer cho receive.
- Giảm copy, tăng throughput.

---

## 2. SocketCAN

### Setup vcan0

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

### Code mẫu

```cpp
int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
struct ifreq ifr;
strcpy(ifr.ifr_name, "vcan0");
ioctl(s, SIOCGIFINDEX, &ifr);

struct sockaddr_can addr = {};
addr.can_family = AF_CAN;
addr.can_ifindex = ifr.ifr_ifindex;
bind(s, (struct sockaddr*)&addr, sizeof(addr));

struct can_frame frame;
read(s, &frame, sizeof(frame));
```

### CAN FD

```cpp
int enable = 1;
setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable));
struct canfd_frame frame;
```

### Ứng dụng

- Ingest vehicle sensor data từ CAN bus.
- Decode signals theo DBC.

---

## 3. Raw Sockets

### Use case

- Nhận tất cả packets ở layer 2/3.
- Protocol parser tùy chỉnh.

### Code mẫu

```cpp
int rs = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
struct sockaddr_ll sll = {};
sll.sll_family = AF_PACKET;
sll.sll_ifindex = ifr.ifr_ifindex;
sll.sll_protocol = htons(ETH_P_ALL);
bind(rs, (struct sockaddr*)&sll, sizeof(sll));

char buf[2048];
ssize_t n = recvfrom(rs, buf, sizeof(buf), 0, nullptr, nullptr);
```

### Cần root hoặc CAP_NET_RAW

```bash
sudo setcap cap_net_raw+ep ./eventstream
```

---

## 4. Protocol Parser

### Finite State Machine

```cpp
enum class State { HEADER, PAYLOAD, CHECKSUM };

class ProtocolParser {
    State state_ = State::HEADER;
    std::vector<uint8_t> buffer_;
public:
    std::optional<Event> feed(const uint8_t* data, size_t len) {
        buffer_.insert(buffer_.end(), data, data + len);
        switch (state_) {
            case State::HEADER:
                if (buffer_.size() >= 8) {
                    parseHeader();
                    state_ = State::PAYLOAD;
                }
                break;
            // ...
        }
        return std::nullopt;
    }
};
```

### Zero-copy parser

- Parser chỉ trỏ vào buffer, không copy.
- Dùng `std::string_view` hoặc `std::span`.

---

## 5. Integration vào EventStreamCore

### IoUringTcpServer

```cpp
class IoUringTcpServer : public IngestServer {
    struct io_uring ring_;
    int listen_fd_;
public:
    void start() override {
        io_uring_queue_init(256, &ring_, 0);
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        // bind, listen
        submitAccept();
        worker_ = std::thread([this]{ eventLoop(); });
    }

    void eventLoop() {
        while (running_) {
            struct io_uring_cqe* cqe;
            io_uring_wait_cqe(&ring_, &cqe);
            // handle accept/read/write
            io_uring_cqe_seen(&ring_, cqe);
        }
    }
};
```

### SocketCanIngestServer

```cpp
class SocketCanIngestServer : public IngestServer {
    int fd_;
public:
    void start() override {
        fd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        // bind vcan0
        worker_ = std::thread([this]{
            struct canfd_frame frame;
            while (running_) {
                ssize_t n = read(fd_, &frame, sizeof(frame));
                if (n > 0) {
                    auto evt = decodeCanFrame(frame);
                    dispatcher_.tryPush(evt);
                }
            }
        });
    }
};
```

---

## 6. Interview Q&A

**Q: io_uring lợi hơn epoll chỗ nào?**
A: Giảm syscall, batch I/O, hỗ trợ async buffered I/O, buffer rings.

**Q: SocketCAN là gì?**
A: Linux CAN bus interface, cho phép ứng dụng đọc/ghi CAN frames như socket thông thường.

**Q: Raw socket cần quyền gì?**
A: CAP_NET_RAW hoặc root.

**Q: Tại sao dùng FSM cho protocol parser?**
A: Dễ maintain, recover từ partial data, tránh copy.

---

## 7. References

- `man io_uring_setup`
- Linux kernel documentation: can.rst
- `man packet`
- `liburing` examples
