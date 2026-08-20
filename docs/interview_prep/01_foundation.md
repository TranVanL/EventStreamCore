# 01 — Foundation Interview Questions

> Câu hỏi nền tảng dành cho vòng phone screen hoặc phỏng vấn mid-level. Trả lời ngắn gọn, rõ ràng, đi thẳng vào vấn đề.

---

## 1. "Tell me about EventStreamCore — what is it and why did you build it?"

**Answer template:**

> EventStreamCore là một **C++17 event streaming engine** nhúng được, không phụ thuộc Kafka/RabbitMQ. Nó ingest, route, process và persist events với throughput hàng triệu ops/sec và latency dưới microsecond.
>
> Tôi build nó để giải quyết khoảng trống giữa 2 loại hệ thống:
> - **Quá nặng:** Kafka/RabbitMQ cần infrastructure riêng, network hop, GC pause.
> - **Quá đơn giản:** `std::queue` + `std::mutex` bị lock contention và không xử lý được priority.
>
> EventStreamCore nằm ở giữa: một binary duy nhất, hot path lock-free, có backpressure, deduplication, priority routing, và failure handling.

**Key numbers to mention:**
- SPSC ring buffer: ~125M ops/s, ~12 ns
- MPSC queue: ~52M ops/s, ~45 ns
- Lock-free dedup: ~71M ops/s, ~32 ns

---

## 2. "What are the main components?"

**Answer:**

```
Ingest Layer (TCP/UDP) → MPSC Queue → Dispatcher → Event Bus → Processors → Storage/DLQ
```

| Component | File chính | Vai trò |
|-----------|-----------|---------|
| TCP/UDP ingest | `src/core/ingest/tcp.cpp`, `udp.cpp`, `epoll.cpp` | Nhận event từ network |
| MPSC queue | `include/eventstream/core/queues/mpsc.hpp` | Fan-in N producers → 1 dispatcher |
| Dispatcher | `src/core/events/dispatcher.cpp` | Route event theo topic + priority |
| Event Bus | `src/core/events/event_bus.cpp` | 3 queue: realtime, transactional, batch |
| Processors | `src/core/processor/*.cpp` | Xử lý event theo từng SLA |
| Storage | `src/core/storage/storage.cpp` | Persist event dạng binary |
| DLQ | `src/core/events/dead_letter_queue.cpp` | Lưu event bị drop để debug |
| C API | `include/eventstream/bridge/esccore.h` | FFI cho Go/Python |

---

## 3. "Why three queues (realtime, transactional, batch)? Why not one?"

**Answer:**

> Vì mỗi loại event có **SLA khác nhau**:
>
> - **Realtime:** cần latency cực thấp, bounded. Dùng SPSC ring buffer lock-free, capacity 16384.
> - **Transactional:** cần ordered delivery, consistent writes. Dùng `std::deque` + mutex + condition variable.
> - **Batch:** chấp nhận delay để aggregate. Dùng deque + window 5s.
>
> Nếu gộp chung, realtime event sẽ bị block bởi batch/transactional → vi phạm SLA.

---

## 4. "How does backpressure work?"

**Answer:**

> Hệ thống monitor queue depth và điều chỉnh behavior theo 3 mức pressure:
>
> - **NORMAL:** hoạt động bình thường.
> - **HIGH:** downgrade HIGH priority event xuống MEDIUM.
> - **CRITICAL:** tiếp tục downgrade, realtime queue đầy thì drop oldest/newest.
>
> Ngoài ra còn có control plane ở `ControlThresholds` (`include/eventstream/core/control/thresholds.hpp`) với `recovery_factor` để tránh flapping.

**Evidence from code:**
- `EventBusMulti::push()` tính pressure dựa trên `RealtimeBus_.ringBuffer.size()`.
- `Dispatcher::adaptToPressure()` downgrade priority khi pressure HIGH/CRITICAL.
- Realtime queue overflow: `DROP_OLD` — pop oldest, push vào DLQ, rồi thử push lại.

---

## 5. "What is the hot path? Why is it fast?"

**Answer:**

> Hot path = từ lúc ingest đến lúc event được đưa vào realtime queue.
>
> Nó nhanh vì:
> 1. **Lock-free:** Vyukov MPSC queue dùng `atomic::exchange`, không mutex.
> 2. **No allocation trên hot path:** Event được pre-allocated từ `IngestEventPool`.
> 3. **Cache-friendly:** `alignas(64)` head/tail/size để tránh false sharing.
> 4. **Bit masking:** SPSC ring buffer capacity là power-of-2, wrap-around bằng `& (N-1)` thay vì `%`.

---

## 6. "What is the C API for?"

**Answer:**

> `libesccore.so` cung cấp C API (`esccore.h`) để các ngôn ngữ khác có thể dùng qua FFI:
> - Go: cgo
> - Python: ctypes
>
> C API dùng flat structs (`esc_event_t`), không heap allocation, không exception, thread-safe cho subscribe/metrics.

**Lifecycle:**
```c
esccore_init("config/config.yaml");
esccore_subscribe("sensor/", callback, nullptr);
esccore_shutdown();
```

---

## 7. "How do you test this project?"

**Answer:**

> 3 lớp test:
>
> 1. **Unit tests** (`unittest/`): GTest cho config loader, event processor, lock-free dedup, TCP ingest, storage.
> 2. **Benchmarks** (`benchmark/`): SPSC, MPSC, dedup, event pool, EventBus multi.
> 3. **System/stress tests** (`tests/`): Python scripts gửi TCP/UDP event, stress test.
>
> Ngoài ra roadmap 2.0 có thêm RT validation (`rt_validation/cyclictest_runner.cpp`) để đo jitter.

---

## 8. "What would you improve next?"

**Answer (honest + roadmap-aware):**

> Project đang trong upgrade 2.0. Các hướng improve chính:
> 1. **Real-time scheduling:** SCHED_FIFO + priority inheritance + CPU affinity.
> 2. **QNX portability:** platform abstraction layer với policy-based templates.
> 3. **POSIX IPC:** message queue, shared memory, eventfd/timerfd.
> 4. **Memory hardening:** hazard pointers, object pool, hugepages.
> 5. **Advanced networking:** io_uring, SocketCAN, raw sockets.
> 6. **RCU:** cho config hot-reload không block readers.

---

## 9. "What is the most challenging bug you fixed?"

**Answer template (dùng nếu chưa có bug thật):**

> Một challenge tôi gặp là **false sharing trong MPSC queue**. Ban đầu `head_`, `tail_`, `size_` nằm gần nhau, khi nhiều producer đẩy vào thì cache line bị ping-pong giữa các CPU.
>
> Fix: thêm `alignas(64)` cho từng atomic field. Kết quả throughput MPSC tăng rõ rệt.
>
> Bài học: trên x86 tưởng không sao nhưng trên ARM/NUMA thì false sharing rất đắt.

---

## 10. "Why C++17 and not Rust/Go/Java?"

**Answer:**

> Vì project target là **low-latency embedded/RTOS**:
> - C++17 cho deterministic memory layout, RAII, zero-cost abstraction.
> - Lock-free atomics chuẩn C++11/17, portable x86/ARM/QNX.
> - Go có GC pause không phù hợp sub-microsecond latency.
> - Java cũng có GC và JNI overhead.
>
> Tuy nhiên tôi vẫn wrap C API để Go/Python có thể integrate khi cần.

---

## 11. "Why not just use Kafka?"

**Answer:**

> Kafka là excellent distributed log, nhưng không phải lúc nào cũng phù hợp:
>
> 1. **Infrastructure:** Cần cluster ZooKeeper/KRaft, brokers, consumers — quá nặng cho embedded/edge.
> 2. **Latency:** Network hop + serialization + GC pause có thể là ms, không phù hợp sub-µs realtime.
> 3. **Dependencies:** Kafka clients thêm dependency vào embedded systems.
> 4. **Resource:** Kafka cần GB RAM, không phù hợp memory-constrained devices.
>
> **When to use Kafka:** Multi-node, durable stream, replay, analytics.
> **When to use EventStreamCore:** Single-node, low-latency, embedded, RTOS, no external deps.

---

## 12. "What is event-driven architecture and how does your project fit?"

**Answer:**

> **Event-driven architecture (EDA):** Components communicate bằng events thay vì direct calls. Producers emit events, consumers react asynchronously.
>
> **EventStreamCore là event broker nhúng:**
> - Producers: TCP/UDP clients, file polling, internal plugins.
> - Broker: dispatcher + event bus.
> - Consumers: realtime/transactional/batch processors + downstream subscribers qua C API.
>
> **Benefits:** Decoupling, scalability, async processing. **Challenges:** Event ordering, delivery guarantees, observability.

---

## 13. "Walk me through the config loader."

**Answer:**

> **ConfigLoader** đọc YAML file (`config/config.yaml`) và tạo `AppConfiguration` struct.
>
> **Key sections:**
> - `app_name`, `version`, `logging`
> - `ingestion`: TCP/UDP/file config
> - `router`: shards, strategy
> - `rule_engine`: threads, cache
> - `storage`: backend, path
> - `numa`: node binding
> - `control`: thresholds, policies
>
> **Validation:** `unittest/config_loader_test.cpp` test missing fields, invalid types, invalid values.
>
> **Code reference:** `include/eventstream/core/config/loader.hpp`, `src/core/config/loader.cpp`

---

## 14. "What is the TopicTable and why shared_mutex?"

**Answer:**

> **TopicTable** map `topic → EventPriority`, load từ `config/topics.conf`.
>
> **Why `std::shared_mutex`?**
> - Read-heavy: dispatcher lookup liên tục.
> - Write-rare: config reload thỉnh thoảng.
> - `shared_lock` cho nhiều reader concurrent; `unique_lock` cho writer.
>
> **Trade-off:** `shared_mutex` nặng hơn `mutex` nếu contention thấp, nhưng tốt hơn nhiều khi read-heavy.

---

## 15. "How do metrics work?"

**Answer:**

> **MetricRegistry** là singleton chứa các `Metrics` object theo tên component.
>
> **Metrics bao gồm:**
> - `total_events_processed`
> - `total_events_dropped`
> - `current_queue_depth`
> - `last_event_timestamp_ns`
>
> **Usage:** Các processor gọi `fetch_add` atomic để update. C API `esccore_metrics()` trả về snapshot.
>
> **Code reference:** `include/eventstream/core/metrics/registry.hpp`, `src/core/metrics/registry.cpp`

---

## 16. "What is the rule engine?"

**Answer:**

> **Rule engine** validate và transform events dựa trên topic.
>
> **Flow trong processor:**
> 1. `handler->validate(event)` — kiểm tra schema/format.
> 2. `handler->checkRules(event)` — áp dụng business rules.
> 3. `handler->handle(event)` — thực hiện action.
>
> **Registration:** `registerDefaultHandlers()` trong `main.cpp` đăng ký handlers theo topic pattern.
>
> **Code reference:** `include/eventstream/core/processor/handler.hpp`

---

## 17. "How do you handle configuration changes at runtime?"

**Answer:**

> **Current:** Config load một lần at startup. TopicTable có thể reload nhưng cần explicit call.
>
> **Roadmap 2.0:** RCU (Read-Copy-Update) cho config hot-reload:
> - Writer allocate new config.
> - Update atomic pointer.
> - Wait for grace period.
> - Free old config.
> - Readers never block.

---

## 18. "What is the difference between a message and an event?"

**Answer:**

> **Event:** Mô tả something that happened in the past. Immutable. Often broadcast to multiple consumers.
> **Message:** Một đơn vị communication, có thể là command, query, hoặc event. Often point-to-point.
>
> **EventStreamCore xử lý events:** Mỗi event có timestamp, topic, priority, body. Events được route và process, không phải command execution.

---

## 19. "What happens during shutdown?"

**Answer:**

> **Shutdown sequence trong `main.cpp`:**
> 1. Signal handler set `g_running = false`.
> 2. Stop UDP server.
> 3. Stop TCP server.
> 4. Stop ProcessManager (join realtime/transactional/batch threads).
> 5. Stop Dispatcher.
> 6. Flush storage.
> 7. Shutdown IngestEventPool.
> 8. Clear observers.
>
> **Why reverse order:** Đảm bảo không còn event mới đi vào pipeline trước khi processors dừng.

---

## 20. "What is your definition of 'production-ready' for this project?"

**Answer:**

> Production-ready cho EventStreamCore nghĩa là:
> 1. **Correctness:** Unit tests + stress tests pass, TSan clean.
> 2. **Observability:** Metrics, health checks, DLQ logging.
> 3. **Failure handling:** Backpressure, graceful degradation, DLQ.
> 4. **Performance:** Benchmarked throughput/latency under load.
> 5. **Operability:** Config-driven, graceful shutdown, logging.
> 6. **Portability:** Linux stable, QNX cross-compile validated.
>
> **Current status:** Linux core production-ready cho single-node use case. QNX/RT features đang trong roadmap 2.0.

---

## 🎯 Foundation Interview Traps

| Trap | Correct Response |
|------|-----------------|
| "Is it lock-free everywhere?" | "Hot path is lock-free. Transactional/batch queues intentionally use mutex." |
| "Does it replace Kafka?" | "No, it complements Kafka for low-latency/embedded use cases." |
| "Is it distributed?" | "Currently single-node. Multi-node is a future extension." |
| "Why not Rust?" | "C++17 chosen for RTOS portability and existing embedded ecosystem." |

---

## ✅ Enhanced Foundation Checklist

- [ ] Giải thích được tại sao không dùng Kafka.
- [ ] Mô tả event-driven architecture fit.
- [ ] Giải thích config loader và validation.
- [ ] Biết TopicTable dùng shared_mutex.
- [ ] Mô tả metrics registry.
- [ ] Giải thích rule engine flow.
- [ ] Mô tả shutdown sequence.
- [ ] Định nghĩa production-ready cho project.
