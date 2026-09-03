#pragma once
#include <initializer_list>
#include <utility>
#include <vector>

namespace eventstream::rt {
    enum class SchedPolicy {
        Other,
        Fifo,
        RoundRobin,
    };

    struct RtPolicy {
        SchedPolicy policy{SchedPolicy::Other};
        int priority{0};
        std::vector<int> cpus;
    };

    class RtPolicyBuilder {
        public:
            RtPolicy build() const {
                return policy_;
            }

            RtPolicyBuilder& other() noexcept {
                policy_.policy = SchedPolicy::Other;
                policy_.priority = 0;
                return *this;
            }

            RtPolicyBuilder& fifo() noexcept {
                policy_.policy = SchedPolicy::Fifo;
                return *this;
            }

            RtPolicyBuilder& roundRobin() noexcept {
                policy_.policy = SchedPolicy::RoundRobin;
                return *this;
            }

            RtPolicyBuilder& priority(int p) noexcept {
                policy_.priority = p;
                return *this;
            }

            RtPolicyBuilder& cpus(std::initializer_list<int> cpu_list) noexcept {
                policy_.cpus = cpu_list;
                return *this;
            }

            RtPolicyBuilder& cpus(std::vector<int> cpu_list) noexcept {
                policy_.cpus = std::move(cpu_list);
                return *this;
            }
        private:
            RtPolicy policy_;
    };

    inline const char* toString(SchedPolicy policy) noexcept {
        switch (policy) {
            case SchedPolicy::Other: return "SCHED_OTHER";
            case SchedPolicy::Fifo: return "SCHED_FIFO";
            case SchedPolicy::RoundRobin: return "SCHED_RR";
            default: return "UNKNOWN";
        }
    }
}
