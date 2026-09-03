# RTOS Foundation & Scheduling Theory

> **Scope:** Core real-time operating system concepts that senior embedded/RTOS interviews expect beyond Linux `SCHED_FIFO`. Covers scheduling theory, task models, synchronization, determinism, watchdog, and deadline monitoring.
> **Target audience:** Senior C++ embedded/RTOS candidates who need to defend EventStreamCore as a real-time platform, not just a fast Linux server.

---

## 1. Why This Module Exists

EventStreamCore 2.0 already adds Linux real-time scheduling (`SCHED_FIFO`, priority inheritance, CPU affinity). But many senior embedded/RTOS JDs distinguish between:

- **Soft real-time on a GPOS** (Linux + PREEMPT_RT)
- **Hard real-time on a true RTOS** (QNX, FreeRTOS, VxWorks, Zephyr, ThreadX)

This module closes that gap by adding:

1. Scheduling theory: RMS, EDF, Liu & Layland bounds.
2. Task model: periodic, sporadic, aperiodic.
3. Determinism rules: no `malloc`, no unbounded loops, pre-allocated pools.
4. Watchdog and deadline monitoring.
5. Static analysis / safety-aware coding patterns.

---

## 2. Scheduling Policies

### 2.1 Fixed-Priority Preemptive Scheduling (FPPS)

- Each task has a static priority.
- Highest-priority ready task runs.
- A running task can be preempted by a higher-priority ready task.
- Used by FreeRTOS, ThreadX, VxWorks, QNX FIFO, Linux `SCHED_FIFO`.

**Pros:** simple, predictable, low overhead.
**Cons:** priority inversion if synchronization is careless; requires priority assignment analysis.

### 2.2 Rate Monotonic Scheduling (RMS)

> **Rule:** Assign priorities inversely proportional to task period. Shortest period = highest priority.

For a set of $n$ independent periodic tasks, RMS is **optimal** among fixed-priority algorithms.

**Liu & Layland utilization bound:**

$$
U = \sum_{i=1}^{n} \frac{C_i}{T_i} \leq n(2^{1/n} - 1)
$$

| n | Utilization bound |
|---|-------------------|
| 1 | 100% |
| 2 | 82.8% |
| 3 | 78.0% |
| 4 | 75.7% |
| ∞ | 69.3% |

If total utilization is below the bound, all deadlines are guaranteed.

**EventStreamCore mapping:**

| Task | Period | Priority (RMS) |
|------|--------|----------------|
| Ingest poll | 100 µs | Highest |
| Dispatch | 250 µs | High |
| Realtime processor | 1 ms | Medium-high |
| Batch flush | 5 s | Low |

### 2.3 Earliest Deadline First (EDF)

> **Rule:** At any moment, run the ready task whose absolute deadline is earliest.

EDF is **optimal** among all dynamic-priority algorithms for preemptive uniprocessors.

**Utilization bound:** up to 100%.

**Pros:** better CPU utilization than RMS.
**Cons:** harder to implement on simple RTOSes; deadline ties need deterministic resolution.

**EventStreamCore use:** `EdfScheduler` is a compile-time/test stub that can be swapped in for research or future hard-deadline tasks.

### 2.4 SCHED_DEADLINE (Linux)

Linux provides `SCHED_DEADLINE` via `sched_setattr()`:

- `runtime`: CPU time needed per period.
- `deadline`: relative deadline.
- `period`: task period.

This is the closest Linux gets to hard real-time scheduling. EventStreamCore documents it as an optional path for cyclictest-style periodic tasks.

---

## 3. Task Model

### 3.1 Periodic Task

Runs at fixed interval $T_i$ with worst-case execution time $C_i$ and relative deadline $D_i$.
m
```cpp
class PeriodicTask {
public:
    PeriodicTask(std::chrono::nanoseconds period,
                 std::function<void()> work);
    void start();
    void stop();
private:
    void runLoop();
    std::chrono::nanoseconds period_;
    std::function<void()> work_;
    std::atomic<bool> running_{false};
};
```

### 3.2 Sporadic Task

Triggered by events with a **minimum inter-arrival time**. Common for interrupt handlers.

### 3.3 Aperiodic Task

No guaranteed arrival pattern. Usually handled by background best-effort threads.

### 3.4 Task States

```
        [Created]
            │
            ▼
        [Ready] ◄───────┐
            │           │
            ▼           │ preempt / yield
        [Running] ──────┘
            │
            ▼ block
        [Blocked]
            │
            ▼ unblock
        [Ready]
            │
            ▼ terminate
        [Terminated]
```

---

## 4. Determinism Rules for Real-Time Threads

### 4.1 No Dynamic Allocation

`malloc`/`free` in a real-time thread is dangerous because:

- May trigger page faults.
- May acquire a global allocator lock.
- Latency is unbounded.

**EventStreamCore rules:**

1. Pre-allocate event pools at startup.
2. Use `LockFreeObjectPool` for queue nodes.
3. Use stack buffers for small fixed-size data.
4. Optional: `mlockall(MCL_CURRENT | MCL_FUTURE)` to pin pages.

### 4.2 Bounded Loops

Every real-time loop must have a bounded iteration count or bounded time.

```cpp
// Good: bounded
for (size_t i = 0; i < max_batch_size; ++i) { ... }

// Bad: unbounded
while (!queue.empty()) { ... }
```

### 4.3 Lock-Free or Bounded Locking

- Hot path: lock-free data structures.
- Cold path: mutex with priority inheritance and bounded hold time.

### 4.4 Avoid Syscalls on Hot Path

Syscalls can cause context switches and cache pollution. Prefer:

- User-space spinlocks for very short critical sections.
- `eventfd`/`timerfd` integrated into existing `epoll` loop instead of separate syscalls.

---

## 5. Priority Inversion & Synchronization

### 5.1 Priority Inversion Scenario

| Time | Thread Low (prio 10) | Thread Medium (prio 50) | Thread High (prio 90) |
|------|----------------------|-------------------------|-----------------------|
| t0 | locks mutex L | — | — |
| t1 | — | preempts Low, runs | — |
| t2 | — | still running | wants L, blocks |
| t3 | cannot run | still running | waits indefinitely |

High is effectively blocked by Medium — a thread that does not even use `L`.

### 5.2 Priority Inheritance Protocol (PIP)

When High blocks on `L` held by Low, Low temporarily inherits High's priority.

| Time | Thread Low (prio 10→90) | Thread Medium (prio 50) | Thread High (prio 90) |
|------|--------------------------|-------------------------|-----------------------|
| t0 | locks mutex L | — | — |
| t1 | — | preempts Low, runs | — |
| t2 | inherits prio 90, preempts Medium | preempted | wants L, blocks |
| t3 | releases L | — | acquires L |

### 5.3 Priority Ceiling Protocol (PCP)

Each mutex has a static ceiling priority = max priority of any thread that can lock it. When a thread locks the mutex, it is immediately boosted to the ceiling.

**Pros:** prevents chained blocking and deadlocks.
**Cons:** requires prior knowledge of all tasks and priorities.

**EventStreamCore choice:** PIP by default (`PTHREAD_PRIO_INHERIT`). PCP available as compile-time option.

---

## 6. Watchdog

### 6.1 Software Watchdog

A thread that expects periodic "pet" events. If the pet is missed, trigger recovery.

```cpp
class RtWatchdog {
public:
    RtWatchdog(std::chrono::milliseconds timeout,
               std::function<void()> on_timeout);
    void pet();          // called by monitored task
    void start();
    void stop();
private:
    void monitorLoop();
    std::atomic<std::chrono::steady_clock::time_point> last_pet_;
    std::chrono::milliseconds timeout_;
    std::function<void()> on_timeout_;
    std::atomic<bool> running_{false};
};
```

### 6.2 Hardware Watchdog

External timer that resets the system if not petted. Used when software watchdog itself may fail.

**EventStreamCore abstraction:** `RtWatchdog` is software-only on Linux; on QNX/embedded it can be wired to a hardware watchdog via `/dev/watchdog` or board-specific API.

---

## 7. Deadline Monitor

Detects when a periodic task misses its deadline.

```cpp
class DeadlineMonitor {
public:
    void registerTask(int task_id, std::chrono::nanoseconds deadline);
    void onStart(int task_id);
    void onComplete(int task_id);  // returns true if deadline met
    size_t missedDeadlineCount(int task_id) const;
};
```

**Actions on miss:** log, increment counter, notify watchdog, degrade to safe state.

---

## 8. Memory in Real-Time Systems

### 8.1 Static Allocation

All memory allocated at initialization. No `malloc`/`free` at runtime.

### 8.2 Memory Pools

`LockFreeObjectPool<T, Capacity>` provides O(1) acquire/release with bounded latency.

### 8.3 Stack Sizing

Each RT task gets a fixed stack. Too small → overflow; too large → waste.

### 8.4 Memory Locking

```cpp
mlockall(MCL_CURRENT | MCL_FUTURE);
```

Prevents page faults by keeping all current and future pages resident.

---

## 9. Interrupts in RTOS

### 9.1 Top-Half / Bottom-Half

- **Top-half (ISR):** minimal work, acknowledge hardware, signal thread.
- **Bottom-half:** deferred processing in a high-priority thread.

### 9.2 QNX Model

ISR runs in kernel space, returns a `sigevent` that wakes a user-space thread.

### 9.3 Linux Model

Threaded interrupts (`request_threaded_irq`) split ISR into hard IRQ and threaded handler.

---

## 10. Safety-Critical Patterns

### 10.1 Fail-Safe State

When a deadline is missed or watchdog fires, transition to a known safe state.

```cpp
enum class SystemState { Normal, Degraded, Safe, Emergency };

class SafeStateManager {
public:
    void enterSafeState();
    void enterEmergencyState();
    SystemState state() const;
};
```

### 10.2 Defensive Coding

- Assert preconditions in debug builds.
- Check all error codes.
- Use bounded buffers.
- Avoid recursion in RT tasks.

### 10.3 Coding Standards

- MISRA C++ 2008
- AUTOSAR C++14
- JSF AV C++

EventStreamCore does not claim full compliance, but documents intent and uses static analysis (clang-tidy, cppcheck) to move toward it.

---

## 11. EventStreamCore Integration

| Component | RTOS Foundation Feature |
|-----------|-------------------------|
| `ProcessManager` | `RmsScheduler` assigns priorities to periodic tasks |
| `RealtimeProcessor` | `DeadlineMonitor` checks 1 ms processing deadline |
| `Dispatcher` | `RtWatchdog` pet each cycle |
| `IngestEventPool` | `LockFreeObjectPool` — no malloc on hot path |
| `BatchProcessor` | Periodic task with bounded window |
| `HazardPointerMpscQueue` | Lock-free reclamation for RT path |

---

## 12. Interview Q&A

**Q: What is the difference between Linux SCHED_FIFO and a true RTOS?**

> A: Linux `SCHED_FIFO` is soft real-time on a general-purpose OS. It gives fixed priority and preempts lower-priority threads, but kernel code may still introduce unbounded latency. A true RTOS like QNX or FreeRTOS has a deterministic kernel, bounded interrupt latency, and scheduling decisions with known worst-case bounds.

**Q: Explain RMS vs EDF.**

> A: RMS assigns fixed priorities inversely proportional to task period. EDF assigns dynamic priorities based on nearest absolute deadline. RMS is simpler and optimal among fixed-priority schemes with a utilization bound around 69% for large task sets. EDF can reach 100% utilization but is harder to implement and analyze.

**Q: How do you avoid malloc in real-time threads?**

> A: Pre-allocate memory pools at startup, use stack buffers, and reserve pages with `mlockall`. In EventStreamCore the ingest event pool and MPSC queue nodes come from a `LockFreeObjectPool`, so the hot path never calls `malloc`.

**Q: What is priority inversion and how do you prevent it?**

> A: Priority inversion occurs when a high-priority thread is blocked by a lower-priority thread indirectly via a medium-priority thread. Priority inheritance boosts the low-priority holder to the high-priority waiter's priority. EventStreamCore uses `PTHREAD_PRIO_INHERIT` mutexes.

**Q: When would you use PCP over PIP?**

> A: PCP is better when all task priorities and mutex usage are known statically because it prevents chained blocking and deadlocks. PIP is more flexible for dynamic systems.

**Q: What is a watchdog and why use both software and hardware?**

> A: A watchdog detects runaway tasks. Software watchdogs are easy to implement but can fail if the OS itself hangs. Hardware watchdogs reset the system independently and are used in safety-critical systems.

---

## 13. References

- Liu, C. L. & Layland, J. W. — "Scheduling Algorithms for Multiprogramming in a Hard-Real-Time Environment" (1973)
- Buttazzo, G. C. — *Hard Real-Time Computing Systems*
- QNX Neutrino RTOS System Architecture
- FreeRTOS documentation: Tasks, Queues, Semaphores
- Zephyr Kernel documentation
- MISRA C++ 2008 / AUTOSAR C++14 guidelines
