#include <spdlog/spdlog.h>
#include "config/ConfigLoader.hpp"
#include "event/EventBusMulti.hpp"
#include "event/Dispatcher.hpp"
#include "event/EventFactory.hpp"
#include "event/Topic_table.hpp"
#include "eventprocessor/event_processor.hpp"
#include "storage_engine/storage_engine.hpp"
#include "ingest/tcpingest_server.hpp"
#include "utils/thread_pool.hpp"

#include <iostream>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>

// Global flag for graceful shutdown
static std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    spdlog::info("Signal {} received, shutting down...", signum);
    g_running.store(false, std::memory_order_release);
}

int main( int argc, char* argv[] ) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("EventStreamCore version 1.0.0 starting up...");
    spdlog::info("Build date: {} {}", __DATE__ , __TIME__);

    if (argc > 1)
        spdlog::info("Config File for Backend Engine: {}", argv[1]);
    else 
        spdlog::info("No config file provided, using default settings.");
    
    // Load configuration
    AppConfig::AppConfiguration config;
    try {
        config = ConfigLoader::loadConfig(argc > 1 ? argv[1] : "config/config.yaml");
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to load configuration: {}", e.what());
        return EXIT_FAILURE;
    }
    spdlog::info("Configuration loaded successfully.");

    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Create multi-queue event bus
    EventStream::EventBusMulti::Config busConfig;
    busConfig.main_capacity = 16384;
    busConfig.log_capacity = 8192;
    busConfig.alert_capacity = 2048;
    busConfig.audit_capacity = 2048;
    auto eventBus = std::make_unique<EventStream::EventBusMulti>(busConfig);
    
    // Create dispatcher for routing events
    EventStream::Dispatcher::Config dispatcherConfig;
    dispatcherConfig.inbound_capacity = 8192;
    auto dispatcher = std::make_unique<EventStream::Dispatcher>(eventBus.get(), dispatcherConfig);
    
    // Load topic priority overrides
    auto topicTable = std::make_shared<EventStream::TopicTable>();
    if (!topicTable->loadFromFile("config/topics.conf")) {
        spdlog::warn("Could not load topic configuration file, using defaults");
    }
    
    dispatcher->setTopicTable(topicTable);
    
    // Initialize storage and thread pool
    StorageEngine storageEngine(config.storage.path);
    size_t poolSize = static_cast<size_t>(config.thread_pool.max_threads);
    auto workerPool = std::make_unique<ThreadPool>(poolSize);
    
    // Create simple EventBus for EventProcessor (legacy pattern for now)
    // TODO: Create adapter to route TCP events through Dispatcher to EventBusMulti queues
    EventStream::EventBus legacyBus;
    EventProcessor eventProcessor(legacyBus, storageEngine, workerPool.get());
    
    // Initialize TCP ingest server
    // TODO: Modify to inject events into Dispatcher instead of direct EventBus
    Ingest::TcpIngestServer tcpServer(legacyBus, config.ingestion.tcpConfig.port);
    
    try {
        // Start all components
        spdlog::info("Starting dispatcher...");
        dispatcher->start();
        
        spdlog::info("Starting event processor...");
        eventProcessor.init();
        eventProcessor.start();
        
        spdlog::info("Starting TCP ingest server on port {}...", config.ingestion.tcpConfig.port);
        tcpServer.start();
        
        spdlog::info("Initialization complete. Running main application...");
        spdlog::info("Press Ctrl+C to shutdown");
        
        // Main loop - keep application running
        while (g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
    } catch (const std::exception& e) {
        spdlog::error("Application error: {}", e.what());
        g_running.store(false, std::memory_order_release);
    }

    spdlog::info("Shutting down services...");
    
    // Graceful shutdown in reverse order
    tcpServer.stop();
    spdlog::info("TCP ingest server stopped");
    
    eventProcessor.stop();
    spdlog::info("Event processor stopped");
    
    dispatcher->stop();
    spdlog::info("Dispatcher stopped");
    
    spdlog::info("EventStreamCore shutdown complete");
    return 0;
}