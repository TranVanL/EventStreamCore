# Cross-RTOS Abstraction

> **Scope:** Extend EventStreamCore's platform abstraction beyond Linux and QNX to FreeRTOS, Zephyr, and ThreadX. Demonstrates broad RTOS literacy and portable design.
> **Target audience:** Senior embedded engineers who may be asked about multiple RTOS ecosystems.

---

## 1. Why Cross-RTOS?

Most embedded/RTOS JDs mention more than one RTOS:

- **Automotive:** QNX, Classic AUTOSAR OS, Adaptive AUTOSAR (POSIX-based).
- **IoT/Edge:** FreeRTOS, Zephyr, ThreadX, RIOT.
- **Aerospace/Defense:** VxWorks, RTEMS.
- **Industrial:** FreeRTOS, Zephyr, ThreadX.

A project that only supports Linux + QNX looks narrow. Adding FreeRTOS/Zephyr/ThreadX abstraction shows:

1. Understanding of common RTOS primitives across ecosystems.
2. Ability to design portable interfaces.
3. Awareness of POSIX PSE52 as a bridge standard.

---

## 2. Design Philosophy

### 2.1 Same Policy-Based Template Interface

The existing `platform::Thread<Platform>`, `platform::Mutex<Platform>`, `platform::Queue<Platform>`, `platform::Semaphore<Platform>` interfaces are reused.

```cpp
template<typename Platform>
class Thread {
public:
    using Handle = typename Platform::ThreadHandle;
    static bool create(Handle& out, void* (*fn)(void*), void* arg);
    static bool join(Handle h);
    static bool setPriority(Handle h, int priority);
};
```

### 2.2 Compile-Time Platform Selection

```cpp
#if defined(__QNX__) || defined(__QNXNTO__)
    #define ESC_PLATFORM_QNX
    using CurrentPlatform = QnxPlatform;
#elif defined(FREERTOS)
    #define ESC_PLATFORM_FREERTOS
    using CurrentPlatform = FreeRtosPlatform;
#elif defined(__ZEPHYR__)
    #define ESC_PLATFORM_ZEPHYR
    using CurrentPlatform = ZephyrPlatform;
#elif defined(TX_INCLUDE_USER_DEFINE_FILE)
    #define ESC_PLATFORM_THREADX
    using CurrentPlatform = ThreadxPlatform;
#elif defined(__linux__)
    #define ESC_PLATFORM_LINUX
    using CurrentPlatform = LinuxPlatform;
#else
    #error "Unsupported platform"
#endif
```

### 2.3 POSIX PSE52 Bridge

For RTOSes with POSIX compatibility layers (Zephyr POSIX subsystem, FreeRTOS+POSIX, ThreadX POSIX), a `PosixPse52Platform` can be used as a fallback.

---

## 3. FreeRTOS Abstraction

### 3.1 Thread = Task

```cpp
struct FreeRtosPlatform {
    using ThreadHandle = TaskHandle_t;

    static bool create(ThreadHandle& out, void* (*fn)(void*), void* arg) {
        BaseType_t rc = xTaskCreate(
            reinterpret_cast<TaskFunction_t>(fn),
            "esc_task",
            configMINIMAL_STACK_SIZE * 4,
            arg,
            tskIDLE_PRIORITY + 1,
            &out);
        return rc == pdPASS;
    }

    static bool join(ThreadHandle h) {
        // FreeRTOS tasks are typically never joined; block on a notification/event.
        // Stub: wait for task notification.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        return true;
    }
};
```

### 3.2 Mutex

```cpp
struct FreeRtosMutex {
    using Handle = SemaphoreHandle_t;
    static Handle create() { return xSemaphoreCreateMutex(); }
    static void lock(Handle h) { xSemaphoreTake(h, portMAX_DELAY); }
    static void unlock(Handle h) { xSemaphoreGive(h); }
    static void destroy(Handle h) { vSemaphoreDelete(h); }
};
```

### 3.3 Queue

```cpp
struct FreeRtosQueue {
    using Handle = QueueHandle_t;
    static Handle create(size_t length, size_t itemSize) {
        return xQueueCreate(length, itemSize);
    }
    static bool send(Handle h, const void* item, uint32_t timeout_ms) {
        return xQueueSend(h, item, pdMS_TO_TICKS(timeout_ms)) == pdPASS;
    }
    static bool receive(Handle h, void* item, uint32_t timeout_ms) {
        return xQueueReceive(h, item, pdMS_TO_TICKS(timeout_ms)) == pdPASS;
    }
};
```

### 3.4 Semaphore

```cpp
struct FreeRtosSemaphore {
    using Handle = SemaphoreHandle_t;
    static Handle create(int initial) { return xSemaphoreCreateCounting(255, initial); }
    static void wait(Handle h) { xSemaphoreTake(h, portMAX_DELAY); }
    static void post(Handle h) { xSemaphoreGive(h); }
};
```

---

## 4. Zephyr Abstraction

### 4.1 Thread

```cpp
struct ZephyrPlatform {
    using ThreadHandle = k_tid_t;

    static bool create(ThreadHandle& out, void* (*fn)(void*), void* arg) {
        static struct k_thread thread_stack;
        static K_THREAD_STACK_DEFINE(stack_area, 2048);
        out = k_thread_create(&thread_stack, stack_area,
                              K_THREAD_STACK_SIZEOF(stack_area),
                              reinterpret_cast<k_thread_entry_t>(fn),
                              arg, nullptr, nullptr,
                              K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
        return out != nullptr;
    }

    static bool join(ThreadHandle h) {
        k_thread_join(h, K_FOREVER);
        return true;
    }
};
```

### 4.2 Mutex

```cpp
struct ZephyrMutex {
    using Handle = struct k_mutex*;
    static Handle create() {
        static struct k_mutex m;
        k_mutex_init(&m);
        return &m;
    }
    static void lock(Handle h) { k_mutex_lock(h, K_FOREVER); }
    static void unlock(Handle h) { k_mutex_unlock(h); }
};
```

### 4.3 Message Queue

```cpp
struct ZephyrQueue {
    using Handle = struct k_msgq*;
    static Handle create(size_t msg_size, size_t max_msgs) {
        static struct k_msgq q;
        static char buffer[64 * 64];  // example sizing
        k_msgq_init(&q, buffer, msg_size, max_msgs);
        return &q;
    }
    static bool send(Handle h, const void* msg, k_timeout_t timeout) {
        return k_msgq_put(h, msg, timeout) == 0;
    }
    static bool receive(Handle h, void* msg, k_timeout_t timeout) {
        return k_msgq_get(h, msg, timeout) == 0;
    }
};
```

---

## 5. ThreadX Abstraction

### 5.1 Thread

```cpp
struct ThreadxPlatform {
    using ThreadHandle = TX_THREAD*;

    static bool create(ThreadHandle& out, void* (*fn)(void*), void* arg) {
        static TX_THREAD thread;
        static CHAR stack[2048];
        UINT rc = tx_thread_create(&thread, "esc_task",
                                   reinterpret_cast<VOID (*)(ULONG)>(fn),
                                   reinterpret_cast<ULONG>(arg),
                                   stack, sizeof(stack),
                                   16, 16, TX_NO_TIME_SLICE, TX_AUTO_START);
        out = &thread;
        return rc == TX_SUCCESS;
    }
};
```

### 5.2 Mutex

```cpp
struct ThreadxMutex {
    using Handle = TX_MUTEX*;
    static Handle create() {
        static TX_MUTEX m;
        tx_mutex_create(&m, "esc_mutex", TX_NO_INHERIT);
        return &m;
    }
    static void lock(Handle h) { tx_mutex_get(h, TX_WAIT_FOREVER); }
    static void unlock(Handle h) { tx_mutex_put(h); }
};
```

### 5.3 Queue

```cpp
struct ThreadxQueue {
    using Handle = TX_QUEUE*;
    static Handle create(size_t msg_size, size_t capacity) {
        static TX_QUEUE q;
        static ULONG queue_area[64];  // example
        tx_queue_create(&q, "esc_queue", msg_size / sizeof(ULONG),
                        queue_area, sizeof(queue_area));
        return &q;
    }
    static bool send(Handle h, void* msg, ULONG timeout) {
        return tx_queue_send(h, msg, timeout) == TX_SUCCESS;
    }
    static bool receive(Handle h, void* msg, ULONG timeout) {
        return tx_queue_receive(h, msg, timeout) == TX_SUCCESS;
    }
};
```

---

## 6. Mapping RTOS Primitives

| Concept | Linux | QNX | FreeRTOS | Zephyr | ThreadX |
|---------|-------|-----|----------|--------|---------|
| Thread | `pthread_t` | `pthread_t` | `TaskHandle_t` | `k_tid_t` | `TX_THREAD*` |
| Mutex | `pthread_mutex_t` | `pthread_mutex_t` | `SemaphoreHandle_t` | `struct k_mutex` | `TX_MUTEX` |
| Semaphore | `sem_t` | `sem_t` | `SemaphoreHandle_t` | `struct k_sem` | `TX_SEMAPHORE` |
| Queue | POSIX MQ | Channel/Msg | `QueueHandle_t` | `struct k_msgq` | `TX_QUEUE` |
| Timer | `timerfd` | `timer_create` | `TimerHandle_t` | `struct k_timer` | `TX_TIMER` |
| Priority range | 1–99 | 1–255 | 0–configMAX_PRIORITIES-1 | preempt/co-op levels | 0–31 |

---

## 7. EventStreamCore Integration

### 7.1 Platform-Agnostic Code

```cpp
#include <eventstream/platform/platform_detect.hpp>
#include <eventstream/platform/rtos_abstraction.hpp>

using Platform = eventstream::platform::CurrentPlatform;
using Thread = eventstream::platform::Thread<Platform>;
using Mutex = eventstream::platform::Mutex<Platform>;

void engineWorker(void* arg) {
    // same code on Linux, QNX, FreeRTOS, Zephyr, ThreadX
}

int main() {
    Thread::Handle t;
    Thread::create(t, engineWorker, nullptr);
    Thread::join(t);
}
```

### 7.2 Feature Flags

Some features are not available on all RTOSes:

```cmake
option(ENABLE_POSIX_IPC "POSIX message queues/shared memory" ON)
option(ENABLE_IO_URING "Linux io_uring" OFF)
option(ENABLE_HUGEPAGES "Linux hugepages" OFF)
option(ENABLE_QNX_APS "QNX adaptive partitioning" OFF)
option(ENABLE_RTOS_SIMULATOR "RTOS simulator on Linux" ON)
```

---

## 8. Build Considerations

### 8.1 FreeRTOS

FreeRTOS is usually compiled as part of a board project, not as a standalone library. The abstraction headers are designed to be dropped into a FreeRTOS project.

### 8.2 Zephyr

Zephyr uses Kconfig and DeviceTree. The abstraction headers rely on `CONFIG_POSIX_API` or native Zephyr APIs.

### 8.3 ThreadX

ThreadX is often bundled with Azure RTOS. Headers assume `tx_api.h` is in the include path.

---

## 9. Validation Strategy

Because real hardware is not available for all RTOSes:

1. **Compile-only CI** for FreeRTOS/Zephyr/ThreadX headers.
2. **RTOS Simulator on Linux** validates logic (see `RTOS_SIMULATOR.md`).
3. **QNX cross-compile** validates QNX path.
4. **Native Linux tests** validate the abstraction interface.

---

## 10. Interview Q&A

**Q: Why support multiple RTOSes instead of just QNX?**

> A: Different markets use different RTOSes. Automotive infotainment uses QNX, IoT edge uses FreeRTOS or Zephyr, industrial controllers use ThreadX. A portable abstraction proves the design is not tied to one OS and that I understand the common primitives across ecosystems.

**Q: How do you handle different priority schemes?**

> A: The platform layer exposes a normalized priority range internally. Each platform maps that range to its native scheme. For example, ThreadX 0–31 maps to our 0–99 range linearly.

**Q: What is POSIX PSE52 and why does it matter?**

> A: PSE52 is the POSIX real-time profile. Some RTOSes like Zephyr provide a POSIX compatibility layer. It lets us reuse a POSIX-based platform implementation where available, reducing porting effort.

**Q: What is the hardest part of porting to FreeRTOS?**

> A: Memory: FreeRTOS tasks need statically allocated stacks, and dynamic allocation may be disabled. Our abstraction uses static buffers and pre-allocated pools. Also, FreeRTOS tasks are not usually joined, so lifecycle semantics differ from pthreads.

**Q: How do you test without hardware?**

> A: Compile-only CI for the target RTOS headers, plus an RTOS simulator on Linux that implements the same abstraction interface using pthreads and POSIX IPC. This catches logic bugs without needing physical boards.

---

## 11. References

- FreeRTOS Kernel documentation
- Zephyr Project documentation
- Microsoft Azure RTOS ThreadX documentation
- POSIX PSE52 Real-Time Profile
- EventStreamCore `docs/RTOS_SIMULATOR.md`
