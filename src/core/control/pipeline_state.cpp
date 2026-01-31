#include <eventstream/core/control/pipeline_state.hpp>

PipelineStateManager::PipelineStateManager() {
    spdlog::info("PipelineStateManager initialized, state={}", toString(PipelineState::RUNNING));
}

void PipelineStateManager::setState(PipelineState newState) {
    PipelineState oldState = getState();
    if (oldState == newState) {
        spdlog::debug("State already {}, no change", toString(newState));
        return;
    }

    state_.store(newState, std::memory_order_release);
    spdlog::warn("[PIPELINE] State transition: {} → {}", toString(oldState), toString(newState));
}

PipelineState PipelineStateManager::getState() const {
    return state_.load(std::memory_order_acquire);
}

const char* PipelineStateManager::toString(PipelineState state) {
    switch (state) {
        case PipelineState::RUNNING:    return "RUNNING";
        case PipelineState::PAUSED:     return "PAUSED";
        case PipelineState::DRAINING:   return "DRAINING";
        case PipelineState::DROPPING:   return "DROPPING";
        case PipelineState::EMERGENCY:  return "EMERGENCY";
        default:                        return "UNKNOWN";
    }
}
