#pragma once

#include <chrono>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <system_error>

namespace eventstream::rt
{
    class RtMutex
    {
    private:
        pthread_mutex_t mutex_;
        bool is_initialized_{false};

    public:
        RtMutex();
        ~RtMutex() noexcept;

        RtMutex(const RtMutex &) = delete;
        RtMutex &operator=(const RtMutex &) = delete;
        RtMutex(RtMutex &&) = delete;
        RtMutex &operator=(RtMutex &&) = delete;

        void lock();
        bool try_lock();
        bool try_lock_for(std::chrono::nanoseconds timeout);
        void unlock();
        pthread_mutex_t *native_handle() noexcept;
    };

    class RtLockGuard
    {
    private:
        RtMutex &mutex_;

    public:
        explicit RtLockGuard(RtMutex &mutex) : mutex_(mutex)
        {
            mutex_.lock();
        }

        ~RtLockGuard() noexcept
        {
            try
            {
                mutex_.unlock();
            }
            catch (...)
            {
                std::terminate();
            }
        }

        RtLockGuard(const RtLockGuard &) = delete;
        RtLockGuard &operator=(const RtLockGuard &) = delete;
        RtLockGuard(RtLockGuard &&) = delete;
        RtLockGuard &operator=(RtLockGuard &&) = delete;
    };

    class RtUniqueLock
    {
    private:
        RtMutex *mutex_{nullptr};
        bool owns_lock{false};

        void release() noexcept;

    public:
        explicit RtUniqueLock(RtMutex &mutex);

        RtUniqueLock(RtMutex &mutex, std::defer_lock_t) noexcept;

        ~RtUniqueLock() noexcept;

        RtUniqueLock(const RtUniqueLock &) = delete;
        RtUniqueLock &operator=(const RtUniqueLock &) = delete;

        RtUniqueLock(RtUniqueLock &&other) noexcept;
        RtUniqueLock &operator=(RtUniqueLock &&other) noexcept;

        void lock();
        bool try_lock();
        bool try_lock_for(std::chrono::nanoseconds timeout);
        void unlock();

        bool ownsLock() const noexcept;
        explicit operator bool() const noexcept;

        RtMutex *mutex() const noexcept;
    };
}