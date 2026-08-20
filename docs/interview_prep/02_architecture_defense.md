# 02 — Architecture Defense

> File này giúp bạn defend toàn bộ kiến trúc EventStreamCore ở cấp senior. Mỗi câu hỏi đi kèm câu trả lời có cấu trúc: Context → Decision → Trade-off → Evidence.

---

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│ INGEST LAYER                                                        │
│  TCP (epoll/thread-per-client)  UDP (recvmmsg)  File (polling)     │
└────────────────────────┬────────────────────────────────────────────┘
                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│ MPSC Queue (Vyukov, lock-free, capacity 65536)                     │
│  N producers (ingest threads) → 1 consumer (dispatcher)            │
└────────────────────────┬────────────────────────────────────────────┘
                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│ DISPATCHER (single thread)                                          │
│  - Pop from MPSC                                                    │
│  - Lookup topic → priority (TopicTable)                             │
│  - Adapt to realtime pressure                                       │
│  - Route to EventBus queue                                          │
└────────────────────────┬────────────────────────────────────────────┘
                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│ EVENT BUS                                                           │
│  REALTIME (SPSC ring, 16384)  TXNAL (deque+mutex)  BATCH (deque)   │
│  Overflow → DLQ (ring buffer, 1000 events)                          │
└────────────────────────┬────────────────────────────────────────────┘
                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│ PROCESSORS                                                          │
│  RealtimeProcessor  TransactionalProcessor  BatchProcessor          │
└────────────────────────┬────────────────────────────────────────────┘
                         ▼
┌─────────────────────────────────────────────────────────────────────┐
│ STORAGE + OBSERVABILITY                                             │
│  Binary persistence  DLQ  Metrics  ProcessedEventStream (Observer)  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Q1: "Walk me through the architecture. Why this design?"

**Answer:**

> **Context:** Tôi cần một engine xử lý event với nhiều mức ưu tiên khác nhau, từ sensor critical alert đến telemetry batch.
>
> **Decision:** Thiết kế pipeline gồm 5 tầng:
> 1. **Ingest:** TCP/UDP nhận event từ bên ngoài.
> 2. **MPSC queue:** fan-in nhiều ingest thread về 1 dispatcher.
> 3. **Dispatcher:** single thread route event theo topic + priority.
> 4. **Event Bus:** 3 queue với semantics khác nhau.
> 5. **Processors + Storage:** xử lý và persist theo SLA.
>
> **Trade-off:** Pipeline tuyến tính dễ reason hơn graph phức tạp, nhưng dispatcher là single point. Tuy nhiên dispatcher chỉ là O(1) hash lookup + priority check, nên bottleneck ở 52M ops/s từ MPSC — đủ nhanh.
>
> **Evidence:** Benchmark MPSC 52M ops/s, SPSC 125M ops/s. Trong thực tế network ingest là bottleneck, không phải dispatcher.

---

## Q2: "Why is the dispatcher single-threaded? Isn't that a bottleneck?"

**Answer:**

> **Context:** Dispatcher nằm giữa MPSC inbound queue và EventBus.
>
> **Decision:** Single-threaded để tránh lock contention trong routing logic.
>
> **Trade-off:**
> - **Pros:** Không cần lock khi đọc topic table, không race condition trong routing decision, đơn giản.
> - **Cons:** Nếu routing logic phức tạp hơn (regex, ML inference), sẽ thành bottleneck.
>
> **Evidence:**
> - MPSC queue chịu được 52M ops/s push.
> - Dispatcher chỉ làm: pop → `findTopic` → `adaptToPressure` → push EventBus.
> - Nếu cần scale hơn, có thể shard theo topic (như `router.shards: 4` trong config) hoặc dùng multiple dispatcher với consistent hashing.

---

## Q3: "Why MPSC queue between ingest and dispatcher?"

**Answer:**

> **Context:** Có nhiều ingest threads (TCP connections, UDP, file polling) nhưng chỉ có 1 dispatcher thread.
>
> **Decision:** Dùng Vyukov MPSC queue — lock-free, O(1) push, không CAS retry loop.
>
> **Trade-off:**
> - **Pros:** Nhiều producer push concurrently mà không contention.
> - **Cons:** Mỗi push allocate node bằng `new` (trong implementation hiện tại). Upgrade 2.0 sẽ thay bằng object pool + hazard pointer.
>
> **Evidence:** `MpscQueue::push()` dùng `tail_.exchange(node, acq_rel)` — single atomic operation, không spin.

---

## Q4: "Why does EventBus have different queue implementations?"

**Answer:**

| Queue | Implementation | Why |
|-------|---------------|-----|
| Realtime | `SpscRingBuffer<EventPtr, 16384>` | Lock-free, bounded, predictable latency |
| Transactional | `std::deque + std::mutex + cv` | Ordered delivery, blocking OK |
| Batch | `std::deque + std::mutex + cv` | Window-based aggregation, blocking OK |

> **Decision:** Không ép tất cả queue phải lock-free. Chỉ hot path cần lock-free.
>
> **Trade-off:**
> - Realtime queue: nhanh nhưng bounded → phải drop khi full.
> - Transactional/Batch: chậm hơn nhưng có ordering và clean blocking semantics.

---

## Q5: "Explain the routing logic in detail."

**Answer:**

> Routing nằm trong `Dispatcher::route()`:
>
> 1. **Topic lookup:** Nếu `TopicTable` có topic, lấy priority từ config. Nếu không, dùng priority từ event header.
> 2. **Priority upgrade:** Chỉ upgrade nếu table priority > client priority. Không downgrade ở đây (trừ khi có pressure).
> 3. **Pressure adaptation:** `adaptToPressure()` downgrade HIGH → MEDIUM nếu realtime queue HIGH/CRITICAL.
> 4. **Route:**
>    - CRITICAL/HIGH → REALTIME
>    - MEDIUM/LOW → TRANSACTIONAL
>    - BATCH → BATCH

**Code reference:**
```cpp
// src/core/events/dispatcher.cpp
if (topic_table_ && topic_table_->findTopic(evt->topic, priority)) { ... }
if (evt->header.priority < priority) { evt->header.priority = priority; }
adaptToPressure(evt);
// route by final priority
```

---

## Q6: "What happens when the realtime queue is full?"

**Answer:**

> **Context:** Realtime queue là SPSC ring buffer capacity 16384.
>
> **Decision:** Policy `DROP_OLD` — drop event cũ nhất để nhường chỗ cho event mới.
>
> **Flow trong `EventBusMulti::push()`:**
> 1. Tính pressure từ `ringBuffer.size()`.
> 2. Thử push. Nếu full:
>    - Pop oldest event.
>    - Push oldest vào DLQ.
>    - Thử push lại. Nếu vẫn fail, push incoming event vào DLQ.
>
> **Trade-off:** Ưu tiên event mới (freshness) hơn event cũ. Phù hợp với realtime alert.
>
> **Evidence:** Log warning `"[EventBusMulti] REALTIME OVERFLOW: Dropped oldest event to DLQ"`.

---

## Q7: "How does the transactional queue handle overflow?"

**Answer:**

> **Context:** Transactional queue dùng `std::deque` + mutex, capacity 131072, policy `BLOCK_PRODUCER`.
>
> **Decision:** Khi full, producer block tối đa 100ms. Nếu vẫn full, trả về false để dispatcher retry/backoff.
>
> **Flow:**
> ```cpp
> queue->cv.wait_for(lock, 100ms, [&]() { return queue->dq.size() < queue->capacity; });
> ```
>
> **Trade-off:**
> - **Pros:** Ordered delivery được bảo toàn, không drop event dễ dàng.
> - **Cons:** Có thể gây backpressure ngược lên dispatcher và ingest.

---

## Q8: "What is the role of the Dead Letter Queue?"

**Answer:**

> DLQ lưu các event bị drop để debug và audit.
>
> **Implementation:** `DeadLetterQueue` là ring buffer capacity 1000 events. Khi full, oldest bị overwrite.
>
> **Khi nào event vào DLQ:**
> - Realtime queue overflow (DROP_OLD/DROP_NEW).
> - Validation/rule check fail trong processors.
> - SLA breach trong RealtimeProcessor.
> - Max retries exceeded trong TransactionalProcessor.
> - Backpressure drop từ BatchProcessor.
>
> **Evidence:** `StorageEngine::appendDLQ()` ghi log dạng text với timestamp, event id, topic, priority, reason.

---

## Q9: "How do processors interact with storage?"

**Answer:**

> **RealtimeProcessor:**
> - Validate → rule check → handle.
> - Nếu SLA breach (< 5ms), drop vào DLQ.
> - Nếu OK, optional store (storage pointer có thể null).
>
> **TransactionalProcessor:**
> - Dedup trước.
> - Validate → rule check → handle với retry (max 3 lần).
> - Nếu success, insert vào dedup table và store.
> - Nếu fail sau retries, drop vào DLQ.
>
> **BatchProcessor:**
> - Bucket theo topic.
> - Flush khi window 5s hết hoặc khi stop.
> - Aggregate rồi store.

---

## Q10: "Why observer pattern for ProcessedEventStream?"

**Answer:**

> **Context:** Cần notify downstream subscribers khi event được process/drop.
>
> **Decision:** `ProcessedEventStream` singleton dùng observer pattern. Subscribers đăng ký callback `onEventProcessed`/`onEventDropped`.
>
> **Trade-off:**
> - **Pros:** Decoupled — processors không biết subscribers là ai. Low latency push.
> - **Cons:** Subscriber chậm có thể block processor thread. Cần document "keep callbacks fast".
>
> **Evidence:** C API `esccore_subscribe()` đăng ký callback từ Go/Python.

---

## Q11: "How would you extend this to multiple nodes?"

**Answer:**

> **Option 1 — Replication layer trên C API:**
> - Mỗi node chạy EventStreamCore. Một gateway node forward event đến replica.
> - Cons: network hop, phức tạp.
>
> **Option 2 — Shared memory / POSIX IPC:**
> - Dùng POSIX message queue hoặc shared memory ring giữa các process trên cùng machine.
> - Phù hợp với roadmap 2.0 (`PosixMqIngestServer`, `PosixShmIngestServer`).
>
> **Option 3 — Raft cho transactional events:**
> - Chỉ replicate transactional stream (cần consistency). Realtime/batch có thể lossy.
> - Trade-off: complexity vs consistency.

---

## Q12: "What are the failure modes?"

**Answer:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| RealtimeProcessor crash | Realtime queue fill, backpressure | Control plane degrade, operator restart |
| Dispatcher crash | Inbound queue fill, ingest drop | Restart dispatcher, MPSC drain |
| Storage disk full | Store fail, exception | Catch exception, DLQ, alert |
| Ingest flood | MPSC full, backpressure | Drop + DLQ, control plane |
| Subscriber callback slow | Block processor | Document fast callback, future async dispatch |

---

## Q13: "Explain the lifecycle management of components."

**Answer:**

> **Startup order trong `main.cpp`:**
> 1. Load config.
> 2. Initialize `IngestEventPool`.
> 3. Register default handlers.
> 4. Register observers.
> 5. Initialize components: EventBus → Dispatcher → Storage → ProcessManager → Ingest servers.
> 6. Start: Dispatcher → ProcessManager → Ingest servers.
>
> **Shutdown order (reverse):**
> 1. Stop ingest servers (không nhận event mới).
> 2. Stop ProcessManager (drain queues).
> 3. Stop Dispatcher.
> 4. Flush storage.
> 5. Shutdown event pool.
>
> **Why order matters:** Nếu stop dispatcher trước process manager, events trong EventBus vẫn được process nhưng không có event mới. Nếu stop storage trước processor, events bị drop.

---

## Q14: "What is the control plane?"

**Answer:**

> **Control plane** monitor health và điều chỉnh behavior dựa trên `ControlThresholds`:
> - `max_queue_depth`
> - `max_drop_rate`
> - `max_latency_ms`
> - `min_events_for_evaluation`
> - `recovery_factor`
>
> **Actions:**
> - Pause/resume transactional processing.
> - Drop/resume batch events.
> - Trigger alerts.
>
> **Integration:** `ProcessManager::pauseTransactions()`, `dropBatchEvents()` được gọi từ control loop.
>
> **Code reference:** `include/eventstream/core/control/thresholds.hpp`

---

## Q15: "How does the alert handler work?"

**Answer:**

> **AlertHandler** là interface với implementation mặc định `LoggingAlertHandler`.
>
> **RealtimeProcessor** emit alert khi:
> - SLA breach.
> - Handler return `ALERT` outcome.
> - Validation/rule fail nghiêm trọng.
>
> **Alert struct:** level, message, source topic, event id, timestamp, context.
>
> **Extensibility:** Có thể implement `MetricsAlertHandler` gửi đến Prometheus, hoặc `WebhookAlertHandler`.
>
> **Code reference:** `include/eventstream/core/processor/alert.hpp`

---

## Q16: "Why singleton for ProcessedEventStream and MetricRegistry?"

**Answer:**

> **Singleton** đảm bảo một global access point cho:
> - `MetricRegistry`: Tất cả components report metrics vào cùng một registry.
> - `ProcessedEventStream`: Tất cả processors notify cùng một observer hub.
>
> **Trade-off:**
> - **Pros:** Đơn giản, dễ integrate, không cần pass reference khắp nơi.
> - **Cons:** Khó test (cần reset state), tight coupling, khó mock.
>
> **Mitigation:** Trong production code lớn hơn, có thể inject registry/observer hub qua constructor. EventStreamCore dùng singleton vì simplicity cho single-process engine.

---

## Q17: "What would you change if you had to process 1M events/sec sustained?"

**Answer:**

> 1. **Shard dispatcher:** Nhiều dispatcher theo topic hash, mỗi cái có MPSC queue riêng.
> 2. **Object pool + hazard pointers:** Loại bỏ allocation trong MPSC.
> 3. **io_uring ingest:** Giảm syscall overhead.
> 4. **Batch storage:** Per-thread write buffer, flush async.
> 5. **Multiple realtime processors:** Partition realtime queue hoặc dùng work-stealing.
> 6. **NUMA sharding:** Mỗi socket một pipeline instance.
> 7. **Reduce logging:** spdlog trong hot path có thể là bottleneck — dùng ring buffer log hoặc async logger.

---

## Q18: "How do you ensure event ordering?"

**Answer:**

> **Ordering guarantees by queue:**
> - **Realtime (SPSC):** Single producer (dispatcher), single consumer → FIFO.
> - **Transactional (deque + mutex):** FIFO within queue.
> - **Batch (deque + mutex):** FIFO within topic bucket.
>
> **Cross-queue ordering:** Không đảm bảo. Một event HIGH có thể được process trước event MEDIUM dù đến sau.
>
> **If strict global ordering needed:** Dùng một queue duy nhất hoặc sequence numbers + reorder buffer.

---

## Q19: "What is the difference between DROP_OLD, DROP_NEW, and BLOCK_PRODUCER?"

**Answer:**

| Policy | Behavior | Use Case |
|--------|----------|----------|
| `DROP_OLD` | Pop oldest, push new | Realtime — freshness matters |
| `DROP_NEW` | Reject new event | Batch — keep aggregated window |
| `BLOCK_PRODUCER` | Wait until space available | Transactional — preserve all events |

> **Trade-off:** DROP_OLD/DROP_NEW mất event nhưng low latency. BLOCK_PRODUCER giữ event nhưng tạo backpressure.

---

## Q20: "Design alternative: what if we used a single priority queue?"

**Answer:**

> **Single priority queue (min-heap):**
> - **Pros:** Linh hoạt, dynamic priority, global ordering.
> - **Cons:**
>   - Heap operations O(log N), chậm hơn ring buffer O(1).
>   - Mutex bảo vệ heap → contention.
>   - Không phân biệt SLA (realtime bị block bởi batch).
>
> **EventStreamCore choice:** Tách queue theo SLA giúp optimize từng loại. Realtime không bị ảnh hưởng bởi transactional/batch.

---

## Q21: "How do you handle poison messages?"

**Answer:**

> **Poison message = event khiến processor crash hoặc loop.**
>
> **Mitigations:**
> 1. **Validation trước process:** Frame parser reject malformed events.
> 2. **Try-catch trong processor:** `process()` catch exception, log, push DLQ.
> 3. **Retry limit:** Transactional processor retry max 3 lần rồi DLQ.
> 4. **SLA enforcement:** Realtime processor drop nếu process quá 5ms.
> 5. **DLQ inspection:** Operator review DLQ để fix root cause.

---

## Q22: "What is the role of `dequeue_time_ns`?"

**Answer:**

> `Event::dequeue_time_ns` ghi lại thời điểm event được pop khỏi queue.
>
> **Usage:**
> - `TransactionalProcessor` tính latency = `nowNs() - dequeue_time_ns`.
> - `LatencyHistogram` record để báo p50/p95/p99.
>
> **Why not enqueue time?** Enqueue time bao gồm thời gian chờ trong queue. Dequeue time cho biết processing latency riêng, không bao gồm queueing delay.

---

## ✅ Enhanced Architecture Checklist

- [ ] Giải thích component lifecycle và shutdown sequence.
- [ ] Mô tả control plane và thresholds.
- [ ] Giải thích alert handler extensibility.
- [ ] Thảo luận singleton trade-offs.
- [ ] Có plan cho 1M events/sec.
- [ ] Giải thích ordering guarantees.
- [ ] So sánh overflow policies.
- [ ] Defend tách queue vs single priority queue.
- [ ] Xử lý poison messages.
