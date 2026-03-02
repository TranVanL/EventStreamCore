#pragma once

#include <string>
#include <vector>


namespace AppConfig {

struct TCPConfig {
    std::string host;
    int port = 0;
    bool enable = false;
    int maxConnections = 100;
};

struct UDPConfig {
    std::string host;
    int port = 0;
    bool enable = false;
    int bufferSize = 65536;
};

struct FileConfig {
    std::string path;
    bool enable = false;
    int poll_interval_ms = 1000;
};

struct IngestionConfig {
    TCPConfig  tcpConfig;
    UDPConfig  udpConfig;
    FileConfig fileConfig;
};

struct Router {
    int shards = 1;
    std::string strategy;
    int buffer_size = 4096;
};

struct RuleEngine {
    bool enable_cache = false;
    std::string rules_file;
    int threads = 1;
    int cache_size = 0;
};

struct StorageConfig {
    std::string backend;
    std::string path;
};

struct PythonConfig {
    bool enable = false;
    std::string script_path;
};

struct BroadCastPushConfig {
    bool enable = false;
    std::string host;
    int port = 0;
};

struct BroadcastConfig {
    BroadCastPushConfig tcp_push;
    BroadCastPushConfig websocket_push;
};

struct ThreadsPoolConfig {
    int min_threads = 2;
    int max_threads = 8;
};

/// Bind threads and memory to specific NUMA nodes for optimal performance.
struct NUMAConfig {
    bool enable = false;
    int dispatcher_node = -1;           ///< -1 = no binding
    int ingest_node = -1;
    int realtime_proc_node = -1;
    int transactional_proc_node = -1;
    int batch_proc_node = -1;
};

struct AppConfiguration {
    std::string app_name;
    std::string version;

    IngestionConfig  ingestion;
    Router           router;
    RuleEngine       rule_engine;
    StorageConfig    storage;
    PythonConfig     python;
    BroadcastConfig  broadcast;
    ThreadsPoolConfig thread_pool;
    NUMAConfig       numa;
    std::vector<std::string> plugin_list;
};

} // namespace AppConfig