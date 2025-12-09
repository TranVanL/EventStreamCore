#include <gtest/gtest.h>
#include "config/ConfigLoader.hpp"
#include "config/AppConfig.hpp"

TEST(ConfigLoader, LoadSuccess){
    AppConfig::AppConfiguration config; 
    config = ConfigLoader::loadConfig("config/config.yaml");
    EXPECT_EQ(config.app_name, "EventStreamCore");
    EXPECT_EQ(config.version, "1.0.0");
    EXPECT_EQ(config.ingestion.udpConfig.host, "127.0.0.1");
    EXPECT_EQ(config.ingestion.udpConfig.port, 9001);
}

TEST(ConfigLoader, FileNotFound){
    EXPECT_THROW({
        ConfigLoader::loadConfig("config/non_existent.yaml");
    }, std::runtime_error);
}

TEST(ConfigLoader, MissingField){
    EXPECT_THROW({
        ConfigLoader::loadConfig("Test/invalidConfig/missing_field.yaml");
    }, std::runtime_error);
}

TEST(ConfigLoader, InvalidType){
    EXPECT_THROW({
        ConfigLoader::loadConfig("Test/invalidConfig/invalid_type.yaml");
    }, std::runtime_error);
}

TEST(ConfigLoader, InvalidValue){
    EXPECT_THROW({
        ConfigLoader::loadConfig("Test/invalidConfig/invalid_value.yaml");
    }, std::runtime_error);
}