cmake_minimum_required(VERSION 3.20)

# Test
enable_testing()

# Single test executable that contains all small unit tests
add_executable(EventStreamTests
    ConfigLoaderTest.cpp
    EventTest.cpp
    EventProcessorTest.cpp
    StorageTest.cpp
    TcpingestTest.cpp
)

target_include_directories(EventStreamTests PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(EventStreamTests
    PRIVATE
        ingest
        config
        events
        eventprocessor
        storage
        GTest::gtest_main
        
)

include(GoogleTest)
gtest_discover_tests(EventStreamTests)

add_test(NAME EventStreamTests COMMAND EventStreamTests)
