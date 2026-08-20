# EventStreamCore 2.0 — CV Bullet Points (Future Version)

> Dựa trên MASTER_UPGRADE_PLAN.md và PLAN_DETAILED.md. Các bullet này phản ánh project sau 60-80 ngày upgrade, với RTOS/QNX portability, real-time scheduling, POSIX IPC, advanced networking, và memory hardening.

---

## Final Version — 5 Bullets (Recommended)

```
• Architected a real-time C++17 event streaming engine sustaining 10M+ events/sec at P99 < 2µs via lock-free SPSC/MPSC queues and cache-aware memory layout.

• Built a portable Linux/QNX platform layer with policy-based templates,
  cross-compiling to x86_64, ARM64, ARM HF, musl, QNX SDP 7.1, and QNX 8.0.

• Hardened determinism with SCHED_FIFO, priority-inheritance mutexes, CPU
  isolation, and cyclictest validation (P99 jitter < 1µs on isolated cores).

• Integrated multi-transport ingest (TCP/UDP/epoll, io_uring, POSIX MQ, shared
  memory, SocketCAN) with adaptive back-pressure and 3-tier priority routing.

• Eliminated hot-path allocation with hazard-pointer reclamation, object pools,
  optional hugepages, and NUMA-aware binding; shipped C API + Python/Go SDKs.
```

---

## Alternative — 4 Bullets (If CV Space Is Tight)

```
• Built a real-time C++17 event engine processing 10M+ events/sec at P99 < 2µs
  via lock-free SPSC/MPSC queues and cache-aware memory layout.

• Designed a portable Linux/QNX platform layer with policy-based templates,
  cross-compiling to x86_64, ARM64, ARM HF, musl, QNX SDP 7.1, and QNX 8.0.

• Hardened determinism with SCHED_FIFO, priority-inheritance mutexes, CPU
  isolation, and cyclictest validation (P99 jitter < 1µs).

• Integrated multi-transport ingest (TCP/UDP/epoll, io_uring, POSIX MQ, shared
  memory, SocketCAN) with adaptive back-pressure, C API, and Python/Go SDKs.
```

---

## Notes on Numbers

| Claim | Source / Basis |
|-------|---------------|
| 10M+ events/sec | Conservative end-to-end throughput estimate; component benchmarks show 52M–125M ops/s. |
| P99 < 2µs | End-to-end target under CPU isolation; component SPSC P99 ~12 ns. |
| P99 jitter < 1µs | Cyclictest target on isolated CPU with PREEMPT_RT. |
| 6 build targets | x86_64, ARM64, ARM HF, musl, QNX 7.1, QNX 8.0. |
| 25+ unit tests | PLAN_DETAILED.md target for Phase 9 + 11. |
| 15+ benchmarks | Phase 1-7 benchmarks + RT/IPC/memory/network additions. |

> **Important:** Nếu chưa hoàn thành toàn bộ roadmap, hãy điều chỉnh số liệu hoặc dùng từ "targeting" / "validated on Linux path". Trung thực về những gì đã làm là quan trọng nhất.

---

## How to Choose

- **Final Version (5 bullets):** Dùng cho hầu hết CV — đủ ngắn, đủ mạnh, cover đủ keywords.
- **Alternative (4 bullets):** Dùng khi CV đã rất dài hoặc project này chỉ là 1 trong nhiều entries.

---

## Suggested Placement

Đặt block này trong CV dưới mục **Projects** hoặc **Experience**:

```
EventStreamCore 2.0 — Real-Time Event Streaming Engine        2026
[paste 5-6 bullets here]
Tech: C++17, POSIX, pthreads, epoll, io_uring, SocketCAN, QNX Neutrino,
      CMake, Docker, GitHub Actions, Google Test, Python/Go FFI
```
