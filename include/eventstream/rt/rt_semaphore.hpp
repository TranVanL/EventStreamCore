#pragma once

#include <semaphore.h>

#include <string>

namespace eventstream::rt {

class RtSemaphore {
public:
    // Unnamed semaphore, use for synchronization between threads in the same process.
    explicit RtSemaphore(unsigned initialValue);

    // Named semaphore.
    //
    // unlinkOnDestroy should only be enabled if this object is the owner of the named
    // semaphore. If multiple processes are using it, unlink manually in the common
    // lifecycle management location.
    RtSemaphore(
        const char* name,
        unsigned initialValue,
        bool unlinkOnDestroy = false);

    ~RtSemaphore() noexcept;

    void wait();
    bool tryWait();
    void post();

    // Only for metrics/debug. Do not use for synchronization decisions.
    int getValue() const;

    // Remove the named semaphore from the namespace.
    // Open handles can still be used until they are closed.
    void unlink();

    RtSemaphore(const RtSemaphore&) = delete;
    RtSemaphore& operator=(const RtSemaphore&) = delete;
    RtSemaphore(RtSemaphore&&) = delete;
    RtSemaphore& operator=(RtSemaphore&&) = delete;

private:
    sem_t unnamedStorage_{};
    sem_t* handle_{nullptr};

    bool named_{false};
    bool unlinkOnDestroy_{false};
    std::string name_;
};

} // namespace eventstream::rt