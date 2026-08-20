# Phase 4 — POSIX IPC & Advanced POSIX Primitives

## Mục tiêu

Hiểu và triển khai POSIX message queues, shared memory, eventfd, timerfd, real-time signals, pipes. Chứng minh "extensive POSIX API experience".

---

## 1. POSIX Message Queue (`mq_*`)

### API

```cpp
#include <mqueue.h>

mqd_t mq = mq_open("/myqueue", O_CREAT | O_RDWR, 0644, &attr);
mq_send(mq, msg, len, prio);
mq_receive(mq, buf, buflen, &prio);
mq_close(mq);
mq_unlink("/myqueue");
```

### Attributes

```cpp
struct mq_attr attr = {0, 10, 256, 0};
// maxmsg = 10, msgsize = 256
```

### Ưu điểm

- Kernel-persisted queue.
- Priority message.
- Cross-process.

### Nhược điểm

- Message size cố định.
- `/dev/mqueue` mount required.

---

## 2. POSIX Shared Memory (`shm_*`)

### API

```cpp
int fd = shm_open("/myshm", O_CREAT | O_RDWR, 0666);
ftruncate(fd, size);
void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

### SPSC Ring Buffer in SHM

```cpp
struct ShmHeader {
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    static constexpr size_t CAPACITY = 1024;
};

// Data nằm ngay sau header
```

### Ưu điểm

- Zero-copy giữa processes.
- Rất nhanh.

### Nhược điểm

- Cần đồng bộ hóa (lock-free ring).
- Không persisted sau reboot.

---

## 3. `eventfd`

### API

```cpp
int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
uint64_t inc = 1;
write(efd, &inc, sizeof(inc));
uint64_t val;
read(efd, &val, sizeof(val));
```

### Use case

- Wake worker thread từ producer.
- Thay thế `pipe` cho event notification.
- Nhanh hơn pipe vì chỉ 8 bytes.

---

## 4. `timerfd`

### API

```cpp
int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
struct itimerspec its;
its.it_value.tv_sec = 1;
its.it_interval.tv_sec = 0;
timerfd_settime(tfd, 0, &its, nullptr);

uint64_t exp;
read(tfd, &exp, sizeof(exp));
```

### Use case

- High-resolution periodic timer.
- Dùng trong batch processor thay vì `sleep_for`.
- Có thể `poll`/`epoll` cùng với socket.

---

## 5. Real-Time Signals (`SIGRTMIN+`)

### API

```cpp
sigset_t set;
sigemptyset(&set);
sigaddset(&set, SIGRTMIN + 1);
sigprocmask(SIG_BLOCK, &set, nullptr);

int sfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
```

### Use case

- Nhận signal có thứ tự, có thể poll.
- Thay thế signal handler truyền thống.

---

## 6. Pipe / FIFO

### API

```cpp
int fds[2];
pipe2(fds, O_NONBLOCK);

// FIFO
mkfifo("/tmp/eventstream.fifo", 0666);
int fd = open("/tmp/eventstream.fifo", O_RDONLY | O_NONBLOCK);
```

### Use case

- Ingest từ shell scripts.
- `echo "event" > /tmp/eventstream.fifo`.

---

## 7. So sánh POSIX IPC

| Mechanism | Latency | Throughput | Cross-Process | Use Case |
|---|---|---|---|---|
| Message Queue | Medium | Medium | Yes | Reliable message passing |
| Shared Memory | Very Low | Very High | Yes | Zero-copy high throughput |
| eventfd | Very Low | High | No | Thread wake |
| timerfd | Very Low | N/A | No | Periodic timer |
| signalfd | Low | Medium | No | Signal handling |
| Pipe | Medium | Medium | Yes | Shell integration |

---

## 8. Integration vào EventStreamCore

### PosixMqIngestServer

```cpp
class PosixMqIngestServer : public IngestServer {
    PosixMessageQueue mq_;
    std::thread worker_;
public:
    void start() override {
        worker_ = std::thread([this]{
            char buf[256];
            while (running_) {
                ssize_t n = mq_.receive(buf, sizeof(buf));
                if (n > 0) {
                    auto evt = parse(buf, n);
                    dispatcher_.tryPush(evt);
                }
            }
        });
    }
};
```

### PosixShmIngestServer

```cpp
class PosixShmIngestServer : public IngestServer {
    PosixSharedMemory shm_;
    ShmSpscRingBuffer* ring_;
public:
    void start() override {
        shm_.open("/eventstream.shm", sizeof(ShmHeader) + dataSize);
        ring_ = new (shm_.ptr()) ShmSpscRingBuffer();
        worker_ = std::thread([this]{
            while (running_) {
                if (auto evt = ring_->pop()) {
                    dispatcher_.tryPush(*evt);
                }
            }
        });
    }
};
```

---

## 9. Interview Q&A

**Q: Khi nào dùng message queue vs shared memory?**
A: MQ cho reliable message passing, dễ dùng. SHM cho zero-copy throughput cao nhưng phải tự đồng bộ.

**Q: eventfd khác pipe chỗ nào?**
A: eventfd chỉ 8 bytes counter, nhanh hơn pipe, dùng cho thread notification.

**Q: timerfd lợi hơn sleep_for?**
A: timerfd dùng monotonic clock, có thể poll/epoll, chính xác hơn.

**Q: signalfd lợi hơn signal handler?**
A: signalfd xử lý signal trong normal thread context, an toàn hơn async-signal context.

---

## 10. References

- `man mq_overview`
- `man shm_overview`
- `man eventfd`
- `man timerfd_create`
- `man signalfd`
- `man pipe`
