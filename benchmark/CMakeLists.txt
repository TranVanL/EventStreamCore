cmake_minimum_required(VERSION 3.10)

# ============================================================================
# EVENTSTREAM CORE BENCHMARK SUITE
# ============================================================================

# All benchmarks link to the unified core library
set(BENCHMARK_LIBS
    eventstream_core
    spdlog::spdlog
    pthread
    stdc++
)

# Lock-Free Dedup Benchmark
add_executable(benchmark_dedup benchmark_dedup.cpp)
target_include_directories(benchmark_dedup PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(benchmark_dedup PRIVATE cxx_std_17)
target_link_libraries(benchmark_dedup PRIVATE ${BENCHMARK_LIBS})

# SPSC Ring Buffer Benchmark
add_executable(benchmark_spsc_detailed benchmark_spsc_detailed.cpp)
target_include_directories(benchmark_spsc_detailed PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(benchmark_spsc_detailed PRIVATE cxx_std_17)
target_link_libraries(benchmark_spsc_detailed PRIVATE ${BENCHMARK_LIBS})

# Event Pool Optimization Benchmark
add_executable(benchmark_event_pool benchmark_event_pool.cpp)
target_include_directories(benchmark_event_pool PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(benchmark_event_pool PRIVATE cxx_std_17)
target_link_libraries(benchmark_event_pool PRIVATE ${BENCHMARK_LIBS})

# MPSC Lock-Free Queue Benchmark
add_executable(benchmark_mpsc benchmark_mpsc.cpp)
target_include_directories(benchmark_mpsc PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(benchmark_mpsc PRIVATE cxx_std_17)
target_link_libraries(benchmark_mpsc PRIVATE ${BENCHMARK_LIBS})

# EventBusMulti Performance Benchmark
add_executable(benchmark_eventbus_multi benchmark_eventbus_multi.cpp)
target_include_directories(benchmark_eventbus_multi PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(benchmark_eventbus_multi PRIVATE cxx_std_17)
target_link_libraries(benchmark_eventbus_multi PRIVATE ${BENCHMARK_LIBS})

# Comprehensive Benchmark Summary
add_executable(benchmark_summary benchmark_summary.cpp)
target_include_directories(benchmark_summary PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(benchmark_summary PRIVATE cxx_std_17)
target_link_libraries(benchmark_summary PRIVATE ${BENCHMARK_LIBS})
