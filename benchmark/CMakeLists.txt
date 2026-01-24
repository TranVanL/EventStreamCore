cmake_minimum_required(VERSION 3.20)

# Original benchmark
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

# Day 31 Baseline Benchmark
add_executable(benchmark_baseline
    benchmark_main.cpp
)

target_include_directories(benchmark_baseline PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(benchmark_baseline PRIVATE cxx_std_17)

target_link_libraries(benchmark_baseline PRIVATE
    pthread
)

# Day 32 SPSC Profiling Benchmark
add_executable(benchmark_spsc
    benchmark_spsc.cpp
)

target_include_directories(benchmark_spsc PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(benchmark_spsc PRIVATE cxx_std_17)

target_link_libraries(benchmark_spsc PRIVATE
    pthread
)

# Day 35 Lock-Free Dedup Benchmark
add_executable(benchmark_dedup
    benchmark_dedup.cpp
)

target_include_directories(benchmark_dedup PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(benchmark_dedup PRIVATE cxx_std_20)

target_link_libraries(benchmark_dedup PRIVATE
    utils
    spdlog::spdlog
    fmt::fmt
    pthread
)

# Day 35 SPSC Detailed Benchmark
add_executable(benchmark_spsc_detailed
    benchmark_spsc_detailed.cpp
)

target_include_directories(benchmark_spsc_detailed PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(benchmark_spsc_detailed PRIVATE cxx_std_20)

target_link_libraries(benchmark_spsc_detailed PRIVATE
    utils
    events
    pthread
)