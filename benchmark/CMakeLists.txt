cmake_minimum_required(VERSION 3.20)

add_executable(benchmark
    benchmark.cpp
)

target_include_directories(benchmark PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(benchmark PRIVATE
    ingest
    events              
    storage            
    eventprocessor     
    pthread
   
)
