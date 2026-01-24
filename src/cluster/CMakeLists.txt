cmake_minimum_required(VERSION 3.20)

add_library(cluster STATIC
    raft.cpp
)

target_include_directories(cluster
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(cluster
  PUBLIC
    spdlog::spdlog
)
