# Phase 2 — RT Tests & Priority Inversion Demo

## Mục tiêu

Viết unit tests và demo chứng minh priority inheritance, robust mutex, semaphore, barrier, spinlock hoạt động đúng.

---

## 1. Testing Real-Time Primitives

### RtThread Test

- Verify policy sau khi apply.
- Verify affinity mask.
- Verify EPERM fallback.

### RtMutex Test

- Basic lock/unlock.
- Recursive lock behavior.
- Robust recovery (`EOWNERDEAD`).
- Priority inversion prevention.

### RtSemaphore Test

- Producer/consumer correctness.
- Named semaphore cross-process.
- No lost wake-up.

### RtBarrier Test

- N threads đồng bộ nhiều lần.
- Không có race trong phase counter.

### RtSpinlock Test

- Correctness: N threads increment counter.
- Performance: so sánh với mutex.

---

## 2. Priority Inversion Demo

### Setup

- Low thread (prio 10): lock mutex, sleep 100ms, unlock.
- Medium thread (prio 50): busy loop 200ms.
- High thread (prio 90): measure time to acquire mutex.

### Không PI

1. Low lock mutex.
2. Medium preempt Low, chiếm CPU.
3. High đợi mutex.
4. High phải đợi Medium chạy xong 200ms → Low mới chạy lại → release.
5. High wait ~200ms.

### Có PI

1. Low lock mutex.
2. Medium preempt Low.
3. High đợi mutex → Low inherit prio 90.
4. Low preempt Medium, chạy xong 100ms, release.
5. High wait ~100ms.

### Code gợi ý

```cpp
void runScenario(bool usePi) {
    RtMutex mutex(usePi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE);
    std::atomic<bool> highDone{false};
    std::chrono::nanoseconds highWait{0};

    auto low = std::thread([&]{
        RtThread::applyToSelf({SCHED_FIFO, 10, {0}});
        auto lk = mutex.lock();
        std::this_thread::sleep_for(100ms);
    });

    auto medium = std::thread([&]{
        RtThread::applyToSelf({SCHED_FIFO, 50, {0}});
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 200ms) {
            // busy loop
        }
    });

    auto high = std::thread([&]{
        RtThread::applyToSelf({SCHED_FIFO, 90, {0}});
        auto start = std::chrono::steady_clock::now();
        auto lk = mutex.lock();
        highWait = std::chrono::steady_clock::now() - start;
        highDone = true;
    });

    low.join(); medium.join(); high.join();
    std::cout << (usePi ? "PI" : "No PI") << " wait: "
              << highWait.count() / 1e6 << " ms\n";
}
```

---

## 3. Robust Mutex Test

### Setup

- Thread A lock mutex.
- Thread A bị `pthread_cancel`.
- Thread B lock mutex → nhận `EOWNERDEAD`.
- Thread B gọi `pthread_mutex_consistent`.

### Code gợi ý

```cpp
TEST(RtMutexTest, RobustRecovery) {
    RtMutex mutex(true); // robust
    std::thread a([&]{
        mutex.lock();
        pthread_cancel(pthread_self());
    });
    a.join();

    std::thread b([&]{
        EXPECT_NO_THROW(mutex.lock()); // handles EOWNERDEAD
        mutex.unlock();
    });
    b.join();
}
```

---

## 4. Semaphore Producer/Consumer

### Pattern

```cpp
RtSemaphore empty(100); // slots available
RtSemaphore filled(0);  // items available
std::queue<int> buffer;
RtMutex bufMutex;

void producer(int n) {
    for (int i = 0; i < n; ++i) {
        empty.wait();
        {
            RtLockGuard lk(bufMutex);
            buffer.push(i);
        }
        filled.post();
    }
}

void consumer(std::atomic<int>& total) {
    while (total < EXPECTED) {
        filled.wait();
        int val;
        {
            RtLockGuard lk(bufMutex);
            val = buffer.front();
            buffer.pop();
        }
        empty.post();
        total += val;
    }
}
```

---

## 5. Barrier Synchronization

### Use case

- Multi-stage pipeline: tất cả threads phải hoàn thành stage N trước khi vào stage N+1.

### Code gợi ý

```cpp
RtBarrier barrier(8);
std::atomic<int> phase{0};

for (int i = 0; i < 8; ++i) {
    std::thread([&]{
        for (int p = 0; p < 10000; ++p) {
            barrier.wait();
            phase.fetch_add(1);
        }
    }).detach();
}
```

---

## 6. Spinlock vs Mutex

### Khi nào dùng spinlock?

- Critical section cực ngắn (vài instruction).
- Contention thấp.
- Không muốn context switch cost.

### Khi nào dùng mutex?

- Critical section dài.
- Có thể block lâu.
- Cần priority inheritance.

### Pause instruction

```cpp
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    sched_yield();
#endif
```

---

## 7. Interview Q&A

**Q: Làm sao test priority inversion?**
A: Tạo 3 thread Low/Medium/High. Low giữ lock, Medium busy loop, High đợi lock. So sánh wait time với và không có PI.

**Q: Robust mutex dùng khi nào?**
A: Khi thread giữ mutex có thể bị kill/crash, tránh deadlock toàn hệ thống.

**Q: Spinlock vs mutex?**
A: Spinlock cho critical section ngắn, tránh context switch. Mutex cho section dài, hỗ trợ PI.

---

## 8. References

- `man pthread_mutexattr_setrobust`
- `man sem_init`
- `man pthread_barrier_init`
- `man sched_yield`
