// ============================================================================
// CONFIG LOADER UNIT TESTS
// ============================================================================
// Tests for YAML configuration loading and validation
// ============================================================================

#include <gtest/gtest.h>
#include <eventstream/core/config/loader.hpp>
#include <eventstream/core/config/app_config.hpp>

// ============================================================================
// SUCCESSFUL LOADING TESTS
// ============================================================================

TEST(ConfigLoader, LoadValidConfiguration) {
    AppConfig::AppConfiguration config = ConfigLoader::loadConfig("config/config.yaml");
    
    // Verify basic app info
    EXPECT_EQ(config.app_name, "EventStreamCore");
    EXPECT_EQ(config.version, "1.0.0");
    
    // Verify ingestion config
    EXPECT_EQ(config.ingestion.udpConfig.host, "127.0.0.1");
    EXPECT_EQ(config.ingestion.udpConfig.port, 9001);
}

// ============================================================================
// ERROR HANDLING TESTS
// ============================================================================

TEST(ConfigLoader, ThrowsOnFileNotFound) {
    EXPECT_THROW(
        ConfigLoader::loadConfig("config/non_existent.yaml"),
        std::runtime_error
    );
}

TEST(ConfigLoader, ThrowsOnMissingRequiredField) {
    EXPECT_THROW(
        ConfigLoader::loadConfig("unittest/invalidConfig/missing_field.yaml"),
        std::runtime_error
    );
}

TEST(ConfigLoader, ThrowsOnInvalidFieldType) {
    EXPECT_THROW(
        ConfigLoader::loadConfig("unittest/invalidConfig/invalid_type.yaml"),
        std::runtime_error
    );
}

TEST(ConfigLoader, ThrowsOnInvalidFieldValue) {
    EXPECT_THROW(
        ConfigLoader::loadConfig("unittest/invalidConfig/invalid_value.yaml"),
        std::runtime_error
    );
}