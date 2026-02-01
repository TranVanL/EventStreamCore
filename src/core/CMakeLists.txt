# =============================================================================
# EventStreamCore - LAYER 1: CORE
# Ultra-low latency event processing engine
# =============================================================================
cmake_minimum_required(VERSION 3.10)

# Collect all source files from submodules
file(GLOB_RECURSE CORE_SOURCES
    "admin/*.cpp"
    "config/*.cpp"
    "control/*.cpp"
    "events/*.cpp"
    "ingest/*.cpp"
    "metrics/*.cpp"
    "processor/*.cpp"
    "queues/*.cpp"
    "storage/*.cpp"
    "utils/*.cpp"
)

# Create the core library
add_library(eventstream_core STATIC ${CORE_SOURCES})

target_include_directories(eventstream_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(eventstream_core
    PUBLIC
        spdlog::spdlog
        yaml-cpp
        ${NUMA_LIBRARIES}
)

# Link Windows socket library on Windows
if(WIN32)
    target_link_libraries(eventstream_core PUBLIC ws2_32)
endif()

# Compiler features
target_compile_features(eventstream_core PUBLIC cxx_std_17)

# Optimization flags for release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(eventstream_core PRIVATE -O3 -march=native)
endif()
