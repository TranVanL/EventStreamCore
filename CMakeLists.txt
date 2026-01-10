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



# Dependencies
#find_package(Boost 1.75 REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(GTest REQUIRED)


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




