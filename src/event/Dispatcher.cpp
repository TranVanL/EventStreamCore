#include "event/Dispatcher.hpp"
#include <algorithm>
#include <iostream>

using namespace EventStream;

Dispatcher::Dispatcher(EventBusMulti* bus, const Config& cfg)
    : bus_(bus), inbound_capacity_(cfg.inbound_capacity) {}

Dispatcher::~Dispatcher() { stop(); }

bool Dispatcher::enqueue(const EventPtr& evt) {
    return inboundTryPush(evt);
}

bool Dispatcher::inboundTryPush(const EventPtr& evt) {
    std::unique_lock lk(inbound_mtx_);
    if (inbound_q_.size() >= inbound_capacity_) {
        return false; // drop new
    }
    inbound_q_.push_back(evt);
    lk.unlock();
    inbound_cv_.notify_one();
    return true;
}

std::optional<EventPtr> Dispatcher::inboundPop(std::chrono::milliseconds timeout) {
    std::unique_lock lk(inbound_mtx_);
    if (inbound_q_.empty()) {
        if (!inbound_cv_.wait_for(lk, timeout, [&]{ return !inbound_q_.empty(); })) {
            return std::nullopt;
        }
    }
    EventPtr ev = inbound_q_.front();
    inbound_q_.pop_front();
    return ev;
}

void Dispatcher::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this]{ workerLoop(); });
}

void Dispatcher::stop() {
    if (!running_.exchange(false)) return;
    inbound_cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

std::vector<EventBusMulti::QueueId> Dispatcher::defaultRoute(const EventPtr& evt) {
    std::vector<EventBusMulti::QueueId> out;
    // Always log
    out.push_back(EventBusMulti::QueueId::LOG);

    // hybrid priority resolution
    PriorityOverride override = PriorityOverride::NONE;
    if (topic_table_) {
        topic_table_->lookup(evt->topic, override);
    }

    EventPriority parsed = evt->header.priority;

    // resolution: topic override > parsed header
    EventPriority resolved = parsed;
    if (override != PriorityOverride::NONE) {
        resolved = static_cast<EventPriority>(static_cast<int>(override));
    }

    // route to main if high/critical or core topic
    if (resolved == EventPriority::HIGH || resolved == EventPriority::CRITICAL) {
        out.push_back(EventBusMulti::QueueId::MAIN);
    } else {
        // MEDIUM/LOW go to MAIN as well but maybe low-priority queue -> here we still push MAIN so main can decide
        out.push_back(EventBusMulti::QueueId::MAIN);
    }

    // route to ALERT if metadata says alert or topic indicates
    auto it = evt->metadata.find("alert");
    if (it != evt->metadata.end() && (it->second == "1" || it->second == "true")) {
        out.push_back(EventBusMulti::QueueId::ALERT);
    }

    // dedupe queue ids
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Dispatcher::workerLoop() {
    while (running_.load()) {
        auto opt = inboundPop(std::chrono::milliseconds(200));
        if (!opt.has_value()) continue;
        EventPtr evt = std::move(*opt);
        if (!evt) continue;

        // routing decisions
        RoutingFn rf;
        {
            std::lock_guard lk(routing_mtx_);
            rf = routing_fn_;
        }

        std::vector<EventBusMulti::QueueId> routes;
        if (rf) routes = rf(evt);
        else routes = defaultRoute(evt);

        // push to all routes (non-blocking)
        for (auto q : routes) {
            bool ok = bus_->push(q, evt);
            if (!ok) {
                // record metric or log drop; for now print
                // In production integrate with metrics collector
                // std::cerr << "Dispatcher: drop event to queue " << static_cast<int>(q) << "\n";
            }
        }
    }

    // drain inbound
    while (true) {
        std::optional<EventPtr> opt;
        {
            std::lock_guard lk(inbound_mtx_);
            if (inbound_q_.empty()) break;
            opt = inbound_q_.front();
            inbound_q_.pop_front();
        }
        if (!opt) break;
        EventPtr evt = *opt;
        auto routes = defaultRoute(evt);
        for (auto q : routes) bus_->push(q, evt);
    }
}
