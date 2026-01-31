cmake_minimum_required(VERSION 3.10)

# ============================================================================
# EVENTSTREAM CORE UNIT TESTS
# ============================================================================

enable_testing()

# Single test executable that contains all unit tests
add_executable(EventStreamTests
    config_loader_test.cpp
    event_test.cpp
    event_processor_test.cpp
    storage_test.cpp
    tcp_ingest_test.cpp
    lock_free_dedup_test.cpp
    raft_test.cpp
)

target_include_directories(EventStreamTests PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(EventStreamTests
    PRIVATE
        eventstream_core
        eventstream_distributed
        eventstream_microservice
        GTest::gtest_main
        spdlog::spdlog
        yaml-cpp
)

include(GoogleTest)
gtest_discover_tests(EventStreamTests)

add_test(NAME EventStreamTests COMMAND EventStreamTests)
