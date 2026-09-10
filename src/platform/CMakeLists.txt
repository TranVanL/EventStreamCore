cmake_minimum_required(VERSION 3.10)

add_library(eventstream_platform STATIC
    linux/linux_thread.cpp
    linux/linux_mutex.cpp
    linux/linux_condvar.cpp
    linux/linux_semaphore.cpp
    linux/linux_timer.cpp
    linux/linux_channel.cpp
)

target_include_directories(eventstream_platform
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(eventstream_platform
    PUBLIC
        pthread
        rt
)

target_compile_features(eventstream_platform
    PUBLIC
        cxx_std_17
)

target_compile_definitions(eventstream_platform
    PRIVATE
        _GNU_SOURCE
)