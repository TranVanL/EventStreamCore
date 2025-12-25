add_library(metrics OBJECT
    metricRegistry.cpp
    metricReporter.cpp
)

target_include_directories(metrics PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(metrics PUBLIC
    spdlog::spdlog
)
