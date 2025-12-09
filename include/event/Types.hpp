#pragma once
#include <memory>
#include "Event.hpp"

namespace EventStream {
    using EventPtr = std::shared_ptr<Event>;
}