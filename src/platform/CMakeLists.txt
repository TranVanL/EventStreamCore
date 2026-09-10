cmake_minimum_required(VERSION 3.10)

add_library(eventstream_platform STATIC)

if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
    target_sources(eventstream_platform
        PRIVATE
            qnx/qnx_thread.cpp
            qnx/qnx_mutex.cpp
            qnx/qnx_condvar.cpp
            qnx/qnx_semaphore.cpp
            qnx/qnx_timer.cpp
            qnx/qnx_channel.cpp
    )
else()
    target_sources(eventstream_platform
        PRIVATE
            linux/linux_thread.cpp
            linux/linux_mutex.cpp
            linux/linux_condvar.cpp
            linux/linux_semaphore.cpp
            linux/linux_timer.cpp
            linux/linux_channel.cpp
    )
endif()

target_include_directories(eventstream_platform
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
    target_link_libraries(eventstream_platform PUBLIC pthread)
else()
    target_link_libraries(eventstream_platform PUBLIC pthread rt)
endif()

target_compile_features(eventstream_platform
    PUBLIC
        cxx_std_17
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX")
    target_compile_definitions(eventstream_platform PRIVATE _GNU_SOURCE)
endif()