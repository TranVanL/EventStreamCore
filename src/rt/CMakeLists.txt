cmake_minimum_required(VERSION 3.10)

add_library(eventstream_rt STATIC 
    rt_thread.cpp
    rt_mutex.cpp
)

target_include_directories(eventstream_rt
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(eventstream_rt
    PUBLIC
        spdlog::spdlog
        pthread
)

target_compile_features(eventstream_rt
    PUBLIC
        cxx_std_17
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(eventstream_rt
        PRIVATE
            _GNU_SOURCE
    )
endif()