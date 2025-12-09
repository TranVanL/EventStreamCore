#include "config/ConfigLoader.hpp"
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>
#include <stdexcept>
#include <fstream>

static void ValidateNodeExists(const YAML::Node& node, const std::string& key) {
    if (!node[key]) {
        spdlog::error("Missing required configuration key: {}", key);
        throw std::runtime_error("Missing configuration key: " + key);
    }
}

static AppConfig::TCPConfig parseTCPConfig(const YAML::Node& node) {
    AppConfig::TCPConfig config;
    config.host = node["host"].as<std::string>();
    config.port = node["port"].as<int>();
    config.enable = node["enable"].as<bool>(false);
    config.maxConnections = node["maxConnections"].as<int>();
    return config;
}

static AppConfig::UDPConfig parseUDPConfig(const YAML::Node& node) {
    AppConfig::UDPConfig config;
    config.host = node["host"].as<std::string>();
    config.port = node["port"].as<int>();
    config.enable = node["enable"].as<bool>(false);
    config.bufferSize = node["bufferSize"].as<int>();
    return config;
}

static AppConfig::FileConfig parseFileConfig(const YAML::Node& node) {
    AppConfig::FileConfig config;
    config.path = node["path"].as<std::string>();
    config.enable = node["enable"].as<bool>(false);
    config.poll_interval_ms = node["poll_interval_ms"].as<int>();
    return config;
}

static AppConfig::BroadCastPushConfig parseBroadCastPushConfig(const YAML::Node& node) {
    AppConfig::BroadCastPushConfig config;
    config.host = node["host"].as<std::string>();
    config.port = node["port"].as<int>();
    config.enable = node["enable"].as<bool>(false);
    return config;
}


AppConfig::AppConfiguration ConfigLoader::loadConfig(const std::string& filepath){
    spdlog::info("Loading configuration from {}", filepath);

    if (!std::ifstream(filepath)) {
        spdlog::error("Configuration file not found: {}", filepath);
        throw std::runtime_error("Configuration file not found: " + filepath);
    }

    YAML::Node root;

    try {
        root = YAML::LoadFile(filepath);
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to load configuration file: {}", e.what());
        throw;
    }
    /* Name and Version */
    AppConfig::AppConfiguration config;
    ValidateNodeExists(root, "app_name");
    ValidateNodeExists(root, "version");
    config.app_name = root["app_name"].as<std::string>();
    config.version = root["version"].as<std::string>();

    /* Ingestion Config */
    ValidateNodeExists(root, "ingestion");
    config.ingestion.tcpConfig = parseTCPConfig(root["ingestion"]["tcp"]);
    config.ingestion.udpConfig = parseUDPConfig(root["ingestion"]["udp"]);
    config.ingestion.fileConfig = parseFileConfig(root["ingestion"]["file"]);

    /* Router Config */
    ValidateNodeExists(root, "router");
    config.router.shards = root["router"]["shards"].as<int>();
    config.router.strategy = root["router"]["strategy"].as<std::string>();
    config.router.buffer_size = root["router"]["buffer_size"].as<int>();

    /* Rule Engine */ 
    ValidateNodeExists(root, "rule_engine");
    config.rule_engine.enable_cache = root["rule_engine"]["enable_cache"].as<bool>(false);
    config.rule_engine.rules_file = root["rule_engine"]["rules_file"].as<std::string>();
    config.rule_engine.threads = root["rule_engine"]["threads"].as<int>();
    config.rule_engine.cache_size = root["rule_engine"]["cache_size"].as<int>();

    /* Storage Config */
    ValidateNodeExists(root, "storage");
    config.storage.backend = root["storage"]["backend"].as<std::string>();
    config.storage.path = root["storage"]["sqlite_path"].as<std::string>();

    /* Python Config */
    ValidateNodeExists(root, "python_integration");
    config.python.enable = root["python_integration"]["enable"].as<bool>(false);
    config.python.script_path = root["python_integration"]["script_path"].as<std::string>();

    /* Broadcast Config */
    ValidateNodeExists(root, "broadcast");
    ValidateNodeExists(root["broadcast"], "tcp_push");
    ValidateNodeExists(root["broadcast"], "websocket_push");
    config.broadcast.tcp_push = parseBroadCastPushConfig(root["broadcast"]["tcp_push"]);
    config.broadcast.websocket_push = parseBroadCastPushConfig(root["broadcast"]["websocket_push"]);

    /* Plugins */
    if (root["plugins"] && root["plugins"]["load"]) {
        for (const auto& pluginNode : root["plugins"]["load"]) {
            config.plugin_list.push_back(pluginNode.as<std::string>());
        }
    }

    /*Threads pool_size*/
    ValidateNodeExists(root, "Threads_pool");
    if (root["Threads_pool"]) {
        config.thread_pool.min_threads = root["Threads_pool"]["min_threads"].as<int>();
        config.thread_pool.max_threads = root["Threads_pool"]["max_threads"].as<int>();
    }

    /* Additional Validations */
    if (config.ingestion.tcpConfig.port <=0 || config.ingestion.tcpConfig.port > 65535) {
        spdlog::error("Invalid TCP port number: {}", config.ingestion.tcpConfig.port);
        throw std::runtime_error("Invalid Port Number");
    }

    if (config.ingestion.udpConfig.port <=0 || config.ingestion.udpConfig.port > 65535) {
        spdlog::error("Invalid UDP port number: {}", config.ingestion.udpConfig.port);
        throw std::runtime_error("Invalid Port Number");
    }

    if (config.ingestion.fileConfig.enable && config.ingestion.fileConfig.path.empty()) {
        spdlog::error("File ingestion enabled but path is empty.");
        throw std::runtime_error("Invalid File Ingestion configuration");
    }

    if (config.router.shards <= 0 || config.router.buffer_size <= 0) {
        spdlog::error("Invalid Info Router: {}", config.router.shards);
        throw std::runtime_error("Invalid Info Router");
    }

    if (config.rule_engine.threads <= 0 || config.rule_engine.cache_size < 0) {
        spdlog::error("Invalid Rule Engine configuration: threads={}, cache_size={}", 
                      config.rule_engine.threads, config.rule_engine.cache_size);
        throw std::runtime_error("Invalid Rule Engine configuration");
    }

    if (config.storage.backend != "sqlite" && config.storage.backend != "filesystem") {
        spdlog::error("Unsupported storage backend: {}", config.storage.backend);
        throw std::runtime_error("Unsupported storage backend");
    }

    if (config.python.enable && config.python.script_path.empty()) {
        spdlog::error("Python integration enabled but script path is empty.");
        throw std::runtime_error("Invalid Python configuration");
    }

    if (config.thread_pool.min_threads <= 0 || config.thread_pool.max_threads <= 0 ||
        config.thread_pool.min_threads > config.thread_pool.max_threads) {
        spdlog::error("Invalid Threads Pool configuration: min_threads={}, max_threads={}",
                      config.thread_pool.min_threads, config.thread_pool.max_threads);
        throw std::runtime_error("Invalid Threads Pool configuration");
    }

    spdlog::info("Configuration loaded successfully.");
    return config;
}

