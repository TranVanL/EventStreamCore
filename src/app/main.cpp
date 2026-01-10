#include <spdlog/spdlog.h>
#include "config/ConfigLoader.hpp"
#include "event/EventBusMulti.hpp"
#include "event/Dispatcher.hpp"
#include "event/EventFactory.hpp"
#include "event/Topic_table.hpp"
#include "eventprocessor/processManager.hpp"
#include "storage_engine/storage_engine.hpp"
#include "ingest/tcpingest_server.hpp"
#include "control/PipelineState.hpp"
#include "admin/admin_loop.hpp"
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
    
    try {
        // Create multi-queue event bus
        EventStream::EventBusMulti eventBus;
        
        // ========== NGÔN NGỮ CHUNG: Tạo Pipeline State Manager ==========
        // Tất cả workers/dispatchers sẽ tham khảo state này
        PipelineStateManager pipelineState;
        
        // Create dispatcher for routing events - truyền reference tới pipeline state
        Dispatcher dispatcher(eventBus, &pipelineState);
        
        // Load topic priority overrides
        auto topicTable = std::make_shared<EventStream::TopicTable>();
        if (!topicTable->LoadFileConfig("config/topics.conf")) {
            spdlog::warn("Could not load topic configuration file, using defaults");
        }
        dispatcher.setTopicTable(topicTable);
        
        // Initialize storage engine
        StorageEngine storageEngine(config.storage.path);
        
        // Initialize event processor
        ProcessManager eventProcessor(eventBus);
        
        // Initialize TCP ingest server with dispatcher
        TcpIngestServer tcpServer(dispatcher, config.ingestion.tcpConfig.port);
        
        // Initialize admin loop (handles all control decisions + metrics reporting)
        Admin admin(eventProcessor);
        
        try {
            // Start all components
            spdlog::info("Starting dispatcher...");
            dispatcher.start();
            
            spdlog::info("Starting event processor...");
            eventProcessor.start();
            
            spdlog::info("Starting TCP ingest server on port {}...", config.ingestion.tcpConfig.port);
            tcpServer.start();
            
            spdlog::info("Starting admin loop (control plane)...");
            admin.start();
            
            spdlog::info("Initialization complete. Running main application...");
            spdlog::info("Press Ctrl+C to shutdown");
        } catch (const std::exception& e) {
            spdlog::error("Failed to start components: {}", e.what());
            g_running.store(false, std::memory_order_release);
        }
        
        // Main loop - keep application running
        while (g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // Explicit cleanup on shutdown
        spdlog::info("===== SHUTDOWN SEQUENCE STARTED =====");
        try {
            spdlog::info("Stopping admin loop...");
            admin.stop();
            
            spdlog::info("Stopping TCP server...");
            tcpServer.stop();
            
            spdlog::info("Stopping event processor...");
            eventProcessor.stop();
            
            spdlog::info("Stopping dispatcher...");
            dispatcher.stop();
        } catch (const std::exception& e) {
            spdlog::error("Error during shutdown sequence: {}", e.what());
        }
        
        spdlog::info("===== EXPLICIT CLEANUP COMPLETE =====");
        // Objects will be destructed here (in reverse order of creation)
        
    } catch (const std::exception& e) {
        spdlog::error("Application error: {}", e.what());
        g_running.store(false, std::memory_order_release);
        return EXIT_FAILURE;
    }

    spdlog::info("===== ALL OBJECTS DESTRUCTED =====");
    spdlog::info("EventStreamCore shutdown complete");
    return 0;
}
