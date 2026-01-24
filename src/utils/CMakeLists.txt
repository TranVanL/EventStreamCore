cmake_minimum_required(VERSION 3.20)

add_library(utils STATIC
    thread_pool.cpp
    thread_affinity.cpp
    spsc_ringBuffer.cpp
    lock_free_dedup.cpp
)

target_include_directories(utils
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
