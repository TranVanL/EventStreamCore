cmake_minimum_required(VERSION 3.10)
project(EventStreamCore VERSION 0.1 LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 17)
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

include(FetchContent)

# ── spdlog ──────────────────────────────────────────────────────────────────
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    if(EXISTS "${CMAKE_SOURCE_DIR}/../spdlog/CMakeLists.txt")
        message(STATUS "spdlog — using local source tree at ../spdlog")
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        add_subdirectory(${CMAKE_SOURCE_DIR}/../spdlog ${CMAKE_BINARY_DIR}/_deps/spdlog EXCLUDE_FROM_ALL)
    else()
        message(STATUS "spdlog — fetching from GitHub")
        FetchContent_Declare(spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG        v1.12.0)
        FetchContent_MakeAvailable(spdlog)
    endif()
endif()

# ── yaml-cpp ────────────────────────────────────────────────────────────────
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
    message(STATUS "yaml-cpp — fetching from GitHub")
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG        yaml-cpp-0.7.0)
    FetchContent_MakeAvailable(yaml-cpp)
endif()

# ── Google Test ─────────────────────────────────────────────────────────────
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest — fetching from GitHub")
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        release-1.12.1)
    FetchContent_MakeAvailable(googletest)
    set(GTEST_LIBRARIES gtest)
    set(GTEST_MAIN_LIBRARIES gtest_main)
endif()

# Find NUMA library for thread affinity and memory binding
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

add_subdirectory(src/core)
add_subdirectory(src/bridge)
add_subdirectory(benchmark)
add_subdirectory(unittest)

add_executable(EventStreamCore
    src/main.cpp
)


target_link_libraries(EventStreamCore
    PRIVATE
        spdlog::spdlog
        yaml-cpp
        eventstream_core
)

if(WIN32)
    target_link_libraries(EventStreamCore PRIVATE ws2_32)
endif()




