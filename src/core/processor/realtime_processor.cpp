/// RealtimeProcessor — low-latency event processing with SLA enforcement.
/// Flow: validate → rules → enrich → handle → decide → alert/notify

#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/processor/processed_event_stream.hpp>
#include <eventstream/core/processor/event_handler.hpp>
#include <eventstream/core/memory/numa_binding.hpp>
#include <chrono>

// Shared default handler (stateless, safe to share)
static const EventStream::DefaultEventHandler s_default_handler;

RealtimeProcessor::RealtimeProcessor(EventStream::AlertHandlerPtr alert_handler,
                                     StorageEngine* storage,
                                     EventStream::DeadLetterQueue* dlq)
    : alert_handler_(alert_handler ? std::move(alert_handler) 
                                   : std::make_shared<EventStream::LoggingAlertHandler>())
    , storage_(storage)
    , dlq_(dlq) {}

RealtimeProcessor::~RealtimeProcessor() noexcept {
    stop();
    spdlog::info("[DESTRUCTOR] RealtimeProcessor destroyed successfully");
}

void RealtimeProcessor::start() {
    spdlog::info("RealtimeProcessor started (SLA: {}ms, AlertHandler: {}, Storage: {})", 
                 max_processing_ms_, 
                 alert_handler_->name(),
                 storage_ ? "enabled" : "disabled");
}

void RealtimeProcessor::stop() {
    if (storage_) {
        storage_->flush();
    }
    spdlog::info("RealtimeProcessor stopped.");
}

void RealtimeProcessor::emitAlert(EventStream::AlertLevel level, 
                                   const std::string& message,
                                   const EventStream::Event& event) {
    EventStream::Alert alert;
    alert.level = level;
    alert.message = message;
    alert.source = event.topic;
    alert.event_id = event.header.id;
    alert.timestamp_ns = EventStream::nowNs();
    alert.context = event.body;
    
    if (alert_handler_) {
        alert_handler_->onAlert(alert);
    }
}

void RealtimeProcessor::process(const EventStream::Event& event) {
    // NUMA binding (lazy, once per thread)
    static thread_local bool bound = false;
    if (!bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        bound = true;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    auto &m = MetricRegistry::getInstance().getMetrics(name());

    // ── Step 1: Lookup handler from registry ──
    auto* handler = EventStream::EventHandlerRegistry::getInstance().getHandler(event.topic);
    if (!handler) handler = const_cast<EventStream::DefaultEventHandler*>(&s_default_handler);

    // ── Step 2: Validate ──
    auto validation = handler->validate(event);
    if (!validation.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Realtime] Validation failed event_id={}: {}", event.header.id, validation.error);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), validation.error.c_str());
        return;
    }

    // ── Step 3: Rule checking ──
    auto rules = handler->checkRules(event);
    if (!rules.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Realtime] Rule check failed event_id={}: {}", event.header.id, rules.error);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), rules.error.c_str());
        return;
    }

    // ── Step 4: Enrich (on mutable copy for metadata) ──
    // Note: we work on const ref from queue, enrichment logged only
    spdlog::debug("[Realtime] Processing event_id={} topic={} via handler={}", 
                  event.header.id, event.topic, handler->name());

    // ── Step 5: Core processing via handler ──
    auto result = handler->handle(event);

    // ── Step 6: Decide outcome ──
    switch (result.outcome) {
        case EventStream::HandleOutcome::ALERT:
            emitAlert(result.alert_level, result.message, event);
            // Alert events are still considered processed successfully
            m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
            break;
            
        case EventStream::HandleOutcome::FAIL:
        case EventStream::HandleOutcome::DROP:
            m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
            if (dlq_) dlq_->push(event);
            EventStream::ProcessedEventStream::getInstance().notifyDropped(
                event, name(), result.message.c_str());
            return;
            
        case EventStream::HandleOutcome::SUCCESS:
            m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    // ── SLA check ──
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    if (elapsed_ms > max_processing_ms_) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        emitAlert(EventStream::AlertLevel::WARNING,
                  fmt::format("SLA breach: {}ms > {}ms", elapsed_ms, max_processing_ms_),
                  event);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "sla_breach");
        return;
    }

    MetricRegistry::getInstance().updateEventTimestamp(name());

    // Persist for audit trail
    if (storage_) {
        storage_->storeEvent(event);
    }
    
    // Notify observers (downstream business actions)
    EventStream::ProcessedEventStream::getInstance().notifyProcessed(event, name());
}

bool RealtimeProcessor::handle(const EventStream::Event& event) {
    // Legacy fallback — now delegated to handler registry in process()
    // Kept for backward compatibility if called directly
    auto* handler = EventStream::EventHandlerRegistry::getInstance().getHandler(event.topic);
    if (handler) {
        auto result = handler->handle(event);
        return result.outcome != EventStream::HandleOutcome::FAIL;
    }
    return true;
}
