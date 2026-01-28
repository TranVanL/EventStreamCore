add_library(admin OBJECT
    AdminLoop.cpp
)

target_include_directories(admin PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(admin PUBLIC
    spdlog::spdlog
    metrics
    eventprocessor
    control
)
