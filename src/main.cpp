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
#include <eventstream/core/processor/manager.hpp>
#include <eventstream/core/storage/storage.hpp>
#include <eventstream/core/ingest/tcp.hpp>
#include <eventstream/core/ingest/udp.hpp>
#include <eventstream/core/ingest/pool.hpp>
#include <eventstream/core/processor/handler.hpp>
#include <eventstream/core/processor/output.hpp>

// Atomic flag to control main loop and signal handling 
static std::atomic<bool> g_running{true};

static void signalHandler(int /*signum*/) {
    // Only async-signal-safe operations here (no spdlog, no malloc)
    g_running.store(false, std::memory_order_release);
}

static void setupLogging() {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("EventStreamCore v1.0.0 starting...");
    spdlog::info("Build: {} {}", __DATE__, __TIME__);
}

// Handle signal INT - Interrupt (Ctrl+C) - Program termination request
// Handle signal TERM - Termination request from OS or other programs
// If don't handle SIGINT , maybe destructor will not be called, and resources will not be released properly.
static void setupSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}


// Load configuration form Yaml file , default path is "config/config.yaml"
static AppConfig::AppConfiguration loadConfiguration(int argc, char* argv[]) {

    // Check arguments for config path 
    const char* configPath = (argc > 1) ? argv[1] : "config/config.yaml";
    spdlog::info("Loading configuration from: {}", configPath);
    return ConfigLoader::loadConfig(configPath);
}

// Components structure to hold all modules and their dependencies for easier management
struct Components {

    // Order init play crucial role , must compliance flow input -> dispatch -> process -> storage
    // Core components (order matters for destruction)
    
    // EventBus contain three queues: Realtime, Transactional, Batch
    std::unique_ptr<EventStream::EventBusMulti> eventBus;

    // Dispatcher routes events from ingest to appropriate processing queue in EventBus
    std::unique_ptr<Dispatcher> dispatcher;

    // Storage engine for persistent storage and DLQ
    std::unique_ptr<StorageEngine> storageEngine;

    // ProcessManager handles three processing threads : RealtimeProcessor, TransactionalProcessor, BatchProcessor
    std::unique_ptr<ProcessManager> eventProcessor;
    
    // Ingest servers (optional)
    std::unique_ptr<TcpIngestServer> tcpServer;
    std::unique_ptr<UdpIngestServer> udpServer;
};

// Function to initialize all components based on configuration
static Components initializeComponents(const AppConfig::AppConfiguration& config) {
    Components c;
    

    // Core infrastructure
    // Use unique pointers to ensure proper cleanup and avoid memory leaks
    c.eventBus = std::make_unique<EventStream::EventBusMulti>();
    
    // Create Dispatcher
    c.dispatcher = std::make_unique<Dispatcher>(*c.eventBus);
    
    // Topic configuration for priority routing (optional, but recommended)
    auto topicTable = std::make_shared<EventStream::TopicTable>();
    if (!topicTable->loadFromFile("config/topics.conf")) {
        spdlog::warn("Topic config not found, using defaults");
    }
    c.dispatcher->setTopicTable(topicTable);
    
    // Storage & Processing
    c.storageEngine = std::make_unique<StorageEngine>(config.storage.path);
    
    // Wire dependencies to ProcessManager to get place to store dropped events in DLQ and access storage engine
    ProcessManager::Dependencies deps;
    deps.storage = c.storageEngine.get();
    deps.dlq = &c.eventBus->getDLQ();
    deps.batch_window = std::chrono::seconds(5);
    
    c.eventProcessor = std::make_unique<ProcessManager>(*c.eventBus, deps);
    spdlog::info("ProcessManager wired with Storage: {}", config.storage.path);
    
    // TCP Ingest (optional)
    // Check enable flag in config before creating server to avoid unnecessary resource allocation
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
    
    return c;
}


// Start and stop components in the correct order to ensure proper initialization and cleanup
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
    
    spdlog::info("All components started successfully");
}

static void stopComponents(Components& c) {
    spdlog::info("=== SHUTDOWN SEQUENCE ===");
    
    // Stop in reverse order of start
    if (c.udpServer) c.udpServer->stop();
    if (c.tcpServer) c.tcpServer->stop();
    if (c.eventProcessor) c.eventProcessor->stop();
    if (c.dispatcher) c.dispatcher->stop();
    
    spdlog::info("=== SHUTDOWN COMPLETE ===");
}

int main(int argc, char* argv[]) {

    // Set format logging  
    setupLogging();
    // Set up signal handlers for graceful shutdown
    setupSignalHandlers();
    
    try {
        // Load configuration
        auto config = loadConfiguration(argc, argv);
        spdlog::info("Configuration loaded successfully");
        
        // Initialize event pool for ingestion (pre-allocates events)
        EventStream::IngestEventPool::initialize();
        spdlog::info("Ingest event pool initialized");
        
        // Register event handlers (Strategy pattern)
        EventStream::registerDefaultHandlers();
        
        // Register observers (downstream business hooks)
        EventStream::clearAllObservers();
        EventStream::registerDefaultObservers();
        
        // Initialize all components
        auto components = initializeComponents(config);
        
        // Start all components
        startComponents(components, config);
        
        spdlog::info("EventStreamCore running. Press Ctrl+C to shutdown.");
        
        // Main loop
        // Signal handler will set g_running to false on SIGINT/SIGTERM
        while (g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        spdlog::info("Shutdown signal received, initiating graceful shutdown...");
        
        // Graceful shutdown
        stopComponents(components);
        
        // Shutdown ingest event pool (prevents use-after-free in custom deleters)
        EventStream::IngestEventPool::shutdown();
        EventStream::clearAllObservers();
        
    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return EXIT_FAILURE;
    }
    
    spdlog::info("EventStreamCore terminated gracefully");
    return EXIT_SUCCESS;
}
