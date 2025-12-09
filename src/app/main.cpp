#include <spdlog/spdlog.h>
#include "config/ConfigLoader.hpp"
#include "event/EventBus.hpp"
#include "event/EventFactory.hpp"
#include "eventprocessor/event_processor.hpp"
#include "storage_engine/storage_engine.hpp"
#include "ingest/tcpingest_server.hpp"
#include "utils/thread_pool.hpp"

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

int main( int argc, char* argv[] ) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("EventStreamCore version 1.0.0 starting up...");
    spdlog::info("Build date: {} {}", __DATE__ , __TIME__);

    if (argc > 1)
        spdlog::info("Config File for Backend Engine: {}", argv[1]);
    else 
        spdlog::info("No config file provided, using default settings.");
    
   

    AppConfig::AppConfiguration config;
    try {
        config = ConfigLoader::loadConfig(argc > 1 ? argv[1] : "config/config.yaml");
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to load configuration: {}", e.what());
        return EXIT_FAILURE;
    }
    spdlog::info("Initialization complete. Running main application...");
    // Application main loop would go here 
    spdlog::info("Application is running "); 


    // Event Bus 
    EventStream::EventBus eventBus;
    Ingest::TcpIngestServer tcpServer(eventBus, config.ingestion.tcpConfig.port);
    StorageEngine storageEngine(config.storage.path);


    // Thread pool for storage tasks
    size_t poolSize = static_cast<size_t>(config.thread_pool.max_threads);
    ThreadPool workerPool(poolSize);
    EventProcessor eventProcessor(eventBus, storageEngine, &workerPool);
    eventProcessor.init();
    eventProcessor.start();
    tcpServer.start();

    // Install signal handlers to allow graceful shutdown (Ctrl+C)
    static std::atomic<bool> g_running{true};
    auto handle_signal = [](int){ g_running.store(false); };
    std::signal(SIGINT, +[](int){ g_running.store(false); });
    std::signal(SIGTERM, +[](int){ g_running.store(false); });
    
    // Wait until signal is received
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    spdlog::info("Shutdown requested, stopping services...");
    spdlog::info("EventStreamCore End !!!");
    return 0;
}