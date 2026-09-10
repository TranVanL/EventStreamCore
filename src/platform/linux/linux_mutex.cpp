#include <eventstream/platform/linux/linux_mutex.hpp>

#include <cerrno>
#include <system_error>

namespace eventstream::platform {
namespace {

[[noreturn]] void throwMutexError(
    int error,
    const char* operation) {
    throw std::system_error(
        error,
        std::generic_category(),
        operation);
}

void makeConsistent(pthread_mutex_t* mutex) {
    const int error = pthread_mutex_consistent(mutex);
    if (error != 0) {
        throwMutexError(error, "pthread_mutex_consistent");
    }
}

void handleLockResult(
    pthread_mutex_t* mutex,
    int error,
    const char* operation) {
    if (error == 0) {
        return;
    }

    if (error == EOWNERDEAD) {
        makeConsistent(mutex);
        return;
    }

    throwMutexError(error, operation);
}

} // namespace

LinuxMutexBackend::LinuxMutexBackend(
    const MutexOptions& options) {
    pthread_mutexattr_t attributes{};
    int error = pthread_mutexattr_init(&attributes);
    if (error != 0) {
        throwMutexError(error, "pthread_mutexattr_init");
    }

    if (options.priorityInheritance) {
        error = pthread_mutexattr_setprotocol(
            &attributes,
            PTHREAD_PRIO_INHERIT);
    }

    if (error == 0 && options.robust) {
        error = pthread_mutexattr_setrobust(
            &attributes,
            PTHREAD_MUTEX_ROBUST);
    }

    if (error == 0) {
        error = pthread_mutex_init(&mutex_, &attributes);
    }

    const int destroyError = pthread_mutexattr_destroy(&attributes);
    if (error != 0) {
        throwMutexError(error, "pthread_mutex initialization");
    }
    if (destroyError != 0) {
        pthread_mutex_destroy(&mutex_);
        throwMutexError(
            destroyError,
            "pthread_mutexattr_destroy");
    }

    initialized_ = true;
}

LinuxMutexBackend::~LinuxMutexBackend() noexcept {
    if (initialized_) {
        (void)pthread_mutex_destroy(&mutex_);
    }
}

void LinuxMutexBackend::lock() {
    handleLockResult(
        &mutex_,
        pthread_mutex_lock(&mutex_),
        "pthread_mutex_lock");
}

bool LinuxMutexBackend::try_lock() {
    const int error = pthread_mutex_trylock(&mutex_);
    if (error == 0) {
        return true;
    }
    if (error == EBUSY) {
        return false;
    }

    handleLockResult(
        &mutex_,
        error,
        "pthread_mutex_trylock");
    return true;
}

void LinuxMutexBackend::unlock() {
    const int error = pthread_mutex_unlock(&mutex_);
    if (error != 0) {
        throwMutexError(error, "pthread_mutex_unlock");
    }
}

LinuxMutexBackend::NativeHandle
LinuxMutexBackend::native_handle() noexcept {
    return &mutex_;
}

} // namespace eventstream::platform