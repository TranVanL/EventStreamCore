#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/processor/processed_event_stream.hpp>
#include <eventstream/core/memory/numa_binding.hpp>
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
    spdlog::info("[DESTRUCTOR] RealtimeProcessor destroyed successfully");
}

void RealtimeProcessor::start() {
    spdlog::info("RealtimeProcessor started (SLA: {}ms, AlertHandler: {}, Storage: {})", 
                 max_processing_ms_, 
                 alert_handler_->name(),
                 storage_ ? "enabled" : "disabled");
}

void RealtimeProcessor::stop() {
    // Flush storage before stopping
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
    // Bind processor thread to NUMA node on first call (lazy binding)
    static thread_local bool bound = false;
    if (!bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        bound = true;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    auto &m = MetricRegistry::getInstance().getMetrics(name());

    // Process the event and detect alerts
    if (!handle(event)) {
        // Processing failed
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::error("RealtimeProcessor failed to process event id {}", event.header.id);
        
        // Push to DLQ
        if (dlq_) {
            dlq_->push(event);
        }
        
        // Notify observers
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "processing_failed");
        return;
    }
    
    // Check latency SLA
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    if (total_elapsed_ms > max_processing_ms_) {
        // SLA breached
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        
        emitAlert(EventStream::AlertLevel::WARNING,
                  fmt::format("SLA breach: {}ms > {}ms", total_elapsed_ms, max_processing_ms_),
                  event);
        
        // Push to DLQ
        if (dlq_) {
            dlq_->push(event);
        }
        
        // Notify observers
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "sla_breach");
        return;
    }
    
    // Successfully processed within SLA
    m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
    MetricRegistry::getInstance().updateEventTimestamp(name());
    
    // *** WRITE TO STORAGE FOR AUDIT TRAIL ***
    if (storage_) {
        storage_->storeEvent(event);
    }
    
    // Notify observers (for Distributed/Microservice layer hooks)
    EventStream::ProcessedEventStream::getInstance().notifyProcessed(event, name());
}

bool RealtimeProcessor::handle(const EventStream::Event& event) {
    // Alert detection for CRITICAL/HIGH priority events
    
    // Large payload alert
    if (event.body.size() > 1024) {
        emitAlert(EventStream::AlertLevel::WARNING,
                  fmt::format("Large payload: {} bytes", event.body.size()),
                  event);
        return true;
    }

    // Temperature sensor alert
    if (event.topic == "sensor/temperature" && !event.body.empty()) {
        uint8_t temp = event.body[0];
        if (temp > 100) {
            emitAlert(EventStream::AlertLevel::CRITICAL,
                      fmt::format("Temperature critical: {}°C", temp),
                      event);
            return true;
        } else if (temp > 80) {
            emitAlert(EventStream::AlertLevel::WARNING,
                      fmt::format("Temperature warning: {}°C", temp),
                      event);
            return true;
        }
    }
    
    // Pressure sensor alert
    if (event.topic == "sensor/pressure" && !event.body.empty()) {
        uint8_t pressure = event.body[0];
        if (pressure > 200) {
            emitAlert(EventStream::AlertLevel::EMERGENCY,
                      fmt::format("Pressure emergency: {} bar", pressure),
                      event);
            return true;
        }
    }

    // Accept all other events (no alert needed)
    return true;
}

