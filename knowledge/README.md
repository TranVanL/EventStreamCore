# EventStreamCore — Knowledge Base & Interview Study Guide

> **Purpose:** Deep-dive reference for every technical concept used in EventStreamCore.  
> Each document below is self-contained and links back to the exact source files.

---

## 📑 Table of Contents

| # | Document | Key Topics |
|---|----------|-----------|
| 01 | [Architecture Overview](01_architecture_overview.md) | System layers, component graph, data-flow diagram, startup / shutdown sequence |
| 02 | [Lock-Free Programming](02_lock_free_programming.md) | SPSC ring buffer, Vyukov MPSC queue, CAS-based dedup, `std::memory_order`, false-sharing prevention |
| 03 | [Memory & NUMA](03_memory_and_numa.md) | NUMA topology, `numa_alloc_onnode`, `mbind`, thread-to-CPU binding, `EventPool`, `IngestEventPool`, cache-line alignment (`alignas(64)`) |
| 04 | [Modern C++17 Patterns](04_cpp17_patterns.md) | `std::optional`, `std::string_view`, structured bindings, `if constexpr`, `inline` variables, `[[nodiscard]]` |
| 05 | [Event Pipeline](05_event_pipeline.md) | Dispatcher routing, MPSC→EventBus fan-out, priority-based routing, adaptive pressure downgrade |
| 06 | [Processors & Dead-Letter Queue](06_processors_and_dlq.md) | RealtimeProcessor (SLA, alerts), TransactionalProcessor (dedup, retry), BatchProcessor (windowed flush), DLQ semantics |
| 07 | [Networking & Wire Protocol](07_networking_and_protocol.md) | TCP & UDP ingest servers, length-prefixed frame format, CRC-32 (polynomial 0xEDB88320), `epoll`-less blocking model |
| 08 | [Metrics & Control Plane](08_metrics_and_control.md) | `MetricRegistry` singleton, `ControlPlane` state machine (5 levels), `PipelineStateManager`, Admin monitoring loop |
| 09 | [C API Bridge & Polyglot SDKs](09_bridge_and_sdks.md) | `esccore.h` C ABI, `libesccore.so`, Python ctypes SDK, Go cgo SDK, FFI design principles |
| 10 | [Design Patterns](10_design_patterns.md) | Observer, Singleton, Strategy, Template Method, RAII, Object Pool, CRTP, `if constexpr` type-erasure |
| 11 | [Interview Q & A (50+)](11_interview_qa.md) | System-design questions, C++ trivia, concurrency puzzles, performance tuning — all answered with project code |

---

## How to Use This Guide

1. **First pass** — Read *01 Architecture Overview* to get the big picture.  
2. **Deep dive** — Pick any topic (lock-free, NUMA, networking…) and study the corresponding document.  
3. **Interview prep** — Finish with *11 Interview Q&A* and try to answer each question before reading the solution.  
4. **Code walk** — Every document references exact source files; open them side-by-side for full context.

---

## Project Quick Stats

| Metric | Value |
|--------|-------|
| Language | C++17 |
| Build system | CMake 3.10+ |
| External deps | spdlog, yaml-cpp, libnuma (optional), GTest |
| Lines of C++ | ~5 500 (headers + sources) |
| Lock-free structures | 3 (SPSC ring buffer, MPSC queue, CAS dedup map) |
| Processor types | 3 (Realtime, Transactional, Batch) |
| Network protocols | TCP + UDP with custom binary frame |
| SDK languages | C, Python, Go |
