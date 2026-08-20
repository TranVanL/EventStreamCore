# 03 — Lock-Free Concurrency Deep Dive

> File này cover concurrency, memory ordering, lock-free data structures — lĩnh vực mạnh nhất của EventStreamCore và thường được hỏi sâu ở senior/staff level.

---

## Q1: "Explain the Vyukov MPSC queue algorithm."

**Answer:**

> Vyukov MPSC queue cho phép nhiều producer push concurrently, single consumer pop.
>
> **Push:**
> ```cpp
> Node* node = new Node(item);
> Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
> prev->next.store(node, std::memory_order_release);
> ```
> 1. Allocate node mới.
> 2. `exchange` tail với node mới, trả về old tail.
> 3. Gán `old_tail->next = new_node`.
>
> **Pop:**
> ```cpp
> Node* head = head_.load(relaxed);
> Node* next = head->next.load(acquire);
> if (next == nullptr) return nullopt;
> T item = std::move(next->data);
> head_.store(next, relaxed);
> delete head;
> ```
> 1. Đọc `head->next`.
> 2. Nếu `next == nullptr`, queue empty HOẶC producer chưa link xong.
> 3. Nếu có next, advance head và delete old dummy.
>
> **Why O(1) without CAS retry?**
> - `exchange` là single atomic operation. Producer không cần retry loop như CAS-based queue.
> - Producer chỉ cần 2 stores: `exchange(tail)` và `store(next)`.

**Code reference:** `include/eventstream/core/queues/mpsc.hpp`

---

## Q2: "What memory ordering do you use and why?"

**Answer:**

| Operation | Ordering | Why |
|-----------|----------|-----|
| `tail_.exchange(node, acq_rel)` | `acq_rel` | Đảm bảo data node visible trước khi tail update, và thấy tail cũ |
| `prev->next.store(node, release)` | `release` | Đảm bảo link visible trước khi consumer đọc |
| `head->next.load(acquire)` | `acquire` | Đảm bảo thấy tất cả writes từ producer trước khi đọc data |
| `size_.fetch_add(relaxed)` | `relaxed` | Chỉ là approximate counter, không cần ordering |
| `head_.store(next, relaxed)` | `relaxed` | Consumer là single thread, không cần sync với ai |

> **Key principle:** Dùng weakest ordering đủ đúng. Không dùng `seq_cst` everywhere vì expensive trên ARM.

---

## Q3: "What is false sharing and how do you prevent it?"

**Answer:**

> **False sharing:** Hai thread cập nhật hai biến khác nhau nhưng nằm chung một cache line (64 bytes). CPU 0 ghi biến A → invalidates cache line trên CPU 1. CPU 1 ghi biến B → invalidates trên CPU 0. Kết quả ping-pong, ~200+ cycles mỗi lần.
>
> **Prevention trong EventStreamCore:**
> ```cpp
> alignas(64) std::atomic<Node*> head_;
> alignas(64) std::atomic<Node*> tail_;
> alignas(64) std::atomic<size_t> size_;
> ```
> Và trong SPSC ring buffer:
> ```cpp
> alignas(64) T buffer_[Capacity];
> alignas(64) std::atomic<size_t> head_{0};
> alignas(64) std::atomic<size_t> tail_{0};
> ```
>
> **Evidence:** SPSC benchmark 125M ops/s — false sharing sẽ làm giảm đáng kể.

---

## Q4: "Explain the SPSC ring buffer."

**Answer:**

> **Design:**
> ```cpp
> bool push(const T& item) {
>     size_t head = head_.load(relaxed);
>     size_t next = (head + 1) & (Capacity - 1);
>     if (next == tail_.load(acquire)) return false;  // full
>     buffer_[head] = item;
>     head_.store(next, release);
>     return true;
> }
> ```
>
> **Why power-of-2 capacity?**
> - Wrap-around bằng bitmask `& (N-1)` thay vì modulo `% N`.
> - Bitmask là 1 instruction, branchless. Modulo cần division (~20-30 cycles).
>
> **Memory ordering:**
> - Producer: `release` sau khi ghi data.
> - Consumer: `acquire` trước khi đọc data.
> - Đảm bảo happens-before: producer ghi data → release head → consumer acquire head → đọc data.

**Code reference:** `include/eventstream/core/queues/spsc.hpp`

---

## Q5: "Is the MPSC queue really lock-free? What about allocation?"

**Answer:**

> **Push/pop là lock-free:** chỉ dùng atomics, không mutex.
>
> **Nhưng `new Node(item)` trong push KHÔNG lock-free:** `new` có thể gọi malloc, malloc có lock global. Đây là known limitation của implementation hiện tại.
>
> **Fix trong roadmap 2.0:**
> - `LockFreeObjectPool<T, Capacity>` — pre-allocate pool.
> - `HazardPointerMpscQueue` — reclaim node an toàn mà không cần lock.
>
> **Interview tip:** Thừa nhận limitation và nêu plan fix. Điều này thể hiện senior thinking.

---

## Q6: "Explain the lock-free deduplicator."

**Answer:**

> **Structure:** 4096 buckets, mỗi bucket là atomic pointer đến linked list.
>
> **is_duplicate:**
> ```cpp
> Entry* entry = buckets_[bucket_idx].load(acquire);
> while (entry) {
>     if (entry->id == event_id) return true;
>     entry = entry->next;
> }
> return false;
> ```
>
> **insert:**
> ```cpp
> new_entry->next = head;
> buckets_[bucket_idx].compare_exchange_strong(head, new_entry, acq_rel, acquire);
> ```
>
> **Cleanup:** Single cleanup thread chạy mỗi 10s, remove entries older than 1 hour.
>
> **Trade-off:**
> - **Pros:** Read path lock-free, no hash map mutex.
> - **Cons:** Linear search on collision; memory tăng theo số event unique.

**Code reference:** `include/eventstream/core/queues/dedup.hpp`, `src/core/queues/dedup.cpp`

---

## Q7: "What is the ABA problem? Does your code have it?"

**Answer:**

> **ABA problem:** Thread 1 đọc A. Thread 2 pop A, push B, push A lại. Thread 1 CAS thành công vì pointer vẫn là A, nhưng list đã thay đổi.
>
> **Trong MPSC queue:** Không có ABA vì chỉ append ở tail, không pop từ tail. Consumer pop từ head và delete node — node không bị reuse.
>
> **Trong dedup:** Có thể có ABA nếu node bị delete và allocate lại cùng address. Hiện tại cleanup single-threaded + insert CAS, nhưng chưa hoàn toàn an toàn nếu cleanup delete node đang được reader duyệt. Roadmap 2.0 dùng hazard pointer để fix.

---

## Q8: "How would you test a lock-free queue?"

**Answer:**

> 1. **Correctness:** N producer + 1 consumer, push N*M items, verify tổng số pop = tổng số push, không mất event.
> 2. **ThreadSanitizer (TSan):** Compile với `-fsanitize=thread`, chạy stress test. TSan bắt được data race trên non-atomic access.
> 3. **Stress test:** Chạy 10M+ ops với nhiều thread, kiểm tra không crash.
> 4. **Benchmark:** So sánh throughput/latency với `std::queue` + mutex.
> 5. **ARM test:** Chạy trên ARM64 (weaker memory model) để phát hiện ordering bug.

---

## Q9: "Why not use `std::memory_order_seq_cst` everywhere?"

**Answer:**

> `seq_cst` là strongest ordering. Trên x86 thì hầu như free, nhưng trên ARM/RISC-V nó tạo full memory fence (`dmb ish`) — rất đắt.
>
> Trong lock-free code, ta chỉ cần đảm bảo:
> - Producer writes visible trước khi consumer reads → `release`/`acquire` là đủ.
> - Không cần total order của tất cả seq_cst operations.
>
> **Rule:** Dùng weakest ordering đủ đúng. Chỉ dùng `seq_cst` khi thực sự cần total ordering (ví dụ: multiple producers phải thấy cùng một order).

---

## Q10: "Compare your MPSC queue with boost::lockfree::queue."

**Answer:**

| Aspect | EventStreamCore MPSC | boost::lockfree::queue |
|--------|---------------------|------------------------|
| Algorithm | Vyukov (exchange-based) | CAS-based ring/circular |
| Allocation | `new` per push (current) | Fixed capacity, pre-allocated |
| Consumer | Single | Single or multiple tùy config |
| Memory ordering | Explicit, tuned | Boost handles internally |
| Portability | Header-only, simple | Dependency on Boost |

> **Trade-off:** Boost lockfree đã optimize sẵn và có fixed capacity, nhưng thêm dependency. Custom Vyukov giúp hiểu sâu và dễ tune cho RTOS/QNX.

---

## Q11: "Draw the happens-before relationship in the SPSC ring buffer."

**Answer:**

> **Producer:**
> ```
> 1. buffer_[head] = item       (write data)
> 2. head_.store(next, release) (signal)
> ```
>
> **Consumer:**
> ```
> 3. tail_.load(acquire) == head? (check)
> 4. item = buffer_[tail]         (read data)
> ```
>
> **Happens-before:**
> ```
> Producer write data  ──release──▶  Consumer acquire head  ──▶ Consumer read data
> ```
> Release-acquire pair đảm bảo consumer thấy data đã ghi trước khi head advance.

---

## Q12: "What is the difference between x86 and ARM memory models?"

**Answer:**

| Aspect | x86/x64 | ARM64 |
|--------|---------|-------|
| Default load | Acquire-like (TSO-ish) | Weak ordering |
| Default store | Release-like | Weak ordering |
| `relaxed` cost | Very cheap | Cheap |
| `acquire` cost | Free (compiler barrier) | `dmb ishld` |
| `release` cost | Free (compiler barrier) | `dmb ishst` |
| `seq_cst` cost | Cheap | Expensive (`dmb ish`) |

> **Implication:** Code lock-free chạy trên x86 có thể "tình cờ" đúng do TSO, nhưng sai trên ARM. Phải dùng explicit memory ordering và test trên ARM.

---

## Q13: "What is a memory barrier and when do you need one?"

**Answer:**

> **Memory barrier** ngăn compiler/CPU reorder instructions.
>
> **Types:**
> - **Load-load:** Đảm bảo load A xảy ra trước load B.
> - **Load-store:** Load trước store.
> - **Store-store:** Đảm bảo store A xảy ra trước store B.
> - **Store-load:** Đảm bảo store trước load (đắt nhất).
>
> **In C++ atomics:**
> - `release` = store-store + load-store barrier.
> - `acquire` = load-load + load-store barrier.
> - `seq_cst` = full barrier.
>
> **EventStreamCore:** Không dùng explicit barrier function; memory ordering trên atomic operations đủ.

---

## Q14: "Explain seqlock and where it could be used."

**Answer:**

> **Seqlock** là read-heavy synchronization mechanism.
>
> **Pattern:**
> ```cpp
> // Writer
> seq_.fetch_add(1, relaxed);      // odd
> data_ = new_data;
> seq_.fetch_add(1, relaxed);      // even
>
> // Reader
> do {
>     seq1 = seq_.load(acquire);
>     copy = data_;
>     seq2 = seq_.load(acquire);
> } while (seq1 != seq2 || seq1 & 1);
> ```
>
> **Use in EventStreamCore:** Config hot-reload (roadmap 2.0). TopicTable hoặc thresholds có thể dùng seqlock thay vì shared_mutex để readers không block.
>
> **Trade-off:** Reader có thể retry nhiều lần nếu writer frequent.

---

## Q15: "What is RCU and how would you use it?"

**Answer:**

> **RCU (Read-Copy-Update):** Readers không block. Writers copy data, update pointer, wait grace period, rồi free old.
>
> **Pattern:**
> ```cpp
> // Reader
> rcu_read_lock();
> auto* cfg = config_ptr.load(acquire);
> // use cfg
> rcu_read_unlock();
>
> // Writer
> auto* new_cfg = new Config(*old_cfg);
> new_cfg->update(...);
> config_ptr.store(new_cfg, release);
> synchronize_rcu();  // wait for all readers to exit
> delete old_cfg;
> ```
>
> **Use in EventStreamCore:** TopicTable reload, threshold updates. Readers (dispatcher) never block.
>
> **Trade-off:** Khó implement đúng; cần quản lý grace period. Simpler alternative: hazard pointers hoặc seqlock.

---

## Q16: "Compare hazard pointers vs epoch-based reclamation."

**Answer:**

| Aspect | Hazard Pointers | Epoch-Based Reclamation (EBR) |
|--------|----------------|------------------------------|
| Reader overhead | Store pointer in HP slot | Update epoch counter |
| Writer overhead | Scan all HP slots | Wait for epoch change |
| Memory usage | O(threads * slots) | O(retired per epoch) |
| Latency | Bounded reclamation latency | Potentially longer delay |
| Complexity | Higher | Lower |

> **EventStreamCore 2.0:** Dùng hazard pointers cho MPSC queue vì cần bounded reclamation latency.

---

## Q17: "What is work-stealing and why add it?"

**Answer:**

> **Work-stealing:** Mỗi worker có local queue. Khi rảnh, worker steal task từ queue của worker khác.
>
> **Algorithm (Chase-Lev deque):**
> - Owner: push/pop bottom.
> - Thieves: steal from top (CAS).
>
> **Use in EventStreamCore 2.0:** `WorkStealingThreadPool` thay thế `ThreadPool` cũ để cải thiện load balancing.
>
> **Trade-off:** Tốt khi tasks có độ dài không đều. Không cần thiết nếu tasks đều nhau.

---

## Q18: "How do you reason about lock-free correctness?"

**Answer:**

> 1. **Identify invariants:** Ví dụ: head luôn trỏ đến dummy node; tail luôn trỏ đến node cuối cùng được exchange.
> 2. **Linearization points:** Xác định điểm operation được coi là atomic. Với MPSC push, linearization point là `tail_.exchange()`.
> 3. **Happens-before:** Vẽ graph giữa producer và consumer.
> 4. **Test:** TSan + stress test + ARM test.
> 5. **Review:** Code review bởi người hiểu memory model.

---

## Q19: "What is the difference between lock-free and wait-free?"

**Answer:**

| Property | Lock-free | Wait-free |
|----------|-----------|-----------|
| Progress | At least one thread makes progress in finite steps | Every thread makes progress in finite steps |
| Starvation | Possible | Impossible |
| Complexity | Hard | Very hard |
| Examples | MPSC queue, SPSC ring | Some specialized algorithms (not in EventStreamCore) |

> **EventStreamCore:** Lock-free là đủ. Wait-free thường phức tạp hơn và overhead cao hơn.

---

## Q20: "What is a Treiber stack and how does it relate to your object pool?"

**Answer:**

> **Treiber stack** là lock-free stack dùng CAS trên head pointer.
>
> ```cpp
> void push(T* node) {
>     do {
>         node->next = head_.load(acquire);
>     } while (!head_.compare_exchange_weak(node->next, node, release, acquire));
> }
> ```
>
> **Relation to object pool:** `LockFreeObjectPool` dùng Treiber stack cho free list. `acquire()` pop, `release()` push.
>
> **ABA risk:** Có thể xảy ra nếu node bị reuse. EventStreamCore 2.0 dùng hazard pointers để bảo vệ.

---

## ✅ Enhanced Concurrency Checklist

- [ ] Vẽ happens-before diagram cho SPSC.
- [ ] So sánh x86 vs ARM memory model.
- [ ] Giải thích 4 loại memory barrier.
- [ ] Mô tả seqlock và RCU use cases.
- [ ] So sánh hazard pointers vs EBR.
- [ ] Giải thích work-stealing.
- [ ] Liệt kê cách reason về lock-free correctness.
- [ ] Phân biệt lock-free vs wait-free.
- [ ] Mô tả Treiber stack.
