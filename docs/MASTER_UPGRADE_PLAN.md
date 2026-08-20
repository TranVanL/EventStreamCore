# EventStreamCore — Master Upgrade Plan

> **Version:** 2.0 Roadmap  
> **Date:** 2026-08-18  
> **Goal:** Transform EventStreamCore from a high-performance Linux event streaming engine into a **real-time, RTOS/QNX-portable, POSIX-hardened event streaming platform** that strongly matches senior embedded/RTOS C++ JDs.

---

## 0. Executive Summary

### Current State

EventStreamCore is a **C++17 event streaming engine** with:

- Lock-free MPSC queue (Vyukov algorithm)
- Lock-free SPSC ring buffer
- Lock-free deduplicator
- `epoll`-based TCP ingest server
- Thread-pool + NUMA binding utilities
- Realtime / Transactional / Batch processors
- C API bridge (`libesccore.so`) with Go + Python SDKs
- Google Test unit tests + microbenchmarks
- GitHub Actions CI (native Linux only)

It is already a **solid Linux concurrency project**, but it is missing the exact keywords that many senior embedded/RTOS JDs filter for: **QNX, RTOS, SCHED_FIFO, priority inheritance, POSIX message queues, shared memory, cross-compilation toolchains, io_uring, hugepages, hazard pointers**.

### Target State

After this upgrade, EventStreamCore will be positioned as:

> **"A real-time event streaming engine portable between Linux and QNX, built with lock-free data structures, real-time scheduling, POSIX IPC, and architecture-aware memory allocation."**

This directly maps to every line of the target JD:

| JD Requirement | How EventStreamCore 2.0 Matches |
|---|---|
| Modern C++ (C++11+, smart pointers, move, lambdas, STL) | C++17 throughout; `std::unique_ptr`, `std::shared_ptr`, `std::move`, `std::optional`, `std::atomic`, custom allocators |
| POSIX API / Linux | `epoll`, `timerfd`, `eventfd`, `mq_*`, `shm_*`, `pthread_*`, `sigaction`, `fcntl`, raw sockets |
| Multithreading / concurrency / sync primitives | Lock-free queues, priority-inheritance mutexes, real-time semaphores, barriers, spinlocks, condition variables |
| RTOS / QNX experience | Full `platform/` abstraction layer; QNX Neutrino message channels, resource manager, `ClockCycles`, cross-compile toolchain |
| OS fundamentals / computer architecture | NUMA, CPU affinity, cache-line alignment, hugepages, memory ordering, cache topology introspection |
| Networking protocols | TCP/UDP, epoll, io_uring, AF_XDP stub, SocketCAN, raw socket protocol parser |
| Cross-compilation toolchains | CMake toolchain files for QNX SDP 7.1/8.0, ARM64, ARM HF, x86_64 musl |

---

## 1. Current Architecture (As-Is)

```
EventStreamCore/
├── include/eventstream/
│   ├── bridge/          esccore.h (C API)
│   └── core/
│       ├── config/      YAML config loader
│       ├── control/     backpressure thresholds
│       ├── events/      Event, EventBus, Dispatcher, TopicTable, DLQ
│       ├── ingest/      epoll, TCP, UDP, frame parser, pool
│       ├── memory/      EventPool, NUMA binding
│       ├── metrics/     histogram, registry
│       ├── processor/   Realtime / Transactional / Batch processors
│       ├── queues/      MPSC, SPSC, dedup
│       ├── storage/     binary persistence
│       └── utils/       thread_pool
├── src/
│   ├── core/            implementation of all core modules
│   ├── bridge/          C API implementation
│   └── main.cpp         standalone server
├── sdk/
│   ├── go/              Go cgo bindings
│   └── python/          Python ctypes bindings
├── benchmark/           microbenchmarks (SPSC, MPSC, dedup, pool, EventBus)
├── unittest/            Google Test suite
├── config/              YAML + topic config
└── .github/workflows/   native Linux CI
```

### Current Strengths

1. **Lock-free hot path** — Vyukov MPSC + SPSC ring buffer.
2. **Backpressure control plane** — adaptive degradation (NORMAL → DEGRADED → CRITICAL → OVERLOAD → EMERGENCY).
3. **Multiple ingest transports** — TCP (thread-per-client + epoll), UDP.
4. **Multiple processing semantics** — realtime, transactional, batch.
5. **FFI-ready** — C API + Go/Python SDKs.
6. **NUMA awareness** — thread + memory binding utilities.

### Current Weaknesses / Gaps

1. **No RTOS/QNX support** — everything is Linux-only.
2. **No real-time scheduling** — threads use default `SCHED_OTHER`.
3. **No priority-inheritance mutexes** — transactional queue uses plain `std::mutex`; risk of priority inversion.
4. **No POSIX IPC** — no message queues, shared memory, or eventfd/timerfd.
5. **No advanced networking** — no io_uring, AF_XDP, raw sockets, SocketCAN.
6. **No architecture-aware memory** — MPSC queue allocates nodes with `new`; no hazard pointers, hugepages, or cache topology introspection.
7. **No cross-compilation story** — only native Linux CI.
8. **No real-time validation** — no cyclictest-style latency tests.

---

## 2. Target Architecture (To-Be)

```
EventStreamCore/
├── include/eventstream/
│   ├── bridge/              esccore.h (C API, extended)
│   ├── core/                existing engine (refactored to use rt/ + platform/)
│   ├── ipc/                 POSIX IPC (mq, shm, eventfd, timerfd, signals)
│   ├── memory/              hugepages, mmap ring, cache topology, hazard pointers
│   ├── net/                 io_uring, raw sockets, SocketCAN, protocol parser
│   ├── platform/            RTOS abstraction + Linux/QNX implementations
│   └── rt/                  real-time scheduling + synchronization primitives
├── src/
│   ├── bridge/
│   ├── core/
│   ├── ipc/
│   ├── memory/
│   ├── net/
│   ├── platform/
│   └── rt/
├── toolchains/              QNX SDP 7.1/8.0, ARM64, ARM HF, musl
├── docker/                  cross-compile containers
├── rt_validation/           cyclictest-style latency tests
├── benchmark/               extended with RT + IPC + memory benchmarks
├── unittest/                extended with RT + IPC + platform tests
├── config/                  YAML config extended for RT/platform settings
├── scripts/                 freeze_aidl.sh-style helpers (not AIDL, but build helpers)
└── .github/workflows/
    ├── ci.yml               native Linux matrix
    └── cross_compile.yml    QNX + ARM64 + musl cross builds
```

---

## 3. Six Upgrade Modules

### Module A — RTOS / QNX Portability Layer ⭐ HIGHEST PRIORITY

**Why first:** This is the single biggest gap vs. the JD. Without QNX/RTOS, the project is "just another Linux server".

**What to build:**

```
include/eventstream/platform/
├── rtos_abstraction.hpp          # Thread, Mutex, Semaphore, CV, Timer interfaces
├── platform_detect.hpp           # __QNX__ / __linux__ macros + feature flags
├── linux/
│   ├── linux_thread.hpp          # pthread wrapper
│   ├── linux_mutex.hpp           # pthread_mutex wrapper
│   ├── linux_semaphore.hpp       # sem_init / sem_t
│   ├── linux_condvar.hpp         # pthread_cond wrapper
│   ├── linux_timer.hpp           # timerfd + CLOCK_MONOTONIC
│   └── linux_channel.hpp         # POSIX message queue fallback for "channel"
└── qnx/
    ├── qnx_thread.hpp            # pthread wrapper (QNX supports pthread)
    ├── qnx_mutex.hpp             # pthread_mutex + priority inheritance
    ├── qnx_semaphore.hpp         # sem_* (QNX supports POSIX semaphores)
    ├── qnx_condvar.hpp           # pthread_cond
    ├── qnx_timer.hpp             # timer_create + ClockCycles
    ├── qnx_channel.hpp           # ChannelCreate / MsgSend / MsgReceive
    ├── qnx_interrupt.hpp         # ISR-to-thread pulse stub
    └── qnx_resource_manager.hpp  # /dev/eventstream resource manager pattern

src/platform/
├── rtos_abstraction.cpp
├── linux/
│   ├── linux_thread.cpp
│   ├── linux_mutex.cpp
│   ├── linux_semaphore.cpp
│   ├── linux_condvar.cpp
│   ├── linux_timer.cpp
│   └── linux_channel.cpp
└── qnx/
    ├── qnx_thread.cpp
    ├── qnx_mutex.cpp
    ├── qnx_semaphore.cpp
    ├── qnx_condvar.cpp
    ├── qnx_timer.cpp
    ├── qnx_channel.cpp
    ├── qnx_interrupt.cpp
    └── qnx_resource_manager.cpp
```

**Key design decisions:**

- Use **policy-based templates**, not virtual inheritance, to avoid runtime overhead on hot path.
- `platform::Thread<Platform>` wraps `pthread_create` + `pthread_setschedparam`.
- `platform::Mutex<Platform>` wraps `pthread_mutex_t` with `PTHREAD_PRIO_INHERIT` on QNX/Linux RT.
- `platform::Channel<Platform>`:
  - Linux fallback: POSIX message queue (`mq_open`/`mq_send`/`mq_receive`).
  - QNX: native `ChannelCreate`/`MsgSend`/`MsgReceive`.
- `platform::Timer<Platform>`:
  - Linux: `timerfd_create` + `CLOCK_MONOTONIC`.
  - QNX: `timer_create` + `SIGEV_THREAD`.

**Tests:**

- `unittest/platform_thread_test.cpp` — create, join, set priority, affinity.
- `unittest/platform_mutex_test.cpp` — lock/unlock, priority inheritance smoke.
- `unittest/platform_channel_test.cpp` — send/receive 1M messages, latency histogram.
- `unittest/platform_timer_test.cpp` — periodic timer accuracy.

**Docs:**

- `docs/QNX_PORT.md` — architecture, channel/resource manager explanation, build instructions.
- `docs/PLATFORM_ABSTRACTION.md` — why policy-based, how to add a new RTOS.

---

### Module B — Real-Time Scheduling & Synchronization ⭐ HIGHEST PRIORITY

**Why first:** Even on Linux, SCHED_FIFO + priority inheritance + real-time semaphores make the project sound like a real RT system, not a generic server.

**What to build:**

```
include/eventstream/rt/
├── rt_thread.hpp          # SCHED_FIFO / SCHED_RR + priority + affinity
├── rt_mutex.hpp           # PTHREAD_PRIO_INHERIT / PTHREAD_PRIO_PROTECT + robust
├── rt_condvar.hpp         # CLOCK_MONOTONIC timedwait
├── rt_semaphore.hpp       # POSIX unnamed/named semaphores
├── rt_barrier.hpp         # pthread_barrier
├── rt_spinlock.hpp        # userspace spinlock + pause/yield
├── rt_policy.hpp          # Policy enum: Realtime, BestEffort, Isochronous
└── rt_affinity.hpp        # CPU set parsing + binding

src/rt/
├── rt_thread.cpp
├── rt_mutex.cpp
├── rt_condvar.cpp
├── rt_semaphore.cpp
├── rt_barrier.cpp
├── rt_spinlock.cpp
└── rt_affinity.cpp
```

**Integration into existing engine:**

| Existing component | Change |
|---|---|
| `ProcessManager::start()` | Pin realtime thread to CPU 2, set `SCHED_FIFO` priority 80. |
| `Dispatcher::dispatchLoop()` | Pin dispatcher thread, set `SCHED_FIFO` priority 70. |
| `EpollIngestServer::workerLoop()` | Pin ingest workers, set `SCHED_FIFO` priority 60. |
| `TransactionalProcessor` queue mutex | Replace `std::mutex` with `RtMutex` (priority inheritance). |
| `BatchProcessor` window timer | Replace `std::this_thread::sleep_for` with `RtTimer` (timerfd). |
| `ThreadPool` | Add constructor overload accepting `RtPolicy`. |

**New config keys in `config.yaml`:**

```yaml
realtime:
  enabled: true
  realtime_policy: "SCHED_FIFO"   # or SCHED_RR
  dispatcher_priority: 70
  ingest_priority: 60
  realtime_processor_priority: 80
  transactional_processor_priority: 50
  batch_processor_priority: 40
  cpu_affinity:
    dispatcher: [0]
    ingest: [1]
    realtime_processor: [2]
    transactional_processor: [3]
    batch_processor: [3]
  priority_inheritance: true
```

**Tests:**

- `unittest/rt_thread_test.cpp` — set SCHED_FIFO, verify policy, graceful EPERM fallback.
- `unittest/rt_mutex_test.cpp` — priority inversion scenario (low-priority holder, high-priority waiter).
- `unittest/rt_semaphore_test.cpp` — producer/consumer with semaphore.
- `unittest/rt_spinlock_test.cpp` — correctness under contention.

**Benchmarks:**

- `benchmark/rt_latency.cpp` — cyclictest-style: wake every 1 ms, measure jitter, report p50/p99/max.
- `benchmark/rt_priority_inversion.cpp` — demonstrate PI mutex prevents inversion.

**Docs:**

- `docs/REAL_TIME.md` — SCHED_FIFO, priority inheritance, priority inversion, CPU isolation tips.

---

### Module C — POSIX IPC & Advanced POSIX Primitives

**Why:** JD explicitly asks for "POSIX API" and "synchronization primitives". This module makes that concrete.

**What to build:**

```
include/eventstream/ipc/
├── posix_mq.hpp           # mqueue: mq_open, mq_send, mq_receive, mq_notify
├── posix_shm.hpp          # shared memory ring buffer
├── posix_signal.hpp       # SIGRTMIN+ real-time signals, signalfd
├── eventfd.hpp            # eventfd notification
├── timerfd.hpp            # high-res periodic timers
├── pipe.hpp               # pipe/fifo ingest
└── unix_socket.hpp        # SOCK_SEQPACKET / SOCK_DGRAM local sockets

src/ipc/
├── posix_mq.cpp
├── posix_shm.cpp
├── posix_signal.cpp
├── eventfd.cpp
├── timerfd.cpp
├── pipe.cpp
└── unix_socket.cpp
```

**Integration into engine:**

- New ingest sources:
  - `PosixMqIngestServer` — external process sends events via `/eventstream.ingest` message queue.
  - `PosixShmIngestServer` — zero-copy SPSC ring buffer in shared memory.
  - `UnixSocketIngestServer` — reliable local socket ingest.
- Replace internal wake mechanism in `ThreadPool` / `EventBusMulti` with `eventfd` instead of `condition_variable` where appropriate.
- Use `timerfd` for precise batch flush timing.

**Tests:**

- `unittest/posix_mq_test.cpp` — round-trip 100k messages.
- `unittest/posix_shm_test.cpp` — two processes, SPSC ring buffer.
- `unittest/timerfd_test.cpp` — 1 kHz periodic accuracy.
- `unittest/eventfd_test.cpp` — wake blocked thread.

**Docs:**

- `docs/POSIX_PRIMITIVES.md` — when to use mq vs shm vs eventfd vs socket.

---

### Module D — Advanced Networking

**Why:** JD asks for "networking protocols". Current engine has basic TCP/UDP. Need io_uring, raw sockets, SocketCAN.

**What to build:**

```
include/eventstream/net/
├── io_uring_socket.hpp    # Linux io_uring (optional, fallback epoll)
├── af_xdp_socket.hpp      # AF_XDP stub
├── raw_socket.hpp         # raw socket + protocol parser
├── zero_copy.hpp          # sendfile / mmap zero-copy
├── protocol_parser.hpp    # Frame parser v2
├── can_socket.hpp         # SocketCAN ingest
└── net_common.hpp         # sockaddr helpers, checksum, endian

src/net/
├── io_uring_socket.cpp
├── raw_socket.cpp
├── can_socket.cpp
└── protocol_parser.cpp
```

**Integration into engine:**

- `IoUringIngestServer` — alternative to `EpollIngestServer` when `CONFIG_IO_URING` available.
- `CanIngestServer` — bind to `vcan0` / `can0`, parse CAN frames into events.
- Raw socket sniffer ingest (optional, for network tap scenarios).

**Tests:**

- `unittest/io_uring_test.cpp` — skip if kernel < 5.1 or no io_uring.
- `unittest/can_socket_test.cpp` — requires `vcan0` setup.
- `unittest/protocol_parser_test.cpp` — Ethernet/IP/UDP/TCP header parsing.

**Docs:**

- `docs/NETWORKING.md` — io_uring vs epoll, SocketCAN, raw sockets.

---

### Module E — Memory & Computer Architecture Hardening

**Why:** JD asks for "computer architecture" and "OS fundamentals". Current MPSC queue uses `new Node` per push — not truly lock-free memory reclamation.

**What to build:**

```
include/eventstream/memory/
├── hugepage_pool.hpp      # hugetlbfs / MAP_HUGETLB allocator
├── mmap_ring.hpp          # mmap-based SPSC ring buffer
├── cache_topology.hpp     # parse /sys/devices/system/cpu
├── memory_order.hpp       # memory ordering docs + compile-time fences
├── hazard_pointer.hpp     # lock-free reclamation
├── object_pool.hpp        # lock-free object pool
└── numa_allocator.hpp     # NUMA-aware allocator

src/memory/
├── hugepage_pool.cpp
├── mmap_ring.cpp
├── cache_topology.cpp
├── hazard_pointer.cpp
├── object_pool.cpp
└── numa_allocator.cpp
```

**Integration into engine:**

- Replace `MpscQueue` node allocation with `HazardPointerMpscQueue<T, Capacity>` using pre-allocated object pool.
- Add `HugepageEventPool` option for ingest event pool.
- Use `CacheTopology` to pin threads to cores sharing L3 cache.
- Add `alignas(64)` audit across all hot-path structs.

**Tests:**

- `unittest/hazard_pointer_test.cpp` — ABA-safe reclamation.
- `unittest/object_pool_test.cpp` — acquire/release under contention.
- `unittest/hugepage_test.cpp` — skip if hugetlbfs not mounted.
- `unittest/cache_topology_test.cpp` — parse topology, assert non-empty.

**Docs:**

- `docs/MEMORY_MODEL.md` — memory ordering, cache coherency, false sharing, hazard pointers.

---

### Module F — Cross-Compilation Toolchains & CI

**Why:** JD explicitly asks for "experience with cross-compilation toolchains".

**What to build:**

```
toolchains/
├── qnx710.cmake           # QNX SDP 7.1 (QCC 5.4)
├── qnx800.cmake           # QNX SDP 8.0 (QCC 12)
├── aarch64-linux-gnu.cmake
├── arm-linux-gnueabihf.cmake
├── x86_64-linux-musl.cmake
└── README.md

docker/
├── Dockerfile.cross       # multi-arch cross build image
├── Dockerfile.qnx         # QNX SDP image (requires user-provided SDP tarball)
└── docker-compose.yml

.github/workflows/
├── ci.yml                 # extended native matrix
└── cross_compile.yml      # QNX + ARM64 + musl cross builds
```

**CMake changes:**

- Detect target platform via `CMAKE_SYSTEM_NAME`:
  - `Linux` → build `linux/` platform + `epoll`/`timerfd`.
  - `QNX` → build `qnx/` platform + Neutrino channel/resource manager stubs.
- Option `ENABLE_IO_URING` (default ON on Linux, OFF on QNX).
- Option `ENABLE_HUGEPAGES` (default ON on Linux, OFF on QNX).
- Option `ENABLE_RT_SCHEDULING` (default ON on Linux, OFF if EPERM).

**CI matrix:**

| Job | Target | Notes |
|---|---|---|
| native-linux-gcc | x86_64 Linux | existing |
| native-linux-clang | x86_64 Linux | add clang |
| cross-arm64 | aarch64-linux-gnu | compile-only + qemu-user test |
| cross-armhf | arm-linux-gnueabihf | compile-only |
| cross-musl | x86_64-linux-musl | static link |
| cross-qnx710 | QNX SDP 7.1 | compile-only (no runtime) |
| cross-qnx800 | QNX SDP 8.0 | compile-only (no runtime) |

**Docs:**

- `docs/CROSS_COMPILE.md` — step-by-step for each toolchain.
- `docs/BUILD.md` — updated build instructions.

---

## 4. Refactoring Plan for Existing Code

### 4.1 Core engine changes

| File | Change |
|---|---|
| `src/core/processor/manager.cpp` | Use `rt::RtThread` + `rt::RtPolicy` for processor threads. |
| `src/core/events/dispatcher.cpp` | Use `rt::RtThread` for dispatcher; replace sleep with `eventfd` wake if beneficial. |
| `src/core/ingest/epoll.cpp` | Keep as Linux implementation; add `IoUringIngestServer` alternative. |
| `src/core/queues/mpsc.hpp` | Deprecate `new Node` version; introduce `memory::HazardPointerMpscQueue`. |
| `src/core/ingest/pool.hpp` | Add hugepage-backed pool option. |
| `src/core/events/event_bus.hpp` | Use `rt::RtMutex` for transactional/batch queues. |

### 4.2 C API extensions

Add to `esccore.h`:

```c
esc_status_t esccore_set_realtime_policy(const esc_rt_policy_t* policy);
esc_status_t esccore_get_latency_histogram(esc_latency_hist_t* out);
esc_status_t esccore_get_thread_stats(esc_thread_stats_t* out);
```

### 4.3 Config extensions

Add sections to `config.yaml`:

```yaml
platform:
  target: "linux"   # linux | qnx

realtime:
  enabled: true
  policy: "SCHED_FIFO"
  priority_inheritance: true

ipc:
  posix_mq:
    enable: false
    queue_name: "/eventstream.ingest"
  posix_shm:
    enable: false
    segment_name: "/eventstream.shm"

memory:
  hugepages: false
  hazard_pointer_buckets: 4096

network:
  io_uring: false
  socketcan:
    enable: false
    interface: "vcan0"
```

---

## 5. Test & Benchmark Expansion

### 5.1 New unit tests

```
unittest/
├── platform_thread_test.cpp
├── platform_mutex_test.cpp
├── platform_channel_test.cpp
├── platform_timer_test.cpp
├── rt_thread_test.cpp
├── rt_mutex_test.cpp
├── rt_semaphore_test.cpp
├── rt_spinlock_test.cpp
├── posix_mq_test.cpp
├── posix_shm_test.cpp
├── timerfd_test.cpp
├── eventfd_test.cpp
├── hazard_pointer_test.cpp
├── object_pool_test.cpp
├── hugepage_test.cpp
├── cache_topology_test.cpp
├── io_uring_test.cpp
├── can_socket_test.cpp
└── protocol_parser_test.cpp
```

### 5.2 New benchmarks

```
benchmark/
├── benchmark_rt_latency.cpp
├── benchmark_rt_priority_inversion.cpp
├── benchmark_posix_mq.cpp
├── benchmark_posix_shm.cpp
├── benchmark_hazard_pointer.cpp
├── benchmark_io_uring.cpp
└── benchmark_can_socket.cpp
```

### 5.3 RT validation suite

```
rt_validation/
├── cyclictest_runner.cpp      # 1 kHz wake, report jitter
├── sched_fifo_stress.cpp      # 24h stress under load
└── priority_inversion_demo.cpp # visual/log proof of PI
```

---

## 6. Documentation Plan

| Doc | Purpose |
|---|---|
| `docs/ARCHITECTURE.md` | Updated end-to-end architecture diagram |
| `docs/QNX_PORT.md` | QNX-specific porting guide |
| `docs/REAL_TIME.md` | SCHED_FIFO, PI, CPU isolation |
| `docs/POSIX_PRIMITIVES.md` | mq/shm/eventfd/timerfd usage |
| `docs/NETWORKING.md` | io_uring, SocketCAN, raw sockets |
| `docs/MEMORY_MODEL.md` | hazard pointers, cache lines, NUMA |
| `docs/CROSS_COMPILE.md` | toolchain instructions |
| `docs/BUILD.md` | build/run instructions |
| `docs/INTERVIEW_STORY.md` | 2-minute pitch for interviews |

---

## 7. Implementation Order & Timeline

### Recommended order

| Phase | Module | Duration | Why this order |
|---|---|---|---|
| 1 | **B — Real-Time Scheduling & Sync** | 1 week | Builds on existing `std::thread`/`std::mutex`; easy wins. |
| 2 | **A — RTOS/QNX Portability Layer** | 1.5 weeks | Highest JD impact; uses primitives from Module B. |
| 3 | **C — POSIX IPC** | 1 week | Natural extension of platform layer. |
| 4 | **E — Memory & Architecture** | 1 week | Replaces MPSC queue internals; do after sync primitives. |
| 5 | **D — Advanced Networking** | 1 week | Optional but strong for "networking protocols". |
| 6 | **F — Toolchains & CI** | 0.5 weeks | Tie everything together with cross-compile proof. |

**Total: ~6 weeks** full depth, or **~3 weeks** if focusing on A+B+C+F.

### Milestones

| Milestone | Definition of Done |
|---|---|
| M1 — RT Core | `ProcessManager` threads run SCHED_FIFO; PI mutex in transactional queue; latency benchmark reports p99. |
| M2 — QNX Abstraction | Code compiles with QNX toolchain; `platform::Channel` has Linux + QNX implementations. |
| M3 — POSIX IPC | `PosixMqIngestServer` and `PosixShmIngestServer` ingest events end-to-end. |
| M4 — Memory Hardening | `HazardPointerMpscQueue` replaces old MPSC; hugepage pool option works. |
| M5 — Advanced Networking | `IoUringIngestServer` and `CanIngestServer` compile and pass tests. |
| M6 — Cross-Compile | CI green for ARM64, musl, QNX SDP 7.1/8.0 cross builds. |

---

## 8. Interview Pitch (After Upgrade)

> "EventStreamCore is a real-time event streaming engine I built in C++17. It ingests events over TCP, UDP, POSIX message queues, shared memory, and SocketCAN, then routes them through lock-free MPSC/SPSC queues to realtime, transactional, and batch processors.
>
> For determinism, I pin threads to isolated CPUs and run them under `SCHED_FIFO` with priority inheritance mutexes to prevent priority inversion. The memory layer uses hazard pointers for lock-free reclamation and optional hugepages to reduce TLB misses.
>
> The engine is portable: a `platform/` abstraction layer lets it compile for both Linux and QNX. On Linux it uses epoll/timerfd/posix_shm; on QNX it uses Neutrino message channels and resource managers. I also maintain CMake toolchain files for QNX SDP 7.1/8.0, ARM64, ARM HF, and musl, with CI cross-compile jobs.
>
> It ships with a C API (`libesccore.so`), Go and Python SDKs, Google Test suite, microbenchmarks, and cyclictest-style latency validation."

---

## 9. Success Metrics

| Metric | Current | Target |
|---|---|---|
| Build targets | Linux x86_64 | Linux x86_64, ARM64, ARM HF, musl, QNX 7.1, QNX 8.0 |
| Unit tests | ~6 files | 20+ test files |
| Benchmarks | 6 | 12+ |
| RT scheduling | None | SCHED_FIFO + PI mutex + CPU affinity |
| POSIX IPC | None | mq, shm, eventfd, timerfd, signals |
| Advanced networking | epoll TCP/UDP | + io_uring, SocketCAN, raw sockets |
| Memory reclamation | `new/delete` per MPSC push | hazard pointer + object pool |
| QNX-specific code | 0 | channel, resource manager, interrupt stub |
| CI jobs | 2 | 6+ |

---

## 10. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| No access to real QNX hardware | Use QNX SDP cross-compile only; document compile-only validation. |
| `SCHED_FIFO` fails on CI/cloud | Graceful fallback to `SCHED_OTHER`; tests assert fallback path. |
| io_uring unavailable on old kernels | Feature-detect at compile/runtime; fallback to epoll. |
| Hugepages not mounted | Skip test with warning; keep default pool. |
| Scope creep | Strict module boundaries; each module has its own CMake target and tests. |

---

## 11. Next Step

Start **Phase 1 — Module B: Real-Time Scheduling & Synchronization**.

This phase gives immediate value: existing engine becomes demonstrably real-time on Linux, and the primitives (`RtThread`, `RtMutex`, `RtSemaphore`) are reused by Module A for QNX.
