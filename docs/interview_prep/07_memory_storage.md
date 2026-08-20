# 07 — Memory Architecture & Storage

> File này cover memory allocation strategy, event pool, NUMA, storage engine, DLQ, và roadmap memory hardening.

---

## Q1: "How are events allocated?"

**Answer:**

> **IngestEventPool:** Pre-allocate pool of `Event` objects để tránh `malloc` trên hot path.
>
> **Flow:**
> 1. `IngestEventPool::initialize()` allocate pool at startup.
> 2. Ingest thread `acquire()` event từ pool.
> 3. Sau khi process xong, `release()` trả về pool.
>
> **Trade-off:**
> - **Pros:** Deterministic latency, no malloc jitter.
> - **Cons:** Higher memory footprint, capacity bounded.
>
> **Code reference:** `include/eventstream/core/ingest/pool.hpp`, `src/core/ingest/pool.cpp`

---

## Q2: "Why shared_ptr for EventPtr?"

**Answer:**

> `EventPtr = std::shared_ptr<Event>` dùng trong queues và processors.
>
> **Pros:**
> - Lifetime management đơn giản giữa nhiều thread.
> - Không lo use-after-free khi event được forward qua nhiều stage.
>
> **Cons:**
> - Reference count atomic có overhead (~10-20 ns).
> - Control block allocate riêng → thêm allocation.
>
> **Alternative:** Trong future version, có thể dùng intrusive pointer hoặc object pool index để giảm overhead.

---

## Q3: "Explain the storage engine."

**Answer:**

> **StorageEngine** ghi event dạng binary append-only file.
>
> **Format:**
> ```
> [timestamp:8][source_type:1][event_id:4][topic_len:4][topic:N][payload_size:8][payload:M]
> ```
>
> **Optimizations:**
> - Batch serialize vào buffer rồi ghi một lần.
> - `FLUSH_BATCH_SIZE`: chỉ flush sau N events.
> - Mutex bảo vệ file stream.
>
> **Trade-off:**
> - **Pros:** Fast, simple, append-only reliable.
> - **Cons:** Không hỗ trợ random read (`retrieveEvent` return false). Cần database/tool riêng để query.

**Code reference:** `src/core/storage/storage.cpp`

---

## Q4: "Why append-only storage?"

**Answer:**

> **Append-only phù hợp với event log:**
> - Ghi tuần tự nhanh hơn random write (đặc biệt trên HDD, cũng tốt trên SSD).
> - Không cần update in-place → ít corruption risk.
> - Dễ replicate: chỉ cần copy log.
>
> **So với SQLite:**
> - SQLite có B-tree, WAL, parsing overhead.
> - Binary append-only đơn giản hơn và nhanh hơn cho write-heavy workload.

---

## Q5: "How does the DLQ work?"

**Answer:**

> **DeadLetterQueue** là ring buffer lưu event bị drop.
>
> **Capacity:** 1000 events (configurable).
> **Policy:** Khi full, oldest event bị overwrite.
>
> **Reasons for dropping:**
> - Validation fail.
> - Rule check fail.
> - SLA breach.
> - Max retries exceeded.
> - Backpressure overflow.
>
> **Storage:** `StorageEngine::appendDLQ()` ghi log text với timestamp, event id, topic, priority, reason.

---

## Q6: "What is NUMA and how do you use it?"

**Answer:**

> **NUMA:** Multi-socket system mà mỗi CPU có local memory. Access remote memory chậm hơn.
>
> **EventStreamCore NUMABinding:**
> - `bindThreadToCPU(cpu_id)` — pin thread.
> - `bindThreadToNUMANode(node)` — pin thread to first CPU of node.
> - `getNumNumaNodes()`, `getCPUsOnNode(node)` — introspection.
>
> **Usage:**
> - Realtime processor pin to CPU 2.
> - Config `numa.realtime_proc_node` cho phép chọn node.
>
> **Trade-off:** Binding giảm scheduling flexibility nhưng cải thiện latency và throughput.

---

## Q7: "What are hazard pointers and why add them?"

**Answer:**

> **Problem:** Trong lock-free data structures, node bị delete trong khi reader khác vẫn đang trỏ đến nó → use-after-free.
>
> **Hazard pointer solution:**
> - Reader đăng ký pointer đang đọc vào hazard pointer slot.
> - Writer khi retire node, kiểm tra nếu node không còn hazard pointer thì mới delete.
>
> **Trong EventStreamCore 2.0:**
> - `HazardPointerMpscQueue` thay thế MPSC hiện tại.
> - Node lấy từ `LockFreeObjectPool`.
> - Reclaim bằng hazard pointer → no `new`/`delete` trên hot path.

---

## Q8: "What are hugepages and when to use them?"

**Answer:**

> **Hugepages:** Memory pages lớn hơn default 4KB (thường 2MB hoặc 1GB).
>
> **Benefits:**
> - Giảm TLB misses (Translation Lookaside Buffer).
> - Tốt cho large contiguous buffers như ring buffer.
>
> **Trong EventStreamCore 2.0:**
> - `HugepagePool` dùng `mmap(..., MAP_HUGETLB)`.
> - Fallback to normal mmap nếu hugepage không available.
>
> **Trade-off:** Cần kernel config `vm.nr_hugepages`, không phải lúc nào cũng available.

---

## Q9: "How would you reduce storage latency?"

**Answer:**

> 1. **Per-thread write buffer:** Mỗi processor thread ghi vào local buffer, flush async.
> 2. **O_DIRECT:** Bypass page cache cho direct I/O.
> 3. **NVMe/SSD:** Dùng storage nhanh hơn.
> 4. **Log-structured merge:** Gom writes, compact sau.
> 5. **Separate realtime storage:** Realtime events optional persistence; transactional always persist.
>
> **Current:** StorageEngine đã có batch serialize + `FLUSH_BATCH_SIZE`.

---

## Q10: "What is the memory footprint at runtime?"

**Answer:**

> **Major allocations:**
> - MPSC queue: 65536 `EventPtr` slots + nodes.
> - SPSC ring buffer: 16384 `EventPtr` slots.
> - Transactional queue: capacity 131072 events.
> - Batch queue: capacity 32768 events.
> - DLQ: 1000 events.
> - Dedup table: 4096 buckets + entries.
> - IngestEventPool: tùy config.
>
> **Note:** `EventPtr` là `shared_ptr`, mỗi instance ~16 bytes + control block. Với hàng trăm nghìn event queued, memory có thể lớn.

---

## Q11: "What allocator types exist and which would you use?"

**Answer:**

| Allocator | Use Case | Pros | Cons |
|-----------|----------|------|------|
| `malloc`/`free` | General | Simple | Non-deterministic, global lock |
| Pool allocator | Fixed-size objects | O(1), deterministic | Wasted space if sizes vary |
| Arena allocator | Short-lived objects | Fast bulk free | Hard to free individual objects |
| TLSF | Real-time embedded | O(1), bounded | Fragmentation over time |
| `mmap` | Large buffers | Kernel managed | Page-aligned overhead |

> **EventStreamCore:**
> - `IngestEventPool`: pool allocator cho `Event` objects.
> - `HugepagePool`: mmap cho large buffers.
> - Roadmap 2.0: `LockFreeObjectPool` cho MPSC nodes.

---

## Q12: "What is memory fragmentation and how to avoid it?"

**Answer:**

> **Fragmentation:**
> - **External:** Free blocks scattered, không có block đủ lớn cho allocation mới.
> - **Internal:** Allocated block lớn hơn requested size.
>
> **Avoidance:**
> 1. **Fixed-size pools:** Mỗi pool cho một size class.
> 2. **Pre-allocation:** Allocate tất cả ở startup.
> 3. **Object reuse:** Không delete/recreate.
> 4. **Compacting GC:** Không phù hợp C++ real-time.
>
> **EventStreamCore:** Dùng pool allocator và object reuse để tránh fragmentation.

---

## Q13: "What is mmap and when to use it?"

**Answer:**

> **mmap:** Map file hoặc anonymous memory vào process address space.
>
> **Use cases:**
> - Large ring buffers.
> - Shared memory IPC.
> - Memory-mapped files cho storage.
>
> **Flags:**
> - `MAP_PRIVATE`: Copy-on-write.
> - `MAP_SHARED`: Shared với other processes.
> - `MAP_ANONYMOUS`: Không backed by file.
> - `MAP_HUGETLB`: Hugepages.
>
> **EventStreamCore:**
> - `PosixSharedMemory` dùng `mmap` cho shared memory ring.
> - `HugepagePool` dùng `MAP_HUGETLB`.

---

## Q14: "How would you add WAL (Write-Ahead Log) for durability?"

**Answer:**

> **WAL pattern:**
> 1. Append event to WAL trước khi process.
> 2. Process event.
> 3. Mark WAL entry as committed.
> 4. On crash, replay uncommitted entries.
>
> **Implementation:**
> - Circular log file với checksum per record.
> - `fsync` WAL trước khi ack producer.
> - Separate WAL cho transactional stream.
>
> **Trade-off:** Tăng latency (fsync) nhưng đảm bảo durability.
>
> **EventStreamCore:** Hiện tại chưa có WAL. Có thể thêm cho transactional events.

---

## Q15: "What is cache topology and why parse it?"

**Answer:**

> **Cache topology:** Biết CPU nào share L1/L2/L3 cache.
>
> **Information:**
> - Core ID, socket ID, thread siblings.
> - L3 cache shared by which CPUs.
>
> **Usage:**
> - Pin producer/consumer threads to CPUs sharing L3 để tối đa cache reuse.
> - Tránh pin threads to hyperthreading siblings nếu cả hai đều busy.
>
> **EventStreamCore 2.0:** `CacheTopology` parse `/sys/devices/system/cpu/cpu*/topology/`.

---

## Q16: "What is the object layout of Event?"

**Answer:**

> ```cpp
> struct Event {
>     EventHeader header;        // ~40 bytes
>     std::string topic;         // SSO + heap if > 15 chars
>     std::vector<uint8_t> body; // 24 bytes + heap
>     std::unordered_map<std::string, std::string> metadata; // ~56 bytes + heap
>     uint64_t dequeue_time_ns;  // 8 bytes
> };
> ```
>
> **Implications:**
> - Event object có nhiều heap allocations (string, vector, map).
> - Không ideal cho zero-copy hot path.
> - Future optimization: flat buffer hoặc arena allocation.

---

## Q17: "How would you reduce memory footprint?"

**Answer:**

> 1. **Flat event format:** Dùng single buffer với offsets thay vì nhiều heap objects.
> 2. **Intrusive pointers:** Thay `shared_ptr` bằng `intrusive_ptr` hoặc raw pointer + lifetime management.
> 3. **Smaller metadata:** Chỉ lưu metadata cần thiết.
> 4. **Compress body:** Nếu payload lớn, dùng LZ4 hoặc Snappy.
> 5. **Bounded queues:** Không cho phép unbounded growth.
> 6. **Shorter dedup TTL:** Giảm memory nếu duplicate window không cần 1 giờ.

---

## Q18: "What is the difference between stack, heap, and mmap memory?"

**Answer:**

| Aspect | Stack | Heap | mmap |
|--------|-------|------|------|
| Allocation | Automatic | malloc/new | mmap syscall |
| Deallocation | Scope end | free/delete | munmap |
| Size | Limited (~8MB default) | Large | Very large |
| Speed | Fast | Slower (allocator) | Slowest (kernel) |
| Thread-safe | Per-thread | Need synchronization | Need synchronization |

> **EventStreamCore:** Tránh heap allocation trên hot path. Dùng stack cho small temporary buffers, mmap cho large shared buffers.

---

## Q19: "How do you recover from storage corruption?"

**Answer:**

> **Detection:**
> - Checksum mismatch trong frame.
> - Unexpected EOF.
> - Invalid field values.
>
> **Recovery:**
> 1. **Skip corrupted record:** Scan forward đến next valid record (dùng magic bytes).
> 2. **Truncate:** Nếu corruption ở cuối file, truncate đến last valid offset.
> 3. **Replay WAL:** Nếu có WAL, replay từ last checkpoint.
> 4. **Alert operator:** DLQ + metrics.
>
> **Prevention:**
> - CRC per record.
> - Atomic append (write full record, then update length/header).
> - Regular backups.

---

## Q20: "What is memory ordering for shared_ptr reference count?"

**Answer:**

> `std::shared_ptr` reference count dùng atomics với memory ordering:
> - `fetch_add` (increment): typically `memory_order_relaxed`.
> - `fetch_sub` (decrement): `memory_order_acq_rel` hoặc `memory_order_seq_cst` khi count reaches zero.
>
> **Why acq_rel on decrement:** Đảm bảo destructor của object chạy sau khi tất cả reads/writes từ previous owners visible.
>
> **Cost:** ~10-20 ns per copy. Trong hot path với hàng triệu ops/s, cost này đáng kể.

---

## ✅ Enhanced Memory & Storage Checklist

- [ ] So sánh các loại allocator.
- [ ] Giải thích fragmentation.
- [ ] Mô tả mmap use cases.
- [ ] Thiết kế WAL.
- [ ] Giải thích cache topology.
- [ ] Mô tả object layout của Event.
- [ ] Có plan reduce memory footprint.
- [ ] So sánh stack/heap/mmap.
- [ ] Xử lý storage corruption.
- [ ] Giải thích shared_ptr memory ordering.
