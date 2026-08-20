# 05 — Performance Tuning & Benchmarking

> File này giúp bạn nói về số liệu, cách benchmark đúng, và các quyết định performance tuning trong EventStreamCore.

---

## Performance Numbers (from README)

| Component | Throughput | P99 Latency |
|-----------|-----------|-------------|
| SPSC Ring Buffer | ~125M ops/s | ~12 ns |
| MPSC Queue | ~52M ops/s | ~45 ns |
| Lock-free Dedup | ~71M ops/s | ~32 ns |
| Event Pool alloc | ~89M ops/s | ~25 ns |

> **Note:** Đo trên standard Linux dev box, no kernel tuning. Nếu phỏng vấn hỏi số cụ thể, hãy nói rõ điều kiện đo.

---

## Q1: "How do you benchmark a lock-free queue correctly?"

**Answer:**

> 1. **Warm-up:** Chạy 10-20% iterations đầu để cache/JIT/branch predictor ổn định, không tính vào kết quả.
> 2. **Pin threads:** Producer/consumer vào CPU riêng biệt, ideally cùng socket/L3.
> 3. **Measure both throughput and latency:** Throughput ẩn tail latency.
> 4. **Report percentiles:** p50/p95/p99/max, không chỉ average.
> 5. **Run long enough:** Ít nhất vài giây để bắt được rare events.
> 6. **Disable CPU frequency scaling:** `cpufreq-set -g performance`.
> 7. **Isolate CPU nếu đo real-time jitter.**

---

## Q2: "Why is SPSC faster than MPSC?"

**Answer:**

> **SPSC (125M ops/s):**
> - Single producer, single consumer → không contention.
> - Pre-allocated ring buffer → no allocation.
> - Chỉ 2 atomics: head và tail.
>
> **MPSC (52M ops/s):**
> - Nhiều producer contention trên tail.
> - Mỗi push allocate node (`new`).
> - Consumer phải đợi producer link `next` pointer.
>
> **Lesson:** SPSC phù hợp cho pipeline stage rõ ràng. MPSC phù hợp cho fan-in.

---

## Q3: "What is the difference between throughput and latency?"

**Answer:**

> **Throughput:** Số operation trên giây (ops/s). Average metric.
> **Latency:** Thời gian cho một operation. Cần percentiles.
>
> **Ví dụ:** Một system có throughput 100M ops/s nhưng p99 latency 100µs do GC pause. Một system khác throughput 80M ops/s nhưng p99 1µs — tốt hơn cho real-time.
>
> **EventStreamCore:** Optimize cả hai. Lock-free hot path cho throughput cao; bounded queues + CPU affinity cho latency ổn định.

---

## Q4: "How does NUMA affect performance?"

**Answer:**

> **NUMA (Non-Uniform Memory Access):** Memory gần CPU nào thì access nhanh hơn.
>
> | Access type | Latency |
> |-------------|---------|
> | Local memory | ~50 ns |
> | Remote memory (2-socket) | ~300 ns |
>
> **EventStreamCore:**
> - `NUMABinding` bind thread đến CPU/node cụ thể.
> - Realtime processor pin to CPU 2 (`ProcessManager::start()`).
> - Config `numa.realtime_proc_node` cho phép chọn node.
>
> **Trade-off:** Binding giảm flexibility nhưng cải thiện cache locality và latency variance.

---

## Q5: "What is cache locality and why does it matter?"

**Answer:**

> **Cache hierarchy:** L1 (~4 cycles) → L2 (~12 cycles) → L3 (~40 cycles) → RAM (~200+ cycles).
>
> **Cache locality trong EventStreamCore:**
> - SPSC ring buffer: head/tail và buffer liên tục trong memory → prefetch hiệu quả.
> - Event pool: pre-allocate objects liền kề → reduce cache miss.
> - `alignas(64)`: tránh false sharing, giữ hot data trên cùng cache line khi cần.
>
> **Evidence:** SPSC 12 ns latency — chỉ có thể đạt được nếu data trong L1/L2.

---

## Q6: "How do you profile this project?"

**Answer:**

> 1. **perf (Linux):**
>    ```bash
>    perf record -g ./benchmark_spsc_detailed
>    perf report
>    ```
> 2. **Cache miss analysis:**
>    ```bash
>    perf stat -e cache-misses,cache-references,cycles ./benchmark_mpsc
>    ```
> 3. **ThreadSanitizer:**
>    ```bash
>    cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread" ..
>    ```
> 4. **Heap profiling:** `valgrind --tool=massif` để tìm allocation hot spot.
> 5. **Latency histogram:** Custom histogram trong `LatencyHistogram` class.

---

## Q7: "What are the main bottlenecks in the current design?"

**Answer:**

> 1. **Allocation trong MPSC push:** `new Node` trên hot path. Fix: object pool + hazard pointer.
> 2. **Storage mutex:** `StorageEngine::storeEvent` dùng `std::lock_guard`. Fix: per-thread write buffer hoặc log-structured append.
> 3. **Dispatcher single thread:** Nếu routing logic phức tạp hơn. Fix: shard by topic.
> 4. **Subscriber callback:** Slow callback block processor. Fix: async dispatch queue.
> 5. **Network ingest:** TCP thread-per-client không scale bằng io_uring. Fix: io_uring ingest server.

---

## Q8: "How would you scale throughput 10x?"

**Answer:**

> 1. **Multiple dispatcher shards:** Shard inbound queue theo topic hash, mỗi shard có dispatcher + realtime queue riêng.
> 2. **io_uring for ingest:** Giảm syscall overhead, một thread xử lý nhiều connections.
> 3. **Object pool + hazard pointers:** Loại bỏ allocation trên hot path.
> 4. **Batch storage writes:** Gom nhiều event rồi flush một lần (đã có `FLUSH_BATCH_SIZE`).
> 5. **NUMA-aware sharding:** Mỗi NUMA node chạy một pipeline instance, tránh remote memory.
> 6. **Kernel bypass (AF_XDP):** Cho UDP ingest ở scale rất cao.

---

## Q9: "What is tail latency and why is it important?"

**Answer:**

> **Tail latency:** Latency ở phần trăm cao (p95, p99, p999).
>
> **Why important:** User experience không phụ thuộc vào average. Một request chậm 100ms trong 1% cũng đủ gây timeout.
>
> **EventStreamCore giảm tail latency bằng:**
> - Lock-free queues (không mutex contention).
> - Bounded queues (không unbounded growth).
> - CPU affinity (giảm context switch).
> - SLA enforcement trong RealtimeProcessor (drop nếu vượt 5ms).

---

## Q10: "How do you interpret cyclictest results?"

**Answer:**

> **Good results (CPU isolated, PREEMPT_RT):**
> - p50: < 100 ns
> - p99: < 1 µs
> - max: < 5 µs
>
> **Bad results và nguyên nhân:**
> - p99 > 10 µs: CPU không isolated, C-states, timer interrupts.
> - max > 100 µs: NMI watchdog, driver ISR, SMI, thermal throttling.
> - Periodic spikes: kernel workqueue hoặc another process migrated to CPU.
>
> **Fix:** `isolcpus`, `nohz_full`, `rcu_nocbs`, disable C-states, PREEMPT_RT kernel.

---

## Q11: "What is Little's Law and how does it apply?"

**Answer:**

> **Little's Law:** $L = \lambda \times W$
> - $L$ = average number of items in system
> - $\lambda$ = arrival rate (items/sec)
> - $W$ = average time in system
>
> **Application:**
> - Nếu ingest rate = 1M events/sec và average processing time = 10µs → queue depth trung bình = 10 events.
> - Nếu queue depth tăng đột biến, có nghĩa là $W$ tăng hoặc $\lambda$ vượt capacity.
>
> **EventStreamCore:** Dùng queue depth metrics để detect imbalance giữa $\lambda$ và processing rate.

---

## Q12: "What is Amdahl's Law and how does it limit scaling?"

**Answer:**

> **Amdahl's Law:** $S_{latency}(s) = \frac{1}{(1-p) + \frac{p}{s}}$
> - $p$ = fraction of program that can be parallelized
> - $s$ = speedup of parallel portion
>
> **Implication:** Nếu 50% code là sequential, dù có vô hạn core thì speedup tối đa chỉ là 2x.
>
> **EventStreamCore:** Dispatcher single-threaded là sequential bottleneck. Để scale 10x, phải parallelize dispatcher (shard by topic).

---

## Q13: "How do you measure queueing delay vs processing delay?"

**Answer:**

> **Queueing delay:** Thời gian event nằm trong queue trước khi được process.
> **Processing delay:** Thời gian processor xử lý event.
>
> **Measurement:**
> - `enqueue_time_ns` (when event enters system).
> - `dequeue_time_ns` (when popped from queue).
> - `nowNs()` (when processing done).
>
> ```
> queueing_delay = dequeue_time_ns - enqueue_time_ns
> processing_delay = nowNs() - dequeue_time_ns
> end_to_end_latency = nowNs() - enqueue_time_ns
> ```
>
> **EventStreamCore:** `dequeue_time_ns` được set trong `EventBusMulti::pop()`.

---

## Q14: "What compiler optimizations matter for this code?"

**Answer:**

> 1. **`-O3`:** Inline, loop unroll, vectorization.
> 2. **`-march=native`:** Dùng instructions của CPU hiện tại (AVX, etc.).
> 3. **`-flto`:** Link-time optimization.
> 4. **`-DNDEBUG`:** Bỏ assert trong release.
> 5. **`-fno-omit-frame-pointer`:** Giữ frame pointer cho profiling (trade-off với performance).
>
> **Pitfall:** Compiler có thể reorder atomic operations nếu không dùng explicit memory ordering. `volatile` không đủ cho thread synchronization.

---

## Q15: "How do you detect cache misses?"

**Answer:**

> **Using perf:**
> ```bash
> perf stat -e L1-dcache-load-misses,L1-dcache-loads,LLC-load-misses ./benchmark
> ```
>
> **Interpretation:**
> - L1 miss rate > 5%: Có thể có false sharing hoặc access pattern kém.
> - LLC miss rate cao: Data không vừa cache, hoặc NUMA remote access.
>
> **Fixes:**
> - Cache-line alignment.
> - Improve access locality.
> - NUMA binding.
> - Reduce object size.

---

## Q16: "What is jitter and why measure it?"

**Answer:**

> **Jitter:** Variance trong latency. Đo bằng stddev hoặc percentiles.
>
> **Why measure:** Real-time systems cần predictable latency. Average latency thấp nhưng jitter cao vẫn gây failure.
>
> **Sources of jitter:**
> - Context switches.
> - Cache misses.
> - Interrupts.
> - GC / malloc.
> - Lock contention.
>
> **EventStreamCore giảm jitter bằng:** lock-free queues, CPU affinity, pre-allocation, bounded queues.

---

## Q17: "Case study: throughput drops under load. Debug steps?"

**Answer:**

> 1. **Check queue depths:** Queue nào đang fill up?
> 2. **Check drop rate:** Có event bị drop không?
> 3. **Profile CPU:** `perf top` xem function nào hot.
> 4. **Check cache misses:** `perf stat`.
> 5. **Check lock contention:** `perf lock` hoặc TSan.
> 6. **Check I/O:** Storage có bị block?
> 7. **Check memory allocation:** `massif` hoặc custom allocator metrics.
>
> **Common causes:**
> - Storage mutex contention.
> - Dispatcher single thread saturated.
> - Logging synchronous quá nhiều.
> - Subscriber callback slow.

---

## Q18: "What is the cost of a context switch?"

**Answer:**

> **Context switch cost:** ~1-10 µs tùy CPU và kernel.
> - Save/restore registers.
> - TLB flush (nếu switch process).
> - Cache pollution.
>
> **EventStreamCore minimizes context switches by:**
> - Pinning threads to CPUs.
> - Lock-free queues (no kernel futex wake/sleep).
> - epoll/io_uring (fewer threads).

---

## Q19: "How does CPU frequency scaling affect benchmarks?"

**Answer:**

> **CPU frequency scaling (SpeedStep/TurboBoost):** CPU tự điều chỉnh frequency dựa trên load.
>
> **Problem:** Benchmark bắt đầu chạy ở tần số thấp, sau đó turbo lên cao → kết quả không ổn định.
>
> **Fix:**
> ```bash
> sudo cpufreq-set -g performance
> # or
> echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
> ```
>
> **Note:** Trên laptop, thermal throttling cũng ảnh hưởng. Server thường ổn định hơn.

---

## Q20: "What is the difference between median and p99?"

**Answer:**

> **Median (p50):** 50% requests nhanh hơn giá trị này.
> **p99:** 99% requests nhanh hơn giá trị này.
>
> **Why p99 matters:** Nó cho thấy trải nghiệm của phần lớn users. Một hệ thống có median 1µs nhưng p99 10ms vẫn gây timeout.
>
> **EventStreamCore:** Báo cả p50 và p99 trong latency histogram.

---

## ✅ Enhanced Performance Checklist

- [ ] Giải thích Little's Law.
- [ ] Giải thích Amdahl's Law.
- [ ] Phân biệt queueing delay vs processing delay.
- [ ] Liệt kê compiler optimizations.
- [ ] Detect cache misses bằng perf.
- [ ] Giải thích jitter và sources.
- [ ] Có debug steps cho throughput drop.
- [ ] Biết cost của context switch.
- [ ] Xử lý CPU frequency scaling.
- [ ] Phân biệt median vs p99.
