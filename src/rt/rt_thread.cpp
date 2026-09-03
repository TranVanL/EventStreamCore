#include <eventstream/rt/rt_thread.hpp>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sstream>

#include <spdlog/spdlog.h>

namespace eventstream::rt
{
    namespace
    {
        int getNativePolicy(SchedPolicy policy) noexcept
        {
            switch (policy)
            {
            case SchedPolicy::Other:
                return SCHED_OTHER;
            case SchedPolicy::Fifo:
                return SCHED_FIFO;
            case SchedPolicy::RoundRobin:
                return SCHED_RR;
            default:
                return -1;
            }
        }

        bool isValidPriority(int priority, SchedPolicy policy) noexcept
        {
            int nativePolicy = getNativePolicy(policy);
            if (nativePolicy == -1)
            {
                return false;
            }
            int minPriority = sched_get_priority_min(nativePolicy);
            int maxPriority = sched_get_priority_max(nativePolicy);

            if (minPriority == -1 || maxPriority == -1)
            {
                return false;
            }

            return priority >= minPriority && priority <= maxPriority;
        }

        bool applyScheduling(pthread_t thread, const RtPolicy &policy)
        {
            int nativePolicy = getNativePolicy(policy.policy);
            if (nativePolicy == -1)
            {
                spdlog::warn("Unsupported scheduling policy");
                return false;
            }
            if (!isValidPriority(policy.priority, policy.policy))
            {
                spdlog::warn("Invalid priority {} for {}", policy.priority,
                             toString(policy.policy));
                return false;
            }

            sched_param schedParam{};
            schedParam.sched_priority = policy.priority;

            const int result = pthread_setschedparam(thread, nativePolicy, &schedParam);
            if (result != 0)
            {
                spdlog::warn("Failed to set {} priority {}: {}",
                             toString(policy.policy), policy.priority,
                             std::strerror(result));
                if (result == EPERM)
                {
                    spdlog::warn("Realtime scheduling permission denied; thread continues with its current policy");
                }
                return false;
            }
            return true;
        }

        bool applyCpuAffinity(pthread_t thread, const RtPolicy &policy)
        {
            if (policy.cpus.empty())
            {
                return true; // No CPU affinity specified, nothing to apply
            }
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            for (auto cpu : policy.cpus)
            {
                if (cpu < 0 || cpu >= CPU_SETSIZE)
                {
                    spdlog::error("Invalid CPU index {}. Valid range is [0, {}]", cpu, CPU_SETSIZE - 1);
                    return false;
                }
                CPU_SET(cpu, &cpuset);
            }
            const int result = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
            if (result != 0)
            {
                spdlog::warn("Failed to set CPU affinity: {}", std::strerror(result));
                return false;
            }
            return true;
        }

    }

    bool RtThread::apply(std::thread &thread, const RtPolicy &policy)
        {
            if (!thread.joinable())
            {
                spdlog::warn("Thread is not joinable; cannot apply RT policy");
                return false;
            }
            pthread_t nativeHandle = thread.native_handle();
            const bool schedulingOk = applyScheduling(nativeHandle, policy);
            const bool affinityOk = applyCpuAffinity(nativeHandle, policy);
            return schedulingOk && affinityOk;
        }

    bool RtThread::applyToSelf(const RtPolicy &policy)
        {
            pthread_t nativeHandle = pthread_self();
            const bool schedulingOk = applyScheduling(nativeHandle, policy);
            const bool affinityOk = applyCpuAffinity(nativeHandle, policy);
            return schedulingOk && affinityOk;
        }

    std::string RtThread::describe(const RtPolicy &policy)
        {

            std::ostringstream output;

            output << toString(policy.policy)
                   << "(priority=" << policy.priority;

            if (!policy.cpus.empty())
            {
                output << ", cpus=[";

                for (std::size_t i = 0; i < policy.cpus.size(); ++i)
                {
                    if (i != 0)
                    {
                        output << ',';
                    }

                    output << policy.cpus[i];
                }

                output << ']';
            }

            output << ')';

            return output.str();
        }
}