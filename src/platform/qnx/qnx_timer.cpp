#include <eventstream/platform/qnx/qnx_timer.hpp>

#include <cerrno>
#include <limits>
#include <system_error>

namespace eventstream::platform {
namespace {

Status invalidArgument() noexcept {
    return {ErrorCode::InvalidArgument, 0};
}

bool toTimespec(std::chrono::nanoseconds duration, timespec& result) noexcept {
    if (duration < std::chrono::nanoseconds::zero()) {
        return false;
    }

    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto remainder =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
    if (seconds.count() > std::numeric_limits<time_t>::max()) {
        return false;
    }

    result.tv_sec = static_cast<time_t>(seconds.count());
    result.tv_nsec = static_cast<long>(remainder.count());
    return true;
}

} // namespace

QnxTimerBackend::QnxTimerBackend() {
    state_ = new State{};

    int error = pthread_mutex_init(&state_->mutex, nullptr);
    if (error != 0) {
        delete state_;
        state_ = nullptr;
        throw std::system_error(
            error,
            std::generic_category(),
            "pthread_mutex_init");
    }

    error = pthread_cond_init(&state_->condition, nullptr);
    if (error != 0) {
        (void)pthread_mutex_destroy(&state_->mutex);
        delete state_;
        state_ = nullptr;
        throw std::system_error(
            error,
            std::generic_category(),
            "pthread_cond_init");
    }
    state_->initialized = true;

    sigevent event{};
    event.sigev_notify = SIGEV_THREAD;
    event.sigev_notify_function = &QnxTimerBackend::callback;
    event.sigev_value.sival_ptr = state_;
    if (timer_create(CLOCK_MONOTONIC, &event, &timer_) != 0) {
        const int nativeError = errno;
        destroyState(*state_);
        delete state_;
        state_ = nullptr;
        throw std::system_error(
            nativeError,
            std::generic_category(),
            "timer_create");
    }

    initialized_ = true;
}

QnxTimerBackend::~QnxTimerBackend() noexcept {
    reset();
}

void QnxTimerBackend::reset() noexcept {
    if (!initialized_) {
        return;
    }

    (void)disarm();
    (void)pthread_mutex_lock(&state_->mutex);
    state_->shuttingDown = true;
    (void)pthread_mutex_unlock(&state_->mutex);
    (void)timer_delete(timer_);

    (void)pthread_mutex_lock(&state_->mutex);
    while (state_->callbacks != 0) {
        (void)pthread_cond_wait(&state_->condition, &state_->mutex);
    }
    (void)pthread_mutex_unlock(&state_->mutex);

    destroyState(*state_);
    delete state_;
    state_ = nullptr;
    timer_ = timer_t{};
    initialized_ = false;
}

QnxTimerBackend::QnxTimerBackend(QnxTimerBackend&& other) noexcept
    : timer_(other.timer_),
      state_(other.state_),
      initialized_(other.initialized_) {
    other.timer_ = timer_t{};
    other.state_ = nullptr;
    other.initialized_ = false;
}

QnxTimerBackend& QnxTimerBackend::operator=(QnxTimerBackend&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();
    timer_ = other.timer_;
    state_ = other.state_;
    initialized_ = other.initialized_;
    other.timer_ = timer_t{};
    other.state_ = nullptr;
    other.initialized_ = false;
    return *this;
}

Status QnxTimerBackend::arm(
    std::chrono::nanoseconds initial,
    std::chrono::nanoseconds period) {
    if (!initialized_) {
        return {ErrorCode::Closed, EBADF};
    }

    itimerspec specification{};
    if (!toTimespec(initial, specification.it_value) ||
        !toTimespec(period, specification.it_interval)) {
        return invalidArgument();
    }
    if (timer_settime(timer_, 0, &specification, nullptr) != 0) {
        return {ErrorCode::NativeError, errno};
    }
    return {};
}

Status QnxTimerBackend::disarm() noexcept {
    if (!initialized_) {
        return {ErrorCode::Closed, EBADF};
    }

    const itimerspec specification{};
    if (timer_settime(timer_, 0, &specification, nullptr) != 0) {
        return {ErrorCode::NativeError, errno};
    }
    return {};
}

std::uint64_t QnxTimerBackend::wait() {
    if (!initialized_) {
        throw std::system_error(
            EBADF,
            std::generic_category(),
            "timer wait on closed timer");
    }

    const int lockError = pthread_mutex_lock(&state_->mutex);
    if (lockError != 0) {
        throw std::system_error(
            lockError,
            std::generic_category(),
            "pthread_mutex_lock");
    }

    while (state_->expirations == 0) {
        const int error = pthread_cond_wait(
            &state_->condition,
            &state_->mutex);
        if (error != 0) {
            (void)pthread_mutex_unlock(&state_->mutex);
            throw std::system_error(
                error,
                std::generic_category(),
                "pthread_cond_wait");
        }
    }

    const std::uint64_t expirations = state_->expirations;
    state_->expirations = 0;
    (void)pthread_mutex_unlock(&state_->mutex);
    return expirations;
}

QnxTimerBackend::NativeHandle QnxTimerBackend::native_handle() noexcept {
    return timer_;
}

void QnxTimerBackend::callback(union sigval value) noexcept {
    auto* state = static_cast<State*>(value.sival_ptr);
    if (pthread_mutex_lock(&state->mutex) != 0) {
        return;
    }
    if (state->shuttingDown) {
        (void)pthread_mutex_unlock(&state->mutex);
        return;
    }

    ++state->callbacks;
    ++state->expirations;
    (void)pthread_cond_signal(&state->condition);
    --state->callbacks;
    if (state->callbacks == 0) {
        (void)pthread_cond_broadcast(&state->condition);
    }
    (void)pthread_mutex_unlock(&state->mutex);
}

void QnxTimerBackend::destroyState(State& state) noexcept {
    if (!state.initialized) {
        return;
    }
    (void)pthread_cond_destroy(&state.condition);
    (void)pthread_mutex_destroy(&state.mutex);
    state.initialized = false;
}

} // namespace eventstream::platform
