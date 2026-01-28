cmake_minimum_required(VERSION 3.20)

add_library(eventprocessor STATIC
    realtime_processor.cpp
    transactional_processor.cpp
    batch_processor.cpp
    ProcessManager.cpp
)

target_include_directories(eventprocessor
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(eventprocessor
  PUBLIC
    events
    storage
    utils
    metrics
    spdlog::spdlog
    ${NUMA_LIBRARIES}
)
