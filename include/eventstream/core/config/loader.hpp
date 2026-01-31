#pragma once
#include <eventstream/core/config/app_config.hpp>
#include <string>

class ConfigLoader {
public:
    static AppConfig::AppConfiguration loadConfig(const std::string& filepath); 
};