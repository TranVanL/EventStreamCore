# EventStreamCore — Real-Time Execution Guide

> **Scope:** Linux real-time scheduling and synchronization primitives implemented in EventStreamCore.
>
> **Important:** real-time behavior is a system property. Selecting `SCHED_FIFO` in code does not, by itself, make an application deterministic. Kernel configuration, CPU topology, IRQ placement, memory behavior, permissions, and workload all affect latency.

## 1. Real-time goals

EventStreamCore separates the **data plane** from the **control plane**:

- The data plane processes latency-sensitive events and should avoid unbounded blocking, dynamic allocation, and unnecessary scheduler transitions.
- The control plane handles configuration, diagnostics, persistence, and lower-priority work where throughput is more important than bounded latency.

The RT layer provides reusable Linux/POSIX building blocks:

| Component | Responsibility |
|---|---|
| `RtThread` | Apply scheduling policy, priority, and CPU affinity to a `std::thread`. |
| `RtMutex` | POSIX mutex with configurable protocol and robust-owner recovery. |
| `RtSemaphore` | Counting synchronization, including named POSIX semaphores. |
| `RtBarrier` | Reusable phase barrier for a group of threads. |
| `RtSpinlock` | Short critical sections using an atomic flag. |
| `RtCondvar` | Condition variable using `CLOCK_MONOTONIC` timed waits. |

The implementation is under [include/eventstream/rt](../include/eventstream/rt/) and [src/rt](../src/rt/). The corresponding tests are registered through [unittest/CMakeLists.txt](../unittest/CMakeLists.txt).

## 2. Linux scheduling policies

Linux assigns every thread a scheduling policy and, for real-time policies, a static priority. The main policies used by this project are:

| Policy | Scheduling behavior | Typical use | Main risk |
|---|---|---|---|
| `SCHED_OTHER` | Normal fair scheduling. Threads share CPU time according to weights and runnable load. | Control plane, logging, configuration, background work. | Latency varies under contention. |
| `SCHED_FIFO` | Fixed-priority real-time scheduling. A runnable thread continues until it blocks, yields, or is preempted by a higher-priority RT thread. | Carefully bounded, latency-sensitive workers. | A non-blocking loop can starve the system. |
| `SCHED_RR` | Like FIFO, but equal-priority runnable threads receive a time quantum in round-robin order. | Several peer RT workers that must share a CPU. | Still capable of starving normal-priority work. |

Linux real-time priorities normally range from 1 to 99, where a larger value has higher priority. The exact usable range should be queried with `sched_get_priority_min()` and `sched_get_priority_max()` rather than hard-coded for portable code.

### Policy selection in EventStreamCore

```cpp
using namespace eventstream::rt;

RtPolicy policy = RtPolicyBuilder{}
    .fifo()
    .priority(60)
    .cpus({2})
    .build();

std::thread worker{runLoop};
const bool applied = RtThread::apply(worker, policy);
```

`RtThread::apply()` uses the native pthread handle. `RtThread::applyToSelf()` is useful when a thread configures itself at the beginning of its entry function. `RtThread::describe()` produces a human-readable policy description for logs.

A failed RT request is not automatically a process failure. In particular, an unprivileged process may receive `EPERM` when requesting `SCHED_FIFO` or `SCHED_RR`. The implementation reports failure and leaves the thread alive, allowing a controlled fallback to `SCHED_OTHER`. Tests therefore distinguish between:

1. **Capability-dependent behavior:** the requested RT policy was actually applied.
2. **Always-required behavior:** the worker remained alive and could be joined safely.

## 3. Priority inversion

Priority inversion occurs when a high-priority thread is indirectly blocked by a low-priority thread while a medium-priority thread prevents the low-priority owner from running:

```text
Priority
  high  H ──────────────── waits for lock L ────────────────┐
                                                            │
  medium M ─────────────── runnable; preempts L ────────────┤
                                                            │
  low   L ── owns lock ─────────────────────── releases ────┘
             ▲
             └──────────── shared mutex

Without priority inheritance:
    H waits → M runs → L cannot run → lock release is delayed → H is delayed
```

This is not simply a low-priority thread being slow. The problem is the **unbounded dependency** introduced by the medium-priority workload. In a real-time system, a lock's blocking time must be part of the response-time analysis.

## 4. Priority-inheritance mutexes

`RtMutex` defaults to `PTHREAD_PRIO_INHERIT`. When a higher-priority thread blocks on a mutex owned by a lower-priority thread, the kernel temporarily boosts the owner so it can finish its critical section and release the lock.

```cpp
RtMutex mutex;  // PI + robust recovery by default

{
    RtLockGuard lock(mutex);
    updateSharedState();
} // unlocks even on the normal scope exit
```

The protocol is configurable when a non-default policy is required:

```cpp
RtMutex normalMutex(PTHREAD_PRIO_NONE, false);
RtMutex piMutex(PTHREAD_PRIO_INHERIT, true);
```

### PI does not make a critical section safe

Priority inheritance reduces scheduler-induced blocking; it does not fix:

- a critical section that performs I/O or sleeps;
- lock-order deadlocks;
- unbounded container operations;
- priority inversion through a different lock;
- priority inversion caused by non-inheriting synchronization objects;
- CPU starvation caused by unrelated runaway RT threads.

Keep RT critical sections short, establish a global lock order, and never hold a PI mutex across an operation whose duration is not bounded.

### Robust-owner recovery

A robust mutex can report `EOWNERDEAD` when its owner terminates while holding the lock. `RtMutex::lock()` makes the mutex consistent after detecting that state. The protected data must still be validated or repaired by the application; mutex consistency does not guarantee data consistency.

If recovery is not possible, the state should be treated as unrecoverable rather than silently reused. This is especially important for shared-memory data structures.

## 5. CPU affinity and isolation

Pinning a thread to a CPU reduces migration-related jitter and makes measurements easier to reproduce:

```cpp
RtPolicy policy = RtPolicyBuilder{}
    .fifo()
    .priority(60)
    .cpus({2, 3})
    .build();
```

Affinity is a restriction, not a reservation. The selected CPUs may still execute kernel threads, interrupts, and other processes. Before selecting a CPU, inspect the effective mask:

```bash
nproc
lscpu -e=CPU,NODE,CORE,ONLINE
taskset -pc $$
cat /proc/interrupts
```

For a dedicated latency-sensitive CPU set, a typical kernel command line is:

```text
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

Meaning:

- `isolcpus=2,3` keeps normal scheduler load away from CPUs 2 and 3 where possible;
- `nohz_full=2,3` reduces periodic scheduler ticks on those CPUs when they have a single runnable userspace task;
- `rcu_nocbs=2,3` offloads RCU callback processing from those CPUs.

These options have system-wide consequences and should be validated on the target kernel. They do not replace IRQ affinity, cgroup/cpuset configuration, or application-level thread affinity. On newer kernels, administrative CPU isolation and cpuset/cgroup controls may be preferable to relying only on boot parameters.

### Practical isolation checklist

1. Reserve CPUs only after checking NUMA topology and sibling SMT threads.
2. Pin the RT worker and its memory-producing/consuming companions consistently.
3. Move device IRQs and `ksoftirqd` activity away from the isolated CPU where appropriate.
4. Keep logging, allocation-heavy work, and background maintenance off the RT CPU.
5. Measure with the same CPU topology and workload used in production.

## 6. Timing and blocking rules

For an RT path:

- Prefer `CLOCK_MONOTONIC` for durations and deadlines; never calculate timeouts from wall-clock time.
- Use absolute deadlines for periodic loops to avoid accumulating wake-up drift.
- Bound queue operations and define an overload policy: backpressure, drop, or dead-letter handling.
- Avoid `malloc`, filesystem access, DNS, unbounded logging, and page faults after entering the steady state.
- Pre-fault or lock required memory only when the deployment policy permits it.
- Treat every mutex, condition variable, semaphore, and system call as a possible blocking point.

`RtCondvar` uses a monotonic clock for `waitFor()`, so a wall-clock adjustment cannot unexpectedly extend or shorten a relative timeout. Always use the condition predicate in a loop because wake-ups can be spurious:

```cpp
RtMutex mutex;
RtCondvar condition;
bool ready = false;

RtLockGuard lock(mutex);
while (!ready) {
    if (!condition.waitFor(mutex, std::chrono::milliseconds(10))) {
        // Deadline reached; re-check the application-level policy.
        break;
    }
}
```

## 7. Validation and CI expectations

The RT tests are designed to run both on a privileged developer machine and on ordinary CI runners:

| Test group | What it validates |
|---|---|
| `RtThreadTest` | Policy read-back, affinity, liveness, and graceful `EPERM` fallback. |
| `RtMutexTest` | Locking, timed lock, robust recovery, and PI configuration/contention. |
| `RtSemaphoreTest` | Bounded producer/consumer behavior and named semaphore IPC. |
| `RtBarrierTest` | Reuse across many synchronization phases. |
| `RtSpinlockTest` | Contended counter correctness. |
| `RtCondvarTest` | Monotonic timeout and `notifyAll()` behavior. |
| `RtConditionTest` | Combined mutex/condition convenience wrapper. |

The CI workflow runs this group explicitly before the complete CTest suite. Capability-dependent assertions may be skipped when the runner lacks `CAP_SYS_NICE`, but thread liveness, cleanup, and join safety remain mandatory.

A local validation sequence is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

cd build
ctest -R '^(RtThreadTest|RtMutexTest|RtSemaphoreTest|RtBarrierTest|RtSpinlockTest|RtCondvarTest|RtConditionTest)\.' \
      --output-on-failure -j"$(nproc)"
```

For latency investigation, run the same tests repeatedly under representative load and record kernel, CPU topology, governor, capabilities, and affinity. A green unit test proves correctness of the primitive; it does not prove a production-level worst-case latency bound.

## 8. Troubleshooting

### `Operation not permitted` when applying FIFO/RR

Expected on many developer machines and hosted CI runners. Confirm that the fallback path keeps the worker alive. For a privileged experiment, configure an appropriate `RLIMIT_RTPRIO`/`CAP_SYS_NICE` policy rather than running the entire application as root.

### `Invalid argument` when setting affinity

The requested CPU may be outside the process's effective affinity mask or offline. Select a CPU from `sched_getaffinity()`/`taskset -pc $$` and retry.

### High latency despite FIFO

Check CPU migration, IRQ placement, page faults, thermal throttling, SMT siblings, frequency scaling, kernel preemption configuration, and unbounded work in the critical path. Scheduler policy alone cannot remove these sources of jitter.

### Priority inheritance unavailable

Check the return code from pthread mutex attribute setup and verify the target platform supports `PTHREAD_PRIO_INHERIT`. Do not silently claim PI guarantees on a platform where the attribute was rejected.

## 9. Design summary

The intended hierarchy is:

```text
System policy
    └── CPU isolation / IRQ placement / capabilities
          └── RtThread: policy + priority + affinity
                └── bounded processing loop
                      └── RtMutex: PI + robust recovery
                            └── short, auditable critical section
```

The engineering rule is simple: **make the blocking and scheduling behavior explicit, measure it under the target deployment conditions, and keep a safe fallback when real-time privileges are unavailable.**
