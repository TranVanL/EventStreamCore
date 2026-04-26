// esccore.cpp — C API bridge (output-only).
// Maps flat C structs from esccore.h to internal C++ types.
// Events flow from the pipeline OUT to registered callbacks; there is no
// push-from-external path in this interface.

#include <eventstream/bridge/esccore.h>

#include <eventstream/core/config/loader.hpp>
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/events/dispatcher.hpp>
#include <eventstream/core/events/topic_table.hpp>
#include <eventstream/core/processor/handler.hpp>
#include <eventstream/core/processor/manager.hpp>
#include <eventstream/core/storage/storage.hpp>
#include <eventstream/core/ingest/pool.hpp>
#include <eventstream/core/ingest/tcp.hpp>
#include <eventstream/core/ingest/udp.hpp>
#include <eventstream/core/metrics/registry.hpp>
#include <eventstream/core/processor/output.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <functional>
#include <string>
#include <memory>

// Helper class for ProcessedEventObserver
struct EscObserver : public EventStream::ProcessedEventObserver {
    std::function<void(const EventStream::Event&, const char*)> onProcessed;
    std::function<void(const EventStream::Event&, const char*, const char*)> onDropped;
    std::string name;

    EscObserver(std::function<void(const EventStream::Event&, const char*)> p,
                std::function<void(const EventStream::Event&, const char*, const char*)> d,
                std::string n)
        : onProcessed(std::move(p)), onDropped(std::move(d)), name(std::move(n)) {}

    void onEventProcessed(const EventStream::Event& event, const char* processor_name) override {
        onProcessed(event, processor_name);
    }

    void onEventDropped(const EventStream::Event& event, const char* processor_name, const char* reason) override {
        onDropped(event, processor_name, reason);
    }

    const char* observerName() const override { return name.c_str(); }
};
#include <memory>
#include <mutex>
#include <cstring>

namespace {

struct Engine {
    std::unique_ptr<EventStream::EventBusMulti> eventBus;
    std::unique_ptr<Dispatcher>                 dispatcher;
    std::unique_ptr<StorageEngine>              storage;
    std::unique_ptr<ProcessManager>             processManager;
    std::unique_ptr<TcpIngestServer>            tcpServer;
    std::unique_ptr<UdpIngestServer>            udpServer;
    std::shared_ptr<EventStream::TopicTable>    topicTable;
};

std::atomic<bool> g_initialised{false};
std::mutex        g_mutex;
Engine*           g_engine = nullptr;

// Convert internal Event → flat C event (shallow — pointers into evt).
void toExternal(const EventStream::Event& evt, esc_event_t& out) {
    out.id       = evt.header.id;
    out.priority = static_cast<esc_priority_t>(evt.header.priority);
    out.topic    = evt.topic.c_str();
    out.body     = evt.body.data();
    out.body_len = evt.body.size();
}

}  // anonymous namespace

// ── Lifecycle ──────────────────────────────────────────────────────────────

esc_status_t esccore_init(const char* config_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialised.load()) return ESC_ERR_INIT;

    try {
        const char* path = config_path ? config_path : "config/config.yaml";
        auto config = ConfigLoader::loadConfig(path);

        auto eng = new Engine();

        eng->eventBus   = std::make_unique<EventStream::EventBusMulti>();
        eng->dispatcher = std::make_unique<Dispatcher>(*eng->eventBus);

        eng->topicTable = std::make_shared<EventStream::TopicTable>();
        eng->topicTable->loadFromFile("config/topics.conf");
        eng->dispatcher->setTopicTable(eng->topicTable);

        eng->storage = std::make_unique<StorageEngine>(config.storage.path);

        ProcessManager::Dependencies deps;
        deps.storage      = eng->storage.get();
        deps.dlq          = &eng->eventBus->getDLQ();
        deps.batch_window = std::chrono::seconds(5);

        eng->processManager = std::make_unique<ProcessManager>(*eng->eventBus, deps);

        EventStream::IngestEventPool::initialize();
        EventStream::registerDefaultHandlers();
        EventStream::registerDefaultObservers();

        eng->dispatcher->start();
        eng->processManager->start();

        if (config.ingestion.tcpConfig.enable) {
            eng->tcpServer = std::make_unique<TcpIngestServer>(
                *eng->dispatcher,
                config.ingestion.tcpConfig.port
            );
            eng->tcpServer->start();
            spdlog::info("[esccore] TCP ingest server started on port {}",
                         config.ingestion.tcpConfig.port);
        }

        if (config.ingestion.udpConfig.enable) {
            eng->udpServer = std::make_unique<UdpIngestServer>(
                *eng->dispatcher,
                config.ingestion.udpConfig.port,
                static_cast<size_t>(config.ingestion.udpConfig.bufferSize)
            );
            eng->udpServer->start();
            spdlog::info("[esccore] UDP ingest server started on port {}",
                         config.ingestion.udpConfig.port);
        }

        g_engine = eng;
        g_initialised.store(true, std::memory_order_release);

        spdlog::info("[esccore] Engine initialised (config={})", path);
        return ESC_OK;

    } catch (const std::exception& ex) {
        spdlog::error("[esccore] Init failed: {}", ex.what());
        return ESC_ERR_INTERNAL;
    }
}

esc_status_t esccore_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised.load()) return ESC_ERR_INIT;

    try {
        g_initialised.store(false, std::memory_order_release);

        if (g_engine) {
            if (g_engine->udpServer) {
                g_engine->udpServer->stop();
            }
            if (g_engine->tcpServer) {
                g_engine->tcpServer->stop();
            }
            g_engine->processManager->stop();
            g_engine->dispatcher->stop();
            EventStream::IngestEventPool::shutdown();
            delete g_engine;
            g_engine = nullptr;
        }

        spdlog::info("[esccore] Engine shut down");
        return ESC_OK;

    } catch (const std::exception& ex) {
        spdlog::error("[esccore] Shutdown error: {}", ex.what());
        return ESC_ERR_INTERNAL;
    }
}

// ── Subscribe ──────────────────────────────────────────────────────────────

esc_status_t esccore_subscribe(const char*          topic_prefix,
                               esc_event_callback_t cb,
                               void*                user_data) {
    if (!g_initialised.load(std::memory_order_acquire)) return ESC_ERR_INIT;
    if (!cb) return ESC_ERR_INVALID;

    std::string prefix = topic_prefix ? topic_prefix : "";

    EventStream::ProcessedEventStream::getInstance().subscribe(
        std::make_shared<EscObserver>(
            [cb, user_data, prefix](const EventStream::Event& evt, const char* /*proc*/) {
                if (!prefix.empty() &&
                    evt.topic.compare(0, prefix.size(), prefix) != 0) {
                    return;  // topic doesn't match filter
                }
                esc_event_t out{};
                toExternal(evt, out);
                cb(&out, user_data);
            },
            [](const EventStream::Event&, const char*, const char*) {}, // onDropped, do nothing
            "esccore_subscriber"
        )
    );

    return ESC_OK;
}

// ── Observability ──────────────────────────────────────────────────────────

esc_status_t esccore_metrics(esc_metrics_t* out) {
    if (!g_initialised.load(std::memory_order_acquire)) return ESC_ERR_INIT;
    if (!out) return ESC_ERR_INVALID;

    std::memset(out, 0, sizeof(*out));

    auto snapshots = MetricRegistry::getInstance().getSnapshots();
    for (auto& [name, snap] : snapshots) {
        out->total_events_processed += snap.total_events_processed;
        out->total_events_dropped   += snap.total_events_dropped;
    }

    out->queue_depth = g_engine->eventBus->size(
        EventStream::EventBusMulti::QueueId::REALTIME) +
        g_engine->eventBus->size(
        EventStream::EventBusMulti::QueueId::TRANSACTIONAL) +
        g_engine->eventBus->size(
        EventStream::EventBusMulti::QueueId::BATCH);

    out->backpressure_level = static_cast<int>(
        g_engine->eventBus->getRealtimePressure());

    return ESC_OK;
}

esc_status_t esccore_health(esc_health_t* out) {
    if (!g_initialised.load(std::memory_order_acquire)) return ESC_ERR_INIT;
    if (!out) return ESC_ERR_INVALID;

    out->is_alive           = 1;
    out->is_ready           = g_initialised.load() ? 1 : 0;
    out->backpressure_level = static_cast<int>(
        g_engine->eventBus->getRealtimePressure());

    return ESC_OK;
}
