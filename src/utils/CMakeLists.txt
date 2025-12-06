cmake_minimum_required(VERSION 3.20)

add_library(utils STATIC
    thread_pool.cpp
)

target_include_directories(utils
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
