#include <eventstream/platform/qnx/qnx_mutex.hpp>

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
#if defined(PTHREAD_MUTEX_ROBUST)
    const int error = pthread_mutex_consistent(mutex);
    if (error != 0) {
        throwMutexError(error, "pthread_mutex_consistent");
    }
#else
    static_cast<void>(mutex);
    throwMutexError(ENOTSUP, "robust mutex is not supported");
#endif
}

void handleLockResult(
    pthread_mutex_t* mutex,
    int error,
    const char* operation) {
    if (error == 0) {
        return;
    }

#if defined(EOWNERDEAD)
    if (error == EOWNERDEAD) {
        makeConsistent(mutex);
        return;
    }
#endif

    throwMutexError(error, operation);
}

} // namespace

QnxMutexBackend::QnxMutexBackend(const MutexOptions& options) {
    pthread_mutexattr_t attributes{};
    int error = pthread_mutexattr_init(&attributes);
    if (error != 0) {
        throwMutexError(error, "pthread_mutexattr_init");
    }

    if (options.priorityInheritance) {
#if defined(PTHREAD_PRIO_INHERIT)
        error = pthread_mutexattr_setprotocol(
            &attributes,
            PTHREAD_PRIO_INHERIT);
#else
        error = ENOTSUP;
#endif
        if (error != 0) {
            (void)pthread_mutexattr_destroy(&attributes);
            throwMutexError(error, "pthread_mutexattr_setprotocol");
        }
    }

    if (options.robust) {
#if defined(PTHREAD_MUTEX_ROBUST)
        error = pthread_mutexattr_setrobust(
            &attributes,
            PTHREAD_MUTEX_ROBUST);
#else
        error = ENOTSUP;
#endif
        if (error != 0) {
            (void)pthread_mutexattr_destroy(&attributes);
            throwMutexError(error, "pthread_mutexattr_setrobust");
        }
    }

    error = pthread_mutex_init(&mutex_, &attributes);
    if (error != 0) {
        (void)pthread_mutexattr_destroy(&attributes);
        throwMutexError(error, "pthread_mutex_init");
    }

    const int destroyError = pthread_mutexattr_destroy(&attributes);
    if (destroyError != 0) {
        (void)pthread_mutex_destroy(&mutex_);
        throwMutexError(destroyError, "pthread_mutexattr_destroy");
    }
    initialized_ = true;
}

QnxMutexBackend::~QnxMutexBackend() noexcept {
    if (initialized_) {
        (void)pthread_mutex_destroy(&mutex_);
        initialized_ = false;
    }
}

void QnxMutexBackend::lock() {
    const int error = pthread_mutex_lock(&mutex_);
    handleLockResult(&mutex_, error, "pthread_mutex_lock");
}

void QnxMutexBackend::unlock() {
    const int error = pthread_mutex_unlock(&mutex_);
    if (error != 0) {
        throwMutexError(error, "pthread_mutex_unlock");
    }
}

bool QnxMutexBackend::try_lock() {
    const int error = pthread_mutex_trylock(&mutex_);
    if (error == 0) {
        return true;
    }
    if (error == EBUSY) {
        return false;
    }
    handleLockResult(&mutex_, error, "pthread_mutex_trylock");
    return false;
}

QnxMutexBackend::NativeHandle QnxMutexBackend::native_handle() noexcept {
    return &mutex_;
}

} // namespace eventstream::platform
