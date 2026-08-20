# 10 — Quick Reference / Cheat Sheet

> Trang này để ôn nhanh trước phỏng vấn. Không cần đọc chi tiết, chỉ cần nhớ số liệu và mapping.

---

## 🔢 Key Numbers

| Metric | Value |
|--------|-------|
| SPSC throughput | ~125M ops/s |
| SPSC p99 latency | ~12 ns |
| MPSC throughput | ~52M ops/s |
| MPSC p99 latency | ~45 ns |
| Dedup throughput | ~71M ops/s |
| Event pool alloc | ~89M ops/s |
| Realtime queue capacity | 16,384 |
| MPSC queue capacity | 65,536 |
| Transactional queue capacity | 131,072 |
| Batch queue capacity | 32,768 |
| DLQ capacity | 1,000 |
| Dedup buckets | 4,096 |
| Dedup TTL | 1 hour |
| Realtime SLA | 5 ms |
| Batch window | 5 s |
| Transactional retries | 3 |

---

## 📁 File Mapping

| Component | Header | Source |
|-----------|--------|--------|
| Event | `core/events/event.hpp` | — |
| MPSC queue | `core/queues/mpsc.hpp` | — |
| SPSC ring | `core/queues/spsc.hpp` | — |
| Dedup | `core/queues/dedup.hpp` | `src/core/queues/dedup.cpp` |
| EventBus | `core/events/event_bus.hpp` | `src/core/events/event_bus.cpp` |
| Dispatcher | `core/events/dispatcher.hpp` | `src/core/events/dispatcher.cpp` |
| TopicTable | `core/events/topic_table.hpp` | `src/core/events/topic_table.cpp` |
| ProcessManager | `core/processor/manager.hpp` | `src/core/processor/manager.cpp` |
| RealtimeProcessor | `core/processor/processor.hpp` | `src/core/processor/realtime.cpp` |
| TransactionalProcessor | `core/processor/processor.hpp` | `src/core/processor/transactional.cpp` |
| BatchProcessor | `core/processor/processor.hpp` | `src/core/processor/batch.cpp` |
| StorageEngine | `core/storage/storage.hpp` | `src/core/storage/storage.cpp` |
| DLQ | `core/events/dead_letter_queue.hpp` | `src/core/events/dead_letter_queue.cpp` |
| NUMABinding | `core/memory/numa.hpp` | — |
| C API | `bridge/esccore.h` | `src/bridge/esccore.cpp` |
| Main | — | `src/main.cpp` |

---

## 🧠 Memory Ordering Cheat Sheet

| Order | Meaning | Use in Project |
|-------|---------|----------------|
| `relaxed` | No ordering | Approximate counters (`size_`) |
| `acquire` | Don't reorder reads after | Consumer reads (`head->next.load`) |
| `release` | Don't reorder writes before | Producer writes (`head_.store`) |
| `acq_rel` | Both | `tail_.exchange` trong MPSC push |
| `seq_cst` | Total order | Rare, chỉ khi cần global order |

---

## 🛡️ Backpressure States

| State | Trigger | Action |
|-------|---------|--------|
| NORMAL | queue < 50% | Normal processing |
| HIGH | queue 50-80% | Downgrade HIGH → MEDIUM |
| CRITICAL | queue > 80% | Drop oldest/newest to DLQ |

---

## 🎯 Routing Rules

| Priority | Queue |
|----------|-------|
| CRITICAL, HIGH | REALTIME |
| MEDIUM, LOW | TRANSACTIONAL |
| BATCH | BATCH |

---

## ⚡ Real-Time Boot Params

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

---

## 🔑 C API Lifecycle

```c
esccore_init("config/config.yaml");
esccore_subscribe("sensor/", callback, nullptr);
esccore_metrics(&metrics);
esccore_health(&health);
esccore_shutdown();
```

---

## 🗣️ 30-Second Pitch

> "EventStreamCore là C++17 event streaming engine nhúng, không cần Kafka. Hot path lock-free với Vyukov MPSC và SPSC ring buffer đạt hàng chục triệu ops/s và sub-microsecond latency. Engine có 3 queue theo SLA — realtime, transactional, batch — cùng adaptive backpressure và DLQ. Có C API cho Go/Python, và đang upgrade 2.0 để portable sang QNX/RTOS với SCHED_FIFO, priority inheritance, và POSIX IPC."

---

## ⚠️ Known Limitations & Roadmap

| Limitation | Status | Fix |
|------------|--------|-----|
| MPSC allocates `new Node` per push | Current | Object pool + hazard pointer (2.0) |
| Storage mutex | Current | Per-thread buffer / log-structured |
| No QNX support | Roadmap | Platform abstraction layer (2.0) |
| No SCHED_FIFO | Roadmap | RtThread/RtMutex (2.0) |
| No POSIX IPC | Roadmap | posix_mq, posix_shm, eventfd, timerfd (2.0) |
| No io_uring | Roadmap | IoUringIngestServer (2.0) |

---

## 🧮 Useful Formulas

| Formula | Meaning |
|---------|---------|
| $L = \lambda \times W$ | Little's Law: queue depth = arrival rate × time in system |
| $S = \frac{1}{(1-p) + \frac{p}{s}}$ | Amdahl's Law: max speedup with parallel fraction $p$ |
| Latency = Queueing + Processing + Serialization | End-to-end latency breakdown |
| Throughput = $\frac{1}{Average\ Service\ Time}$ | Ideal throughput with no contention |
| Cache line size | 64 bytes (typical x86/ARM) |
| Page size | 4 KB default, 2 MB hugepage, 1 GB hugepage |

---

## 🎯 One-Liner Defenses

| Topic | One-Liner |
|-------|-----------|
| Why 3 queues? | "Different SLAs need different queue semantics." |
| Why lock-free? | "Mutex contention kills tail latency at millions of ops/sec." |
| Why C++17? | "Deterministic memory layout + portable atomics for RTOS." |
| Why policy-based templates? | "Zero runtime overhead on the hot path." |
| Why bounded queues? | "Unbounded queues hide backpressure until it's too late." |
| Why DLQ? | "Dropped events are debuggable failures, not silent losses." |
| Why NUMA binding? | "Remote memory is 6x slower than local memory." |
| Why hazard pointers? | "Safe lock-free reclamation without stopping readers." |

---

## 🪤 Common Trap Answers

| Trap Question | Correct One-Liner |
|---------------|-------------------|
| "Is it distributed?" | "Single-node now; multi-node is a future extension." |
| "Does it replace Kafka?" | "It complements Kafka for low-latency/embedded cases." |
| "Is everything lock-free?" | "Hot path is lock-free; transactional/batch use mutex by design." |
| "Why not Rust?" | "C++17 chosen for RTOS portability and embedded ecosystem." |
| "Is QNX fully tested?" | "QNX path is compile-only validated without hardware." |
| "What's the bottleneck?" | "Currently MPSC allocation and storage mutex; fixing in 2.0." |

---

## 🏛️ Architecture Decision Records (ADRs)

| Decision | Context | Trade-off |
|----------|---------|-----------|
| Vyukov MPSC | N producers → 1 dispatcher | O(1) push, but allocates per push |
| SPSC ring buffer | Realtime queue | Lock-free, bounded, cache-friendly |
| Deque + mutex | Transactional/batch | Ordered delivery, blocking OK |
| shared_ptr | Lifetime across threads | Simple but ~10-20ns overhead |
| Binary append-only storage | Event log | Fast writes, no random reads |
| Singleton registry | Global metrics/observer | Simple but harder to test |
| Policy-based platform | Linux/QNX portability | Zero overhead, compile-time |

---

## 📊 Benchmark Interpretation Cheat Sheet

| Observation | Possible Cause | Action |
|-------------|---------------|--------|
| Throughput low, CPU low | I/O bottleneck, blocking | Check storage/network, profile syscalls |
| Throughput low, CPU high | Lock contention, cache misses | TSan, perf cache-misses |
| p99 spikes | Context switches, interrupts | CPU isolation, disable C-states |
| Latency increases with load | Queueing delay dominates | Scale processors, shard queues |
| MPSC slower than expected | False sharing, allocation | Align atomics, add object pool |

---

## 🗓️ Interview Day Checklist

- [ ] Print/read `10_quick_reference.md`.
- [ ] Review `01_foundation.md` và `02_architecture_defense.md`.
- [ ] Chuẩn bị 3 câu chuyện STAR.
- [ ] Xem JD, identify 5 keywords.
- [ ] Test camera/mic nếu online.
- [ ] Chuẩn bị nước và giấy bút.
- [ ] Ngủ ít nhất 7 tiếng.
- [ ] Đến sớm 10 phút (hoặc join call sớm).

---

## 🎤 60-Second Elevator Pitch

> "EventStreamCore là một C++17 event streaming engine nhúng, tự chứa trong một binary duy nhất, không cần Kafka. Hot path từ ingest đến realtime queue là lock-free: Vyukov MPSC queue đạt 52M ops/s và SPSC ring buffer đạt 125M ops/s. Engine phân loại event thành 3 queue theo SLA — realtime, transactional, batch — với adaptive backpressure và dead letter queue. Có C API để Go/Python integrate. Hiện tôi đang upgrade lên 2.0 để portable sang QNX/RTOS với SCHED_FIFO, priority inheritance, POSIX IPC, và hazard pointers."

---

## 🔥 Hot Path Code Walkthrough (Mental Model)

```
TCP worker thread:
  read(fd, buf) → frame_parser → EventPtr evt
  dispatcher.tryPush(evt)        // MPSC push: tail_.exchange(node)

Dispatcher thread:
  inbound_queue.pop()            // MPSC pop
  route(evt)                     // topic table + pressure
  event_bus.push(REALTIME, evt)  // SPSC push: head_.store(next, release)

RealtimeProcessor thread:
  event_bus.pop(REALTIME, timeout) // SPSC pop: tail_.load(acquire)
  validate → rules → handle
  if storage: storeEvent(evt)
  notifyProcessed(evt)
```

---

## 🧩 JD Keyword → Project Mapping

| JD Keyword | What to Say |
|------------|-------------|
| Modern C++ | C++17, RAII, smart pointers, lambdas, atomics, templates |
| Multithreading | Lock-free queues, mutex + CV, thread pools, CPU affinity |
| POSIX/Linux | epoll, pthread, timerfd, eventfd, POSIX MQ, signals |
| RTOS/QNX | Platform abstraction, Neutrino message passing, resource manager |
| Real-time | SCHED_FIFO, PI mutex, cyclictest, CPU isolation |
| Networking | TCP/UDP, epoll, io_uring, SocketCAN, raw sockets |
| Performance | Benchmarking, NUMA, cache locality, latency percentiles |
| Embedded | Cross-compile, static linking, memory-constrained design |
| System design | Scale, failover, consistency, backpressure |
| Testing | GTest, TSan, stress tests, benchmarks |

---

## ✅ Final Pre-Interview Review

- [ ] Đọc 30-second pitch 3 lần.
- [ ] Nhớ 4 số benchmark.
- [ ] Nhớ 3 queue và implementation.
- [ ] Nhớ memory ordering table.
- [ ] Nhớ shutdown sequence.
- [ ] Nhớ 3 câu chuyện STAR.
- [ ] Nhớ 5 limitations + roadmap.
- [ ] Tự tin, bình tĩnh, honest.
