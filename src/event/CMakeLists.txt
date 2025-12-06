cmake_minimum_required(VERSION 3.20)

add_library(events STATIC
    EventBus.cpp
    EventFactory.cpp
)

target_include_directories(events
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(events
  PUBLIC
    spdlog::spdlog
)