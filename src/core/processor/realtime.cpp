#include <eventstream/core/processor/processor.hpp>
#include <eventstream/core/processor/output.hpp>
#include <eventstream/core/processor/handler.hpp>
#include <eventstream/core/memory/numa.hpp>
#include <chrono>

RealtimeProcessor::RealtimeProcessor(EventStream::AlertHandlerPtr alert_handler,
                                     StorageEngine* storage,
                                     EventStream::DeadLetterQueue* dlq)
    : alert_handler_(alert_handler ? std::move(alert_handler)
                                   : std::make_shared<EventStream::LoggingAlertHandler>())
    , storage_(storage)
    , dlq_(dlq) {}

RealtimeProcessor::~RealtimeProcessor() noexcept {
    stop();
}

void RealtimeProcessor::start() {
    spdlog::info("RealtimeProcessor started (SLA={}ms, alert={}, storage={})",
                 max_processing_ms_, alert_handler_->name(),
                 storage_ ? "on" : "off");
}

void RealtimeProcessor::stop() {
    if (storage_) storage_->flush();
    spdlog::info("RealtimeProcessor stopped");
}

void RealtimeProcessor::emitAlert(EventStream::AlertLevel level,
                                   const std::string& message,
                                   const EventStream::Event& event) {
    if (!alert_handler_) return;
    EventStream::Alert alert;
    alert.level = level;
    alert.message = message;
    alert.source = event.topic;
    alert.event_id = event.header.id;
    alert.timestamp_ns = EventStream::nowNs();
    alert.context = event.body;
    alert_handler_->onAlert(alert);
}

void RealtimeProcessor::process(const EventStream::Event& event) {
    static thread_local bool numa_bound = false;
    if (!numa_bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        numa_bound = true;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    auto& m = MetricRegistry::getInstance().getMetrics(name());
    auto* handler = EventStream::getOrDefault(event.topic);

    // Validate
    auto v = handler->validate(event);
    if (!v.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), v.error.c_str());
        return;
    }

    // Rule check
    auto r = handler->checkRules(event);
    if (!r.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), r.error.c_str());
        return;
    }

    // Handle
    auto result = handler->handle(event);

    switch (result.outcome) {
    case EventStream::HandleOutcome::ALERT:
        emitAlert(result.alert_level, result.message, event);
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        break;
    case EventStream::HandleOutcome::FAIL:
    case EventStream::HandleOutcome::DROP:
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), result.message.c_str());
        return;
    case EventStream::HandleOutcome::SUCCESS:
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    // SLA enforcement
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start_time).count();

    if (elapsed_ms > max_processing_ms_) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        emitAlert(EventStream::AlertLevel::WARNING,
                  fmt::format("SLA breach: {}ms > {}ms", elapsed_ms, max_processing_ms_), event);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), "sla_breach");
        return;
    }

    MetricRegistry::getInstance().updateEventTimestamp(name());
    if (storage_) storage_->storeEvent(event);
    EventStream::ProcessedEventStream::getInstance().notifyProcessed(event, name());
}

bool RealtimeProcessor::handle(const EventStream::Event& event) {
    auto result = EventStream::getOrDefault(event.topic)->handle(event);
    return result.outcome != EventStream::HandleOutcome::FAIL;
}
