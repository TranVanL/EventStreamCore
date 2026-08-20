# Phase 6 — Advanced Concurrency Primitives

## Mục tiêu

Triển khai work-stealing queue, lock-free stack, seqlock, read-write lock. Hiểu khi nào dùng primitive nào.

---

## 1. Work-Stealing Queue

### Use case

- Thread pool: mỗi worker có local queue, idle worker có thể "steal" task từ worker khác.
- Giảm contention trên global queue.

### Code mẫu

```cpp
template<typename T, size_t N>
class WorkStealingQueue {
    std::array<T, N> buffer_;
    alignas(64) std::atomic<size_t> top_{0};
    alignas(64) std::atomic<size_t> bottom_{0};

public:
    bool push(const T& item) {
        size_t b = bottom_.load(std::memory_order_relaxed);
        size_t t = top_.load(std::memory_order_acquire);
        if (b - t >= N) return false; // full
        buffer_[b % N] = item;
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        size_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        size_t t = top_.load(std::memory_order_relaxed);

        if (t <= b) {
            T item = buffer_[b % N];
            if (t == b) {
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            return item;
        } else {
            bottom_.store(b + 1, std::memory_order_relaxed);
            return std::nullopt;
        }
    }

    std::optional<T> steal() {
        size_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        size_t b = bottom_.load(std::memory_order_acquire);

        if (t < b) {
            T item = buffer_[t % N];
            if (top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
                return item;
            }
        }
        return std::nullopt;
    }
};
```

### Lưu ý

- `pop` và `steal` có thể race → cần CAS trên top.
- Buffer size phải là power of 2 hoặc dùng modulo.

---

## 2. Lock-Free Stack (Treiber Stack)

### Code mẫu

```cpp
template<typename T>
class LockFreeStack {
    struct Node {
        T data;
        Node* next;
    };
    std::atomic<Node*> head_{nullptr};

public:
    void push(const T& item) {
        Node* node = new Node{item, head_.load(std::memory_order_relaxed)};
        while (!head_.compare_exchange_weak(node->next, node,
                std::memory_order_release, std::memory_order_relaxed)) {}
    }

    std::optional<T> pop() {
        Node* old = head_.load(std::memory_order_acquire);
        while (old && !head_.compare_exchange_weak(old, old->next,
                std::memory_order_release, std::memory_order_acquire)) {}
        if (!old) return std::nullopt;
        T item = old->data;
        // retire(old) with hazard pointer
        return item;
    }
};
```

### ABA Problem

- Giải pháp: hazard pointer hoặc tagged pointer.

```cpp
struct TaggedPtr {
    Node* ptr;
    uint64_t tag;
};
```

---

## 3. Seqlock

### Use case

- Read-mostly data, writer hiếm.
- Reader không block, writer tăng sequence counter.

### Code mẫu

```cpp
template<typename T>
class SeqLock {
    alignas(64) std::atomic<uint64_t> seq_{0};
    T data_;

public:
    void write(const T& value) {
        uint64_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_release);
        data_ = value;
        seq_.store(seq + 2, std::memory_order_release);
    }

    std::optional<T> read() {
        uint64_t seq1 = seq_.load(std::memory_order_acquire);
        if (seq1 & 1) return std::nullopt; // writer active
        T value = data_;
        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t seq2 = seq_.load(std::memory_order_acquire);
        if (seq1 != seq2) return std::nullopt;
        return value;
    }
};
```

### Ứng dụng

- Configuration hot-reload.
- Metrics snapshot.

---

## 4. Read-Write Lock

### POSIX rwlock

```cpp
pthread_rwlock_t rwlock;
pthread_rwlock_init(&rwlock, nullptr);
pthread_rwlock_rdlock(&rwlock);
pthread_rwlock_wrlock(&rwlock);
pthread_rwlock_unlock(&rwlock);
```

### Priority Inheritance rwlock

- `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` trên Linux.
- Writer-preference tránh writer starvation.

### Trade-off

- rwlock có overhead, không phù hợp hot path.
- Dùng cho metadata/config thay đổi hiếm.

---

## 5. Spinlock

### Code mẫu

```cpp
class SpinLock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
public:
    void lock() { while (flag_.test_and_set(std::memory_order_acquire)); }
    void unlock() { flag_.clear(std::memory_order_release); }
};
```

### Khi nào dùng?

- Critical section cực ngắn.
- Không sleep/block.
- Tránh trên uniprocessor.

---

## 6. So sánh

| Primitive | Use Case | Contention |
|---|---|---|
| Work-stealing queue | Thread pool tasks | Low |
| Lock-free stack | Lock-free LIFO | Low |
| Seqlock | Read-mostly config | Very low |
| RWLock | Read-heavy metadata | Medium |
| Spinlock | Very short critical section | Low |

---

## 7. Integration vào EventStreamCore

### WorkStealingDispatcher

```cpp
class WorkStealingDispatcher {
    std::vector<std::unique_ptr<WorkStealingQueue<Event, 1024>>> queues_;
    std::vector<std::thread> workers_;

public:
    void dispatch(Event evt) {
        size_t idx = std::hash<Event>{}(evt) % queues_.size();
        queues_[idx]->push(evt);
    }

    void workerLoop(size_t id) {
        while (running_) {
            if (auto evt = queues_[id]->pop()) {
                process(*evt);
            } else {
                for (size_t i = 0; i < queues_.size(); ++i) {
                    if (i == id) continue;
                    if (auto stolen = queues_[i]->steal()) {
                        process(*stolen);
                        break;
                    }
                }
            }
        }
    }
};
```

---

## 8. Interview Q&A

**Q: Work-stealing queue lợi ích gì?**
A: Giảm contention, cân bằng tải động giữa các worker thread.

**Q: Seqlock khác rwlock chỗ nào?**
A: Seqlock reader không block, retry nếu detect writer. Rwlock reader block writer.

**Q: ABA problem là gì?**
A: Thread A đọc giá trị X, thread B thay X bằng Y rồi lại X, thread A CAS thành công nhầm tưởng X chưa đổi.

**Q: Khi nào không dùng spinlock?**
A: Critical section dài, uniprocessor, hoặc có thể sleep.

---

## 9. References

- Chase & Lev — "Dynamic Circular Work-Stealing Deque"
- Treiber — "Systems Programming: Coping with Parallelism"
- Linux kernel seqlock implementation
