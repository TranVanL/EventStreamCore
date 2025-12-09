#pragma once
#include "config/AppConfig.hpp"
#include <string>

class ConfigLoader {
public:
    static AppConfig::AppConfiguration loadConfig(const std::string& filepath); 

};