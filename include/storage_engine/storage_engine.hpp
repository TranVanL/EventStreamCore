#pragma once
#include "event/Event.hpp"
#include <string> 
#include <mutex>
#include <fstream>

class StorageEngine {
public:
    StorageEngine(const std::string& storagePath);
    ~StorageEngine();

    void storeEvent(const EventStream::Event& event);
    bool retrieveEvent(uint64_t eventId, EventStream::Event& event);

private:
    std::ofstream storageFile;
    std::mutex storageMutex;
};