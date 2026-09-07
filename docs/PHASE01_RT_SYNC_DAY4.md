# Day 4 — `RtSemaphore`, `RtBarrier`, `RtSpinlock`

> **Phase:** Phase 1 — Real-Time Scheduling Foundation  
> **Mục tiêu:** Hiểu và triển khai ba primitive synchronization khác nhau trong POSIX/C++ real-time code.  
> **Files dự kiến:**
>
> ```text
> include/eventstream/rt/rt_semaphore.hpp
> include/eventstream/rt/rt_barrier.hpp
> include/eventstream/rt/rt_spinlock.hpp
> src/rt/rt_semaphore.cpp
> src/rt/rt_barrier.cpp
> src/rt/rt_spinlock.cpp
> ```

---

## 1. Bức tranh tổng thể

Day 3 xây dựng `RtMutex`, primitive có ownership và priority inheritance. Day 4 bổ sung ba primitive khác:

| Primitive | Câu hỏi nó trả lời |
|---|---|
| `RtSemaphore` | Có bao nhiêu permit/resource đang available? |
| `RtBarrier` | Tất cả thread đã hoàn thành phase hiện tại chưa? |
| `RtSpinlock` | Làm sao bảo vệ một critical section cực ngắn mà không block vào kernel? |

Không nên xem ba primitive này là các phiên bản khác nhau của mutex.

```text
RtMutex    = Ai đang sở hữu critical section?
Semaphore  = Còn bao nhiêu permit?
Barrier    = Đã đủ thread đến điểm đồng bộ chưa?
Spinlock   = Lock cực ngắn, chờ chủ động bằng CPU
```

---

# Phần I — `RtSemaphore`

## 2. Semaphore là gì?

Semaphore là một counter được kernel hỗ trợ để điều phối thread.

Giả sử semaphore có giá trị ban đầu là `3`:

```text
S = 3
```

Mỗi lần gọi `wait()` thành công:

```text
S = S - 1
```

Mỗi lần gọi `post()`:

```text
S = S + 1
```

Nếu `S == 0`, thread gọi `wait()` sẽ bị block cho đến khi thread khác gọi `post()`.

Mô hình đơn giản:

```text
wait():
    nếu count > 0:
        count--
        tiếp tục chạy
    nếu count == 0:
        block thread

post():
    count++
    đánh thức một waiter nếu có
```

Điểm quan trọng: semaphore **không có owner**. Thread A có thể gọi `wait()`, còn thread B gọi `post()`.

```text
Thread A: wait()
Thread B: post()
```

Điều này hợp lệ với semaphore nhưng không phải semantics thông thường của mutex.

---

## 3. Counting semaphore và binary semaphore

### 3.1. Counting semaphore

Counting semaphore biểu diễn nhiều resource giống nhau.

Ví dụ EventStreamCore có pool gồm 10 buffer:

```cpp
RtSemaphore availableBuffers(10);
```

Worker muốn lấy một buffer:

```cpp
availableBuffers.wait();
```

Worker trả buffer:

```cpp
availableBuffers.post();
```

Giá trị semaphore đại diện cho số buffer còn available.

### 3.2. Binary semaphore

Binary semaphore chỉ có hai trạng thái:

```text
0 = unavailable
1 = available
```

Có thể dùng để signal giữa hai thread:

```text
Worker hoàn thành
        ↓
      post()
        ↓
Main thread đang wait()
        ↓
      thức dậy
```

Binary semaphore gần giống notification/event hơn là mutex.

---

## 4. Semaphore khác mutex như thế nào?

| Đặc điểm | Semaphore | Mutex |
|---|---|---|
| Có owner | Không | Có |
| Thread khác được release | Có | Không hợp lệ |
| Có counting value | Có | Không |
| Dùng để signal | Rất phù hợp | Không phải mục đích chính |
| Dùng producer/consumer | Rất phù hợp | Không đủ một mình |
| Priority inheritance | Thông thường không | Có thể bật |
| Bảo vệ invariant của data | Không trực tiếp | Có |
| Có robust owner-death | Không theo mutex semantics | Có thể hỗ trợ |

Cách ghi nhớ:

```text
Semaphore = Có bao nhiêu permit?
Mutex     = Ai đang sở hữu quyền sửa protected state?
```

Nếu cần bảo vệ `std::deque`, `std::map` hoặc state invariant, dùng `RtMutex`.

Nếu cần giới hạn tối đa 100 task đang chạy đồng thời, dùng semaphore.

---

## 5. API cơ bản của `RtSemaphore`

Thiết kế tối thiểu:

```cpp
class RtSemaphore {
public:
    explicit RtSemaphore(unsigned initialValue);
    ~RtSemaphore();

    void wait();
    bool tryWait();
    void post();
    int getValue() const;

    RtSemaphore(const RtSemaphore&) = delete;
    RtSemaphore& operator=(const RtSemaphore&) = delete;
    RtSemaphore(RtSemaphore&&) = delete;
    RtSemaphore& operator=(RtSemaphore&&) = delete;
};
```

Các method nên có behavior rõ ràng:

```text
wait()     = block nếu không có permit
tryWait()  = không block, trả false nếu hết permit
post()     = trả lại một permit
getValue() = đọc snapshot cho metrics/debug
```

---

## 6. `wait()` và `EINTR`

POSIX API:

```cpp
sem_wait(&sem_);
```

`sem_wait()` có thể bị signal interrupt và trả lỗi `EINTR`.

Nếu muốn behavior blocking đúng nghĩa, thường phải retry:

```cpp
while (sem_wait(&sem_) == -1) {
    if (errno != EINTR) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "sem_wait failed");
    }
}
```

Tại sao retry `EINTR`?

```text
EINTR không có nghĩa semaphore hỏng.
Nó chỉ có nghĩa system call bị signal ngắt trước khi hoàn thành.
```

Không được retry mọi error. Chỉ retry khi `errno == EINTR`.

Nếu API của `RtSemaphore::wait()` cần cancellation hoặc timeout, cần document behavior rõ ràng thay vì âm thầm retry vô hạn.

---

## 7. `tryWait()` và `EAGAIN`

`tryWait()` không block:

```cpp
if (sem_trywait(&sem_) == 0) {
    return true;
}

if (errno == EAGAIN) {
    return false;
}
```

`EAGAIN` không phải fatal error. Nó chỉ có nghĩa:

```text
Ngay thời điểm kiểm tra, không có permit available.
```

Không nên viết:

```cpp
try {
    semaphore.tryWait();
} catch (...) {
    // EAGAIN không nên bị coi là exception thông thường.
}
```

Behavior mong muốn:

```text
permit available   → true
permit unavailable → false
POSIX error khác   → exception/error report
```

---

## 8. `post()` và giới hạn counter

POSIX API:

```cpp
sem_post(&sem_);
```

`post()` tăng counter và đánh thức một waiter nếu có.

Có thể fail nếu counter vượt giới hạn implementation, thường là `EOVERFLOW`.

Nếu semaphore mô tả resource bounded, số lần `post()` phải cân bằng với số lần `wait()` thành công.

Ví dụ sai:

```cpp
RtSemaphore slots(10);

slots.post();
slots.post();
slots.post();
// Counter không còn phản ánh số slot thật.
```

Semaphore không tự biết application có gọi `post()` hợp lý hay không. Invariant đó thuộc về thiết kế của application.

---

## 9. `getValue()` chỉ là snapshot

POSIX cung cấp:

```cpp
int value = 0;
sem_getvalue(&sem_, &value);
```

Nhưng giá trị có thể thay đổi ngay sau khi đọc.

```cpp
int value = semaphore.getValue();
// Thread khác có thể wait/post ngay sau dòng trên.
```

Vì vậy `getValue()` chỉ nên dùng cho:

- Debug.
- Metrics.
- Logging.
- Test đơn giản.

Không được dùng làm synchronization logic:

```cpp
if (semaphore.getValue() > 0) {
    // Sai: thread khác có thể lấy permit trước.
}
```

Nếu muốn lấy permit an toàn, phải gọi `tryWait()`.

---

## 10. Unnamed semaphore

Unnamed semaphore dùng `sem_init()`:

```cpp
sem_t sem;
sem_init(&sem, 0, 3);
```

Tham số thứ hai là `pshared`:

```cpp
pshared == 0
```

nghĩa là semaphore được dùng giữa các thread trong process hiện tại.

Cleanup:

```cpp
sem_destroy(&sem);
```

Không được destroy khi thread khác vẫn có thể gọi `wait()` hoặc `post()` trên semaphore.

---

## 11. Named semaphore

Named semaphore được kernel quản lý bằng tên:

```cpp
sem_t* sem = sem_open(
    "/eventstream_slots",
    O_CREAT,
    0644,
    10);
```

Tên POSIX thường bắt đầu bằng `/`:

```text
/eventstream_slots
```

Named semaphore cho phép nhiều process mở cùng một object.

```text
Process A ── sem_open("/eventstream_slots")
Process B ── sem_open("/eventstream_slots")
                         ↓
                 Cùng một semaphore
```

---

## 12. `sem_close()` và `sem_unlink()`

Hai operation này khác nhau.

### `sem_close()`

Đóng handle của process hiện tại:

```cpp
sem_close(sem);
```

### `sem_unlink()`

Xóa tên semaphore khỏi namespace:

```cpp
sem_unlink("/eventstream_slots");
```

Lifecycle concept:

```text
sem_open()
    ↓
sem_wait()/sem_post()
    ↓
sem_close()
    ↓
sem_unlink()
```

`sem_close()` không nhất thiết xóa named semaphore. Nếu quên `sem_unlink()`, object có thể tồn tại sau khi process kết thúc và ảnh hưởng lần chạy sau.

Nếu nhiều process cùng dùng semaphore, không được một process tùy tiện unlink khi process khác còn cần nó.

---

## 13. Một vấn đề thiết kế quan trọng của `RtSemaphore`

Unnamed semaphore dùng:

```cpp
sem_t
```

Named semaphore dùng handle trả về từ:

```cpp
sem_open()
```

Handle đó có kiểu:

```cpp
sem_t*
```

Vì vậy class không nên chỉ có:

```cpp
sem_t sem_;
```

cho cả hai loại.

Một thiết kế phù hợp hơn:

```cpp
class RtSemaphore {
private:
    sem_t unnamedStorage_{};
    sem_t* handle_{nullptr};
    bool named_{false};
    bool unlinkOnDestroy_{false};
    std::string name_;
};
```

Unnamed:

```text
handle_ = &unnamedStorage_
sem_init(handle_, ...)
```

Named:

```text
handle_ = sem_open(...)
named_ = true
```

Destructor:

```text
named semaphore:
    sem_close(handle_)
    nếu ownership cho phép:
        sem_unlink(name_)

unnamed semaphore:
    sem_destroy(&unnamedStorage_)
```

Cần xác định rõ class có sở hữu lifecycle của named semaphore hay không.

---

## 14. Producer/consumer với semaphore

Bounded buffer thường dùng hai semaphore:

```text
emptySlots = số slot còn trống
fullSlots  = số item đã sẵn sàng
```

Buffer capacity là `100`:

```cpp
RtSemaphore emptySlots(100);
RtSemaphore fullSlots(0);
RtMutex bufferMutex;
```

### Producer

```text
emptySlots.wait()
        ↓
lock(bufferMutex)
        ↓
push item
        ↓
unlock(bufferMutex)
        ↓
fullSlots.post()
```

### Consumer

```text
fullSlots.wait()
        ↓
lock(bufferMutex)
        ↓
pop item
        ↓
unlock(bufferMutex)
        ↓
emptySlots.post()
```

Pseudocode:

```cpp
void produce(Event event) {
    emptySlots.wait();

    {
        RtLockGuard lock(bufferMutex);
        buffer.push_back(std::move(event));
    }

    fullSlots.post();
}

Event consume() {
    fullSlots.wait();

    Event event;

    {
        RtLockGuard lock(bufferMutex);
        event = std::move(buffer.front());
        buffer.pop_front();
    }

    emptySlots.post();
    return event;
}
```

Semaphore điều phối capacity. Mutex bảo vệ cấu trúc `buffer`.

Không thể dùng chỉ một semaphore để thay hoàn toàn mutex trong ví dụ này.

---

## 15. Semaphore và priority inheritance

Semaphore thông thường không có owner và không tự cung cấp priority inheritance như PI mutex.

Nếu thread high priority bị block vì thread low priority đang bảo vệ shared state, dùng:

```text
RtMutex + PTHREAD_PRIO_INHERIT
```

Không dùng semaphore như mutex chỉ vì cả hai đều có method `wait()`/`post()` hoặc `lock()`/`unlock()` tương tự.

### Dùng semaphore cho

- Resource counting.
- Producer/consumer.
- Event notification.
- Rate limiting.
- Bounded concurrency.

### Dùng `RtMutex` cho

- Bảo vệ queue/map/state.
- Bảo vệ invariant.
- Mutual exclusion.
- Priority inversion mitigation.
- Robust owner-death recovery.

---

# Phần II — `RtBarrier`

## 16. Barrier là gì?

Barrier đồng bộ một nhóm thread tại một điểm cụ thể.

Ví dụ có bốn thread:

```text
Thread 1 ── work ── wait()
Thread 2 ── work ── wait()
Thread 3 ── work ── wait()
Thread 4 ── work ── wait()
```

Không thread nào được đi qua barrier cho đến khi đủ bốn thread gọi `wait()`.

```text
Thread 1 ─┐
Thread 2 ─┤
Thread 3 ─┼── barrier ── tất cả cùng đi tiếp
Thread 4 ─┘
```

POSIX API:

```cpp
pthread_barrier_init(
    &barrier_,
    nullptr,
    participantCount);
```

---

## 17. Barrier là reusable

Một barrier có thể dùng cho nhiều phase:

```text
Phase 1:
    tất cả thread làm việc
    tất cả gọi wait()

Phase 2:
    tất cả thread làm việc
    tất cả gọi wait()

Phase 3:
    tất cả thread làm việc
    tất cả gọi wait()
```

Sau khi thread cuối cùng tới, barrier tự reset cho phase tiếp theo.

Use case:

- Simulation tick.
- Parallel algorithm.
- Worker startup.
- Multi-stage processing.
- Synchronized benchmark.

---

## 18. `PTHREAD_BARRIER_SERIAL_THREAD`

`pthread_barrier_wait()` trả về:

- `0` cho phần lớn thread.
- `PTHREAD_BARRIER_SERIAL_THREAD` cho một thread được chọn.

Ví dụ:

```cpp
const int result = pthread_barrier_wait(&barrier_);

if (result == PTHREAD_BARRIER_SERIAL_THREAD) {
    // Một thread thực hiện operation một lần cho phase.
} else if (result != 0) {
    // Xử lý lỗi.
}
```

Thread nhận special value có thể:

- Reset phase state.
- In metrics một lần.
- Publish phase completion.
- Tạo work cho phase tiếp theo.

Không nên phụ thuộc vào thread cụ thể nào luôn nhận special value. POSIX không bảo đảm điều đó.

---

## 19. Barrier không bảo vệ data

Barrier chỉ trả lời:

```text
Tất cả thread đã tới điểm này chưa?
```

Nó không tự bảo vệ shared data.

Ví dụ sau vẫn có data race:

```cpp
barrier.wait();
sharedState.value++;
```

Nếu nhiều thread cùng ghi `sharedState`, vẫn cần:

- `RtMutex`.
- `RtSpinlock`.
- `std::atomic`.
- Hoặc thiết kế ownership riêng.

Barrier cũng không tự bỏ qua thread bị thiếu. Nếu barrier cần 8 participants nhưng chỉ có 7 thread gọi `wait()`, 7 thread có thể block vô hạn.

---

## 20. Barrier lifecycle

Khởi tạo:

```cpp
pthread_barrier_init(
    &barrier_,
    nullptr,
    participantCount);
```

Chờ:

```cpp
pthread_barrier_wait(&barrier_);
```

Hủy:

```cpp
pthread_barrier_destroy(&barrier_);
```

Không destroy barrier khi còn thread đang chờ. Không reuse object sau destroy.

Điều kiện cần validate:

```text
participantCount > 0
barrier chưa destroy
không còn waiter khi destroy
```

`RtBarrier` nên non-copyable và non-movable.

---

# Phần III — `RtSpinlock`

## 21. Spinlock là gì?

Spinlock là lock mà thread không block vào kernel. Nếu lock đang bận, thread chủ động thử lại:

```text
while lock busy:
    spin
```

Implementation dùng:

```cpp
std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
```

Acquire:

```cpp
while (flag_.test_and_set(std::memory_order_acquire)) {
    pause_or_yield();
}
```

Release:

```cpp
flag_.clear(std::memory_order_release);
```

---

## 22. `atomic_flag::test_and_set()`

Ban đầu:

```text
flag = false
```

Thread A gọi `test_and_set()`:

1. Đọc giá trị cũ.
2. Set flag thành `true`.
3. Trả về giá trị cũ.

Với thread A:

```text
old value = false
new value = true
→ A lấy được lock
```

Thread B gọi cùng lúc:

```text
old value = true
new value = true
→ B không lấy được lock
→ B tiếp tục spin
```

Bảng:

| Thread | Giá trị cũ | Giá trị mới | Kết quả |
|---|---:|---:|---|
| A | `false` | `true` | Acquire lock |
| B | `true` | `true` | Spin |
| A unlock | — | `false` | Lock available |

`test_and_set()` là một read-modify-write atomic operation.

Không thể thay bằng:

```cpp
if (!flag_) {
    flag_ = true;
}
```

vì hai operation đó không atomic với nhau. Hai thread có thể cùng đọc `false` rồi cùng bước vào critical section.

---

## 23. Memory ordering của spinlock

### 23.1. Acquire khi lock

```cpp
flag_.test_and_set(std::memory_order_acquire)
```

Acquire ngăn các thao tác sau khi lock bị reorder lên trước lock.

### 23.2. Release khi unlock

```cpp
flag_.clear(std::memory_order_release)
```

Release bảo đảm các thay đổi trong critical section được publish trước khi lock được mở.

Ví dụ:

```cpp
// Thread A
lock();
data = 42;
unlock();

// Thread B
lock();
read(data); // Có thể thấy 42
unlock();
```

Pattern cần ghi nhớ:

```text
lock   = acquire
unlock = release
```

Nếu dùng `memory_order_relaxed` cho cả hai, atomic flag vẫn có thể ngăn hai thread cùng set flag, nhưng không cung cấp đầy đủ ordering/visibility cho protected data.

---

## 24. `pause_or_yield()`

Vòng spin rỗng:

```cpp
while (flag_.test_and_set(...)) {
}
```

gây:

- CPU usage cao.
- Pipeline contention.
- Ảnh hưởng SMT sibling.
- Power consumption cao.
- Cache/coherency traffic.

Vì vậy cần processor hint hoặc scheduler yield.

### x86

```cpp
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
```

Instruction `PAUSE`:

- Báo cho CPU đây là spin-wait loop.
- Giảm penalty khi thoát loop.
- Giảm resource pressure trên hyper-threading.
- Không tạo system call.

### ARM

ARM có instruction tương đương `YIELD`. Intrinsic cụ thể phụ thuộc compiler/toolchain. Không nên giả định `__yield()` luôn có trên mọi environment.

### Fallback

```cpp
sched_yield();
```

hoặc:

```cpp
std::this_thread::yield();
```

Yield chuyển quyền chạy cho scheduler, nhưng behavior phụ thuộc OS và workload. Nó không phải sleep chính xác và không bảo đảm thread khác chạy ngay.

---

## 25. Exponential backoff

Một spinlock thực tế có thể dùng nhiều phase:

```text
Lần 1–100:
    CPU pause

Sau đó:
    scheduler yield

Nếu vẫn bận lâu:
    backoff tăng dần
```

Concept:

```cpp
for (unsigned attempt = 0; ; ++attempt) {
    if (!flag_.test_and_set(std::memory_order_acquire)) {
        return;
    }

    if (attempt < 100) {
        cpuPause();
    } else {
        std::this_thread::yield();
    }
}
```

Không nên yield ngay từ lần đầu vì lock có thể chỉ bị giữ vài nanoseconds. Context switch hoặc scheduler overhead có thể lớn hơn thời gian chờ thực tế.

Backoff giúp giảm contention, nhưng không bảo đảm fairness.

---

## 26. Khi nào dùng spinlock?

Spinlock phù hợp khi:

- Critical section cực ngắn.
- Lock holder chắc chắn sẽ chạy sớm.
- Không có I/O.
- Không có blocking operation.
- Contention thấp.
- Có nhiều CPU.
- Muốn tránh context switch.

Ví dụ:

```cpp
lock();
counter++;
unlock();
```

Spinlock không phù hợp với:

```cpp
lock();
database.write();
network.send();
sleep();
unlock();
```

Đặc biệt không nên dùng spinlock trên single-core nếu lock holder có thể bị preempt:

```text
CPU 0:
    Low giữ spinlock nhưng bị preempt
    High chạy và spin
    Low không có cơ hội chạy để unlock
```

Đây là starvation hoặc deadlock do scheduler.

---

## 27. Spinlock không có priority inheritance

Tình huống:

```text
Low priority giữ spinlock
High priority spin chờ
Medium priority chạy liên tục
```

`Low` có thể không được chạy để unlock. Spinlock không có owner metadata và không tích hợp `PTHREAD_PRIO_INHERIT`.

So sánh:

| Tình huống | Primitive phù hợp |
|---|---|
| Critical section vài instructions | `RtSpinlock` |
| Lock có thể giữ lâu | `RtMutex` |
| Cần priority inheritance | `RtMutex` PI |
| Có thể block | `RtMutex` |
| Cần signal/resource count | `RtSemaphore` |
| Đồng bộ phase | `RtBarrier` |

---

## 28. Spinlock không phải lock-free algorithm

Đây là điểm thường bị nói sai.

`RtSpinlock` sử dụng atomic instruction nhưng bản thân nó vẫn là một locking primitive. Thread có thể chờ vô hạn.

```text
Spinlock ≠ lock-free algorithm
```

Spinlock chỉ có nghĩa:

```text
Thread không sleep trong lúc chờ; nó busy-wait.
```

Busy-wait không đồng nghĩa với lock-free.

Một algorithm lock-free yêu cầu hệ thống luôn có global progress: trong một số bước hữu hạn, ít nhất một thread sẽ hoàn thành operation. Spinlock không bảo đảm property này.

---

# Phần IV — So sánh và lựa chọn

## 29. So sánh ba primitive

| Đặc điểm | `RtSemaphore` | `RtBarrier` | `RtSpinlock` |
|---|---|---|---|
| Có counter | Có | Không | Không |
| Block thread | Có | Có | Không, chủ yếu spin |
| Dùng để signal | Có | Không chính | Không |
| Đồng bộ phase | Không | Có | Không |
| Bảo vệ data | Không trực tiếp | Không | Có |
| Có ownership | Không | Không | Logic ownership ngầm |
| Priority inheritance | Không mặc định | Không | Không |
| Cross-process | Named/pshared | Có thể hỗ trợ | Không |
| Dùng trong hot path | Có thể | Thường không | Chỉ khi section rất ngắn |

### Decision tree

```text
Cần đếm permit/resource?
    └── RtSemaphore

Cần chờ tất cả thread hoàn thành phase?
    └── RtBarrier

Cần bảo vệ vài instructions và không muốn block?
    └── RtSpinlock

Cần ownership, robust recovery hoặc priority inheritance?
    └── RtMutex
```

---

# Phần V — Testing

## 30. Test cho `RtSemaphore`

Nên có:

```text
BasicWaitPost
TryWaitReturnsFalseWhenEmpty
WaitBlocksUntilPost
ProducerConsumer
GetValue
NamedSemaphoreRoundTrip
```

### Producer/consumer stress test

```text
3 producers × 10,000 items
2 consumers
Tổng consumed == tổng produced
Không deadlock
Không mất item
```

Named semaphore test cần:

- Tên unique.
- Cleanup với `sem_unlink()`.
- Không để object cũ từ lần test trước.
- Có timeout để tránh test treo vô hạn.
- Không unlink khi process khác còn sử dụng.

## 31. Test cho `RtBarrier`

Nên có:

```text
AllThreadsReachBarrier
ReusableAcrossMultiplePhases
ExactlyOneSerialThread
InvalidParticipantCount
```

Ví dụ:

```text
8 threads
1,000 phases
mỗi phase tất cả thread phải qua barrier
```

Cần test nhiều phase vì barrier có thể đúng ở phase đầu nhưng sai khi reuse.

## 32. Test cho `RtSpinlock`

Nên có:

```text
BasicLockUnlock
ConcurrentCounterIncrement
HighContention
RepeatedLockUnlock
```

Ví dụ:

```text
N threads
mỗi thread tăng counter 100,000 lần
counter cuối == N × 100,000
```

Không nên đặt latency threshold quá cứng vì CI/cloud có scheduler noise.

---

# Phần VI — Pitfalls

## 33. Pitfalls của semaphore

- Gọi `post()` thừa làm counter sai.
- Dùng semaphore thay mutex để bảo vệ data.
- Không retry `EINTR` trong `wait()`.
- Coi `EAGAIN` của `tryWait()` là fatal error.
- Dùng `getValue()` để quyết định synchronization.
- Quên `sem_unlink()` với named semaphore.
- Destroy khi thread khác vẫn đang wait.
- Nhầm `sem_t` và `sem_t*` của unnamed/named semaphore.

## 34. Pitfalls của barrier

- Một thread không bao giờ tới barrier.
- Sai participant count.
- Destroy khi waiter còn tồn tại.
- Nghĩ barrier bảo vệ shared data.
- Dùng barrier thay condition variable.
- Phụ thuộc thread cụ thể nhận `PTHREAD_BARRIER_SERIAL_THREAD`.

## 35. Pitfalls của spinlock

- Giữ spinlock quá lâu.
- Gọi I/O trong critical section.
- Không dùng acquire/release.
- Không có pause hoặc backoff.
- Spin trên single CPU với owner có thể bị preempt.
- Dùng spinlock cho contention cao.
- Tưởng spinlock có priority inheritance.
- Allocation hoặc operation nặng bên trong spin loop.
- Gọi scheduler yield quá sớm ở mọi vòng lặp.

---

# Phần VII — Design và Definition of Done

## 36. Design tổng thể

```text
RtSemaphore
├── unnamed: sem_init/sem_destroy
├── named: sem_open/sem_close/sem_unlink
├── wait()
├── tryWait()
├── post()
└── getValue()

RtBarrier
├── pthread_barrier_init
├── pthread_barrier_wait
├── pthread_barrier_destroy
└── reusable phase synchronization

RtSpinlock
├── atomic_flag
├── test_and_set(acquire)
├── clear(release)
├── CPU pause
├── scheduler yield
└── optional backoff
```

Tất cả wrapper nên non-copyable và non-movable vì underlying synchronization object không nên bị copy/move tùy tiện.

## 37. Definition of Done

### `RtSemaphore`

- [ ] Unnamed semaphore hoạt động.
- [ ] Named semaphore hoạt động đúng lifecycle.
- [ ] `wait()` xử lý `EINTR`.
- [ ] `tryWait()` phân biệt `EAGAIN`.
- [ ] `post()` kiểm tra lỗi.
- [ ] `getValue()` được document là snapshot.
- [ ] Không dùng `getValue()` để synchronize.
- [ ] Cleanup `sem_destroy`, `sem_close`, `sem_unlink` đúng loại.

### `RtBarrier`

- [ ] Đồng bộ đúng participant count.
- [ ] Reusable qua nhiều phase.
- [ ] Xử lý `PTHREAD_BARRIER_SERIAL_THREAD`.
- [ ] Destructor không chạy khi còn waiter.
- [ ] Invalid participant count được xử lý.

### `RtSpinlock`

- [ ] Dùng `std::atomic_flag`.
- [ ] Lock dùng `memory_order_acquire`.
- [ ] Unlock dùng `memory_order_release`.
- [ ] Có processor pause/yield.
- [ ] Có thể thêm backoff.
- [ ] Critical section được document là phải ngắn.
- [ ] Không claim spinlock là lock-free algorithm.
- [ ] Không dùng spinlock để thay PI mutex.

### Testing

- [ ] Basic semaphore test.
- [ ] Producer/consumer stress test.
- [ ] Named semaphore test.
- [ ] Barrier multi-phase test.
- [ ] Barrier serial-thread test.
- [ ] Spinlock contention test.
- [ ] Test không bị deadlock nếu input hợp lệ.

---

# Phần VIII — Câu hỏi phỏng vấn

## Semaphore khác mutex thế nào?

Semaphore là counter dùng để quản lý permit hoặc signal giữa các thread và không có owner. Mutex có ownership và dùng để bảo vệ mutual exclusion. Thread khác gọi `post()` trên semaphore là hợp lệ, nhưng thread khác unlock mutex không phải owner là lỗi.

## Khi nào dùng semaphore thay vì condition variable?

Dùng semaphore khi cần lưu số lượng permit hoặc signal. Condition variable thường đi kèm mutex và predicate; notification không đại diện trực tiếp cho một counter resource.

## Barrier dùng để làm gì?

Barrier buộc một nhóm thread hoàn thành phase hiện tại trước khi đi sang phase tiếp theo. Nó dùng cho phase synchronization, không dùng để bảo vệ shared data.

## Spinlock có nhanh hơn mutex không?

Chỉ khi critical section rất ngắn và contention thấp. Spinlock tránh context switch nhưng tiêu thụ CPU. Nếu lock bị giữ lâu, mutex thường tốt hơn vì thread có thể block.

## Spinlock có lock-free không?

Không. Spinlock là locking primitive dùng atomic flag. Thread vẫn có thể chờ vô hạn.

## Semaphore có priority inheritance không?

Thông thường không. Nếu cần giảm priority inversion khi bảo vệ shared resource, dùng POSIX mutex với `PTHREAD_PRIO_INHERIT`.

## `PTHREAD_BARRIER_SERIAL_THREAD` là gì?

Đó là return value đặc biệt từ `pthread_barrier_wait()`, được trả cho một thread trong mỗi phase. Nó cho phép một thread thực hiện operation một lần, nhưng không bảo đảm thread đó luôn cố định.

## Vì sao không dùng spinlock cho mọi thứ?

Vì spinlock tiêu thụ CPU trong thời gian chờ, không có priority inheritance, có thể tạo starvation trên single CPU và gây latency tệ hơn mutex nếu critical section dài hoặc contention cao.

---

# 39. Tóm tắt cần nhớ

```text
Semaphore quản lý permit.
Barrier đồng bộ phase.
Spinlock bảo vệ critical section cực ngắn.
Mutex bảo vệ ownership và hỗ trợ priority inheritance.
```

Hoặc:

```text
Semaphore = resource counting
Barrier   = phase synchronization
Spinlock  = short busy-wait exclusion
Mutex     = ownership + mutual exclusion + optional PI/robustness
```
