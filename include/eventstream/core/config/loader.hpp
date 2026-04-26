#pragma once
#include <eventstream/core/config/config.hpp>
#include <string>

class ConfigLoader {
public:
    static AppConfig::AppConfiguration loadConfig(const std::string& filepath); 
};