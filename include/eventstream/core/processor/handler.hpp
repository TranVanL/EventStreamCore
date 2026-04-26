#pragma once

#include <eventstream/core/events/event.hpp>
#include <eventstream/core/processor/alert.hpp>
#include <unordered_map>
#include <memory>
#include <string>
#include <cstring>
#include <spdlog/spdlog.h>

namespace EventStream {

struct ValidationResult {
    bool valid = true;
    std::string error;
    static ValidationResult ok() { return {true, ""}; }
    static ValidationResult fail(const std::string& msg) { return {false, msg}; }
};

enum class HandleOutcome { SUCCESS, FAIL, ALERT, DROP };

struct HandleResult {
    HandleOutcome outcome = HandleOutcome::SUCCESS;
    AlertLevel alert_level = AlertLevel::INFO;
    std::string message;

    static HandleResult success(const std::string& msg = "") {
        return {HandleOutcome::SUCCESS, AlertLevel::INFO, msg};
    }
    static HandleResult fail(const std::string& msg) {
        return {HandleOutcome::FAIL, AlertLevel::INFO, msg};
    }
    static HandleResult alert(AlertLevel lvl, const std::string& msg) {
        return {HandleOutcome::ALERT, lvl, msg};
    }
    static HandleResult drop(const std::string& msg) {
        return {HandleOutcome::DROP, AlertLevel::INFO, msg};
    }
};

class EventHandler {
public:
    virtual ~EventHandler() = default;

    virtual ValidationResult validate(const Event& event) const {
        if (event.topic.empty()) return ValidationResult::fail("empty topic");
        return ValidationResult::ok();
    }

    virtual ValidationResult checkRules(const Event& event) const {
        return ValidationResult::ok();
    }

    virtual HandleResult handle(const Event& event) const = 0;
    virtual const char* name() const = 0;
};

using EventHandlerPtr = std::shared_ptr<EventHandler>;

class EventHandlerRegistry {
public:
    static EventHandlerRegistry& getInstance() {
        static EventHandlerRegistry instance;
        return instance;
    }

    void registerHandler(const std::string& topic_prefix, EventHandlerPtr handler) {
        handlers_[topic_prefix] = std::move(handler);
    }

    EventHandler* getHandler(const std::string& topic) const {
        auto it = handlers_.find(topic);
        if (it != handlers_.end()) return it->second.get();

        EventHandler* best = nullptr;
        size_t best_len = 0;
        for (const auto& [prefix, handler] : handlers_) {
            if (topic.rfind(prefix, 0) == 0 && prefix.size() > best_len) {
                best = handler.get();
                best_len = prefix.size();
            }
        }
        return best;
    }

    void clear() { handlers_.clear(); }

private:
    EventHandlerRegistry() = default;
    std::unordered_map<std::string, EventHandlerPtr> handlers_;
};

class DefaultEventHandler : public EventHandler {
public:
    const char* name() const override { return "DefaultEventHandler"; }
    HandleResult handle(const Event& event) const override {
        return HandleResult::success();
    }
};

class SensorTemperatureHandler : public EventHandler {
public:
    const char* name() const override { return "SensorTemperatureHandler"; }

    ValidationResult validate(const Event& event) const override {
        if (event.body.empty())
            return ValidationResult::fail("empty body for temperature sensor");
        return ValidationResult::ok();
    }

    ValidationResult checkRules(const Event& event) const override {
        if (event.body[0] > 150)
            return ValidationResult::fail("temperature out of physical range");
        return ValidationResult::ok();
    }

    HandleResult handle(const Event& event) const override {
        uint8_t temp = event.body[0];
        if (temp > 100)
            return HandleResult::alert(AlertLevel::CRITICAL,
                fmt::format("Temperature critical: {}C", temp));
        if (temp > 80)
            return HandleResult::alert(AlertLevel::WARNING,
                fmt::format("Temperature warning: {}C", temp));
        return HandleResult::success();
    }
};

class SensorPressureHandler : public EventHandler {
public:
    const char* name() const override { return "SensorPressureHandler"; }

    ValidationResult validate(const Event& event) const override {
        if (event.body.empty())
            return ValidationResult::fail("empty body for pressure sensor");
        return ValidationResult::ok();
    }

    HandleResult handle(const Event& event) const override {
        uint8_t pressure = event.body[0];
        if (pressure > 200)
            return HandleResult::alert(AlertLevel::EMERGENCY,
                fmt::format("Pressure emergency: {} bar", pressure));
        if (pressure > 150)
            return HandleResult::alert(AlertLevel::WARNING,
                fmt::format("Pressure high: {} bar", pressure));
        return HandleResult::success();
    }
};

class PaymentEventHandler : public EventHandler {
public:
    const char* name() const override { return "PaymentEventHandler"; }

    ValidationResult validate(const Event& event) const override {
        if (event.body.size() < 4)
            return ValidationResult::fail("payment body too short");
        return ValidationResult::ok();
    }

    ValidationResult checkRules(const Event& event) const override {
        uint32_t amount = 0;
        std::memcpy(&amount, event.body.data(), sizeof(uint32_t));
        if (amount == 0) return ValidationResult::fail("payment amount is zero");
        if (amount > 1000000) return ValidationResult::fail("payment amount exceeds limit");
        return ValidationResult::ok();
    }

    HandleResult handle(const Event& event) const override {
        uint32_t amount = 0;
        std::memcpy(&amount, event.body.data(), sizeof(uint32_t));
        return HandleResult::success(fmt::format("payment_ok amount={}", amount));
    }
};

class AuditEventHandler : public EventHandler {
public:
    const char* name() const override { return "AuditEventHandler"; }

    ValidationResult validate(const Event& event) const override {
        if (event.body.empty())
            return ValidationResult::fail("empty audit log body");
        return ValidationResult::ok();
    }

    HandleResult handle(const Event& event) const override {
        return HandleResult::success();
    }
};

inline EventHandler* getOrDefault(const std::string& topic) {
    static DefaultEventHandler fallback;
    auto* h = EventHandlerRegistry::getInstance().getHandler(topic);
    return h ? h : &fallback;
}

inline void registerDefaultHandlers() {
    auto& reg = EventHandlerRegistry::getInstance();
    reg.registerHandler("sensor/temperature", std::make_shared<SensorTemperatureHandler>());
    reg.registerHandler("sensor/pressure",    std::make_shared<SensorPressureHandler>());
    reg.registerHandler("payment",            std::make_shared<PaymentEventHandler>());
    reg.registerHandler("audit",              std::make_shared<AuditEventHandler>());
    spdlog::info("[HandlerRegistry] Default handlers registered");
}

} // namespace EventStream
