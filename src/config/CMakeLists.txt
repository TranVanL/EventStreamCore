cmake_minimum_required(VERSION 3.20)

add_library(config STATIC ConfigLoader.cpp)

target_include_directories(config
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(config
  PUBLIC
    yaml-cpp
    spdlog::spdlog
)
