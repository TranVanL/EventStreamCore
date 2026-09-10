#pragma once

#include <eventstream/platform/platform_detect.hpp>
#include <eventstream/platform/platform_contract.hpp>
#include <eventstream/rt/rt_policy.hpp>

#include <chrono>
#include <functional>
#include <mutex>
#include <time.h>
#include <utility>

#if ESC_HAS_PTHREAD
    #include <pthread.h>
#endif

#if ESC_HAS_POSIX_SEMAPHORE
    #include <semaphore.h>
#endif

#if ESC_HAS_POSIX_MESSAGE_QUEUE
    #include <mqueue.h>
#endif

#if ESC_PLATFORM_ID == ESC_PLATFORM_ID_LINUX
    #include <eventstream/platform/linux/linux_channel.hpp>
    #include <eventstream/platform/linux/linux_condvar.hpp>
    #include <eventstream/platform/linux/linux_mutex.hpp>
    #include <eventstream/platform/linux/linux_semaphore.hpp>
    #include <eventstream/platform/linux/linux_thread.hpp>
    #include <eventstream/platform/linux/linux_timer.hpp>
#elif ESC_PLATFORM_ID == ESC_PLATFORM_ID_QNX
    #include <eventstream/platform/qnx/qnx_channel.hpp>
    #include <eventstream/platform/qnx/qnx_condvar.hpp>
    #include <eventstream/platform/qnx/qnx_mutex.hpp>
    #include <eventstream/platform/qnx/qnx_semaphore.hpp>
    #include <eventstream/platform/qnx/qnx_thread.hpp>
    #include <eventstream/platform/qnx/qnx_timer.hpp>
#endif

namespace eventstream::platform {

struct LinuxPlatform {};
struct QnxPlatform {};

class LinuxThreadBackend;
class LinuxMutexBackend;
class LinuxCondvarBackend;
class LinuxSemaphoreBackend;
class LinuxTimerBackend;
class LinuxChannelBackend;

class QnxThreadBackend;
class QnxMutexBackend;
class QnxCondvarBackend;
class QnxSemaphoreBackend;
class QnxTimerBackend;
class QnxChannelBackend;

template<typename Platform>
struct PlatformTraits;

#if ESC_PLATFORM_ID == ESC_PLATFORM_ID_LINUX
using CurrentPlatform = LinuxPlatform;
#elif ESC_PLATFORM_ID == ESC_PLATFORM_ID_QNX
using CurrentPlatform = QnxPlatform;
#else
#error "EventStreamCore: no current platform selected"
#endif

template<>
struct PlatformTraits<LinuxPlatform> {
    using ThreadBackend = LinuxThreadBackend;
    using MutexBackend = LinuxMutexBackend;
    using CondvarBackend = LinuxCondvarBackend;
    using SemaphoreBackend = LinuxSemaphoreBackend;
    using TimerBackend = LinuxTimerBackend;
    using ChannelBackend = LinuxChannelBackend;
};

template<>
struct PlatformTraits<QnxPlatform> {
    using ThreadBackend = QnxThreadBackend;
    using MutexBackend = QnxMutexBackend;
    using CondvarBackend = QnxCondvarBackend;
    using SemaphoreBackend = QnxSemaphoreBackend;
    using TimerBackend = QnxTimerBackend;
    using ChannelBackend = QnxChannelBackend;
};

template<typename Platform>
class Thread {
private:
    using Backend = typename PlatformTraits<Platform>::ThreadBackend;

public:
    using NativeHandle = typename Backend::NativeHandle;

    Thread() noexcept = default;
    ~Thread() noexcept = default;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&&) noexcept = default;
    Thread& operator=(Thread&&) noexcept = default;

    template<typename Function>
    static Thread create(Function&& function) {
        return Thread(
            Backend::create(
                typename Backend::Entry(
                    std::forward<Function>(function))));
    }

    bool joinable() const noexcept {
        return backend_.joinable();
    }

    void join() {
        backend_.join();
    }

    void detach() {
        backend_.detach();
    }

    bool apply(const eventstream::rt::RtPolicy& policy) {
        return backend_.apply(policy);
    }

    NativeHandle native_handle() noexcept {
        return backend_.native_handle();
    }

private:
    explicit Thread(Backend&& backend) noexcept
        : backend_(std::move(backend)) {}

    Backend backend_{};
};

template<typename Platform>
class Mutex {
private:
    using Backend = typename PlatformTraits<Platform>::MutexBackend;

public:
    using NativeHandle = typename Backend::NativeHandle;

    explicit Mutex(const MutexOptions& options = {}) 
        : backend_(options) {}

    ~Mutex() noexcept = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void lock() {
        backend_.lock();
    }

    bool try_lock() {
        return backend_.try_lock();
    }

    void unlock() {
        backend_.unlock();
    }

    NativeHandle native_handle() noexcept {
        return backend_.native_handle();
    }

private:
    Backend& backend() noexcept {
        return backend_;
    }

    template<typename>
    friend class Condvar;

    Backend backend_;
};

template<typename Platform>
class Condvar {
private:
    using Backend = typename PlatformTraits<Platform>::CondvarBackend;

public:
    using NativeHandle = typename Backend::NativeHandle;

    Condvar() = default;
    ~Condvar() noexcept = default;

    Condvar(const Condvar&) = delete;
    Condvar& operator=(const Condvar&) = delete;
    Condvar(Condvar&&) = delete;
    Condvar& operator=(Condvar&&) = delete;

    void wait(std::unique_lock<Mutex<Platform>>& lock) {
        backend_.wait(lock.mutex()->backend());
    }

    template<typename Predicate>
    void wait(
        std::unique_lock<Mutex<Platform>>& lock,
        Predicate predicate) {
        while (!predicate()) {
            wait(lock);
        }
    }

    bool wait_for(
        std::unique_lock<Mutex<Platform>>& lock,
        std::chrono::nanoseconds timeout) {
        return backend_.wait_for(
            lock.mutex()->backend(), timeout);
    }

    void notify_one() noexcept {
        backend_.notify_one();
    }

    void notify_all() noexcept {
        backend_.notify_all();
    }

    NativeHandle native_handle() noexcept {
        return backend_.native_handle();
    }

private:
    Backend backend_;
};

template<typename Platform>
class Semaphore {
private:
    using Backend = typename PlatformTraits<Platform>::SemaphoreBackend;

public:
    using NativeHandle = typename Backend::NativeHandle;

    explicit Semaphore(unsigned initialCount = 0)
        : backend_(initialCount) {}

    ~Semaphore() noexcept = default;

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;

    void wait() {
        backend_.wait();
    }

    bool try_wait() {
        return backend_.try_wait();
    }

    bool wait_for(std::chrono::nanoseconds timeout) {
        return backend_.wait_for(timeout);
    }

    void post() {
        backend_.post();
    }

    unsigned value() const {
        return backend_.value();
    }

    NativeHandle native_handle() noexcept {
        return backend_.native_handle();
    }

private:
    Backend backend_;
};

template<typename Platform>
class Timer {
private:
    using Backend = typename PlatformTraits<Platform>::TimerBackend;

public:
    using NativeHandle = typename Backend::NativeHandle;

    Timer() = default;
    ~Timer() noexcept = default;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    Timer(Timer&&) noexcept = default;
    Timer& operator=(Timer&&) noexcept = default;

    Status arm(
        std::chrono::nanoseconds initial,
        std::chrono::nanoseconds period) {
        return backend_.arm(initial, period);
    }

    Status disarm() noexcept {
        return backend_.disarm();
    }

    std::uint64_t wait() {
        return backend_.wait();
    }

    NativeHandle native_handle() noexcept {
        return backend_.native_handle();
    }

private:
    Backend backend_;
};

template<typename Platform>
class Channel {
private:
    using Backend = typename PlatformTraits<Platform>::ChannelBackend;

public:
    using NativeHandle = typename Backend::NativeHandle;

    explicit Channel(const ChannelOptions& options = {})
        : backend_(options) {}

    ~Channel() noexcept = default;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    Channel(Channel&&) noexcept = default;
    Channel& operator=(Channel&&) noexcept = default;

    Status send(const ByteView& message) {
        return backend_.send(message);
    }

    ReceiveResult receive(const MutableByteView& buffer) {
        return backend_.receive(buffer);
    }

    Status close() noexcept {
        return backend_.close();
    }

    NativeHandle native_handle() noexcept {
        return backend_.native_handle();
    }

private:
    Backend backend_;
};

using CurrentThread = Thread<CurrentPlatform>;
using CurrentMutex = Mutex<CurrentPlatform>;
using CurrentCondvar = Condvar<CurrentPlatform>;
using CurrentSemaphore = Semaphore<CurrentPlatform>;
using CurrentTimer = Timer<CurrentPlatform>;
using CurrentChannel = Channel<CurrentPlatform>;

using thread = CurrentThread;
using mutex = CurrentMutex;
using condvar = CurrentCondvar;
using semaphore = CurrentSemaphore;
using timer = CurrentTimer;
using channel = CurrentChannel;

} // namespace eventstream::platform
