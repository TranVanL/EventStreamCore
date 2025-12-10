#pragma once 
#include <string>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include "event/Dispatcher.hpp"


    class IngestServer { 
        public:
            IngestServer(Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
            virtual void start() = 0;
            virtual void stop() = 0;

        protected: 
            virtual void acceptConnections() = 0;

        protected:
            Dispatcher& dispatcher_;
            
    };


