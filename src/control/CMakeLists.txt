add_library(control
    PipelineState.cpp
)

target_include_directories(control PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(control PRIVATE spdlog::spdlog)
