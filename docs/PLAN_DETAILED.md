# 📅 EventStreamCore 2.0 — Plan chi tiết 60 ngày nâng cấp toàn diện

> **Version:** 2.0 Real-Time / RTOS / QNX-Portable Upgrade  
> **Ngày bắt đầu:** 2026-08-19  
> **Thời gian:** 60 ngày code (12 tuần)  
> **Mục tiêu:** Biến EventStreamCore từ Linux event streaming engine thành **real-time event streaming platform portable Linux ↔ QNX**, mạnh nhất có thể cho JD senior embedded/RTOS C++.

---

## 🎯 Quy tắc bất biến

- Mỗi tối **2-3h thuần code** + 30' đọc/note.
- **1 commit có ý nghĩa** mỗi ngày.
- **Cuối mỗi tuần**: push + tag `wXX-done` + 5 dòng "what I learned" trong `docs/lessons.md`.
- **MUST** = bắt buộc, **NICE** = cut nếu trễ.
- **Không cần hardware QNX thật** — cross-compile + emulator/compile-only validation là đủ.

---

## 🖥️ Môi trường thực tế

| Thành phần | Chạy ở đâu | Cần gì? | Notes |
|---|---|---|---|
| Real-time scheduling (SCHED_FIFO) | Linux host | `sudo` optional | Cloud CI → graceful EPERM fallback |
| POSIX IPC (mq, shm, eventfd, timerfd) | Linux host | kernel 4.x+ | Chạy hoàn toàn trên Linux |
| io_uring | Linux host | kernel 5.1+ | Feature-detect, fallback epoll |
| SocketCAN | Linux host | `sudo modprobe vcan` | Virtual CAN, không cần hardware |
| QNX cross-compile | Linux host | QNX SDP 7.1/8.0 (hoặc toolchain file sẵn) | Compile-only nếu không có SDP |
| ARM64 cross-compile | Linux host | `gcc-aarch64-linux-gnu` + `qemu-aarch64` | Zero hardware |
| x86_64 musl static | Linux host | `musl-tools` | Static linking demo |
| Hazard pointer / hugepage | Linux host | `libhugetlbfs` optional | Skip test nếu không có |

### ⚡ Setup 1 lần

```bash
# Real-time + POSIX IPC
sudo apt-get install -y build-essential cmake libnuma-dev libhugetlbfs-dev \
    g++-aarch64-linux-gnu qemu-user-static musl-tools

# vcan0 (SocketCAN)
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
ip link show vcan0

# Verify io_uring
uname -r   # >= 5.1
```

---

## 📅 PHASE 1 / TUẦN 1 — Real-Time Scheduling Foundation (10%)

> **Goal:** Engine threads chạy SCHED_FIFO, CPU affinity, priority inheritance mutex, cyclictest benchmark. Đây là nền tảng cho mọi module sau.

### Day 1 (T2) — Đọc + note real-time Linux (1.5h đọc + 30' note)

**Đọc:**
1. `man sched` — SCHED_FIFO / SCHED_RR policies.
2. `man pthread_setschedparam` — thread priority.
3. `man pthread_mutexattr_setprotocol` — PTHREAD_PRIO_INHERIT.
4. https://wiki.linuxfoundation.org/realtime/documentation/howto/applications/application_base — PREEMPT_RT basics.

**Note 1 trang giấy:**
- SCHED_FIFO: fixed priority, no time slicing, preempt lower priority.
- Priority inversion: L holds lock, M preempts L, H waits on L → H blocked by M.
- Priority inheritance: L temporarily inherits H's priority while holding lock.
- CPU isolation: `isolcpus` + `cgroup.cpuset` để giảm jitter.
- `pthread_setaffinity_np` để pin thread.

**Commit:** *(không cần — chỉ note giấy)*

---

### Day 2 (T3) — Tạo module `rt/` + `RtThread` (2h)

**Files:**
```
include/eventstream/rt/rt_thread.hpp
src/rt/rt_thread.cpp
include/eventstream/rt/rt_policy.hpp
```

**Code gợi ý:**
- `enum class SchedPolicy { Other, Fifo, RoundRobin };`
- `struct RtPolicy { SchedPolicy policy; int priority; std::vector<int> cpus; };`
- `class RtThread`:
  - `static bool apply(std::thread& t, const RtPolicy& p)` — gọi `pthread_setschedparam` + `pthread_setaffinity_np`.
  - `static bool applyToSelf(const RtPolicy& p)`.
  - `static std::string describe(const RtPolicy& p)` — log friendly.
- `RtPolicyBuilder` fluent API: `RtPolicyBuilder().fifo().priority(80).cpus({2}).build()`.
- Graceful fallback: nếu `EPERM`, log warn và trả về `false`, không throw.

**Flow:**
1. Lấy `native_handle()` từ `std::thread`.
2. `pthread_setschedparam(handle, policy, &param)`.
3. Nếu OK → `pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset)`.
4. Nếu fail → log warn, vẫn trả về false nhưng thread vẫn chạy.

**Commit:** `[W1D2] feat(rt): RtThread — SCHED_FIFO + affinity + EPERM fallback`

---

### Day 3 (T4) — `RtMutex` priority inheritance + robust (2.5h)

**Files:**
```
include/eventstream/rt/rt_mutex.hpp
src/rt/rt_mutex.cpp
```

**Code gợi ý:**
- `class RtMutex`:
  - `pthread_mutex_t mutex_`.
  - `pthread_mutexattr_t attr_`.
  - Constructor: `pthread_mutexattr_init`, `pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)`, `pthread_mutexattr_setrobust(PTHREAD_MUTEX_ROBUST)`, `pthread_mutex_init`.
  - `lock()`, `tryLock()`, `unlock()`.
  - Destructor: destroy.
- `class RtLockGuard` RAII.
- `class RtUniqueLock` hỗ trợ `try_lock_for`.
- Xử lý `EOWNERDEAD`: nếu holder crash, gọi `pthread_mutex_consistent` để phục hồi.

**Flow lock:**
1. `pthread_mutex_lock(&mutex_)`.
2. Nếu return `EOWNERDEAD` → gọi `pthread_mutex_consistent` → log warning.
3. Nếu return `ENOTRECOVERABLE` → throw.

**Commit:** `[W1D3] feat(rt): RtMutex — priority inheritance + robust mutex`

---

### Day 4 (T5) — `RtSemaphore`, `RtBarrier`, `RtSpinlock` (2.5h)

**Files:**
```
include/eventstream/rt/rt_semaphore.hpp
include/eventstream/rt/rt_barrier.hpp
include/eventstream/rt/rt_spinlock.hpp
src/rt/rt_semaphore.cpp
src/rt/rt_barrier.cpp
src/rt/rt_spinlock.cpp
```

**Code gợi ý:**
- `class RtSemaphore`:
  - `sem_t sem_`.
  - `wait()`, `tryWait()`, `post()`, `getValue()`.
  - Named semaphore overload: `RtSemaphore(const char* name, int initial)`.
- `class RtBarrier`:
  - `pthread_barrier_t barrier_`.
  - `wait()` — `pthread_barrier_wait`.
- `class RtSpinlock`:
  - `std::atomic_flag flag_ = ATOMIC_FLAG_INIT`.
  - `lock()` — `while (flag_.test_and_set(std::memory_order_acquire)) { pause_or_yield(); }`.
  - `unlock()` — `flag_.clear(std::memory_order_release)`.
  - `pause_or_yield()`: x86 `__builtin_ia32_pause`, ARM `__yield`, fallback `sched_yield()`.

**Commit:** `[W1D4] feat(rt): semaphore + barrier + spinlock primitives`

---

### Day 5 (T6) — `RtCondvar` monotonic + `RtPolicy` config (2h)

**Files:**
```
include/eventstream/rt/rt_condvar.hpp
src/rt/rt_condvar.cpp
```

**Code gợi ý:**
- `class RtCondvar`:
  - `pthread_cond_t cv_`.
  - Constructor: `pthread_condattr_setclock(CLOCK_MONOTONIC)`.
  - `wait(RtMutex& m)`, `waitFor(RtMutex& m, std::chrono::nanoseconds timeout)`, `notifyOne()`, `notifyAll()`.
- `class RtCondition` kết hợp `RtMutex + RtCondvar` cho đơn giản.

**Flow waitFor:**
1. `clock_gettime(CLOCK_MONOTONIC, &now)`.
2. Tính `abs_timeout = now + timeout`.
3. `pthread_cond_timedwait(&cv_, &m.mutex_, &abs_timeout)`.

**Commit:** `[W1D5] feat(rt): monotonic condvar + RtPolicy config struct`

---

### Day 6 (T7) — Wire `RtThread` vào `ProcessManager` (2.5h)

**Files:**
```
src/core/processor/manager.cpp
include/eventstream/core/processor/manager.hpp
```

**Code gợi ý:**
- Thêm `RtPolicy realtimePolicy_`, `dispatcherPolicy_`, `ingestPolicy_`, `transactionalPolicy_`, `batchPolicy_`.
- Trong `ProcessManager::start()`:
  - Sau khi `std::thread` được tạo, gọi `RtThread::apply(realtimeThread_, realtimePolicy_)`.
  - Log: `"[RT] RealtimeProcessor pinned to CPU 2, SCHED_FIFO prio 80"`.
- Tương tự cho dispatcher, ingest, transactional, batch.
- Nếu apply fail → log warn, thread vẫn chạy với default.

**Flow:**
1. Tạo thread.
2. Apply RT policy.
3. Thread chạy `runLoop`.

**Commit:** `[W1D6] feat(core): wire RtThread into ProcessManager`

---

### Day 7 (CN) — Cyclictest benchmark + push (2h) → M1 ✓

**Files:**
```
rt_validation/cyclictest_runner.cpp
```

**Code gợi ý:**
- `struct LatencySample { uint64_t expected_ns; uint64_t actual_ns; uint64_t jitter_ns; };`
- Thread chính:
  - Set SCHED_FIFO priority 90, CPU 3.
  - Vòng lặp 30s: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr)`, đo thời gian thức dậy thực tế.
  - Tính jitter = actual - expected.
  - Lưu vào vector.
- Sau vòng lặp: tính p50/p95/p99/max jitter.
- In bảng kết quả.

**Flow:**
1. Set RT policy.
2. `clock_gettime` lấy start.
3. Vòng lặp: `next += 1ms`, `clock_nanosleep`, `clock_gettime`, tính jitter.
4. Tính percentile.

**Commit:** `[W1D7] test(rt): cyclictest_runner + p99 latency benchmark`

### ✅ Checkpoint W1 (M1 — RT Scheduling alive)
- [ ] `RtThread` set SCHED_FIFO + affinity.
- [ ] `RtMutex` priority inheritance.
- [ ] `ProcessManager` threads có policy/priority log.
- [ ] `cyclictest_runner` chạy được, báo p99.
- [ ] Hiểu: priority inversion, PI, CPU isolation.

---

## 📅 PHASE 2 / TUẦN 2 — RT Tests + Priority Inversion Demo (10%)

> **Goal:** Chứng minh priority inheritance hoạt động, viết test multithreading nâng cao.

### Day 8 (T2) — `rt_thread_test.cpp` (2h)

**Files:**
```
unittest/rt_thread_test.cpp
```

**Code gợi ý:**
- `TEST(RtThreadTest, SetFifoPolicy)`:
  - Tạo thread, apply SCHED_FIFO priority 50.
  - Trong thread, đọc lại policy bằng `pthread_getschedparam`.
  - Assert policy == SCHED_FIFO.
- `TEST(RtThreadTest, EpermFallback)`:
  - Chạy không sudo, apply SCHED_FIFO → expect false nhưng thread vẫn chạy.
- `TEST(RtThreadTest, SetAffinity)`:
  - Pin thread tới CPU 0.
  - Đọc `/proc/self/status` Cpus_allowed_list.

**Commit:** `[W2D8] test(rt): RtThread unit tests`

---

### Day 9 (T3) — `rt_mutex_test.cpp` + PI scenario (2.5h)

**Files:**
```
unittest/rt_mutex_test.cpp
```

**Code gợi ý:**
- `TEST(RtMutexTest, BasicLockUnlock)`.
- `TEST(RtMutexTest, RobustRecovery)`:
  - Thread khác lock mutex rồi bị cancel/kill.
  - Thread hiện tại lock → nhận `EOWNERDEAD` → gọi consistent.
- `TEST(RtMutexTest, PriorityInversionPrevention)`:
  - 3 thread: Low (prio 10), Medium (prio 50), High (prio 90).
  - Low lock mutex, bị Medium preempt chiếm CPU, High đợi mutex.
  - Với PI: Low inherit prio 90, Medium bị preempt, High nhanh được lock.
  - Đo thời gian High chờ.

**Flow PI test:**
1. Low lock mutex.
2. Medium bắt đầu busy loop để chiếm CPU.
3. High cố lock mutex → block.
4. Nếu PI hoạt động: Low preempt Medium, release lock, High acquire ngay.
5. Assert High wait time < threshold (ví dụ 50ms).

**Commit:** `[W2D9] test(rt): RtMutex + priority inversion scenario`

---

### Day 10 (T4) — `rt_semaphore_test.cpp` + producer/consumer (2h)

**Files:**
```
unittest/rt_semaphore_test.cpp
```

**Code gợi ý:**
- `TEST(RtSemaphoreTest, ProducerConsumer)`:
  - Buffer size 100.
  - 3 producers, mỗi producer gửi 10000 items.
  - 2 consumers nhận.
  - Semaphore đếm số item available.
  - Assert total consumed == 30000.
- `TEST(RtSemaphoreTest, NamedSemaphore)`:
  - `/test_sem`, post từ process khác (dùng fork).

**Commit:** `[W2D10] test(rt): RtSemaphore producer/consumer stress`

---

### Day 11 (T5) — `rt_barrier_test.cpp` + `rt_spinlock_test.cpp` (2h)

**Files:**
```
unittest/rt_barrier_test.cpp
unittest/rt_spinlock_test.cpp
```

**Code gợi ý:**
- Barrier test:
  - 8 threads, mỗi thread tăng `std::atomic<int> phase` sau mỗi lần qua barrier.
  - 10000 vòng lặp.
  - Assert phase counter đúng.
- Spinlock test:
  - N threads tăng shared counter 1M lần.
  - Assert counter == N * 1M.
  - Benchmark thời gian.

**Commit:** `[W2D11] test(rt): barrier + spinlock tests`

---

### Day 12 (T6) — Priority inversion demo executable (2.5h)

**Files:**
```
rt_validation/priority_inversion_demo.cpp
```

**Code gợi ý:**
- `void runScenario(bool usePiMutex)`:
  - Tạo mutex (PI hoặc normal).
  - Low thread: lock mutex, sleep 100ms, unlock.
  - Medium thread: busy loop 200ms để chiếm CPU.
  - High thread: đo thời gian từ khi bắt đầu lock đến khi acquire được.
- In kết quả 2 scenario cạnh nhau.
- Kết luận: PI giảm High wait time từ ~200ms xuống ~100ms.

**Flow:**
1. Không PI: High đợi Low, nhưng Medium chiếm CPU → High đợi ~200ms.
2. Có PI: Low inherit prio 90, preempt Medium → High đợi ~100ms.

**Commit:** `[W2D12] test(rt): priority_inversion_demo — PI vs non-PI`

---

### Day 13 (T7) — `RtCondvar` test + monotonic timeout (2h)

**Files:**
```
unittest/rt_condvar_test.cpp
```

**Code gợi ý:**
- `TEST(RtCondvarTest, WaitTimeoutNotSpurious)`:
  - Thread chờ 100ms, không có notify → timeout sau ~100ms.
  - Đo bằng monotonic clock.
- `TEST(RtCondvarTest, BroadcastWakesAll)`:
  - 10 threads wait, broadcast → tất cả thức dậy.

**Commit:** `[W2D13] test(rt): RtCondvar monotonic timedwait`

---

### Day 14 (CN) — Polish RT docs + push (1.5h) → M2 ✓

**Files:**
```
docs/REAL_TIME.md
```

**Code gợi ý:**
- Giải thích SCHED_FIFO/RR/OTHER.
- Vẽ diagram priority inversion.
- Giải thích PI mutex.
- Hướng dẫn CPU isolation: `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`.

**Commit:** `[W2D14] docs(rt): REAL_TIME.md + lessons W2`

### ✅ Checkpoint W2 (M2 — RT primitives proven)
- [ ] 5+ RT unit test files pass.
- [ ] Priority inversion demo chạy được.
- [ ] `RtMutex` PI + robust tested.
- [ ] `docs/REAL_TIME.md` hoàn chỉnh.

---

## 📅 PHASE 3 / TUẦN 3 — RTOS / QNX Portability Layer (12%) ⭐ TUẦN QUAN TRỌNG NHẤT

> **Goal:** Có abstraction layer để engine compile trên QNX. Đây là JD keyword mạnh nhất.

### Day 15 (T2) — `platform_detect.hpp` + `rtos_abstraction.hpp` (2h)

**Files:**
```
include/eventstream/platform/platform_detect.hpp
include/eventstream/platform/rtos_abstraction.hpp
```

**Code gợi ý:**
- `platform_detect.hpp`:
  - `#if defined(__QNX__) || defined(__QNXNTO__)` → `ESC_PLATFORM_QNX`.
  - `#elif defined(__linux__)` → `ESC_PLATFORM_LINUX`.
- `rtos_abstraction.hpp`:
  - `template<typename Platform> class Thread`.
  - `template<typename Platform> class Mutex`.
  - `template<typename Platform> class Semaphore`.
  - `template<typename Platform> class Condvar`.
  - `template<typename Platform> class Timer`.
  - `template<typename Platform> class Channel`.
- Mỗi class có `using NativeHandle = ...` tùy platform.

**Flow:**
- Policy-based template: `platform::Thread<LinuxPlatform>::create(...)`.
- Không dùng virtual inheritance để tránh overhead.

**Commit:** `[W3D15] feat(platform): platform_detect + rtos_abstraction skeleton`

---

### Day 16 (T3) — Linux platform implementations (2.5h)

**Files:**
```
include/eventstream/platform/linux/linux_thread.hpp
include/eventstream/platform/linux/linux_mutex.hpp
include/eventstream/platform/linux/linux_semaphore.hpp
include/eventstream/platform/linux/linux_condvar.hpp
include/eventstream/platform/linux/linux_timer.hpp
include/eventstream/platform/linux/linux_channel.hpp
src/platform/linux/*.cpp
```

**Code gợi ý:**
- `LinuxThread`: wrap `pthread_create`, `pthread_setschedparam`, `pthread_setaffinity_np`.
- `LinuxMutex`: `pthread_mutex_t` với PI.
- `LinuxSemaphore`: `sem_t`.
- `LinuxCondvar`: `pthread_cond_t` monotonic.
- `LinuxTimer`: `timerfd_create(CLOCK_MONOTONIC)` + `read()`.
- `LinuxChannel`: POSIX message queue fallback (`mq_open`, `mq_send`, `mq_receive`).

**Flow LinuxChannel send:**
1. `mq_open(name, O_RDWR | O_CREAT, 0644, &attr)`.
2. `mq_send(mq, msg, len, prio)`.
3. `mq_close(mq)`.

**Commit:** `[W3D16] feat(platform): Linux implementations of RTOS primitives`

---

### Day 17 (T4) — QNX platform implementations (3h)

**Files:**
```
include/eventstream/platform/qnx/qnx_thread.hpp
include/eventstream/platform/qnx/qnx_mutex.hpp
include/eventstream/platform/qnx/qnx_semaphore.hpp
include/eventstream/platform/qnx/qnx_condvar.hpp
include/eventstream/platform/qnx/qnx_timer.hpp
include/eventstream/platform/qnx/qnx_channel.hpp
src/platform/qnx/*.cpp
```

**Code gợi ý:**
- `QnxThread`: pthread wrapper (QNX hỗ trợ pthread).
- `QnxMutex`: pthread mutex + PI.
- `QnxSemaphore`: `sem_*`.
- `QnxCondvar`: pthread cond.
- `QnxTimer`: `timer_create(CLOCK_MONOTONIC, ...)` + `SIGEV_THREAD`.
- `QnxChannel`: `ChannelCreate(0)`, `MsgSend`, `MsgReceive`, `ConnectAttach`.

**Flow QnxChannel:**
1. Server: `chid = ChannelCreate(0)`.
2. Server: `MsgReceive(chid, &msg, sizeof(msg), &info)`.
3. Client: `coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0)`.
4. Client: `MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply))`.
5. Server: `MsgReply(rcvid, 0, &reply, sizeof(reply))`.

**Commit:** `[W3D17] feat(platform): QNX Neutrino channel + timer implementations`

---

### Day 18 (T5) — QNX resource manager + interrupt stub (2.5h)

**Files:**
```
include/eventstream/platform/qnx/qnx_resource_manager.hpp
include/eventstream/platform/qnx/qnx_interrupt.hpp
src/platform/qnx/qnx_resource_manager.cpp
src/platform/qnx/qnx_interrupt.cpp
```

**Code gợi ý:**
- `QnxResourceManager`:
  - `resmgr_attach()` để tạo `/dev/eventstream`.
  - `io_open`, `io_read`, `io_write` handlers.
  - Dispatch loop `dispatch_block()` / `dispatch_handler()`.
- `QnxInterrupt`:
  - `InterruptAttach()` stub.
  - ISR-to-thread pulse: `MsgSendPulse()`.

**Flow resource manager:**
1. `resmgr_attach(&dpp, &resmgr_attr, "/dev/eventstream", _FTYPE_ANY, ...)`.
2. `iofunc_func_init()` setup handlers.
3. `dispatch_handler()` loop.

**Commit:** `[W3D18] feat(platform): QNX resource manager + interrupt stub`

---

### Day 19 (T6) — Platform tests (2.5h)

**Files:**
```
unittest/platform_thread_test.cpp
unittest/platform_mutex_test.cpp
unittest/platform_channel_test.cpp
unittest/platform_timer_test.cpp
```

**Code gợi ý:**
- `platform_thread_test`: create/join Linux thread, set policy.
- `platform_mutex_test`: lock/unlock, PI smoke.
- `platform_channel_test`: send/receive 1M messages, latency histogram.
- `platform_timer_test`: periodic 1ms accuracy.
- QNX path guarded by `#ifdef __QNX__` → compile-only trên Linux.

**Commit:** `[W3D19] test(platform): platform abstraction unit tests`

---

### Day 20 (T7) — Wire platform layer vào engine (2h)

**Files:**
```
src/core/processor/manager.cpp
src/core/events/dispatcher.cpp
include/eventstream/core/events/event_bus.hpp
```

**Code gợi ý:**
- `using PlatformThread = platform::Thread<ESC_PLATFORM>`.
- `using PlatformMutex = platform::Mutex<ESC_PLATFORM>`.
- Thay `std::thread` bằng `PlatformThread::Handle`.
- Thay `std::mutex` trong transactional queue bằng `PlatformMutex`.

**Flow:**
1. Define platform alias ở đầu file.
2. Thay thread creation.
3. Thay mutex.
4. Build Linux path.

**Commit:** `[W3D20] feat(core): wire platform abstraction into engine`

---

### Day 21 (CN) — QNX docs + push (1.5h) → M3 ✓

**Files:**
```
docs/QNX_PORT.md
docs/PLATFORM_ABSTRACTION.md
```

**Code gợi ý:**
- `QNX_PORT.md`:
  - Tại sao QNX trong automotive.
  - Neutrino message passing.
  - Resource manager pattern.
  - Build command: `cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/qnx710.cmake ..`.
- `PLATFORM_ABSTRACTION.md`:
  - Policy-based templates.
  - Cách thêm RTOS mới (ví dụ FreeRTOS).

**Commit:** `[W3D21] docs(platform): QNX_PORT + PLATFORM_ABSTRACTION`

### ✅ Checkpoint W3 (M3 — QNX abstraction alive)
- [ ] `platform/` abstraction layer compile trên Linux.
- [ ] QNX implementations có code đầy đủ.
- [ ] Engine dùng `PlatformThread`/`PlatformMutex`.
- [ ] `docs/QNX_PORT.md` hoàn chỉnh.
- [ ] Giải thích được: Neutrino message passing, resource manager.

---

## 📅 PHASE 4 / TUẦN 4 — POSIX IPC (10%)

> **Goal:** Thêm POSIX message queue, shared memory, eventfd, timerfd. Chứng minh "POSIX API".

### Day 22 (T2) — `posix_mq.hpp/cpp` (2h)

**Files:**
```
include/eventstream/ipc/posix_mq.hpp
src/ipc/posix_mq.cpp
```

**Code gợi ý:**
- `class PosixMessageQueue`:
  - `open(name, flags, maxMsg, msgSize)`.
  - `send(const void* data, size_t len, unsigned prio)`.
  - `receive(void* buf, size_t len, unsigned* prio)`.
  - `notify(sigevent*)`.
  - `close()`, `unlink()`.
- RAII: destructor gọi close.

**Flow send:**
1. `mqd_t mq = mq_open(name, O_RDWR)`.
2. `mq_send(mq, data, len, prio)`.
3. `mq_close(mq)`.

**Commit:** `[W4D22] feat(ipc): PosixMessageQueue implementation`

---

### Day 23 (T3) — `posix_shm.hpp/cpp` + SPSC ring buffer in shm (2.5h)

**Files:**
```
include/eventstream/ipc/posix_shm.hpp
src/ipc/posix_shm.cpp
include/eventstream/ipc/shm_spsc_ring.hpp
```

**Code gợi ý:**
- `class PosixSharedMemory`:
  - `create(name, size)` → `shm_open` + `ftruncate` + `mmap`.
  - `open(name, size)` → `shm_open` + `mmap`.
  - `unmap()`, `unlink()`.
- `class ShmSpscRingBuffer`:
  - Header chứa head/tail atomics.
  - Buffer data theo sau.
  - `push(const T&)` / `std::optional<T> pop()`.

**Flow create:**
1. `fd = shm_open(name, O_CREAT | O_RDWR, 0666)`.
2. `ftruncate(fd, totalSize)`.
3. `ptr = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`.
4. Khởi tạo header.

**Commit:** `[W4D23] feat(ipc): POSIX shared memory + SPSC ring buffer`

---

### Day 24 (T4) — `eventfd.hpp/cpp` + `timerfd.hpp/cpp` (2h)

**Files:**
```
include/eventstream/ipc/eventfd.hpp
include/eventstream/ipc/timerfd.hpp
src/ipc/eventfd.cpp
src/ipc/timerfd.cpp
```

**Code gợi ý:**
- `class EventFd`:
  - `create()` → `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)`.
  - `write(uint64_t val)`.
  - `read()`.
  - `fd()`.
- `class TimerFd`:
  - `create(CLOCK_MONOTONIC)`.
  - `setInterval(std::chrono::nanoseconds)`.
  - `wait()` → `read()` số lần expired.

**Flow TimerFd:**
1. `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)`.
2. `timerfd_settime(fd, 0, &new_value, nullptr)`.
3. `poll`/`read` để đợi.

**Commit:** `[W4D24] feat(ipc): eventfd + timerfd wrappers`

---

### Day 25 (T5) — `posix_signal.hpp/cpp` + `pipe.hpp/cpp` (2h)

**Files:**
```
include/eventstream/ipc/posix_signal.hpp
include/eventstream/ipc/pipe.hpp
src/ipc/posix_signal.cpp
src/ipc/pipe.cpp
```

**Code gợi ý:**
- `class PosixSignal`:
  - `block(int sig)`.
  - `signalfd()`.
  - `setHandler(int sig, handler)`.
- `class Pipe`:
  - `pipe2(fds, O_NONBLOCK)`.
  - `write/read/fd0/fd1`.
- `class PipeIngestServer`:
  - `open(fifoPath)`.
  - Thread đọc pipe, parse frame, push dispatcher.

**Commit:** `[W4D25] feat(ipc): POSIX real-time signals + pipe ingest`

---

### Day 26 (T6) — `PosixMqIngestServer` (2.5h)

**Files:**
```
include/eventstream/ingest/posix_mq_ingest.hpp
src/ingest/posix_mq_ingest.cpp
```

**Code gợi ý:**
- `class PosixMqIngestServer : public IngestServer`.
- Constructor nhận `queueName`, `maxMsg`, `msgSize`.
- `start()`: tạo thread, `mq_open`, vòng lặp `mq_receive`.
- `stop()`: gửi poison pill hoặc `mq_unlink`.
- Parse message thành `EventStream::Event` → `dispatcher_.tryPush()`.

**Flow:**
1. `mq_open`.
2. Vòng lặp `mq_receive`.
3. Parse JSON/binary.
4. Push dispatcher.

**Commit:** `[W4D26] feat(ingest): PosixMqIngestServer`

---

### Day 27 (T7) — `PosixShmIngestServer` (2.5h)

**Files:**
```
include/eventstream/ingest/posix_shm_ingest.hpp
src/ingest/posix_shm_ingest.cpp
```

**Code gợi ý:**
- `class PosixShmIngestServer : public IngestServer`.
- Mở shared memory SPSC ring.
- Thread consumer pop từ ring → push dispatcher.
- Producer (external process) push vào ring.

**Flow:**
1. `PosixSharedMemory::open(name, size)`.
2. Cast header + buffer.
3. Vòng lặp pop.
4. Push dispatcher.

**Commit:** `[W4D27] feat(ingest): PosixShmIngestServer — zero-copy IPC`

---

### Day 28 (CN) — IPC tests + docs + push (2h) → M4 ✓

**Files:**
```
unittest/posix_mq_test.cpp
unittest/posix_shm_test.cpp
unittest/timerfd_test.cpp
unittest/eventfd_test.cpp
docs/POSIX_PRIMITIVES.md
```

**Code gợi ý:**
- `posix_mq_test`: round-trip 100k messages, measure latency.
- `posix_shm_test`: fork 2 processes, producer/consumer qua shm ring.
- `timerfd_test`: 1kHz periodic, assert jitter < threshold.
- `eventfd_test`: wake blocked thread.

**Commit:** `[W4D28] test(ipc): POSIX IPC unit tests + docs`

### ✅ Checkpoint W4 (M4 — POSIX IPC alive)
- [ ] `PosixMqIngestServer` nhận event từ external process.
- [ ] `PosixShmIngestServer` zero-copy hoạt động.
- [ ] `eventfd`/`timerfd` tests pass.
- [ ] `docs/POSIX_PRIMITIVES.md` hoàn chỉnh.

---

## 📅 PHASE 5 / TUẦN 5 — Memory & Computer Architecture Hardening (10%)

> **Goal:** Thay thế `new Node` trong MPSC queue, thêm hazard pointer, hugepage, cache topology.

### Day 29 (T2) — `hazard_pointer.hpp/cpp` (2.5h)

**Files:**
```
include/eventstream/memory/hazard_pointer.hpp
src/memory/hazard_pointer.cpp
```

**Code gợi ý:**
- `class HazardPointer`:
  - `HazardPointer* hp_` thread-local array.
  - `protect(T* ptr)`.
  - `clear()`.
- `class HazardPointerDomain`:
  - Quản lý retired list.
  - `retire(T* ptr, Deleter)`.
  - `collect()`.
- ABA-safe reclamation.

**Flow:**
1. Reader: `hp[0] = ptr`.
2. Reader: đọc data.
3. Reader: `hp[0] = nullptr`.
4. Writer: `retire(oldPtr)` → đưa vào retired list.
5. Writer: `collect()` → chỉ delete nếu không còn hazard pointer trỏ tới.

**Commit:** `[W5D29] feat(memory): hazard pointer lock-free reclamation`

---

### Day 30 (T3) — `object_pool.hpp/cpp` (2h)

**Files:**
```
include/eventstream/memory/object_pool.hpp
src/memory/object_pool.cpp
```

**Code gợi ý:**
- `template<size_t Capacity> class LockFreeObjectPool<T, Capacity>`:
  - Pre-allocate array of T.
  - Free list lock-free stack.
  - `acquire()` / `release(T*)`.

**Flow:**
1. Init: tất cả slots trong free list.
2. `acquire`: pop từ free list.
3. `release`: push vào free list.

**Commit:** `[W5D30] feat(memory): lock-free object pool`

---

### Day 31 (T4) — `HazardPointerMpscQueue` (3h) ⭐

**Files:**
```
include/eventstream/queues/hazard_pointer_mpsc.hpp
```

**Code gợi ý:**
- `template<typename T, size_t Capacity> class HazardPointerMpscQueue`.
- Dùng Vyukov algorithm nhưng node lấy từ `LockFreeObjectPool`.
- Reclaim node bằng hazard pointer.
- `push(const T&)` / `std::optional<T> pop()`.

**Flow push:**
1. `Node* node = pool_.acquire()`.
2. `node->data = item`.
3. `prev = tail_.exchange(node, acq_rel)`.
4. `prev->next.store(node, release)`.

**Flow pop:**
1. `HazardPointer::protect(head_.load()->next)`.
2. Nếu next != nullptr → move item, advance head.
3. `retire(oldHead)`.

**Commit:** `[W5D31] feat(queues): HazardPointerMpscQueue — no new/delete on hot path`

---

### Day 32 (T5) — `hugepage_pool.hpp/cpp` (2h)

**Files:**
```
include/eventstream/memory/hugepage_pool.hpp
src/memory/hugepage_pool.cpp
```

**Code gợi ý:**
- `class HugepagePool`:
  - `allocate(size)` → `mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0)`.
  - `deallocate(ptr, size)`.
  - Fallback to normal mmap nếu hugepage fail.

**Commit:** `[W5D32] feat(memory): hugepage pool allocator`

---

### Day 33 (T6) — `cache_topology.hpp/cpp` (2h)

**Files:**
```
include/eventstream/memory/cache_topology.hpp
src/memory/cache_topology.cpp
```

**Code gợi ý:**
- `struct CpuInfo { int id; int coreId; int socketId; std::vector<int> siblings; };`
- `class CacheTopology`:
  - `parse()` đọc `/sys/devices/system/cpu/cpu*/topology/core_id`, `physical_package_id`, `thread_siblings_list`.
  - `cpusSharingL3(int cpu)`.
  - `firstCpuOnSocket(int socket)`.

**Commit:** `[W5D33] feat(memory): cache topology introspection`

---

### Day 34 (T7) — Wire memory hardening vào engine (2.5h)

**Files:**
```
src/core/events/dispatcher.cpp
src/core/ingest/pool.hpp
src/core/memory/numa.hpp
```

**Code gợi ý:**
- `Dispatcher::inbound_queue_` thay bằng `HazardPointerMpscQueue<EventPtr>`.
- `IngestEventPool` có option `useHugepage`.
- `NUMABinding::bindThreadToNUMANode` dùng `CacheTopology` để chọn CPU share L3.

**Commit:** `[W5D34] feat(core): wire hazard pointer queue + hugepage pool`

---

### Day 35 (CN) — Memory tests + docs + push (2h) → M5 ✓

**Files:**
```
unittest/hazard_pointer_test.cpp
unittest/object_pool_test.cpp
unittest/hugepage_test.cpp
unittest/cache_topology_test.cpp
docs/MEMORY_MODEL.md
```

**Code gợi ý:**
- `hazard_pointer_test`: ABA scenario, concurrent retire/protect.
- `object_pool_test`: acquire/release under contention.
- `hugepage_test`: allocate 2MB, skip nếu không có hugetlb.
- `cache_topology_test`: parse, assert non-empty.

**Commit:** `[W5D35] test(memory): memory hardening tests + MEMORY_MODEL.md`

### ✅ Checkpoint W5 (M5 — Memory hardened)
- [ ] `HazardPointerMpscQueue` thay thế MPSC cũ.
- [ ] Hugepage pool option hoạt động.
- [ ] Cache topology parse được.
- [ ] `docs/MEMORY_MODEL.md` hoàn chỉnh.

---

## 📅 PHASE 6 / TUẦN 6 — Advanced Multithreading & Concurrency (10%)

> **Goal:** Đẩy mạnh thêm multithreading: work-stealing queue, lock-free stack, RCU, seqlock, read-write lock.

### Day 36 (T2) — `work_stealing_queue.hpp` (2.5h)

**Files:**
```
include/eventstream/queues/work_stealing_queue.hpp
```

**Code gợi ý:**
- `template<typename T> class WorkStealingQueue`:
  - Chase-Lev deque.
  - `pushBottom(T)` — owner thread.
  - `popBottom()` — owner thread.
  - `steal()` — other threads.
  - Dùng `std::atomic<size_t> top, bottom` + dynamic array.

**Flow steal:**
1. `size = bottom - top`.
2. Nếu size <= 0 → empty.
3. `top++` CAS.
4. Đọc item.
5. Nếu CAS thất bại → retry.

**Commit:** `[W6D36] feat(queues): work-stealing deque (Chase-Lev)`

---

### Day 37 (T3) — `lock_free_stack.hpp` (2h)

**Files:**
```
include/eventstream/queues/lock_free_stack.hpp
```

**Code gợi ý:**
- `template<typename T> class LockFreeStack`:
  - Treiber stack.
  - `push(T)` — CAS head.
  - `std::optional<T> pop()` — CAS head + hazard pointer.

**Commit:** `[W6D37] feat(queues): lock-free stack with hazard pointers`

---

### Day 38 (T4) — `seqlock.hpp` (2h)

**Files:**
```
include/eventstream/rt/seqlock.hpp
```

**Code gợi ý:**
- `class SeqLock`:
  - `std::atomic<uint64_t> seq_`.
  - `read(T& out)` — lặp: đọc seq (chẵn), copy data, đọc lại seq, so sánh.
  - `write(const T& in)` — tăng seq (lẻ), ghi data, tăng seq (chẵn).

**Commit:** `[W6D38] feat(rt): seqlock for read-heavy shared state`

---

### Day 39 (T5) — `rwlock.hpp` (readers-writer lock) (2h)

**Files:**
```
include/eventstream/rt/rwlock.hpp
src/rt/rwlock.cpp
```

**Code gợi ý:**
- `class RwLock`:
  - `pthread_rwlock_t rwlock_`.
  - `readLock()`, `writeLock()`, `unlock()`.
  - RAII guards.

**Commit:** `[W6D39] feat(rt): readers-writer lock wrapper`

---

### Day 40 (T6) — `WorkStealingThreadPool` (2.5h)

**Files:**
```
include/eventstream/utils/work_stealing_thread_pool.hpp
src/utils/work_stealing_thread_pool.cpp
```

**Code gợi ý:**
- `class WorkStealingThreadPool`:
  - N worker threads, mỗi thread có `WorkStealingQueue<std::function<void()>>`.
  - `submit(task)` → random queue.
  - Worker: popBottom, nếu empty → steal từ queue khác.
  - `shutdown()`.

**Flow worker:**
1. Pop bottom.
2. Nếu empty → lặp qua các queue khác, steal.
3. Nếu vẫn empty → sleep/condition variable.

**Commit:** `[W6D40] feat(utils): WorkStealingThreadPool`

---

### Day 41 (T7) — Multithreading stress tests (2.5h)

**Files:**
```
unittest/work_stealing_queue_test.cpp
unittest/lock_free_stack_test.cpp
unittest/seqlock_test.cpp
unittest/rwlock_test.cpp
```

**Code gợi ý:**
- `work_stealing_queue_test`: N threads push + M threads steal, 1M ops, no lost.
- `lock_free_stack_test`: concurrent push/pop, ABA test.
- `seqlock_test`: 1 writer, 10 readers, readers luôn đọc consistent data.
- `rwlock_test`: 10 readers, 2 writers, no data corruption.

**Commit:** `[W6D41] test(concurrency): work-stealing + seqlock + rwlock stress`

---

### Day 42 (CN) — Concurrency benchmark + push (2h) → M6 ✓

**Files:**
```
benchmark/benchmark_work_stealing.cpp
benchmark/benchmark_lock_free_stack.cpp
docs/CONCURRENCY.md
```

**Code gợi ý:**
- `benchmark_work_stealing`: so sánh `WorkStealingThreadPool` vs `ThreadPool` cũ.
- `benchmark_lock_free_stack`: throughput push/pop.

**Commit:** `[W6D42] bench(concurrency): work-stealing + lock-free stack benchmarks`

### ✅ Checkpoint W6 (M6 — Concurrency supercharged)
- [ ] Work-stealing queue tested.
- [ ] Lock-free stack + hazard pointer.
- [ ] Seqlock + rwlock.
- [ ] `WorkStealingThreadPool` hoạt động.
- [ ] `docs/CONCURRENCY.md` hoàn chỉnh.

---

## 📅 PHASE 7 / TUẦN 7 — Advanced Networking (10%)

> **Goal:** io_uring, SocketCAN, raw socket, protocol parser.

### Day 43 (T2) — `io_uring_socket.hpp/cpp` (2.5h)

**Files:**
```
include/eventstream/net/io_uring_socket.hpp
src/net/io_uring_socket.cpp
```

**Code gợi ý:**
- `class IoUring`:
  - `io_uring_setup(entries, &params)`.
  - `submitAccept(fd)`.
  - `submitRecv(fd, buf, len)`.
  - `submitSend(fd, buf, len)`.
  - `waitCompletion()`.
- `class IoUringIngestServer : public IngestServer`.
- Feature-detect: nếu kernel < 5.1 hoặc `io_uring_setup` fail → throw/compile guard.

**Flow:**
1. Setup ring.
2. Submit accept.
3. Loop `io_uring_wait_cqe`.
4. Xử lý accept/recv/send.

**Commit:** `[W7D43] feat(net): io_uring ingest server`

---

### Day 44 (T3) — `can_socket.hpp/cpp` (2.5h)

**Files:**
```
include/eventstream/net/can_socket.hpp
src/net/can_socket.cpp
```

**Code gợi ý:**
- `struct CanFrame { uint32_t id; uint8_t dlc; uint8_t data[8]; };`
- `class CanSocket`:
  - `open(const char* ifname)` → `socket(PF_CAN, SOCK_RAW, CAN_RAW)`.
  - `bind()`.
  - `send(const CanFrame&)`.
  - `receive(CanFrame&)`.
  - `close()`.

**Commit:** `[W7D44] feat(net): SocketCAN wrapper`

---

### Day 45 (T5) — `CanIngestServer` (2.5h)

**Files:**
```
include/eventstream/ingest/can_ingest.hpp
src/ingest/can_ingest.cpp
```

**Code gợi ý:**
- `class CanIngestServer : public IngestServer`.
- Thread đọc `CanSocket`.
- Parse CAN frame → `EventStream::Event`:
  - topic = `can/<id>`.
  - body = raw bytes.
- Push dispatcher.

**Commit:** `[W7D45] feat(ingest): CanIngestServer`

---

### Day 46 (T6) — `raw_socket.hpp/cpp` + protocol parser (2.5h)

**Files:**
```
include/eventstream/net/raw_socket.hpp
include/eventstream/net/protocol_parser.hpp
src/net/raw_socket.cpp
src/net/protocol_parser.cpp
```

**Code gợi ý:**
- `class RawSocket`:
  - `open(const char* ifname)` → `socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))`.
  - `receive(uint8_t* buf, size_t len)`.
- `class ProtocolParser`:
  - `parseEthernet(buf)` → src/dst MAC, ethertype.
  - `parseIp(buf)` → src/dst IP, protocol.
  - `parseUdp(buf)` → src/dst port, payload.
  - `parseTcp(buf)` → src/dst port, flags, payload.

**Commit:** `[W7D46] feat(net): raw socket + protocol parser`

---

### Day 47 (T7) — `zero_copy.hpp/cpp` (2h)

**Files:**
```
include/eventstream/net/zero_copy.hpp
src/net/zero_copy.cpp
```

**Code gợi ý:**
- `sendfileZeroCopy(int outFd, int inFd, off_t offset, size_t count)`.
- `mmapSend(int fd, const void* addr, size_t len)`.

**Commit:** `[W7D47] feat(net): zero-copy helpers`

---

### Day 48 (CN) — Networking tests + docs + push (2h) → M7 ✓

**Files:**
```
unittest/io_uring_test.cpp
unittest/can_socket_test.cpp
unittest/protocol_parser_test.cpp
docs/NETWORKING.md
```

**Code gợi ý:**
- `io_uring_test`: skip nếu kernel không hỗ trợ.
- `can_socket_test`: yêu cầu `vcan0`, gửi/nhận frame.
- `protocol_parser_test`: parse Ethernet/IP/UDP/TCP headers từ raw packet.

**Commit:** `[W7D48] test(net): io_uring + SocketCAN + protocol parser tests`

### ✅ Checkpoint W7 (M7 — Networking advanced)
- [ ] `IoUringIngestServer` compile + test.
- [ ] `CanIngestServer` nhận từ `vcan0`.
- [ ] Raw socket parser hoạt động.
- [ ] `docs/NETWORKING.md` hoàn chỉnh.

---

## 📅 PHASE 8 / TUẦN 8 — Cross-Compilation Toolchains (10%)

> **Goal:** CMake toolchain files cho QNX, ARM64, ARM HF, musl. CI cross-compile.

### Day 49 (T2) — `aarch64-linux-gnu.cmake` + build (2h)

**Files:**
```
toolchains/aarch64-linux-gnu.cmake
```

**Code gợi ý:**
```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```
- Build: `cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.cmake -B build-arm64 ..`
- Test: `qemu-aarch64 ./build-arm64/EventStreamTests`.

**Commit:** `[W8D49] build(toolchain): aarch64-linux-gnu cross compile`

---

### Day 50 (T3) — `arm-linux-gnueabihf.cmake` + `x86_64-linux-musl.cmake` (2.5h)

**Files:**
```
toolchains/arm-linux-gnueabihf.cmake
toolchains/x86_64-linux-musl.cmake
```

**Code gợi ý:**
- ARM HF: compiler `arm-linux-gnueabihf-g++`.
- musl: compiler `musl-g++`, thêm `-static`.

**Commit:** `[W8D50] build(toolchain): armhf + musl cross compile`

---

### Day 51 (T4) — `qnx710.cmake` + `qnx800.cmake` (2.5h)

**Files:**
```
toolchains/qnx710.cmake
toolchains/qnx800.cmake
```

**Code gợi ý:**
```cmake
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 7.1)
set(CMAKE_C_COMPILER qcc)
set(CMAKE_CXX_COMPILER q++)
set(CMAKE_CXX_FLAGS "-Vgcc_ntoaarch64le")
```
- QNX 8.0 tương tự, version 8.0.

**Commit:** `[W8D51] build(toolchain): QNX SDP 7.1/8.0 toolchain files`

---

### Day 52 (T5) — Docker cross-compile images (2h)

**Files:**
```
docker/Dockerfile.cross
docker/Dockerfile.qnx
docker/docker-compose.yml
```

**Code gợi ý:**
- `Dockerfile.cross`: Ubuntu base, cài cross compilers.
- `Dockerfile.qnx`: base image, copy QNX SDP từ build arg.
- `docker-compose.yml`: services cho từng target.

**Commit:** `[W8D52] build(docker): cross-compile containers`

---

### Day 53 (T6) — GitHub Actions cross-compile workflow (2.5h)

**Files:**
```
.github/workflows/cross_compile.yml
```

**Code gợi ý:**
```yaml
jobs:
  cross:
    strategy:
      matrix:
        target: [arm64, armhf, musl, qnx710, qnx800]
    steps:
      - uses: actions/checkout@v4
      - name: Install cross tools
        run: sudo apt-get install -y g++-aarch64-linux-gnu g++-arm-linux-gnueabihf musl-tools
      - name: Configure
        run: cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/${{ matrix.target }}.cmake -B build ..
      - name: Build
        run: cmake --build build -j$(nproc)
```

**Commit:** `[W8D53] ci: cross-compile matrix — ARM64/musl/QNX`

---

### Day 54 (T7) — Extend `ci.yml` + clang build (2h)

**Files:**
```
.github/workflows/ci.yml
```

**Code gợi ý:**
- Thêm job `native-clang`.
- Thêm job `asan`.
- Thêm job `tsan`.

**Commit:** `[W8D54] ci: native matrix gcc/clang + ASAN + TSAN`

---

### Day 55 (CN) — Cross-compile docs + push (1.5h) → M8 ✓

**Files:**
```
docs/CROSS_COMPILE.md
docs/BUILD.md
```

**Code gợi ý:**
- `CROSS_COMPILE.md`: command từng toolchain, expected output, troubleshooting.
- `BUILD.md`: native build, dependencies, run tests.

**Commit:** `[W8D55] docs(build): CROSS_COMPILE + BUILD instructions`

### ✅ Checkpoint W8 (M8 — Cross-compile proven)
- [ ] ARM64 cross build + qemu test.
- [ ] musl static build.
- [ ] QNX toolchain files sẵn sàng.
- [ ] CI cross-compile matrix green.
- [ ] `docs/CROSS_COMPILE.md` hoàn chỉnh.

---

## 📅 PHASE 9 / TUẦN 9 — Integration & End-to-End Real-Time Demo (8%)

> **Goal:** Tất cả module hoạt động cùng nhau, demo real-time end-to-end.

### Day 56 (T2) — Config extensions (2h)

**Files:**
```
config/config.yaml
src/core/config/loader.cpp
include/eventstream/core/config/config.hpp
```

**Code gợi ý:**
- Thêm struct `RealtimeConfig`, `IpcConfig`, `MemoryConfig`, `NetworkConfig`.
- Parse YAML sections mới.
- `RealtimeConfig` gồm: enabled, policy, priorities, affinity, priority_inheritance.

**Commit:** `[W9D56] feat(config): parse realtime + ipc + memory + network sections`

---

### Day 57 (T3) — `esccore.h` extensions (2h)

**Files:**
```
include/eventstream/bridge/esccore.h
src/bridge/esccore.cpp
```

**Code gợi ý:**
```c
typedef struct {
    int policy;
    int priority;
    int cpu_affinity[8];
    int cpu_count;
} esc_rt_policy_t;

esc_status_t esccore_set_realtime_policy(const esc_rt_policy_t* policy);
esc_status_t esccore_get_latency_histogram(esc_latency_hist_t* out);
esc_status_t esccore_get_thread_stats(esc_thread_stats_t* out);
```

**Commit:** `[W9D57] feat(bridge): extend C API with RT policy + latency hist`

---

### Day 58 (T4) — End-to-end POSIX MQ demo (2.5h)

**Files:**
```
tools/mq_producer.py
```

**Code gợi ý:**
- Python dùng `posix_ipc` module.
- Gửi JSON message mỗi 10ms.
- EventStreamCore `PosixMqIngestServer` nhận, route, process.
- Go/Python SDK nhận output qua `esccore_subscribe`.

**Flow:**
1. `mq = posix_ipc.MessageQueue("/eventstream.ingest", flags=posix_ipc.O_CREAT)`.
2. Loop: `mq.send(json.dumps(evt).encode())`.
3. Engine nhận, parse, route.

**Commit:** `[W9D58] demo(ipc): end-to-end POSIX MQ ingest`

---

### Day 59 (T5) — End-to-end SocketCAN demo (2.5h)

**Files:**
```
tools/can_producer.py
```

**Code gợi ý:**
- Python dùng `python-can`.
- Gửi frame lên `vcan0`.
- `CanIngestServer` nhận, route, process.

**Flow:**
```python
bus = can.interface.Bus(channel='vcan0', bustype='socketcan')
msg = can.Message(arbitration_id=0x123, data=[0x11, 0x22, 0x33])
bus.send(msg)
```

**Commit:** `[W9D59] demo(net): end-to-end SocketCAN ingest`

---

### Day 60 (T6) — RT latency demo (2h)

**Files:**
```
rt_validation/engine_latency_demo.cpp
```

**Code gợi ý:**
- Khởi động engine với SCHED_FIFO.
- Chạy `mq_producer.py` hoặc TCP producer.
- Đo end-to-end latency: timestamp gửi → timestamp nhận output.
- Report p50/p95/p99/max.

**Commit:** `[W9D60] demo(rt): SCHED_FIFO engine under load + latency report`

---

### Day 61 (T7) — Integration tests (2.5h)

**Files:**
```
unittest/integration_mq_test.cpp
unittest/integration_can_test.cpp
unittest/integration_rt_test.cpp
```

**Code gợi ý:**
- `integration_mq_test`: spawn producer subprocess, assert output events.
- `integration_can_test`: gửi frame, assert event topic/body.
- `integration_rt_test`: set SCHED_FIFO, assert policy applied.

**Commit:** `[W9D61] test(integration): MQ + CAN + RT end-to-end tests`

---

### Day 62 (CN) — Integration docs + push (1.5h) → M9 ✓

**Files:**
```
docs/INTEGRATION_DEMOS.md
```

**Code gợi ý:**
- Hướng dẫn chạy từng demo.
- Expected output.
- Screenshot/log mẫu.

**Commit:** `[W9D62] docs(demo): integration demos + lessons W9`

### ✅ Checkpoint W9 (M9 — Integration alive)
- [ ] POSIX MQ end-to-end demo.
- [ ] SocketCAN end-to-end demo.
- [ ] RT latency under load.
- [ ] Integration tests pass.

---

## 📅 PHASE 10 / TUẦN 10 — Docs, README, Interview Prep (7%)

> **Goal:** Polish toàn bộ docs, README bá đạo, chuẩn bị phỏng vấn.

### Day 63 (T2) — `docs/ARCHITECTURE.md` cập nhật (2h)

**Files:**
```
docs/ARCHITECTURE.md
```

**Code gợi ý:**
- ASCII diagram mới:
  - Layer 1: Ingest (TCP/UDP/MQ/SHM/CAN/io_uring/raw).
  - Layer 2: Platform (Linux/QNX).
  - Layer 3: RT primitives (RtThread/RtMutex/...).
  - Layer 4: Queues (MPSC/SPSC/work-stealing/lock-free stack).
  - Layer 5: Processors (realtime/transactional/batch).
  - Layer 6: Storage + C API + SDKs.

**Commit:** `[W10D63] docs: updated ARCHITECTURE.md v2.0`

---

### Day 64 (T3) — README final (2.5h)

**Files:**
```
README.md
```

**Code gợi ý:**
- Badges: CI passing, tests count, ASAN/TSAN, cross-compile targets.
- 30s pitch.
- Architecture ASCII.
- Quick start 3 commands.
- JD mapping table.
- Performance numbers table.

**Commit:** `[W10D64] docs: README final — badges + pitch + JD mapping`

---

### Day 65 (T4) — `docs/INTERVIEW_STORY.md` (2h)

**Files:**
```
docs/INTERVIEW_STORY.md
```

**Code gợi ý:**
- 2-minute pitch.
- 10 Q&A:
  - Làm sao tránh priority inversion?
  - Tại sao dùng hazard pointer?
  - Khác biệt epoll vs io_uring?
  - QNX message passing vs Linux IPC?
  - Làm sao cross-compile QNX?
  - ...

**Commit:** `[W10D65] docs: INTERVIEW_STORY.md + Q&A`

---

### Day 66 (T5) — `docs/lessons.md` tổng hợp (1.5h)

**Files:**
```
docs/lessons.md
```

**Code gợi ý:**
- Tổng hợp lessons từ W1-W9.
- Thêm 10 dòng W10.

**Commit:** `[W10D66] docs: lessons.md full summary`

---

### Day 67 (T6) — CV update + practice (2h)

**Files:**
```
docs/CV_EventStreamCore.md
```

**Code gợi ý:**
- Update CV: "EventStreamCore 2.0 — Real-Time Event Streaming Engine".
- Bullet points: 6 build targets, 25+ tests, 15+ benchmarks, QNX port, RT scheduling.
- Practice pitch 5 lần.

**Commit:** `[W10D67] docs: CV draft + interview practice notes`

---

### Day 68 (CN) — Final push + tag v2.0 (1.5h) → M10 ✓

```bash
git tag v2.0
git push --tags
```

**Commit:** `[W10D68] release: EventStreamCore v2.0 — 60 commits, 10 tags`

### ✅ Checkpoint W10 (M10 — v2.0 ship)
- [ ] README badges + GIF/perf table.
- [ ] `docs/INTERVIEW_STORY.md` hoàn chỉnh.
- [ ] CV update.
- [ ] Pitch trôi chảy < 2 phút.
- [ ] **60 commits, 10 tags, v2.0**.

---

## 📅 PHASE 11 / TUẦN 11 — Bonus: Lock-Free Upgrades & RCU (NICE — cut nếu trễ)

> **Goal:** Đẩy multithreading lên mức "bá đạo" với RCU, epoch-based reclamation.

### Day 69 (T2) — `rcu.hpp` read-copy-update (2.5h)

**Files:**
```
include/eventstream/memory/rcu.hpp
src/memory/rcu.cpp
```

**Code gợi ý:**
- `class RCU`:
  - `readLock()` / `readUnlock()`.
  - `synchronize()` — đợi tất cả readers qua grace period.
  - `call(void* ptr, Deleter)` — queue để free sau grace period.
- Dùng per-thread quiescent state.

**Flow:**
1. Reader: `rcu_read_lock()`.
2. Reader: dereference pointer.
3. Reader: `rcu_read_unlock()`.
4. Writer: copy, modify, publish new pointer.
5. Writer: `synchronize_rcu()`.
6. Writer: free old pointer.

**Commit:** `[W11D69] feat(memory): RCU read-copy-update primitive`

---

### Day 70 (T3) — `epoch_based_reclamation.hpp` (2h)

**Files:**
```
include/eventstream/memory/epoch_based_reclamation.hpp
```

**Code gợi ý:**
- `class EpochBasedReclamation`:
  - Global epoch counter.
  - Per-thread local epoch.
  - Retired list theo epoch.
  - `enterCriticalSection()`, `exitCriticalSection()`, `retire(ptr)`.

**Commit:** `[W11D70] feat(memory): epoch-based reclamation`

---

### Day 71 (T4) — RCU tests (2h)

**Files:**
```
unittest/rcu_test.cpp
```

**Code gợi ý:**
- `TEST(RcuTest, ReadDuringUpdate)`:
  - 1 writer cập nhật shared pointer mỗi 1ms.
  - 10 readers đọc liên tục.
  - Không crash, không use-after-free.

**Commit:** `[W11D71] test(memory): RCU unit tests`

---

### Day 72 (T5) — Apply RCU to `TopicTable` (2.5h)

**Files:**
```
src/core/events/topic_table.cpp
include/eventstream/core/events/topic_table.hpp
```

**Code gợi ý:**
- `TopicTable` dùng `std::shared_ptr<const TopicMap>`.
- Update: tạo bản sao, modify, atomic store shared_ptr.
- Read: `std::shared_ptr<const TopicMap> snapshot = topicMap_.load()`.

**Commit:** `[W11D72] feat(core): RCU-backed TopicTable`

---

### Day 73 (T6) — RCU benchmark (2h)

**Files:**
```
benchmark/benchmark_rcu.cpp
```

**Code gợi ý:**
- So sánh RCU read vs mutex read vs seqlock read.
- 10 readers, 1 writer, 5s.

**Commit:** `[W11D73] bench(memory): RCU read-heavy benchmark`

---

### Day 74 (CN) — Bonus docs + push (1.5h)

**Files:**
```
docs/ADVANCED_RECLAMATION.md
```

**Code gợi ý:**
- So sánh hazard pointer vs RCU vs epoch-based reclamation.
- Khi nào dùng cái nào.

**Commit:** `[W11D74] docs(memory): advanced lock-free reclamation`

### ✅ Checkpoint W11 (NICE)
- [ ] RCU primitive tested.
- [ ] `TopicTable` dùng RCU.
- [ ] RCU benchmark.

---

## 📅 PHASE 12 / TUẦN 12 — Final Hardening & Polish (NICE)

> **Goal:** ASAN/TSAN/UBSAN clean, static analysis, final performance run.

### Day 75 (T2) — UBSAN build + fix (2h)

```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=undefined" -B build-ubsan ..
cmake --build build-ubsan -j$(nproc)
./build-ubsan/unittest/EventStreamTests
```

**Commit:** `[W12D75] test: UBSAN clean`

---

### Day 76 (T3) — Static analysis: clang-tidy + cppcheck (2h)

```bash
clang-tidy src/**/*.cpp -- -Iinclude
clang-tidy src/**/*.cpp -- -Iinclude > clang-tidy.log 2>&1 || true
cppcheck --enable=all --std=c++17 -Iinclude src/ 2>&1 | tee cppcheck.log
```

**Commit:** `[W12D76] chore: clang-tidy + cppcheck fixes`

---

### Day 77 (T4) — Final benchmark run + numbers (2h)

**Files:**
```
docs/PERFORMANCE.md
```

**Code gợi ý:**
- Chạy tất cả benchmark.
- Lưu bảng: component, throughput, p50 latency, p99 latency.

**Commit:** `[W12D77] docs: PERFORMANCE.md with final numbers`

---

### Day 78 (T5) — Final integration test marathon (2h)

```bash
for i in {1..10}; do ./build/unittest/EventStreamTests || break; done
```

**Commit:** `[W12D78] test: final integration marathon — 10x stable`

---

### Day 79 (T6) — Code review + cleanup (2h)

- Xóa TODO, dead code.
- Format consistency.
- Kiểm tra tất cả `#ifdef` platform.

**Commit:** `[W12D79] chore: final cleanup + formatting`

---

### Day 80 (CN) — Final release v2.1 + push (1.5h)

```bash
git tag v2.1
git push --tags
```

**Commit:** `[W12D80] release: EventStreamCore v2.1 — polished`

### ✅ Checkpoint W12 (NICE)
- [ ] ASAN/TSAN/UBSAN clean.
- [ ] clang-tidy/cppcheck clean.
- [ ] `docs/PERFORMANCE.md` final.
- [ ] v2.1 tag.

---

## 🏁 Final Success Metrics

| Metric | Current | Target v2.0 |
|---|---|---|
| Build targets | Linux x86_64 | Linux x86_64, ARM64, ARM HF, musl, QNX 7.1, QNX 8.0 |
| Unit tests | ~6 files | 25+ test files |
| Benchmarks | 6 | 15+ |
| RT scheduling | None | SCHED_FIFO + PI mutex + CPU affinity + cyclictest |
| POSIX IPC | None | mq, shm, eventfd, timerfd, signals, pipe |
| Advanced networking | epoll TCP/UDP | + io_uring, SocketCAN, raw sockets |
| Memory reclamation | `new/delete` | hazard pointer + object pool + hugepages |
| QNX-specific code | 0 | channel, resource manager, interrupt stub |
| Multithreading extras | basic | work-stealing, lock-free stack, seqlock, rwlock, RCU |
| CI jobs | 2 | 8+ |
| Commits/tags | existing | 60+ commits, 10+ tags, v2.0 |

---

## 🎤 Interview Pitch (Sau 60 ngày)

> "EventStreamCore 2.0 là một real-time event streaming engine tôi viết bằng C++17. Engine ingest event qua TCP, UDP, POSIX message queues, shared memory, SocketCAN, và io_uring, sau đó route qua lock-free MPSC/SPSC queues tới realtime, transactional, batch processors.
>
> Tôi pin thread bằng CPU affinity, chạy SCHED_FIFO, dùng priority-inheritance mutex để tránh priority inversion. Memory layer dùng hazard pointers cho lock-free reclamation, object pool, và hugepages để giảm TLB misses.
>
> Engine portable giữa Linux và QNX nhờ `platform/` abstraction layer — trên Linux dùng epoll/timerfd/posix_shm, trên QNX dùng Neutrino message channels và resource managers. Tôi cũng viết sẵn CMake toolchain files cho QNX SDP 7.1/8.0, ARM64, ARM HF, musl, với CI cross-compile.
>
> Project có C API (`libesccore.so`), Go/Python SDKs, 25+ unit test files, 15+ benchmarks, cyclictest-style latency validation, và docs đầy đủ."

---

## 🚀 Next Step

Bắt đầu **Phase 1 / Day 1** — đọc real-time Linux docs + note giấy, sau đó **Day 2** code `RtThread`.
