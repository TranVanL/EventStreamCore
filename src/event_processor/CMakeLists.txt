cmake_minimum_required(VERSION 3.20)

add_library(eventprocessor STATIC
    event_processor.cpp
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
    spdlog::spdlog
)
