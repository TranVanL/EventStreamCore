
#include <spdlog/spdlog.h>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>

#include <eventstream/core/config/loader.hpp>
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/events/dispatcher.hpp>
#include <eventstream/core/events/topic_table.hpp>
#include <eventstream/core/processor/process_manager.hpp>
#include <eventstream/core/storage/storage_engine.hpp>
#include <eventstream/core/ingest/tcp_server.hpp>
#include <eventstream/core/ingest/udp_server.hpp>
#include <eventstream/core/control/pipeline_state.hpp>
#include <eventstream/core/admin/admin_loop.hpp>

// ============================================================================
// Global State
// ============================================================================

static std::atomic<bool> g_running{true};

static void signalHandler(int signum) {
    spdlog::info("Signal {} received, initiating shutdown...", signum);
    g_running.store(false, std::memory_order_release);
}

// ============================================================================
// Initialization Functions
// ============================================================================

static void setupLogging() {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("EventStreamCore v1.0.0 starting...");
    spdlog::info("Build: {} {}", __DATE__, __TIME__);
}

static void setupSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}

static AppConfig::AppConfiguration loadConfiguration(int argc, char* argv[]) {
    const char* configPath = (argc > 1) ? argv[1] : "config/config.yaml";
    spdlog::info("Loading configuration from: {}", configPath);
    return ConfigLoader::loadConfig(configPath);
}

// ============================================================================
// Component Lifecycle
// ============================================================================

struct Components {
    // Core components (order matters for destruction)
    std::unique_ptr<EventStream::EventBusMulti> eventBus;
    std::unique_ptr<PipelineStateManager> pipelineState;
    std::unique_ptr<Dispatcher> dispatcher;
    std::unique_ptr<StorageEngine> storageEngine;
    std::unique_ptr<ProcessManager> eventProcessor;
    
    // Ingest servers (optional)
    std::unique_ptr<TcpIngestServer> tcpServer;
    std::unique_ptr<UdpIngestServer> udpServer;
    
    // Control plane
    std::unique_ptr<Admin> admin;
};

static Components initializeComponents(const AppConfig::AppConfiguration& config) {
    Components c;
    
    // Core infrastructure
    c.eventBus = std::make_unique<EventStream::EventBusMulti>();
    c.pipelineState = std::make_unique<PipelineStateManager>();
    c.dispatcher = std::make_unique<Dispatcher>(*c.eventBus, c.pipelineState.get());
    
    // Topic configuration
    auto topicTable = std::make_shared<EventStream::TopicTable>();
    if (!topicTable->LoadFileConfig("config/topics.conf")) {
        spdlog::warn("Topic config not found, using defaults");
    }
    c.dispatcher->setTopicTable(topicTable);
    
    // Storage & Processing
    c.storageEngine = std::make_unique<StorageEngine>(config.storage.path);
    c.eventProcessor = std::make_unique<ProcessManager>(*c.eventBus);
    
    // TCP Ingest (optional)
    if (config.ingestion.tcpConfig.enable) {
        c.tcpServer = std::make_unique<TcpIngestServer>(
            *c.dispatcher,
            config.ingestion.tcpConfig.port
        );
        spdlog::info("TCP ingest configured on port {}", config.ingestion.tcpConfig.port);
    }
    
    // UDP Ingest (optional)
    if (config.ingestion.udpConfig.enable) {
        c.udpServer = std::make_unique<UdpIngestServer>(
            *c.dispatcher,
            config.ingestion.udpConfig.port,
            config.ingestion.udpConfig.bufferSize
        );
        spdlog::info("UDP ingest configured on port {}", config.ingestion.udpConfig.port);
    }
    
    // Control plane
    c.admin = std::make_unique<Admin>(*c.eventProcessor);
    
    return c;
}

static void startComponents(Components& c, const AppConfig::AppConfiguration& config) {
    spdlog::info("Starting components...");
    
    c.dispatcher->start();
    c.eventProcessor->start();
    
    if (c.tcpServer) {
        c.tcpServer->start();
        spdlog::info("TCP server started on port {}", config.ingestion.tcpConfig.port);
    }
    
    if (c.udpServer) {
        c.udpServer->start();
        spdlog::info("UDP server started on port {}", config.ingestion.udpConfig.port);
    }
    
    c.admin->start();
    
    spdlog::info("All components started successfully");
}

static void stopComponents(Components& c) {
    spdlog::info("=== SHUTDOWN SEQUENCE ===");
    
    // Stop in reverse order of start
    if (c.admin) c.admin->stop();
    if (c.udpServer) c.udpServer->stop();
    if (c.tcpServer) c.tcpServer->stop();
    if (c.eventProcessor) c.eventProcessor->stop();
    if (c.dispatcher) c.dispatcher->stop();
    
    spdlog::info("=== SHUTDOWN COMPLETE ===");
}

int main(int argc, char* argv[]) {
    setupLogging();
    setupSignalHandlers();
    
    try {
        // Load configuration
        auto config = loadConfiguration(argc, argv);
        spdlog::info("Configuration loaded successfully");
        
        // Initialize all components
        auto components = initializeComponents(config);
        
        // Start all components
        startComponents(components, config);
        
        spdlog::info("EventStreamCore running. Press Ctrl+C to shutdown.");
        
        // Main loop
        while (g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // Graceful shutdown
        stopComponents(components);
        
    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return EXIT_FAILURE;
    }
    
    spdlog::info("EventStreamCore terminated gracefully");
    return EXIT_SUCCESS;
}
