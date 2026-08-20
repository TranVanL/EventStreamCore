# Phase 1 — Real-Time Scheduling Foundation

## Mục tiêu

Hiểu và triển khai real-time scheduling trên Linux: SCHED_FIFO, CPU affinity, priority inheritance mutex. Đây là nền tảng để engine có thể đảm bảo latency determinism.

---

## 1. Linux Scheduling Policies

### `SCHED_OTHER` (default)

- Time-sharing policy.
- Dynamic priority từ -20 đến +19 (nice value).
- Không đảm bảo real-time — kernel có thể preempt bất cứ lúc nào.

### `SCHED_FIFO` (First-In-First-Out)

- Fixed priority từ 1 đến 99.
- Thread chạy cho đến khi tự block, yield, hoặc bị thread priority cao hơn preempt.
- Không có time slice.
- Dùng cho real-time tasks cần determinism.

### `SCHED_RR` (Round-Robin)

- Giống SCHED_FIFO nhưng có time slice.
- Ít dùng hơn trong real-time hard.

### API

```cpp
#include <sched.h>

struct sched_param param;
param.sched_priority = 80;
pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
```

---

## 2. CPU Affinity

### Tại sao cần?

- Giảm cache thrashing.
- Tránh migration giữa các CPU gây jitter.
- Cô lập real-time core khỏi kernel workqueues.

### API

```cpp
#include <pthread.h>

cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(2, &cpuset);
pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
```

### CPU Isolation (boot param)

```bash
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

- `isolcpus`: CPU không được scheduler đưa task vào.
- `nohz_full`: tắt timer tick trên CPU.
- `rcu_nocbs`: offload RCU callbacks.

---

## 3. Priority Inversion

### Vấn đề

- Thread Low priority giữ lock L.
- Thread Medium priority preempt Low.
- Thread High priority cần lock L → phải đợi Medium (gián tiếp).

### Giải pháp: Priority Inheritance Protocol (PIP)

- Khi High đợi lock do Low giữ, Low tạm thời inherit priority của High.
- Low chạy xong, release lock, High chạy ngay.

### API

```cpp
pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
```

### Priority Ceiling Protocol (PCP)

- Mỗi lock có ceiling priority = max priority của thread nào có thể giữ nó.
- Thread giữ lock được nâng lên ceiling priority ngay lập tức.
- Tránh chained blocking.

```cpp
pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT);
pthread_mutexattr_setprioceiling(&attr, ceiling);
```

---

## 4. Robust Mutex

### Vấn đề

- Thread giữ mutex bị kill → mutex treo.

### Giải pháp

```cpp
pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
```

- Thread tiếp theo lock sẽ nhận `EOWNERDEAD`.
- Gọi `pthread_mutex_consistent()` để phục hồi.
- Nếu `ENOTRECOVERABLE` → mutex không thể dùng.

---

## 5. Monotonic Clock

### Tại sao?

- `CLOCK_REALTIME` có thể bị NTP điều chỉnh → timeout không chính xác.
- `CLOCK_MONOTONIC` không bị điều chỉnh, phù hợp đo latency.

### API

```cpp
clock_gettime(CLOCK_MONOTONIC, &now);
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
```

---

## 6. Cyclictest

### Nguyên lý

- Thread periodic wake every T (ví dụ 1ms).
- Đo `actual_wake_time - expected_wake_time`.
- Tính percentile jitter.

### Cách interpret

- p50: typical jitter.
- p99: tail latency.
- max: worst case (thường do kernel interrupt).

### Giảm jitter

- CPU isolation.
- Disable hyperthreading.
- PREEMPT_RT kernel patch.
- Disable C-states deep.

---

## 7. Design trong EventStreamCore

### `RtThread`

- Wrapper `pthread_setschedparam` + `pthread_setaffinity_np`.
- Graceful EPERM fallback.

### `RtPolicy`

```cpp
struct RtPolicy {
    SchedPolicy policy;      // Other, Fifo, RoundRobin
    int priority;            // 1-99 for FIFO/RR
    std::vector<int> cpus;   // affinity list
};
```

### `RtPolicyBuilder`

```cpp
auto policy = RtPolicyBuilder()
    .fifo()
    .priority(80)
    .cpus({2})
    .build();
```

### Integration

```cpp
auto t = std::thread(&RealtimeProcessor::run, this);
RtThread::apply(t, policy);
```

---

## 8. Common Pitfalls

1. **Không check EPERM** → crash trên cloud/CI.
2. **Dùng `CLOCK_REALTIME` cho timeout** → bị NTP làm sai.
3. **Không set priority inheritance** → priority inversion silent.
4. **Pin nhiều real-time thread cùng CPU** → contention.
5. **SCHED_FIFO mà busy loop** → lock up CPU.

---

## 9. Interview Q&A

**Q: Tại sao SCHED_FIFO lại deterministic hơn SCHED_OTHER?**
A: SCHED_FIFO có fixed priority, không bị time-sliced, và chỉ bị preempt bởi thread priority cao hơn. SCHED_OTHER là time-sharing với dynamic priority.

**Q: Priority inversion là gì? Giải pháp?**
A: High-priority thread bị block bởi low-priority thread giữ lock. Giải pháp: priority inheritance (PTHREAD_PRIO_INHERIT) hoặc priority ceiling (PTHREAD_PRIO_PROTECT).

**Q: Làm sao giảm jitter trong cyclictest?**
A: CPU isolation, disable C-states, PREEMPT_RT kernel, pin thread, dùng monotonic clock.

---

## 10. References

- `man sched`
- `man pthread_setschedparam`
- `man pthread_setaffinity_np`
- `man pthread_mutexattr_setprotocol`
- Linux Foundation Real-Time Wiki
