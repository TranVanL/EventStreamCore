#include <eventstream/rt/rt_mutex.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace eventstream::rt
{

    [[noreturn]] void throwMutexError(int errorCode, const char *operation)
    {
        throw std::system_error(
            errorCode,
            std::generic_category(),
            operation);
    }

    timespec makeRealtimeDeadline(std::chrono::nanoseconds timeout)
    {
        timespec now{};

        const int result = clock_gettime(CLOCK_REALTIME, &now);
        if (result != 0)
        {
            throw std::system_error(
                errno,
                std::generic_category(),
                "clock_gettime(CLOCK_REALTIME)");
        }

        const auto timeoutSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(timeout);

        const auto timeoutNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                timeout - timeoutSeconds);

        timespec deadline{};
        deadline.tv_sec = now.tv_sec + timeoutSeconds.count();
        deadline.tv_nsec =
            now.tv_nsec + timeoutNanoseconds.count();

        if (deadline.tv_nsec >= 1'000'000'000L)
        {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1'000'000'000L;
        }

        return deadline;
    }

    RtMutex::RtMutex()
    {
        pthread_mutexattr_t attr{};
        int result = pthread_mutexattr_init(&attr);
        if (result != 0)
        {
            spdlog::error("Failed to initialize mutex attributes: {}", std::strerror(result));
            throwMutexError(result, "pthread_mutexattr_init");
        }
        result = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        if (result != 0)
        {
            spdlog::error("Failed to set mutex protocol: {}", std::strerror(result));
            pthread_mutexattr_destroy(&attr);
            throwMutexError(result, "pthread_mutexattr_setprotocol");
        }

        result = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (result != 0)
        {
            spdlog::error("Failed to set mutex robust attribute: {}", std::strerror(result));
            pthread_mutexattr_destroy(&attr);
            throwMutexError(result, "pthread_mutexattr_setrobust");
        }

        result = pthread_mutex_init(&mutex_, &attr);

        const int destroyResult = pthread_mutexattr_destroy(&attr);
        if (result != 0)
        {
            spdlog::error("Failed to initialize mutex: {}", std::strerror(result));
            throwMutexError(result, "pthread_mutex_init");
        }
        if (destroyResult != 0)
        {
            spdlog::error("Failed to destroy mutex attributes: {}", std::strerror(destroyResult));
            pthread_mutex_destroy(&mutex_);
            throwMutexError(destroyResult, "pthread_mutexattr_destroy");
        }
        is_initialized_ = true;
    }

    RtMutex::~RtMutex() noexcept
    {
        if (!is_initialized_)
        {
            return;
        }
        const int result = pthread_mutex_destroy(&mutex_);
        if (result != 0)
        {
            spdlog::error("Failed to destroy mutex: {}", std::strerror(result));
        }
    }

    void RtMutex::lock()
    {
        const int result = pthread_mutex_lock(&mutex_);
        if (result == 0)
        {
            return;
        }
        else if (result == EOWNERDEAD)
        {
            spdlog::warn("Mutex owner died, marking mutex as consistent");
            const int consistentResult = pthread_mutex_consistent(&mutex_);
            if (consistentResult != 0)
            {
                spdlog::error("Failed to mark mutex as consistent: {}", std::strerror(consistentResult));
                throwMutexError(consistentResult, "pthread_mutex_consistent");
            }
            return;
        }
        else if (result == ENOTRECOVERABLE)
        {
            spdlog::error("Mutex is not recoverable");
            throwMutexError(result, "pthread_mutex_lock");
        }
        throwMutexError(result, "pthread_mutex_lock");
    }

    bool RtMutex::try_lock()
    {
        const int result = pthread_mutex_trylock(&mutex_);
        if (result == 0)
        {
            return true;
        }
        if (result == EBUSY)
        {
            return false;
        }
        else if (result == EOWNERDEAD)
        {
            spdlog::warn("Mutex owner died, marking mutex as consistent");
            const int consistentResult = pthread_mutex_consistent(&mutex_);
            if (consistentResult != 0)
            {
                spdlog::error("Failed to mark mutex as consistent: {}", std::strerror(consistentResult));
                throwMutexError(consistentResult, "pthread_mutex_consistent");
            }
            return true;
        }
        else if (result == ENOTRECOVERABLE)
        {
            spdlog::error("Mutex is not recoverable");
            throwMutexError(result, "pthread_mutex_trylock");
        }
        throwMutexError(result, "pthread_mutex_trylock");
    }

    bool RtMutex::try_lock_for(std::chrono::nanoseconds timeout)
    {
        if (timeout < std::chrono::nanoseconds::zero())
        {
            throw std::invalid_argument("RtMutex::tryLockFor timeout cannot be negative");
        }

        const timespec deadline = makeRealtimeDeadline(timeout);

        const int result = pthread_mutex_timedlock(&mutex_, &deadline);
        if (result == 0)
        {
            return true;
        }
        if (result == ETIMEDOUT)
        {
            return false;
        }
        else if (result == EOWNERDEAD)
        {
            spdlog::warn("Mutex owner died, marking mutex as consistent");
            const int consistentResult = pthread_mutex_consistent(&mutex_);
            if (consistentResult != 0)
            {
                spdlog::error("Failed to mark mutex as consistent: {}", std::strerror(consistentResult));
                throwMutexError(consistentResult, "pthread_mutex_consistent");
            }
            return true;
        }
        else if (result == ENOTRECOVERABLE)
        {
            spdlog::error("Mutex is not recoverable");
            throwMutexError(result, "pthread_mutex_timedlock");
        }
        throwMutexError(result, "pthread_mutex_timedlock");
        
    }

    void RtMutex::unlock()
    {
        const int result = pthread_mutex_unlock(&mutex_);
        if (result != 0)
        {
            throwMutexError(result, "pthread_mutex_unlock");
        }
    }

    pthread_mutex_t *RtMutex::native_handle() noexcept
    {
        return &mutex_;
    }


    RtUniqueLock::RtUniqueLock(RtMutex &mutex) : mutex_(&mutex), owns_lock(false)
    {
        lock();
    }

    RtUniqueLock::RtUniqueLock(RtMutex &mutex, std::defer_lock_t) noexcept : mutex_(&mutex), owns_lock(false)
    {
    }

    RtUniqueLock::~RtUniqueLock() noexcept
    {
       release();
    }

    RtUniqueLock::RtUniqueLock(RtUniqueLock &&other) noexcept : mutex_(other.mutex_), owns_lock(other.owns_lock)
    {
        other.owns_lock = false;
        other.mutex_ = nullptr;
    }

    RtUniqueLock& RtUniqueLock::operator=(RtUniqueLock &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        release();
        mutex_ = other.mutex_;
        owns_lock = other.owns_lock;
        other.owns_lock = false;
        other.mutex_ = nullptr;
        return *this;
    }

    void RtUniqueLock::lock()
    {
        if (mutex_ == nullptr) {
            throw std::runtime_error("RtUniqueLock: mutex is null");
        }
        if (owns_lock) {
            throw std::runtime_error("RtUniqueLock: already owns the lock");
        }
        mutex_->lock();
        owns_lock = true;
    }

    bool RtUniqueLock::try_lock()
    {
        if (mutex_ == nullptr) {
            throw std::runtime_error("RtUniqueLock: mutex is null");
        }
        if (owns_lock) {
            throw std::runtime_error("RtUniqueLock: already owns the lock");
        }
        owns_lock = mutex_->try_lock();
        return owns_lock;
    }

    bool RtUniqueLock::try_lock_for(std::chrono::nanoseconds timeout)
    {
        if (mutex_ == nullptr) {
            throw std::runtime_error("RtUniqueLock: mutex is null");
        }
        if (owns_lock) {
            throw std::runtime_error("RtUniqueLock: already owns the lock");
        }
        owns_lock = mutex_->try_lock_for(timeout);
        return owns_lock;
    }

    void RtUniqueLock::unlock()
    {
        if (mutex_ == nullptr) {
            throw std::runtime_error("RtUniqueLock: mutex is null");
        }
        if (!owns_lock) {
            throw std::runtime_error("RtUniqueLock: does not own the lock");
        }
        mutex_->unlock();
        owns_lock = false;
    }

    void RtUniqueLock::release() noexcept
    {
        if (mutex_ != nullptr && owns_lock)
        {
            try
            {
                mutex_->unlock();
            }
            catch (...)
            {
                std::terminate();
            }
            owns_lock = false;
        }
        mutex_ = nullptr;
    }

}