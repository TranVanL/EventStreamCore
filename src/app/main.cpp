#include <spdlog/spdlog.h>
#include <iostream>

int main( int argc, char* argv[] ) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("EventStreamCore version 1.0.0 starting up...");
    spdlog::info("Build date: {}", __DATE__ , __TIME__);

    if (argc > 1)
        spdlog::info("Config File for Backend Engine: {}", argv[1]);
    else 
        spdlog::warn("No config file provided, using default settings.");
    
    spdlog::info("Initialization complete. Running main application...");
    return 0;
}