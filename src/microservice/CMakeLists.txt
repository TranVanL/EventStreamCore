# =============================================================================
# EventStreamCore - LAYER 3: MICROSERVICE
# gRPC gateway and external integrations
# =============================================================================
cmake_minimum_required(VERSION 3.10)

# Collect all source files
file(GLOB_RECURSE MICROSERVICE_SOURCES
    "*.cpp"
)

# Create the microservice library
add_library(eventstream_microservice STATIC ${MICROSERVICE_SOURCES})

target_include_directories(eventstream_microservice
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(eventstream_microservice
    PUBLIC
        eventstream_core
        eventstream_distributed
        spdlog::spdlog
)

target_compile_features(eventstream_microservice PUBLIC cxx_std_17)

# Optional: gRPC support (uncomment when needed)
# find_package(gRPC CONFIG)
# if(gRPC_FOUND)
#     target_link_libraries(eventstream_microservice PUBLIC gRPC::grpc++)
# endif()
