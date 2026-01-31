#pragma once

#include <eventstream/core/events/event.hpp>
#include <functional>
#include <vector>
#include <memory>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @enum AlertLevel
 * @brief Severity level of alerts
 */
enum class AlertLevel {
    INFO = 0,       // Informational, no action needed
    WARNING = 1,    // Warning, may need attention
    CRITICAL = 2,   // Critical, immediate action required
    EMERGENCY = 3   // Emergency, system-wide issue
};

/**
 * @struct Alert
 * @brief Alert data structure emitted by RealtimeProcessor
 */
struct Alert {
    AlertLevel level;
    std::string message;
    std::string source;         // Which topic/sensor triggered
    uint32_t event_id;          // Original event ID
    uint64_t timestamp_ns;      // When alert was generated
    
    // Optional: Original event payload for context
    std::vector<uint8_t> context;
};

/**
 * @class AlertHandler
 * @brief Interface for handling alerts from RealtimeProcessor
 * 
 * This is the OUTPUT interface for RealtimeProcessor.
 * Implementations can:
 * - Send notifications (email, SMS, Slack)
 * - Trigger actions (shutdown, scale, failover)
 * - Log to monitoring systems (Prometheus, Datadog)
 * - Forward to external services
 * 
 * Design:
 * - Non-blocking: Implementations should not block
 * - Thread-safe: May be called from processor thread
 * - Composable: Use CompositeAlertHandler for multiple handlers
 */
class AlertHandler {
public:
    virtual ~AlertHandler() = default;
    
    /**
     * @brief Handle an alert (non-blocking)
     * @param alert The alert to handle
     */
    virtual void onAlert(const Alert& alert) = 0;
    
    /**
     * @brief Get handler name for logging
     */
    virtual const char* name() const = 0;
};

using AlertHandlerPtr = std::shared_ptr<AlertHandler>;

/**
 * @class LoggingAlertHandler
 * @brief Simple handler that logs alerts via spdlog
 */
class LoggingAlertHandler : public AlertHandler {
public:
    void onAlert(const Alert& alert) override {
        switch (alert.level) {
            case AlertLevel::INFO:
                spdlog::info("[ALERT] {} - {}", alert.source, alert.message);
                break;
            case AlertLevel::WARNING:
                spdlog::warn("[ALERT] {} - {}", alert.source, alert.message);
                break;
            case AlertLevel::CRITICAL:
                spdlog::critical("[ALERT] {} - {}", alert.source, alert.message);
                break;
            case AlertLevel::EMERGENCY:
                spdlog::critical("[EMERGENCY ALERT] {} - {}", alert.source, alert.message);
                break;
        }
    }
    
    const char* name() const override { return "LoggingAlertHandler"; }
};

/**
 * @class CallbackAlertHandler
 * @brief Handler that calls user-provided callback
 */
class CallbackAlertHandler : public AlertHandler {
public:
    using Callback = std::function<void(const Alert&)>;
    
    explicit CallbackAlertHandler(Callback cb, const char* name = "CallbackAlertHandler")
        : callback_(std::move(cb)), name_(name) {}
    
    void onAlert(const Alert& alert) override {
        if (callback_) {
            callback_(alert);
        }
    }
    
    const char* name() const override { return name_; }

private:
    Callback callback_;
    const char* name_;
};

/**
 * @class CompositeAlertHandler
 * @brief Fan-out to multiple alert handlers
 */
class CompositeAlertHandler : public AlertHandler {
public:
    void addHandler(AlertHandlerPtr handler) {
        handlers_.push_back(std::move(handler));
    }
    
    void onAlert(const Alert& alert) override {
        for (auto& handler : handlers_) {
            try {
                handler->onAlert(alert);
            } catch (const std::exception& e) {
                spdlog::error("AlertHandler {} threw exception: {}", 
                             handler->name(), e.what());
            }
        }
    }
    
    const char* name() const override { return "CompositeAlertHandler"; }

private:
    std::vector<AlertHandlerPtr> handlers_;
};

/**
 * @class NullAlertHandler
 * @brief Discards all alerts (for testing/benchmarks)
 */
class NullAlertHandler : public AlertHandler {
public:
    void onAlert(const Alert&) override {}
    const char* name() const override { return "NullAlertHandler"; }
};

} // namespace EventStream
