#include <eventstream/platform/qnx/qnx_thread.hpp>

#include <cerrno>
#include <exception>
#include <sched.h>
#include <stdexcept>
#include <system_error>

namespace eventstream::platform {

namespace {

int nativePolicy(const eventstream::rt::SchedPolicy policy) noexcept {
    switch (policy) {
        case eventstream::rt::SchedPolicy::Other:
            return SCHED_OTHER;
        case eventstream::rt::SchedPolicy::Fifo:
            return SCHED_FIFO;
        case eventstream::rt::SchedPolicy::RoundRobin:
            return SCHED_RR;
    }
    return -1;
}

bool validPriority(
    const eventstream::rt::SchedPolicy policy,
    int priority) noexcept {
    const int native = nativePolicy(policy);
    if (native < 0) {
        return false;
    }

    const int minimum = sched_get_priority_min(native);
    const int maximum = sched_get_priority_max(native);
    return minimum >= 0 && maximum >= 0 &&
        priority >= minimum && priority <= maximum;
}

bool applyScheduling(
    pthread_t thread,
    const eventstream::rt::RtPolicy& policy) noexcept {
    const int native = nativePolicy(policy.policy);
    if (!validPriority(policy.policy, policy.priority)) {
        return false;
    }

    sched_param parameters{};
    parameters.sched_priority = policy.priority;

    return pthread_setschedparam(thread, native, &parameters) == 0;
}

bool applyAffinity(
    pthread_t thread,
    const eventstream::rt::RtPolicy& policy) noexcept {
    if (policy.cpus.empty()) {
        return true;
    }

#if defined(CPU_ZERO) && defined(CPU_SET)
    cpu_set_t cpus;
    CPU_ZERO(&cpus);

    for (const int cpu : policy.cpus) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            return false;
        }

        CPU_SET(cpu, &cpus);
    }

    return pthread_setaffinity_np(
        thread,
        sizeof(cpus),
        &cpus) == 0;
#else
    static_cast<void>(thread);
    return false;
#endif
}

} // namespace

QnxThreadBackend::~QnxThreadBackend() noexcept {
    if (joinable_) {
        std::terminate();
    }
}

QnxThreadBackend::QnxThreadBackend(QnxThreadBackend&& other) noexcept {
    this->handle_ = other.handle_;
    this->joinable_ = other.joinable_;
    other.handle_ = {};
    other.joinable_ = false;
}

QnxThreadBackend& QnxThreadBackend::operator=(QnxThreadBackend&& other) noexcept {
    if (this != &other) {
        if (joinable_) {
            std::terminate();
        }
        this->handle_ = other.handle_;
        this->joinable_ = other.joinable_;
        other.handle_ = {};
        other.joinable_ = false;
    }
    return *this;
}

QnxThreadBackend QnxThreadBackend::create(Entry entry) {
    if (!entry) {
        throw std::invalid_argument(
            "QnxThreadBackend requires a callable entry");
    }
    auto* context = new StartContext{std::move(entry)};
    QnxThreadBackend result;

    const int error = pthread_create(
        &result.handle_,
        nullptr,
        &QnxThreadBackend::trampoline,
        context);

    if (error != 0) {
        delete context;
        throw std::system_error(error, std::generic_category(), "Failed to create thread");
    }

    result.joinable_ = true;

    return result;
}
bool QnxThreadBackend::joinable() const noexcept {
    return joinable_;
}

void QnxThreadBackend::join() {
    if (!joinable_) {
        throw std::logic_error(
            "join on non-joinable QNX thread"); 
    }

    const int error = pthread_join(handle_, nullptr);

    if (error != 0) {
        throw std::system_error(error, std::generic_category(), "Failed to join thread");
    }
    joinable_ = false;
    handle_ = pthread_t{};
}

void QnxThreadBackend::detach() {
    if (!joinable_) {
        throw std::logic_error(
            "detach on non-joinable QNX thread"); 
    }
    const int error = pthread_detach(handle_);
    if (error != 0) {
        throw std::system_error(error, std::generic_category(), "Failed to detach thread");
    }
    joinable_ = false;
    handle_ = pthread_t{};
}

bool QnxThreadBackend::apply(const eventstream::rt::RtPolicy& policy) {
    if (!joinable_) {
        return false;
    }
    return applyScheduling(handle_, policy) && applyAffinity(handle_, policy);
}
QnxThreadBackend::NativeHandle QnxThreadBackend::native_handle() noexcept{
    return handle_;
}

void* QnxThreadBackend::trampoline(void* argument) noexcept {
    auto* context = static_cast<StartContext*>(argument);

    try {
        context->entry();
    } catch (...) {
        delete context;
        std::terminate();
    }

    delete context;
    return nullptr;
}

} // namespace eventstream::platform