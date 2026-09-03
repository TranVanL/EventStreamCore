# RTOS Simulator on Linux

> **Scope:** Validate RTOS abstraction logic on a Linux host without QNX/FreeRTOS/Zephyr hardware. The simulator implements the same platform interface using pthreads and POSIX IPC, enabling end-to-end tests and CI coverage.
> **Target audience:** Developers who need to test RTOS portability without access to target hardware or licenses.

---

## 1. Why a Simulator?

Real QNX/RTOS development requires:

- QNX SDP license and toolchain.
- Target board or emulator.
- RTOS SDK and board support package.

Most individual developers and CI environments do not have these. The RTOS simulator solves this by:

1. **Running on Linux** using standard pthreads and POSIX IPC.
2. **Implementing the same abstraction interface** as QNX/FreeRTOS/Zephyr.
3. **Allowing unit tests** to run against simulated and real platforms with the same test code.
4. **Providing a cyclictest + deadline monitor environment** that mimics RTOS scheduling.

---

## 2. Simulator Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    RTOS Simulator Layer                      │
│  SimQnxChannel  │  SimFreeRtosQueue  │  SimZephyrMsgq       │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│              Platform Abstraction Interface                  │
│  platform::Thread  │  platform::Mutex  │  platform::Queue   │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                     Linux Host                               │
│              pthreads + POSIX MQ + shared memory             │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Simulated QNX Channel

### 3.1 Design

QNX `ChannelCreate` / `MsgSend` / `MsgReceive` is simulated with POSIX message queues.

- Each channel maps to a POSIX MQ `/esc_sim_ch_<chid>`.
- `ConnectAttach` returns a simulated connection ID.
- `MsgSend` writes a message with reply buffer metadata.
- `MsgReceive` reads the message.
- `MsgReply` writes the reply to a per-client MQ.

### 3.2 Code Sketch

```cpp
class SimQnxChannel {
public:
    int ChannelCreate(int flags);
    int ConnectAttach(int nd, pid_t pid, int chid, int side, int flags);
    int MsgSend(int coid, const void* msg, int msgLen, void* reply, int replyLen);
    int MsgReceive(int chid, void* buf, int bufLen, struct _msg_info* info);
    int MsgReply(int rcvid, int status, const void* reply, int replyLen);
private:
    std::atomic<int> next_chid_{1};
    std::unordered_map<int, mqd_t> channels_;
    std::mutex mutex_;
};
```

### 3.3 Limitations

- No true kernel message passing semantics.
- No priority inheritance through message passing.
- No `sigevent` pulse delivery.
- Sufficient for logic validation, not timing accuracy.

---

## 4. Simulated FreeRTOS Queue

### 4.1 Design

FreeRTOS `xQueueCreate` / `xQueueSend` / `xQueueReceive` is simulated with a pthread-mutex-protected ring buffer.

### 4.2 Code Sketch

```cpp
template<typename T, size_t Capacity>
class SimFreeRtosQueue {
public:
    bool send(const T& item, uint32_t timeout_ms);
    bool receive(T& item, uint32_t timeout_ms);
private:
    std::array<T, Capacity> buffer_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
};
```

### 4.3 Limitations

- Uses `std::condition_variable` instead of task notifications.
- No `portYIELD` semantics.
- Validates queue logic and bounded behavior.

---

## 5. Simulated Zephyr Message Queue

### 5.1 Design

Zephyr `k_msgq_init` / `k_msgq_put` / `k_msgq_get` is simulated similarly to FreeRTOS queue with fixed message size.

### 5.2 Code Sketch

```cpp
class SimZephyrMsgq {
public:
    bool init(size_t msg_size, size_t max_msgs);
    bool put(const void* msg, k_timeout_t timeout);
    bool get(void* msg, k_timeout_t timeout);
private:
    std::vector<uint8_t> buffer_;
    size_t msg_size_ = 0;
    size_t max_msgs_ = 0;
    size_t count_ = 0;
    size_t head_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
};
```

---

## 6. Simulated RTOS Scheduler

### 6.1 Fixed-Priority Preemptive Scheduler

The simulator can enforce fixed-priority scheduling among a set of pthreads:

```cpp
class SimRtosScheduler {
public:
    void registerThread(pthread_t tid, int priority);
    void yield();
    void block();
    void unblock(pthread_t tid);
private:
    std::map<pthread_t, int> priorities_;
    std::mutex mutex_;
};
```

In practice, Linux `SCHED_FIFO` is used to approximate fixed-priority preemptive behavior.

### 6.2 RMS/EDF Validation

The simulator runs a set of periodic tasks and verifies:

- Highest-priority task preempts lower-priority tasks.
- RMS priority assignment meets deadlines under bounded utilization.
- EDF ordering is correct.

---

## 7. Integration with Unit Tests

### 7.1 Same Test, Multiple Backends

```cpp
TEST(RtosSimulator, QnxChannelRoundTrip) {
    SimQnxChannel qnx;
    int chid = qnx.ChannelCreate(0);
    int coid = qnx.ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    std::thread server([&] {
        char buf[256];
        struct _msg_info info;
        int rcvid = qnx.MsgReceive(chid, buf, sizeof(buf), &info);
        char reply[] = "reply";
        qnx.MsgReply(rcvid, 0, reply, sizeof(reply));
    });

    char msg[] = "hello";
    char reply[256];
    int rc = qnx.MsgSend(coid, msg, sizeof(msg), reply, sizeof(reply));
    EXPECT_EQ(rc, 0);
    server.join();
}
```

### 7.2 Compile-Time Switch

```cpp
#ifdef ESC_PLATFORM_QNX
    using Channel = QnxChannel;
#elif defined(ESC_USE_SIMULATOR)
    using Channel = SimQnxChannel;
#else
    using Channel = LinuxChannel;
#endif
```

---

## 8. CI Strategy

| Job | Platform | Validation |
|-----|----------|------------|
| native-linux-gcc | Linux | Full unit tests + simulator tests |
| cross-qnx710 | QNX SDP 7.1 | Compile-only |
| cross-qnx800 | QNX SDP 8.0 | Compile-only |
| freertos-compile | FreeRTOS headers | Compile-only |
| zephyr-compile | Zephyr headers | Compile-only |
| threadx-compile | ThreadX headers | Compile-only |

The simulator ensures that QNX/FreeRTOS/Zephyr logic is exercised even when cross-compilation is compile-only.

---

## 9. Limitations & Honesty

The simulator is **not** a replacement for real hardware testing:

- Timing is approximate; Linux kernel jitter is present.
- Interrupt latency is not modeled.
- Cache and memory-ordering effects differ from embedded targets.
- Some QNX features (APS, PPS, IFS) are documented but not simulated.

**Interview line:** *"I use the simulator to validate abstraction logic and run CI. For production, I would validate on real QNX/RTOS hardware with cyclictest and tracing."*

---

## 10. Interview Q&A

**Q: How do you test QNX code without a QNX board?**

> A: I built an RTOS simulator on Linux that implements the same platform abstraction interface using pthreads and POSIX IPC. It simulates QNX message passing, FreeRTOS queues, and Zephyr message queues. This lets me run end-to-end round-trip tests in CI. The QNX path is also cross-compiled with QNX SDP to catch API mismatches.

**Q: What can the simulator validate and what can it not?**

> A: It validates logic, message sequencing, bounded queue behavior, and scheduling priority ordering. It cannot validate true interrupt latency, cache effects, or kernel-level timing guarantees. Those need real hardware.

**Q: Why not just mock everything?**

> A: Mocks test one unit in isolation. The simulator tests the abstraction interface across multiple RTOS personalities with shared test code, giving higher confidence that the platform layer is actually portable.

**Q: How does the simulator help with cross-RTOS abstraction?**

> A: The same unit tests run against SimQnxChannel, SimFreeRtosQueue, and SimZephyrMsgq. If a test passes on all three, the abstraction interface is consistent. This catches API mismatches early.

---

## 11. References

- QNX Neutrino message passing documentation
- FreeRTOS Queue API
- Zephyr Kernel Message Queue API
- POSIX message queues (`mq_open`, `mq_send`, `mq_receive`)
- EventStreamCore `docs/CROSS_RTOS.md`
