#pragma once

#include <string>
#include <vector>


namespace AppConfig {

    struct TCPConfig 
    {
        std::string host;
        int port;
        bool enable = false;
        int maxConnections;
    };

    struct UDPConfig 
    {
        std::string host;
        int port;
        bool enable = false;
        int bufferSize;
    };

    struct FileConfig
    {
        std::string path;
        bool enable = false;
        int poll_interval_ms;

    };

    struct IngestionConfig
    {
        TCPConfig tcpConfig;
        UDPConfig udpConfig;
        FileConfig fileConfig;
    };  
    
    struct Router
    {
        int shards;
        std::string strategy;
        int buffer_size;

    };

    struct Rule_Engine
    {
        bool enable_cache = false;
        std::string rules_file;
        int threads;
        int cache_size;
    };

    struct StorageConfig
    {
        std::string backend;
        std::string path;
    };

    struct PythonConfig
    {
        bool enable = false;
        std::string script_path;
    };
    
    struct BroadCastPushConfig
    {
        bool enable = false;
        std::string host;
        int port;
    };

    struct BoardCastConfig
    {
        BroadCastPushConfig tcp_push;
        BroadCastPushConfig websocket_push;
    };

    struct ThreadsPoolConfig
    {
        int min_threads;
        int max_threads;
    };

    struct AppConfiguration 
    {
        std::string app_name;
        std::string version;

        IngestionConfig ingestion;
        Router router;
        Rule_Engine rule_engine;
        StorageConfig storage;
        PythonConfig python;
        BoardCastConfig broadcast;
        ThreadsPoolConfig thread_pool;
        std::vector<std::string> plugin_list;
    };
    
    
    


   
}