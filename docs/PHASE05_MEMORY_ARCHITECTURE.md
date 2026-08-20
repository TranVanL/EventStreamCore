# Phase 5 — Memory & Computer Architecture Hardening

## Mục tiêu

Hiểu lock-free memory reclamation, hugepages, cache topology, memory ordering. Thay thế `new/delete` trong hot path bằng hazard pointer + object pool.

---

## 1. Vấn đề của `new/delete` trong Lock-Free

- `new Node` mỗi push gây heap contention.
- `delete` trong lock-free dễ dẫn đến use-after-free (ABA problem).
- Cần reclamation mechanism an toàn.

---

## 2. Hazard Pointers

### Nguyên lý

- Mỗi reader có 1+ hazard pointer (HP).
- Trước khi dereference node, reader set HP = node.
- Writer muốn delete node phải đợi cho đến khi không còn HP nào trỏ tới node.

### Code mẫu

```cpp
class HazardPointer {
    static thread_local HazardPointer* hp_;
public:
    static void protect(void* ptr) { hp_ = ptr; }
    static void clear() { hp_ = nullptr; }
};

class HazardPointerDomain {
    std::vector<void*> retired_;
public:
    void retire(void* ptr) { retired_.push_back(ptr); }
    void collect() {
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (!isHazard(*it)) {
                delete *it;
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
    }
};
```

### Ưu điểm

- Bounded memory overhead (số HP × số thread).
- ABA-safe.

### Nhược điểm

- Reader phải gọi protect/clear.
- Writer delay delete.

---

## 3. Object Pool

### Lock-Free Object Pool

```cpp
template<typename T, size_t N>
class LockFreeObjectPool {
    struct Node { T data; Node* next; };
    std::array<Node, N> storage_;
    std::atomic<Node*> freeList_{nullptr};
public:
    LockFreeObjectPool() {
        for (size_t i = 0; i < N - 1; ++i) {
            storage_[i].next = &storage_[i + 1];
        }
        storage_[N - 1].next = nullptr;
        freeList_.store(&storage_[0], std::memory_order_relaxed);
    }

    T* acquire() {
        Node* node = freeList_.load(std::memory_order_acquire);
        while (node && !freeList_.compare_exchange_weak(node, node->next,
                std::memory_order_acquire, std::memory_order_relaxed)) {}
        return node ? &node->data : nullptr;
    }

    void release(T* obj) {
        Node* node = reinterpret_cast<Node*>(
            reinterpret_cast<char*>(obj) - offsetof(Node, data));
        node->next = freeList_.load(std::memory_order_relaxed);
        while (!freeList_.compare_exchange_weak(node->next, node,
                std::memory_order_release, std::memory_order_relaxed)) {}
    }
};
```

---

## 4. HazardPointerMpscQueue

### Thiết kế

- Node lấy từ `LockFreeObjectPool`.
- Pop dùng hazard pointer để bảo vệ node.
- Retire node cũ, collect định kỳ.

### Code mẫu

```cpp
template<typename T, size_t N>
class HazardPointerMpscQueue {
    struct Node {
        T data;
        std::atomic<Node*> next{nullptr};
    };

    LockFreeObjectPool<Node, N> pool_;
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    HazardPointerDomain domain_;

public:
    bool push(const T& item) {
        Node* node = pool_.acquire();
        if (!node) return false;
        node->data = item;
        node->next.store(nullptr, std::memory_order_relaxed);

        Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        Node* head = head_.load(std::memory_order_relaxed);
        Node* next = head->next.load(std::memory_order_acquire);

        HazardPointer::protect(next);
        if (head_.load(std::memory_order_acquire) != head) {
            HazardPointer::clear();
            return std::nullopt;
        }

        if (!next) {
            HazardPointer::clear();
            return std::nullopt;
        }

        T item = std::move(next->data);
        head_.store(next, std::memory_order_release);
        HazardPointer::clear();

        domain_.retire(head);
        domain_.collect();
        return item;
    }
};
```

---

## 5. Hugepages

### Tại sao?

- Default page size 4KB → nhiều TLB misses với large working set.
- Hugepage 2MB hoặc 1GB → giảm TLB misses, tăng throughput.

### API

```cpp
void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
```

### Hoặc hugetlbfs

```bash
sudo mount -t hugetlbfs none /mnt/huge
```

### Trade-off

- Tốn memory (allocate cả 2MB).
- Cần kernel hỗ trợ.
- Không phù hợp small allocations.

---

## 6. Cache Topology

### Thông tin cần parse

- `/sys/devices/system/cpu/cpu*/topology/core_id`
- `/sys/devices/system/cpu/cpu*/topology/physical_package_id`
- `/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`
- `/sys/devices/system/cpu/cpu*/cache/index*/shared_cpu_list`

### Ứng dụng

- Pin producer/consumer cùng L3 cache để giảm latency.
- Tránh pin 2 real-time thread cùng hyperthread sibling.

---

## 7. Memory Ordering

| Order | Ý nghĩa |
|---|---|
| `relaxed` | Không đảm bảo ordering |
| `acquire` | Load sau acquire không được reorder trước acquire |
| `release` | Store trước release không được reorder sau release |
| `acq_rel` | Kết hợp acquire + release |
| `seq_cst` | Total ordering mạnh nhất |

### Ví dụ SPSC ring buffer

```cpp
// Producer
buffer_[head] = item;
head_.store(next, std::memory_order_release); // publish

// Consumer
size_t tail = tail_.load(std::memory_order_acquire); // subscribe
if (tail != head_.load(std::memory_order_acquire)) {
    item = buffer_[tail];
}
```

---

## 8. False Sharing

### Vấn đề

- Hai biến khác nhau nằm cùng cache line (64 bytes).
- Hai CPU ghi vào hai biến → cache line bounce.

### Giải pháp

```cpp
alignas(64) std::atomic<size_t> head_{0};
alignas(64) std::atomic<size_t> tail_{0};
```

---

## 9. Interview Q&A

**Q: Tại sao cần hazard pointer?**
A: Trong lock-free data structures, reader có thể đang đọc node mà writer muốn delete. Hazard pointer bảo vệ node khỏi bị delete sớm.

**Q: Hugepage lợi ích gì?**
A: Giảm TLB misses, tăng throughput cho large working set.

**Q: False sharing là gì?**
A: Hai biến khác nhau trong cùng cache line bị hai CPU ghi liên tục, gây cache coherence traffic.

**Q: Khi nào dùng memory_order_release/acquire?**
A: Producer-consumer pattern: producer release, consumer acquire.

---

## 10. References

- Maged M. Michael — "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects"
- `man mmap`
- Linux kernel documentation: `/sys/devices/system/cpu`
- Herb Sutter — "atomic<> Weapons" talk
