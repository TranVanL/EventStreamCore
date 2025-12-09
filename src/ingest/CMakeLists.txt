cmake_minimum_required(VERSION 3.20)

add_library(ingest STATIC
    tcp_parser.cpp
    tcpingest_server.cpp
)

target_include_directories(ingest
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

if (WIN32)
    target_link_libraries(ingest PRIVATE ws2_32)
endif()


target_link_libraries(ingest
  PUBLIC
    events
    spdlog::spdlog
)
