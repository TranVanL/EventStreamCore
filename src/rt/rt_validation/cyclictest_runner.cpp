#include <eventstream/rt/rt_thread.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <time.h>

namespace {

constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr uint64_t kPeriodNs = 1'000'000ULL; // 1 ms
constexpr unsigned kDurationSeconds = 30;
constexpr int kRealtimePriority = 90;
constexpr int kCpu = 3; 

struct LatencySample {
    uint64_t expected_ns;
    uint64_t actual_ns;
    uint64_t jitter_ns;
};

uint64_t monotonicNowNs() {
    timespec ts{};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        throw std::runtime_error(
            std::string("clock_gettime failed: ") + std::strerror(errno));
    }

    return static_cast<uint64_t>(ts.tv_sec) *
               kNanosecondsPerSecond +
           static_cast<uint64_t>(ts.tv_nsec);
}

timespec nsToTimespec(uint64_t ns) {
    timespec ts{};

    ts.tv_sec = static_cast<time_t>(ns / kNanosecondsPerSecond);
    ts.tv_nsec = static_cast<long>(ns % kNanosecondsPerSecond);

    return ts;
}

bool sleepUntil(uint64_t deadline_ns) {
    const timespec deadline = nsToTimespec(deadline_ns);

    int result = 0;

    do {
        result = clock_nanosleep(
            CLOCK_MONOTONIC,
            TIMER_ABSTIME,
            &deadline,
            nullptr);
    } while (result == EINTR);

    if (result != 0) {
        std::cerr << "clock_nanosleep failed: "
                  << std::strerror(result) << '\n';
        return false;
    }

    return true;
}

uint64_t percentile(const std::vector<uint64_t>& sorted_values, double percentage) {
    if (sorted_values.empty() || !(percentage > 0.0)) {
        return 0;
    }

    const double rank = (percentage / 100.0) * static_cast<double>(sorted_values.size());
    const std::size_t index = static_cast<std::size_t>(std::ceil(rank)) - 1;
    return sorted_values[std::min(index, sorted_values.size() - 1)];
}

} // namespace

int main() {
    const auto policy = eventstream::rt::RtPolicyBuilder()
                            .fifo()
                            .priority(kRealtimePriority)
                            .cpus({kCpu})
                            .build();

    const bool realtime_applied =
        eventstream::rt::RtThread::applyToSelf(policy);
    std::cout << "Requested policy: "
              << eventstream::rt::RtThread::describe(policy)
              << '\n';

    std::cout << "Realtime policy applied: "
              << (realtime_applied ? "yes" : "no")
              << " (best-effort mode is allowed)\n";
    const uint64_t iterations =
        (static_cast<uint64_t>(kDurationSeconds) *
         kNanosecondsPerSecond) /
        kPeriodNs;
    std::vector<LatencySample> samples;
    samples.reserve(iterations);

    const uint64_t start_ns = monotonicNowNs();
    uint64_t expected_ns = start_ns + kPeriodNs;

    for (uint64_t i = 0; i < iterations; ++i) {
        if (!sleepUntil(expected_ns)) {
            return 1;
        }
        const uint64_t actual_ns = monotonicNowNs();

        const uint64_t jitter_ns =
            actual_ns >= expected_ns
                ? actual_ns - expected_ns
                : 0;

        samples.push_back({
            expected_ns,
            actual_ns,
            jitter_ns
        });

        expected_ns += kPeriodNs;
    }

    std::vector<uint64_t> jitter_values;
    jitter_values.reserve(samples.size());

    for (const auto& sample : samples) {
        jitter_values.push_back(sample.jitter_ns);
    }

    std::sort(jitter_values.begin(), jitter_values.end());
    const uint64_t p50_ns = percentile(jitter_values, 50.0);
    const uint64_t p95_ns = percentile(jitter_values, 95.0);
    const uint64_t p99_ns = percentile(jitter_values, 99.0);
    const uint64_t max_ns = jitter_values.empty() ? 0 : jitter_values.back();

    const auto toMicroseconds = [](uint64_t ns) {
        return static_cast<double>(ns) / 1'000.0;
    };

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\nCyclictest result\n";
    std::cout << "-----------------\n";
    std::cout << "Samples: " << samples.size() << '\n';
    std::cout << "Period:  " << kPeriodNs / 1'000'000.0
              << " ms\n";
    std::cout << "p50:     " << toMicroseconds(p50_ns)
              << " us\n";
    std::cout << "p95:     " << toMicroseconds(p95_ns)
              << " us\n";
    std::cout << "p99:     " << toMicroseconds(p99_ns)
              << " us\n";
    std::cout << "max:     " << toMicroseconds(max_ns)
              << " us\n";

    return 0;
}