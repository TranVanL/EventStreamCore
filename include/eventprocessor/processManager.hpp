#include "eventprocessor/event_processor.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

class ProcessManager {
public:     
    ProcessManager(EventStream::EventBusMulti& bus) 
        : event_bus(bus), 
          isRunning_(false),
          realtimeProcessor_(std::make_unique<RealtimeProcessor>()),
          transactionalProcessor_(std::make_unique<TransactionalProcessor>()),
          batchProcessor_(std::make_unique<BatchProcessor>()) {}
    ~ProcessManager() noexcept {
        spdlog::info("[DESTRUCTOR] ProcessManager being destroyed...");
        stop();
        spdlog::info("[DESTRUCTOR] ProcessManager destroyed successfully");
    }

    void stop();
    void start(); 

    void runLoop(const EventStream::EventBusMulti::QueueId& qid, EventProcessor* processor);
    


private: 
    EventStream::EventBusMulti& event_bus;
    std::atomic<bool> isRunning_;

    std::unique_ptr<RealtimeProcessor> realtimeProcessor_;
    std::unique_ptr<TransactionalProcessor> transactionalProcessor_;
    std::unique_ptr<BatchProcessor> batchProcessor_;

    std::thread realtimeThread_;
    std::thread transactionalThread_;
    std::thread batchThread_;
};