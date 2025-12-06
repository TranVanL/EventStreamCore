cmake_minimum_required(VERSION 3.20)

add_library(storage STATIC
    storage_engine.cpp
)

target_include_directories(storage
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(storage
  PUBLIC
    spdlog::spdlog
)
