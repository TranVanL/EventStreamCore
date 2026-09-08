#include <eventstream/rt/rt_mutex.hpp>
#include <eventstream/rt/rt_thread.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct ScenarioResult {
    bool allPoliciesApplied{true};
    bool highAcquired{false};
    std::chrono::nanoseconds highWait{};
};

bool waitFor(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;

    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }

    return true;
}

int firstAllowedCpu() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (pthread_getaffinity_np(
            pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        return -1;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &cpuset)) {
            return cpu;
        }
    }

    return -1;
}

eventstream::rt::RtPolicy fifoPolicy(int priority, int cpu) {
    return eventstream::rt::RtPolicyBuilder()
        .fifo()
        .priority(priority)
        .cpus({cpu})
        .build();
}

ScenarioResult runScenario(bool usePiMutex) {
    const int cpu = firstAllowedCpu();
    if (cpu < 0) {
        return {false, false, 0ns};
    }

    eventstream::rt::RtMutex mutex(
        usePiMutex ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE,
        true);

    std::atomic<bool> lowReady{false};
    std::atomic<bool> mediumReady{false};
    std::atomic<bool> highReady{false};
    std::atomic<bool> lowLocked{false};
    std::atomic<bool> begin{false};
    std::atomic<bool> highWaiting{false};
    std::atomic<bool> highAcquired{false};
    std::atomic<bool> stopMedium{false};
    std::atomic<bool> policiesApplied{true};

    std::chrono::steady_clock::time_point highStart{};
    std::chrono::steady_clock::time_point highEnd{};

    auto applyPolicy = [&](int priority) {
        const bool applied =
            eventstream::rt::RtThread::applyToSelf(
                fifoPolicy(priority, cpu));

        if (!applied) {
            policiesApplied.store(false, std::memory_order_release);
        }
    };

    std::thread low([&] {
        applyPolicy(10);
        lowReady.store(true, std::memory_order_release);

        mutex.lock();
        lowLocked.store(true, std::memory_order_release);

        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        while (!highWaiting.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const auto deadline =
            std::chrono::steady_clock::now() + 100ms;
        while (std::chrono::steady_clock::now() < deadline) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }

        mutex.unlock();
    });

    std::thread medium([&] {
        applyPolicy(50);
        mediumReady.store(true, std::memory_order_release);

        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const auto deadline =
            std::chrono::steady_clock::now() + 200ms;
        while (!stopMedium.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    });

    std::thread high([&] {
        applyPolicy(90);
        highReady.store(true, std::memory_order_release);

        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        highStart = std::chrono::steady_clock::now();
        highWaiting.store(true, std::memory_order_release);
        mutex.lock();
        highEnd = std::chrono::steady_clock::now();
        highAcquired.store(true, std::memory_order_release);
        mutex.unlock();
    });

    waitFor(lowReady);
    waitFor(mediumReady);
    waitFor(highReady);
    waitFor(lowLocked);

    begin.store(true, std::memory_order_release);

    const bool acquired = waitFor(highAcquired);
    stopMedium.store(true, std::memory_order_release);

    low.join();
    medium.join();
    high.join();

    ScenarioResult result;
    result.allPoliciesApplied =
        policiesApplied.load(std::memory_order_acquire);
    result.highAcquired = acquired;

    if (acquired) {
        result.highWait = std::chrono::duration_cast<
            std::chrono::nanoseconds>(highEnd - highStart);
    }

    return result;
}

void printResult(const char* label, const ScenarioResult& result) {
    std::cout << label << ": ";

    if (!result.highAcquired) {
        std::cout << "high-priority thread did not acquire mutex\n";
        return;
    }

    std::cout << "high wait = "
              << result.highWait.count() / 1'000'000.0
              << " ms\n";
}

} // namespace

int main() {
    const auto withoutPi = runScenario(false);
    const auto withPi = runScenario(true);

    printResult("PTHREAD_PRIO_NONE", withoutPi);
    printResult("PTHREAD_PRIO_INHERIT", withPi);

    if (!withoutPi.allPoliciesApplied || !withPi.allPoliciesApplied) {
        std::cout
            << "warning: SCHED_FIFO or affinity was unavailable; "
               "results are best-effort and not a scheduler benchmark\n";
    }

    return withoutPi.highAcquired && withPi.highAcquired ? 0 : 1;
}
