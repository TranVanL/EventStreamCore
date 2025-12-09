#pragma once 
#include <string>
#include <thread>
#include <atomic>
#include <event/EventBus.hpp>
#include <event/Event.hpp>

namespace Ingest {
    class IngestServer { 
        public:
            IngestServer(EventStream::EventBus& bus) : eventBus(bus) {}
            virtual void start() = 0;
            virtual void stop() = 0;

        protected: 
            virtual void acceptConnections() = 0;

        protected:
            EventStream::EventBus& eventBus;
            
    };


}