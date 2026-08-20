# Phase 10 — Documentation & Interview Story

## Mục tiêu

Viết README, architecture docs, benchmark report, và chuẩn bị câu chuyện phỏng vấn.

---

## 1. README.md

### Cấu trúc

```markdown
# EventStreamCore 2.0

High-performance, real-time event streaming engine for Linux/QNX.

## Features
- Lock-free MPSC/SPSC queues with hazard pointers
- SCHED_FIFO real-time threads with priority inheritance
- POSIX IPC: message queues, shared memory, eventfd, timerfd
- io_uring, SocketCAN, raw sockets
- QNX Neutrino message passing abstraction
- Cross-compile: ARM64/musl, QNX 7.1/8.0
- Go/Python SDKs

## Build
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run Demo
```bash
sudo ./scripts/setup_vcan.sh
./build/eventstream --config demo.json
```

## Architecture
See docs/ARCHITECTURE.md
```

---

## 2. Architecture Document

### Diagram

```text
┌─────────────────────────────────────────────────────────────┐
│                        Ingest Layer                          │
│  TCP (io_uring) │ UDP │ POSIX MQ │ POSIX SHM │ SocketCAN    │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                    Dispatcher Layer                          │
│         Work-Stealing Queue / MPSC Lock-Free                 │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                   Processing Layer                           │
│  RealtimeProcessor │ TransactionalProcessor │ BatchProcessor │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                    Output Layer                              │
│         POSIX MQ / SHM / C API / Go SDK / Python SDK         │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Benchmark Report

### Metrics

- Throughput: events/second
- P99 latency
- Tail latency
- CPU usage
- Cache misses

### Công cụ

```bash
# Latency
./benchmark_latency --duration 60

# Throughput
./benchmark_throughput --threads 8

# Cache misses
perf stat -e cache-misses,cache-references ./benchmark
```

### Mẫu report

```markdown
| Scenario | Throughput | P99 Latency | P99.9 Latency |
|---|---|---|---|
| TCP ingest, 1 producer | 1.2M evt/s | 12 us | 45 us |
| POSIX SHM, 2 producers | 3.5M evt/s | 5 us | 18 us |
| SocketCAN, 1 producer | 80K evt/s | 25 us | 60 us |
```

---

## 4. Interview Story (STAR)

### Situation

"Dự án EventStreamCore ban đầu là engine event streaming C++17 với lock-free queues và epoll. JD yêu cầu real-time, RTOS/QNX, POSIX API, networking system level."

### Task

"Tôi cần nâng cấp engine để đáp ứng các yêu cầu senior embedded/RTOS, đảm bảo portable giữa Linux và QNX."

### Action

"Tôi thiết kế 6 module:
1. RT scheduling với SCHED_FIFO, PI mutex, CPU affinity.
2. Lock-free memory reclamation với hazard pointers + object pools.
3. QNX portability layer qua policy-based templates.
4. POSIX IPC ingest (mq, shm, eventfd, timerfd).
5. Networking với io_uring, SocketCAN, raw sockets.
6. Cross-compile và CI cho ARM64/musl, QNX."

### Result

"Engine đạt P99 < 20us trên Linux, compile thành công trên QNX, có end-to-end demo với CAN bus."

---

## 5. Common Interview Questions

**Q: Tell me about a challenging concurrency problem you solved.**
A: Lock-free MPSC queue với hazard pointers, giải quyết ABA và memory reclamation.

**Q: How do you ensure real-time behavior?**
A: SCHED_FIFO, priority inheritance, CPU isolation, cache-aware pinning, timerfd.

**Q: How do you port Linux code to QNX?**
A: Policy-based templates, feature detection, abstract platform-specific APIs.

**Q: What is priority inversion and how to prevent it?**
A: Priority inheritance/ceiling mutexes.

---

## 6. References

- STAR method
- Google Technical Writing guide
- CppCon talks on documentation
