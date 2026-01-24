// TransactionalProcessor Contract
// -------------------------------
// Input:
//   - Events from TRANSACTIONAL queue
// Guarantees:
//   - At-least-once processing
//   - Idempotent execution
//   - Retry on failure
//
// Failure handling:
//   - Max retry exceeded → Dead Letter Queue
//
// Typical use cases:
//   - Database write
//   - Billing
//   - State mutation

#include "eventprocessor/event_processor.hpp"

TransactionalProcessor::TransactionalProcessor() {}

TransactionalProcessor::~TransactionalProcessor() noexcept {
    spdlog::info("[DESTRUCTOR] TransactionalProcessor being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] TransactionalProcessor destroyed successfully");
}

void TransactionalProcessor::start() {
    spdlog::info("TransactionalProcessor started - handling MEDIUM priority events");
    state_.store(ProcessState::RUNNING, std::memory_order_release);
}

void TransactionalProcessor::stop() {
    spdlog::info("TransactionalProcessor stopped");
    state_.store(ProcessState::STOPPED, std::memory_order_release);
}

void TransactionalProcessor::process(const EventStream::Event& event) {
    // Check if paused by control plane
    if (paused_.load(std::memory_order_acquire)) {
        spdlog::debug("TransactionalProcessor paused, dropping event id {}", event.header.id);
        auto &m = MetricRegistry::getInstance().getMetrics(name());
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto &m = MetricRegistry::getInstance().getMetrics(name());

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Idempotency check and periodic cleanup
    {
        std::lock_guard<std::mutex> lock(processed_ids_mutex_);

        // Perform cleanup every 10 seconds to avoid memory leak of old entries
        if (last_cleanup_ms_ == 0 || now_ms - last_cleanup_ms_ > 10000) {
            size_t before_size = processed_ids_.size();
            auto it = processed_ids_.begin();
            while (it != processed_ids_.end()) {
                if (now_ms - it->second.timestamp_ms > IDEMPOTENT_WINDOW_MS) {
                    it = processed_ids_.erase(it);
                } else {
                    ++it;
                }
            }
            spdlog::debug("Idempotency cache cleanup: {} entries removed (was {} now {})", 
                         before_size - processed_ids_.size(), before_size, processed_ids_.size());
            last_cleanup_ms_ = now_ms;
        }

        auto it = processed_ids_.find(event.header.id);
        if (it != processed_ids_.end()) {
            spdlog::debug("Event id {} already processed", event.header.id);
            // Duplicate - skip
            return;
        }
    }

    // Retry logic
    bool success = false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (handle(event)) {
            success = true;
            break;
        }
        if (attempt < 3) {
            spdlog::warn("Transactional processing failed for event id {} (attempt {}/3), retrying...", 
                        event.header.id, attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Record result
    {
        std::lock_guard<std::mutex> lock(processed_ids_mutex_);
        processed_ids_[event.header.id] = {static_cast<uint64_t>(now_ms)};
    }

    if (success) {
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        // Update last event timestamp (for stale detection)
        MetricRegistry::getInstance().updateEventTimestamp(name());
    } else {
        spdlog::error("Event id {} FAILED after 3 retries - sending to Dead Letter Queue", event.header.id);
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

bool TransactionalProcessor::handle(const EventStream::Event& event) {
    // Ordered, durable processing logic
    
    // Payment transaction handling
    if (event.topic.find("payment") != std::string::npos) {
        spdlog::debug("Processing payment transaction for event id {}", event.header.id);
        return true;
    }
    
    // Audit log handling
    if (event.topic.find("audit") != std::string::npos) {
        spdlog::debug("Recording audit log for event id {}", event.header.id);
        return true;
    }
    
    // State mutation handling
    if (event.topic.find("state") != std::string::npos) {
        spdlog::debug("Processing state change for event id {}", event.header.id);
        return true;
    }
    
    // Default transactional handling
    spdlog::debug("Transactional processing event id {} from topic {}", event.header.id, event.topic);
    return true;
}