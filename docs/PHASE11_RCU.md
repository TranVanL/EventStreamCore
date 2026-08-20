# Phase 11 — RCU & Epoch-Based Reclamation

## Mục tiêu

Hiểu RCU (Read-Copy-Update) và epoch-based reclamation. Triển khai lightweight RCU cho read-mostly data structures trong EventStreamCore.

---

## 1. Vấn đề RCU giải quyết

- Read-mostly data: readers không cần lock.
- Writer tạo bản sao mới, cập nhật pointer, đợi "grace period" rồi mới xóa bản cũ.
- Phù hợp cho config, routing tables, subscriber lists.

---

## 2. RCU Core Concepts

### 3 bước

1. **Read**: reader enter read-side critical section.
2. **Copy-Update**: writer tạo bản sao, sửa đổi, publish qua atomic pointer.
3. **Wait**: writer đợi tất cả readers cũ qua grace period.

### Grace Period

- Khoảng thời gian đảm bảo mọi reader đã bắt đầu sau publish đều thấy dữ liệu mới.
- Sau grace period, dữ liệu cũ an toàn để xóa.

---

## 3. Userspace RCU (liburcu)

### API

```c
#include <urcu.h>

rcu_register_thread();

rcu_read_lock();
// read data
rcu_read_unlock();

synchronize_rcu(); // wait for grace period
call_rcu(&head, free_fn); // defer free

rcu_unregister_thread();
```

### Trade-off

- Rất nhanh cho read-mostly.
- Writer phải đợi grace period.
- Cần quản lý thread registration.

---

## 4. Epoch-Based Reclamation (EBR)

### Nguyên lý

- Mỗi thread có local epoch (0, 1, 2).
- Global epoch tăng khi writer publish.
- Writer chỉ xóa node sau khi tất cả thread đã qua epoch mới.

### Code mẫu

```cpp
class EpochBasedReclamation {
    static constexpr int EPOCH_COUNT = 3;
    std::atomic<uint64_t> globalEpoch_{0};
    alignas(64) std::atomic<uint64_t> threadEpochs_[MAX_THREADS];
    std::vector<void*> retired_[EPOCH_COUNT];

public:
    void enterCritical(int tid) {
        threadEpochs_[tid].store(globalEpoch_.load(std::memory_order_acquire),
                                 std::memory_order_release);
    }

    void exitCritical(int tid) {
        threadEpochs_[tid].store(UINT64_MAX, std::memory_order_release);
    }

    void retire(void* ptr) {
        uint64_t epoch = globalEpoch_.load(std::memory_order_relaxed);
        retired_[epoch % EPOCH_COUNT].push_back(ptr);
    }

    void synchronize() {
        uint64_t oldEpoch = globalEpoch_.fetch_add(1, std::memory_order_acq_rel);
        // wait for all threads to leave oldEpoch
        for (int i = 0; i < MAX_THREADS; ++i) {
            while (threadEpochs_[i].load(std::memory_order_acquire) == oldEpoch) {
                std::this_thread::yield();
            }
        }
        // free retired[(oldEpoch) % EPOCH_COUNT]
        for (void* ptr : retired_[oldEpoch % EPOCH_COUNT]) {
            free(ptr);
        }
        retired_[oldEpoch % EPOCH_COUNT].clear();
    }
};
```

---

## 5. RCU Config Store

### Use case

- Hot-reload configuration không block readers.

```cpp
template<typename T>
class RcuConfigStore {
    std::atomic<T*> current_{nullptr};
    EpochBasedReclamation ebr_;

public:
    void update(T* newConfig) {
        T* old = current_.exchange(newConfig, std::memory_order_acq_rel);
        ebr_.retire(old);
        ebr_.synchronize();
    }

    T* read() {
        int tid = getThreadId();
        ebr_.enterCritical(tid);
        T* ptr = current_.load(std::memory_order_acquire);
        ebr_.exitCritical(tid);
        return ptr;
    }
};
```

---

## 6. RCU Subscriber List

```cpp
class RcuSubscriberList {
    struct Node {
        Subscriber sub;
        Node* next;
    };
    std::atomic<Node*> head_{nullptr};

public:
    void add(const Subscriber& sub) {
        Node* node = new Node{sub, head_.load(std::memory_order_relaxed)};
        while (!head_.compare_exchange_weak(node->next, node,
                std::memory_order_release, std::memory_order_relaxed)) {}
    }

    void forEach(std::function<void(const Subscriber&)> fn) {
        rcu_read_lock();
        for (Node* p = head_.load(std::memory_order_acquire); p; p = p->next) {
            fn(p->sub);
        }
        rcu_read_unlock();
    }
};
```

---

## 7. RCU vs Hazard Pointers

| | RCU/EBR | Hazard Pointers |
|---|---|---|
| Read overhead | Rất thấp | Thấp |
| Write overhead | Đợi grace period | Đợi reader clear HP |
| Memory bound | Có (số epoch × retired) | Có (số HP × thread) |
| Use case | Read-mostly | Lock-free data structures |

---

## 8. Integration vào EventStreamCore

- `RcuConfigStore<RuntimeConfig>` cho hot-reload config.
- `RcuSubscriberList` cho subscriber management.
- `RcuRoutingTable` cho topic → processor mapping.

---

## 9. Interview Q&A

**Q: RCU là gì?**
A: Read-Copy-Update: readers không lock, writer copy-update-publish, xóa cũ sau grace period.

**Q: Grace period là gì?**
A: Khoảng thời gian đảm bảo mọi reader bắt đầu trước publish đã thoát critical section.

**Q: RCU khác hazard pointers?**
A: RCU tối ưu read-mostly, writer đợi global epoch. Hazard pointers tối ưu lock-free LIFO/queue, reader chủ động protect node.

**Q: Khi nào không dùng RCU?**
A: Write-heavy workloads, hoặc không thể đăng ký/quản lý tất cả reader threads.

---

## 10. References

- Paul E. McKenney — "Is Parallel Programming Hard, And, If So, What Can You Do About It?"
- liburcu documentation
- Fraser — "Practical Lock-Freedom"
