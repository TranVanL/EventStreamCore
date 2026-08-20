# Phase 3 — RTOS / QNX Portability Layer

## Mục tiêu

Tạo abstraction layer để EventStreamCore có thể compile và chạy trên cả Linux và QNX. Hiểu QNX Neutrino message passing, resource manager, và cách viết portable code.

---

## 1. Tại sao QNX?

- QNX là microkernel RTOS phổ biến trong automotive (AAOS, infotainment, ADAS).
- Cung cấp determinism, safety certification (QNX SDP 8.0 hướng đến ASIL D).
- JD senior embedded/RTOS thường yêu cầu QNX experience.

---

## 2. QNX vs Linux Key Differences

| Feature | Linux | QNX |
|---|---|---|
| Kernel | Monolithic | Microkernel |
| IPC | sockets, pipes, mq | message passing (Channel/Connect) |
| Drivers | in-kernel | user-space resource managers |
| Scheduling | SCHED_FIFO | adaptive partition + FIFO/RR |
| Timers | timerfd | timer_create + ClockCycles |
| Interrupts | top/bottom half | ISR + pulse to thread |

---

## 3. Policy-Based Templates

### Tại sao không dùng virtual inheritance?

- Virtual function call có overhead (indirect branch, vtable).
- Trên hot path lock-free queue, overhead này không chấp nhận được.
- Policy-based templates compile-time dispatch, zero overhead.

### Code mẫu

```cpp
template<typename Platform>
class Thread {
public:
    using Handle = typename Platform::ThreadHandle;
    static bool create(Handle& out, void* (*fn)(void*), void* arg) {
        return Platform::threadCreate(out, fn, arg);
    }
};

// Linux
struct LinuxPlatform {
    using ThreadHandle = pthread_t;
    static bool threadCreate(pthread_t& out, void* (*fn)(void*), void* arg) {
        return pthread_create(&out, nullptr, fn, arg) == 0;
    }
};

// QNX
typedef LinuxPlatform QnxPlatform; // QNX cũng hỗ trợ pthread
```

---

## 4. QNX Neutrino Message Passing

### Core Concepts

- **Channel**: server tạo channel để nhận message.
- **Connection**: client tạo connection tới channel.
- **MsgSend**: client gửi message, block cho đến khi server reply.
- **MsgReceive**: server nhận message.
- **MsgReply**: server trả lời.

### Code mẫu

```cpp
// Server
int chid = ChannelCreate(0);
struct _msg_info info;
char buf[256];
int rcvid = MsgReceive(chid, buf, sizeof(buf), &info);
// process buf
MsgReply(rcvid, 0, reply, sizeof(reply));

// Client
int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
MsgSend(coid, msg, sizeof(msg), reply, sizeof(reply));
```

### Ưu điểm

- Synchronous IPC có thể chuyển qua kernel rất nhanh.
- Priority inheritance tự động qua message passing.

---

## 5. QNX Resource Manager

### Khái niệm

- Resource manager là user-space driver trong QNX.
- Tạo device path như `/dev/eventstream`.
- Xử lý `open`, `read`, `write`, `close` qua callback.

### Code mẫu

```cpp
resmgr_attr_t resmgr_attr;
iofunc_attr_t iofunc_attr;
iofunc_funcs_t iofunc_funcs;
resmgr_connect_funcs_t connect_funcs;
resmgr_io_funcs_t io_funcs;

iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                 _RESMGR_IO_NFUNCS, &io_funcs);
io_funcs.read = eventstream_read;
iofuncs.write = eventstream_write;

iofunc_attr_init(&iofunc_attr, S_IFCHR | 0666, nullptr, nullptr);
resmgr_attach(dpp, &resmgr_attr, "/dev/eventstream",
              _FTYPE_ANY, _RESMGR_FLAG_BEFORE,
              &connect_funcs, &io_funcs, &iofunc_attr);
```

---

## 6. QNX Interrupt Handling

### Pattern

1. ISR (Interrupt Service Routine) chạy ở kernel space, rất ngắn.
2. ISR gửi pulse tới user-space thread.
3. User-space thread xử lý event.

### Code mẫu

```cpp
const struct sigevent* isr(void* area, int id) {
    // gửi pulse
    return &event;
}

int id = InterruptAttach(irq, isr, nullptr, 0, 0);
```

---

## 7. Platform Detection

```cpp
#if defined(__QNX__) || defined(__QNXNTO__)
    #define ESC_PLATFORM_QNX
    using CurrentPlatform = QnxPlatform;
#elif defined(__linux__)
    #define ESC_PLATFORM_LINUX
    using CurrentPlatform = LinuxPlatform;
#else
    #error "Unsupported platform"
#endif
```

---

## 8. Linux Fallback cho Channel

Trên Linux, `platform::Channel` dùng POSIX message queue để mô phỏng QNX channel:

```cpp
struct LinuxChannel {
    static bool send(const char* name, const void* msg, size_t len);
    static bool receive(const char* name, void* buf, size_t len);
};
```

---

## 9. Cross-Compile QNX

### Toolchain

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/qnx710.cmake -B build-qnx ..
cmake --build build-qnx -j$(nproc)
```

### CMake toolchain

```cmake
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_C_COMPILER qcc)
set(CMAKE_CXX_COMPILER q++)
set(CMAKE_CXX_FLAGS "-Vgcc_ntoaarch64le")
```

---

## 10. Interview Q&A

**Q: QNX khác Linux chỗ nào?**
A: QNX là microkernel, drivers chạy user-space qua resource manager, IPC bằng message passing. Linux là monolithic.

**Q: Tại sao dùng policy-based templates thay vì virtual?**
A: Zero runtime overhead, phù hợp hot path lock-free.

**Q: QNX message passing hoạt động thế nào?**
A: Server tạo Channel, client ConnectAttach + MsgSend, server MsgReceive + MsgReply.

**Q: Resource manager là gì?**
A: User-space driver trong QNX, xử lý I/O cho `/dev/...` path.

---

## 11. References

- QNX SDP Documentation: Neutrino RTOS
- `man ChannelCreate` (QNX)
- `man resmgr_attach` (QNX)
- Andrei Alexandrescu — Modern C++ Design (policy-based templates)
