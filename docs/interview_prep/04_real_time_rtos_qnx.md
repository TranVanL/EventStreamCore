# 04 — Real-Time, RTOS & QNX Portability

> File này cover real-time Linux, SCHED_FIFO, priority inheritance, và QNX/RTOS portability — những keyword mạnh nhất cho JD senior embedded/RTOS C++.

---

## Q1: "What makes EventStreamCore 'real-time'?"

**Answer:**

> Hiện tại (v1.x) engine đã có kiến trúc **real-time capable** với lock-free hot path và bounded queues. Upgrade 2.0 thêm:
>
> 1. **SCHED_FIFO threads:** Dispatcher, ingest workers, realtime processor chạy với fixed priority.
> 2. **Priority inheritance mutexes:** Thay `std::mutex` trong transactional queue bằng `RtMutex` (`PTHREAD_PRIO_INHERIT`).
> 3. **CPU affinity:** Pin threads to isolated CPUs để giảm jitter.
> 4. **Monotonic timers:** `timerfd` + `CLOCK_MONOTONIC` thay vì `sleep_for`.
> 5. **Cyclictest validation:** Đo jitter p50/p95/p99/max.

---

## Q2: "Explain SCHED_FIFO vs SCHED_RR vs SCHED_OTHER."

**Answer:**

| Policy | Behavior | Use case |
|--------|----------|----------|
| `SCHED_OTHER` | CFS, time-sharing, dynamic priority | Default threads, best-effort |
| `SCHED_FIFO` | Fixed priority, no time slice, preempt lower prio | Real-time threads cần deterministic |
| `SCHED_RR` | Round-robin within same priority | Real-time với cùng priority cần fairness |

> **EventStreamCore dùng SCHED_FIFO** cho realtime processor (prio 80), dispatcher (70), ingest (60).
>
> **Risk:** SCHED_FIFO thread không yield có thể starve CPU. Phải kết hợp CPU isolation và priority inheritance.

---

## Q3: "What is priority inversion? How does priority inheritance fix it?"

**Answer:**

> **Priority inversion scenario:**
> - Thread Low (prio 10) giữ mutex.
> - Thread Medium (prio 50) preempt Low.
> - Thread High (prio 90) đợi mutex → bị block bởi Medium!
>
> High đợi ~time slice của Medium (có thể hàng trăm ms), trong khi chỉ cần Low release lock.
>
> **Priority inheritance fix:**
> - Khi High đợi mutex, Low **tạm thời inherit priority 90**.
> - Low preempt Medium, chạy xong, release lock.
> - High acquire lock ngay lập tức.
>
> **Code:**
> ```cpp
> pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
> pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
> ```

---

## Q4: "What is a robust mutex and why use it?"

**Answer:**

> **Robust mutex** (`PTHREAD_MUTEX_ROBUST`) xử lý trường hợp thread giữ mutex bị crash/kill.
>
> **Behavior:**
> - Thread khác lock mutex → nhận `EOWNERDEAD`.
> - Gọi `pthread_mutex_consistent()` để phục hồi state.
> - Nếu không gọi consistent, mutex trở thành `ENOTRECOVERABLE`.
>
> **Why use:** Trong embedded/RTOS, process/thread có thể bị watchdog kill. Robust mutex ngăn resource leak và deadlock.

---

## Q5: "How do you measure real-time performance?"

**Answer:**

> **Cyclictest-style benchmark:**
> ```cpp
> for (int i = 0; i < 30000; ++i) {
>     expected += 1ms;
>     clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &expected, nullptr);
>     actual = now_ns();
>     jitter[i] = actual - expected;
> }
> // report p50/p95/p99/max
> ```
>
> **Expected trên CPU isolated:**
> - p50: ~50 ns
> - p99: ~500 ns
> - max: ~1-2 µs
>
> **Nếu p99 cao bất thường (ví dụ 10 µs), kiểm tra:**
> - CPU không isolated (`isolcpus`).
> - C-states enabled.
> - Kernel timer interrupts.
> - NMI watchdog.
> - PREEMPT_RT kernel chưa enable.

---

## Q6: "Why policy-based templates for platform abstraction?"

**Answer:**

> **Context:** Cần chạy trên Linux và QNX mà không duplicate hot path code.
>
> **Decision:** Dùng policy-based templates:
> ```cpp
> template<typename Platform>
> class Thread { using Handle = typename Platform::ThreadHandle; ... };
> ```
>
> **Why not virtual inheritance?**
> - Virtual function có vtable indirection → branch misprediction + cache miss.
> - Trên hot path lock-free, overhead này không chấp nhận được.
>
> **Policy-based:** Compile-time dispatch, zero runtime overhead. Trade-off là compile time lâu hơn và binary lớn hơn nếu instantiate nhiều platform.

---

## Q7: "Explain QNX Neutrino message passing."

**Answer:**

> QNX là microkernel RTOS. IPC chính là **message passing** qua channel/connection.
>
> **Server:**
> ```cpp
> int chid = ChannelCreate(0);
> int rcvid = MsgReceive(chid, buf, sizeof(buf), &info);
> // process
> MsgReply(rcvid, 0, reply, sizeof(reply));
> ```
>
> **Client:**
> ```cpp
> int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
> MsgSend(coid, msg, sizeof(msg), reply, sizeof(reply));
> ```
>
> **Ưu điểm so với Linux pipes/sockets:**
> - Synchronous, priority inheritance built-in.
> - Lightweight context switch.
> - Microkernel optimize cho IPC.
>
> **Trong EventStreamCore:** `QnxChannel` wrap `ChannelCreate/MsgReceive/MsgSend`, `LinuxChannel` fallback dùng POSIX message queue.

---

## Q8: "What is a QNX resource manager?"

**Answer:**

> Resource manager là user-space driver cho một device path, ví dụ `/dev/eventstream`.
>
> **Pattern:**
> 1. `resmgr_attach()` đăng ký path.
> 2. Định nghĩa handlers: `io_open`, `io_read`, `io_write`, `io_close`.
> 3. `dispatch_block()` / `dispatch_handler()` loop.
>
> **Why useful:** Cho phép ứng dụng khác mở `/dev/eventstream` như một file thông thường, nhưng backend là message passing với engine.
>
> **Code reference:** `include/eventstream/platform/qnx/qnx_resource_manager.hpp` (roadmap 2.0).

---

## Q9: "How do you handle EPERM when setting SCHED_FIFO?"

**Answer:**

> Không phải lúc nào cũng chạy với root/sudo. `pthread_setschedparam` có thể trả về `EPERM`.
>
> **Graceful fallback:**
> ```cpp
> int ret = pthread_setschedparam(handle, SCHED_FIFO, &param);
> if (ret == EPERM) {
>     spdlog::warn("EPERM: running as non-root, SCHED_FIFO unavailable");
>     return false;  // thread vẫn chạy với SCHED_OTHER
> }
> ```
>
> **Why:** Không throw exception, không crash. Engine vẫn functional nhưng không real-time. CI/cloud cũng chạy được.

---

## Q10: "What is CPU isolation and why does it matter?"

**Answer:**

> **CPU isolation:** Ngăn kernel scheduler đặt task khác lên CPU dành cho real-time thread.
>
> **Boot params:**
> ```
> isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
> ```
> - `isolcpus=2,3`: CPU 2,3 không dùng cho general scheduling.
> - `nohz_full=2,3`: Tắt timer tick trên CPU đó.
> - `rcu_nocbs=2,3`: Offload RCU callbacks.
>
> **Impact:** Giảm jitter từ µs xuống ns. Nếu chỉ pin thread mà không isolate CPU, kernel vẫn có thể chạy interrupt/workqueue trên CPU đó.

---

## Q11: "What is the difference between timerfd and sleep_for?"

**Answer:**

| Aspect | `std::this_thread::sleep_for` | `timerfd` + `CLOCK_MONOTONIC` |
|--------|------------------------------|-------------------------------|
| Clock | Usually CLOCK_MONOTONIC | Explicit CLOCK_MONOTONIC |
| Precision | ~1-10 ms (OS dependent) | ~µs với `timerfd_settime` |
| Integration | Blocking sleep | File descriptor → dùng với epoll/select |
| Real-time | Không deterministic | Tốt hơn cho periodic tasks |

> Trong EventStreamCore 2.0, `BatchProcessor` window timer dùng `TimerFd` thay vì `sleep_for`.

---

## Q12: "What is PREEMPT_RT and how does it help?"

**Answer:**

> **PREEMPT_RT** là patchset biến Linux thành real-time OS bằng cách làm hầu hết kernel code preemptible.
>
> **Key changes:**
> - Threaded interrupts: ISR chạy trong kernel thread có thể preempt.
> - Spinlocks → sleeping spinlocks (rt_mutex).
> - High-resolution timers.
> - Priority inheritance trong kernel.
>
> **Impact:** Giảm scheduling latency từ ms xuống µs/ns.
>
> **Validation:** Chạy cyclictest trên PREEMPT_RT kernel và so sánh với generic kernel.

---

## Q13: "Explain SCHED_DEADLINE."

**Answer:**

> **SCHED_DEADLINE** là real-time scheduling policy dựa trên **earliest-deadline-first (EDF).**
>
> **Parameters:**
> - `runtime`: CPU time cần trong mỗi period.
> - `deadline`: Thời điểm phải hoàn thành.
> - `period`: Chu kỳ lặp lại.
>
> **Use case:** Periodic tasks với hard deadline, ví dụ: sensor sampling every 1ms.
>
> **Comparison:**
> - `SCHED_FIFO`: priority-based, không đảm bảo deadline.
> - `SCHED_DEADLINE`: deadline-based, kernel đảm bảo bandwidth.
>
> **EventStreamCore:** Có thể dùng SCHED_DEADLINE cho cyclictest-style tasks hoặc periodic ingest.

---

## Q14: "What are the dangers of SCHED_FIFO?"

**Answer:**

> 1. **CPU starvation:** Thread SCHED_FIFO cao priority không yield sẽ chiếm CPU vô hạn.
> 2. **Priority inversion:** Nếu không dùng PI mutex.
> 3. **System lockup:** Real-time thread chiếm CPU mà block trên I/O → kernel threads bị starve.
> 4. **Memory allocation:** `malloc` trong RT thread có thể trigger page fault hoặc lock.
>
> **Mitigations trong EventStreamCore:**
> - CPU isolation.
> - PI mutex.
> - Pre-allocated memory pools.
> - Bounded loops.
> - Graceful EPERM fallback.

---

## Q15: "How do you cross-compile for QNX?"

**Answer:**

> **Toolchain:** QNX SDP 7.1/8.0 cung cấp `qcc` và CMake toolchain file.
>
> **CMake command:**
> ```bash
> cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/qnx710.cmake ..
> make
> ```
>
> **Challenges:**
> - Không phải tất cả POSIX APIs đều available.
> - `pthread_setaffinity_np` có thể khác hoặc absent.
> - `epoll` không có — cần QNX socket API hoặc `select`/`poll`.
> - `numa` không có — cần abstract away.
>
> **Validation:** Compile-only nếu không có hardware. Dùng QNX emulator nếu có.

---

## Q16: "What is the difference between QNX and Linux for real-time?"

**Answer:**

| Aspect | Linux + PREEMPT_RT | QNX Neutrino |
|--------|-------------------|--------------|
| Kernel | Monolithic + RT patch | Microkernel |
| IPC | Sockets, pipes, POSIX MQ | Message passing (Channel/ConnectAttach) |
| Priority inheritance | pthread PI mutex | Built into message passing |
| Interrupt latency | ~10-100 µs | ~1-10 µs |
| Determinism | Good | Excellent |
| Ecosystem | Huge | Smaller, automotive-focused |

> **EventStreamCore abstraction:** Platform layer cho phép compile trên cả hai, dùng policy-based templates.

---

## Q17: "How does QNX handle interrupts?"

**Answer:**

> **QNX interrupt model:**
> 1. **ISR (Interrupt Service Routine):** Chạy ở kernel level, rất ngắn. Chỉ thông báo cho thread.
> 2. **InterruptAttach():** Gắn ISR vào interrupt vector.
> 3. **Pulse/Signal:** ISR gửi pulse đến thread để xử lý.
> 4. **Thread:** Chạy ở user space với high priority, xử lý interrupt event.
>
> **Why:** Giữ ISR ngắn để giảm interrupt latency. Phần lớn xử lý diễn ra ở user-space thread có thể preempt.
>
> **EventStreamCore:** `QnxInterrupt` stub trong roadmap 2.0.

---

## Q18: "What is priority ceiling protocol?"

**Answer:**

> **Priority ceiling protocol (PCP):** Mỗi mutex có "ceiling priority" = max priority của thread nào có thể lock nó. Khi thread lock mutex, nó immediately boost to ceiling priority.
>
> **Comparison with PI:**
> - **PI:** Boost dynamic dựa trên thread đang đợi.
> - **PCP:** Boost static dựa trên ceiling. Tránh deadlock tốt hơn nhưng cần biết trước priorities.
>
> **EventStreamCore:** Dùng PI (`PTHREAD_PRIO_INHERIT`) vì linh hoạt hơn. PCP có thể dùng nếu priorities cố định và known.

---

## Q19: "What is a timer wheel and why does it matter?"

**Answer:**

> **Timer wheel** là data structure để quản lý nhiều timers hiệu quả.
>
> **Linux kernel:** Dùng hierarchical timer wheel cho `timerfd` và `epoll_wait` timeouts.
>
> **Impact:** Khi có hàng nghìn timers, timer wheel giúp insert/delete/query O(1) thay vì O(N).
>
> **EventStreamCore:** Batch processor window timer dùng `timerfd`, kernel internally dùng timer wheel.

---

## Q20: "How do you avoid malloc in real-time threads?"

**Answer:**

> **Why avoid malloc:**
> - `malloc` có thể gọi `sbrk`/`mmap` → page fault.
> - Global lock trong allocator → contention.
> - Non-deterministic latency.
>
> **Strategies trong EventStreamCore:**
> 1. **Pre-allocate pools:** `IngestEventPool`, `LockFreeObjectPool`.
> 2. **Stack allocation:** Dùng local buffers khi có thể.
> 3. **Custom allocator:** Per-thread arena hoặc TLSF (Two-Level Segregated Fit) cho embedded.
> 4. **Reserve memory:** `mlockall(MCL_CURRENT | MCL_FUTURE)` để tránh page faults.

---

## Q21: "Explain Rate Monotonic Scheduling (RMS)."

**Answer:**

> RMS assigns fixed priorities inversely proportional to task period: the shortest-period task gets the highest priority. It is optimal among fixed-priority algorithms for independent periodic tasks.
>
> **Liu & Layland bound:**
> $$
> U = \sum \frac{C_i}{T_i} \leq n(2^{1/n} - 1)
> $$
> For large n the bound approaches ~69%. If utilization is below the bound, all deadlines are guaranteed.
>
> **EventStreamCore use:** `RmsScheduler` assigns priorities to ingest, dispatch, realtime processor, and batch tasks based on their periods.

---

## Q22: "When would you use EDF over RMS?"

**Answer:**

> EDF assigns dynamic priorities based on the nearest absolute deadline. It can reach 100% CPU utilization, so it is better when the task set is utilization-heavy.
>
> **Trade-offs:**
> - EDF is harder to implement on simple RTOSes.
> - EDF requires runtime deadline tracking.
> - RMS is simpler and sufficient for many automotive systems.
>
> **EventStreamCore use:** `EdfScheduler` is provided as a compile-time/test option for future hard-deadline tasks.

---

## Q23: "How do you ensure no malloc in the real-time hot path?"

**Answer:**

> 1. Pre-allocate event pools at startup.
> 2. Use `LockFreeObjectPool` for MPSC queue nodes.
> 3. Use stack buffers for small fixed-size data.
> 4. Optionally call `mlockall(MCL_CURRENT | MCL_FUTURE)` to pin pages.
>
> In EventStreamCore, `HazardPointerMpscQueue` acquires nodes from a pre-allocated pool, so `push`/`pop` never call `malloc`.

---

## Q24: "What is the role of a watchdog in a real-time system?"

**Answer:**

> A watchdog detects runaway tasks. A monitored task must "pet" the watchdog within a deadline; if it fails, the watchdog triggers recovery.
>
> **Software watchdog:** implemented in firmware, easy to add.
> **Hardware watchdog:** external timer that resets the system if not petted; used when the OS itself may hang.
>
> EventStreamCore has `RtWatchdog` for the dispatcher and realtime processor loops.

---

## Q25: "How do you detect and handle deadline misses?"

**Answer:**

> `DeadlineMonitor` registers each periodic task with its deadline. `onStart`/`onComplete` timestamps are recorded. If completion exceeds the deadline, the monitor increments a miss counter and can:
> - Log the event.
> - Notify the watchdog.
> - Degrade to a safe state.
>
> This is essential for safety-critical systems where a missed deadline is a fault.

---

## Q26: "What is QNX Adaptive Partitioning Scheduler (APS)?"

**Answer:**

> APS reserves a CPU budget for a group of threads. Even if the rest of the system is overloaded, the partition still receives its guaranteed share.
>
> **Parameters:**
> - `budget_percent`: guaranteed CPU share.
> - `critical_budget_ms`: how much a critical thread can borrow.
>
> **EventStreamCore use:** realtime ingest/processing threads go into a partition with ~40% budget so background batch work cannot starve them.

---

## Q27: "What is QNX PPS and how would you use it?"

**Answer:**

> PPS (Persistent Publish/Subscribe) is a QNX service where objects live as files under `/pps/...`. Publishers write attributes; subscribers receive change notifications. Objects persist even if the publisher restarts.
>
> **EventStreamCore use:** expose metrics via `/pps/eventstream/metrics` so QNX CAR dashboards or diagnostic tools can subscribe without polling.

---

## Q28: "What is in a QNX IFS buildfile?"

**Answer:**

> The Image File System buildfile describes the boot image:
> - Startup program (`startup-xxx`).
> - `procnto` microkernel.
> - Shared libraries.
> - Drivers and scripts.
> - User applications like EventStreamCore.
>
> For production, EventStreamCore would be listed as an application entry and started by an init script.

---

## Q29: "Walk through the QNX resource manager lifecycle."

**Answer:**

> 1. `dispatch_create()` to create dispatch context.
> 2. `iofunc_func_init()` to initialize connect/io function tables.
> 3. Set handlers: `io_funcs.read`, `io_funcs.write`, etc.
> 4. `iofunc_attr_init()` to set device attributes.
> 5. `resmgr_attach()` to register `/dev/eventstream`.
> 6. Run `dispatch_block()` / `dispatch_handler()` loop.
> 7. On shutdown: `resmgr_detach()` then `dispatch_destroy()`.
>
> Each `open()` creates an OCB for per-client state.

---

## Q30: "How do you measure high-resolution time on QNX?"

**Answer:**

> Use `ClockCycles()` to read the hardware counter and divide by `SYSPAGE_ENTRY(qtime)->cycles_per_sec` for seconds. For periodic timers, `timer_create(CLOCK_MONOTONIC, SIGEV_THREAD, ...)` delivers expiry in a dedicated thread.

---

## Q31: "How would you port EventStreamCore to FreeRTOS?"

**Answer:**

> The policy-based platform abstraction already defines `Thread`, `Mutex`, `Queue`, `Semaphore`. For FreeRTOS:
> - `Thread` maps to `xTaskCreate`.
> - `Mutex` maps to `xSemaphoreCreateMutex`.
> - `Queue` maps to `xQueueCreate`.
>
> Challenges:
> - Tasks need statically allocated stacks.
> - Tasks are not usually joined.
> - Dynamic allocation may be disabled.
>
> EventStreamCore uses pre-allocated pools and static buffers to fit these constraints.

---

## Q32: "How does the RTOS simulator help without hardware?"

**Answer:**

> The simulator implements the same platform interface on Linux using pthreads and POSIX IPC. It simulates:
> - QNX `ChannelCreate`/`MsgSend`/`MsgReceive` over POSIX MQ.
> - FreeRTOS queues over mutex + condvar ring buffer.
> - Zephyr message queues over fixed-size buffer.
> - Fixed-priority scheduler using `SCHED_FIFO`.
>
> This lets CI run end-to-end round-trip tests for QNX/FreeRTOS/Zephyr logic without target hardware. The real targets are still cross-compiled to catch API mismatches.

---

## Q33: "What are the limitations of the RTOS simulator?"

**Answer:**

> The simulator validates logic and sequencing, not true timing:
> - Linux kernel jitter is present.
> - Interrupt latency is not modeled.
> - Cache and memory-ordering effects differ from embedded targets.
> - QNX APS, PPS, and IFS are documented but not simulated.
>
> It is a CI and development aid, not a replacement for hardware validation.

---

## Q34: "Map RTOS primitives across Linux, QNX, FreeRTOS, Zephyr, ThreadX."

**Answer:**

| Concept | Linux | QNX | FreeRTOS | Zephyr | ThreadX |
|---------|-------|-----|----------|--------|---------|
| Thread | `pthread_t` | `pthread_t` | `TaskHandle_t` | `k_tid_t` | `TX_THREAD*` |
| Mutex | `pthread_mutex_t` | `pthread_mutex_t` | `SemaphoreHandle_t` | `struct k_mutex` | `TX_MUTEX` |
| Semaphore | `sem_t` | `sem_t` | `SemaphoreHandle_t` | `struct k_sem` | `TX_SEMAPHORE` |
| Queue | POSIX MQ | Channel/Msg | `QueueHandle_t` | `struct k_msgq` | `TX_QUEUE` |
| Timer | `timerfd` | `timer_create` | `TimerHandle_t` | `struct k_timer` | `TX_TIMER` |

> EventStreamCore's platform layer hides these differences behind policy-based templates.
