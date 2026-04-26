cmake_minimum_required(VERSION 3.10)

add_library(esccore_bridge STATIC
    esccore.cpp
)

target_include_directories(esccore_bridge
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(esccore_bridge
    PUBLIC
        eventstream_core
        spdlog::spdlog
        yaml-cpp
)

target_compile_features(esccore_bridge PUBLIC cxx_std_17)

# Also build a shared library that Python/Go can load
add_library(esccore SHARED
    esccore.cpp
)

target_include_directories(esccore
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(esccore
    PUBLIC
        eventstream_core
        spdlog::spdlog
        yaml-cpp
)

if(WIN32)
    target_link_libraries(esccore PRIVATE ws2_32)
endif()

target_compile_features(esccore PUBLIC cxx_std_17)

# Ensure the shared library exports C symbols properly
set_target_properties(esccore PROPERTIES
    OUTPUT_NAME "esccore"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
)
