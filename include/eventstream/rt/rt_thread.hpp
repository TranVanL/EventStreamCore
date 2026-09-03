#pragma once 
#include <eventstream/rt/rt_policy.hpp>
#include <string>
#include <thread>

namespace eventstream::rt {
    class RtThread {
        public:
            static bool apply(std::thread& thread, const RtPolicy& policy);
            static bool applyToSelf(const RtPolicy& policy);
            static std::string describe(const RtPolicy& policy);
    };
}