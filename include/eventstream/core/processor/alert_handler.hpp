#pragma once

#include <eventstream/core/events/event.hpp>
#include <functional>
#include <vector>
#include <memory>
#include <spdlog/spdlog.h>

namespace EventStream {

enum class AlertLevel { INFO = 0, WARNING = 1, CRITICAL = 2, EMERGENCY = 3 };

struct Alert {
    AlertLevel level;
    std::string message;
    std::string source;
    uint32_t event_id;
    uint64_t timestamp_ns;
    std::vector<uint8_t> context;
};

class AlertHandler {
public:
    virtual ~AlertHandler() = default;
    virtual void onAlert(const Alert& alert) = 0;
    virtual const char* name() const = 0;
};

using AlertHandlerPtr = std::shared_ptr<AlertHandler>;

class LoggingAlertHandler : public AlertHandler {
public:
    void onAlert(const Alert& alert) override {
        switch (alert.level) {
        case AlertLevel::INFO:      spdlog::info("[ALERT] {} - {}", alert.source, alert.message); break;
        case AlertLevel::WARNING:   spdlog::warn("[ALERT] {} - {}", alert.source, alert.message); break;
        case AlertLevel::CRITICAL:  spdlog::critical("[ALERT] {} - {}", alert.source, alert.message); break;
        case AlertLevel::EMERGENCY: spdlog::critical("[EMERGENCY] {} - {}", alert.source, alert.message); break;
        }
    }
    const char* name() const override { return "LoggingAlertHandler"; }
};

class CallbackAlertHandler : public AlertHandler {
public:
    using Callback = std::function<void(const Alert&)>;
    explicit CallbackAlertHandler(Callback cb, const char* n = "CallbackAlertHandler")
        : callback_(std::move(cb)), name_(n) {}
    void onAlert(const Alert& alert) override { if (callback_) callback_(alert); }
    const char* name() const override { return name_; }
private:
    Callback callback_;
    const char* name_;
};

class CompositeAlertHandler : public AlertHandler {
public:
    void addHandler(AlertHandlerPtr h) { handlers_.push_back(std::move(h)); }
    void onAlert(const Alert& alert) override {
        for (auto& h : handlers_) {
            try { h->onAlert(alert); }
            catch (const std::exception& e) {
                spdlog::error("AlertHandler {} threw: {}", h->name(), e.what());
            }
        }
    }
    const char* name() const override { return "CompositeAlertHandler"; }
private:
    std::vector<AlertHandlerPtr> handlers_;
};

class NullAlertHandler : public AlertHandler {
public:
    void onAlert(const Alert&) override {}
    const char* name() const override { return "NullAlertHandler"; }
};

} // namespace EventStream
