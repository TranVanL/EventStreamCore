#include <eventstream/platform/linux/linux_thread.hpp>

#include <cerrno>
#include <cstring>
#include <exception>
#include <sched.h>
#include <stdexcept>
#include <system_error>

namespace eventstream::platform {
namespace {

int nativePolicy(eventstream::rt::SchedPolicy policy) noexcept {
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
    int priority,
    eventstream::rt::SchedPolicy policy) noexcept {
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
    if (!validPriority(policy.priority, policy.policy)) {
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
}

} // namespace

LinuxThreadBackend::~LinuxThreadBackend() noexcept {
    if (joinable_) {
        std::terminate();
    }
}

LinuxThreadBackend::LinuxThreadBackend(
    LinuxThreadBackend&& other) noexcept
    : handle_(other.handle_),
      joinable_(other.joinable_) {
    other.handle_ = pthread_t{};
    other.joinable_ = false;
}

LinuxThreadBackend& LinuxThreadBackend::operator=(
    LinuxThreadBackend&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (joinable_) {
        std::terminate();
    }

    handle_ = other.handle_;
    joinable_ = other.joinable_;
    other.handle_ = pthread_t{};
    other.joinable_ = false;
    return *this;
}

LinuxThreadBackend LinuxThreadBackend::create(Entry entry) {
    if (!entry) {
        throw std::invalid_argument(
            "LinuxThreadBackend requires a callable entry");
    }

    StartContext* context = new StartContext{std::move(entry)};
    LinuxThreadBackend result;

    const int error = pthread_create(
        &result.handle_,
        nullptr,
        &LinuxThreadBackend::trampoline,
        context);

    if (error != 0) {
        delete context;
        throw std::system_error(
            error,
            std::generic_category(),
            "pthread_create");
    }

    result.joinable_ = true;
    return result;
}

bool LinuxThreadBackend::joinable() const noexcept {
    return joinable_;
}

void LinuxThreadBackend::join() {
    if (!joinable_) {
        throw std::logic_error(
            "LinuxThreadBackend::join on non-joinable thread");
    }

    const int error = pthread_join(handle_, nullptr);
    if (error != 0) {
        throw std::system_error(
            error,
            std::generic_category(),
            "pthread_join");
    }

    joinable_ = false;
    handle_ = pthread_t{};
}

void LinuxThreadBackend::detach() {
    if (!joinable_) {
        throw std::logic_error(
            "LinuxThreadBackend::detach on non-joinable thread");
    }

    const int error = pthread_detach(handle_);
    if (error != 0) {
        throw std::system_error(
            error,
            std::generic_category(),
            "pthread_detach");
    }

    joinable_ = false;
    handle_ = pthread_t{};
}

bool LinuxThreadBackend::apply(
    const eventstream::rt::RtPolicy& policy) {
    if (!joinable_) {
        return false;
    }

    const bool schedulingOk = applyScheduling(handle_, policy);
    const bool affinityOk = applyAffinity(handle_, policy);
    return schedulingOk && affinityOk;
}

LinuxThreadBackend::NativeHandle
LinuxThreadBackend::native_handle() noexcept {
    return handle_;
}

void* LinuxThreadBackend::trampoline(void* argument) noexcept {
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