# =============================================================================
# EventStreamCore - LAYER 2: DISTRIBUTED
# Raft consensus and cluster management
# =============================================================================
cmake_minimum_required(VERSION 3.10)

# Collect all source files
file(GLOB_RECURSE DISTRIBUTED_SOURCES
    "*.cpp"
)

# Create the distributed library
add_library(eventstream_distributed STATIC ${DISTRIBUTED_SOURCES})

target_include_directories(eventstream_distributed
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(eventstream_distributed
    PUBLIC
        eventstream_core
        spdlog::spdlog
)

target_compile_features(eventstream_distributed PUBLIC cxx_std_17)
