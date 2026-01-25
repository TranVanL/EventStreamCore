cmake_minimum_required(VERSION 3.20)
project(EventStreamCore VERSION 0.1 LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


option(ENABLE_DEBUG "Enable debug mode" ON)
if(ENABLE_DEBUG)
    message(STATUS "Debug mode enabled")
    add_definitions(-DDEBUG)
    set(CMAKE_BUILD_TYPE Debug)
else()
    message(STATUS "Release mode enabled")
    set(CMAKE_BUILD_TYPE Release)
endif()

# Use spdlog header-only mode to avoid multiple definition errors
set(SPDLOG_BUILD_SHARED OFF)

# For MinGW: Allow multiple weak symbol definitions (spdlog header-only mode creates these)
if(MINGW)
    add_link_options(-Wl,--allow-multiple-definition)
endif()

# Dependencies
#find_package(Boost 1.75 REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(GTest REQUIRED)

# Day 38: Find NUMA library for thread affinity and memory binding
find_package(numa QUIET)
if(NOT numa_FOUND)
    # Try pkg-config as fallback
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(NUMA numa)
    endif()
endif()

if(NUMA_FOUND)
    message(STATUS "NUMA library found: ${NUMA_LIBRARIES}")
    include_directories(${NUMA_INCLUDE_DIRS})
else()
    # Try manual find
    find_library(NUMA_LIBRARY numa)
    if(NUMA_LIBRARY)
        message(STATUS "NUMA library found (manual): ${NUMA_LIBRARY}")
        set(NUMA_LIBRARIES ${NUMA_LIBRARY})
    else()
        message(WARNING "NUMA library not found - thread binding will be limited")
        set(NUMA_LIBRARIES "")
    endif()
endif()


# Export project's include dir to consumers
include_directories(${PROJECT_SOURCE_DIR}/include)

# Add modules
add_subdirectory(src/config)
add_subdirectory(src/event)
add_subdirectory(src/ingest)
add_subdirectory(src/storage_engine)
add_subdirectory(src/event_processor)
add_subdirectory(src/utils)
add_subdirectory(src/metrics)
add_subdirectory(src/control)
add_subdirectory(src/admin)
add_subdirectory(src/cluster)
add_subdirectory(benchmark)
add_subdirectory(unittest)

add_executable(EventStreamCore
    src/app/main.cpp
)


target_link_libraries(EventStreamCore
    PRIVATE
        
        spdlog::spdlog
        yaml-cpp
        events
        config
        ingest
        storage
        eventprocessor
        utils
        metrics
        control
        admin
)




