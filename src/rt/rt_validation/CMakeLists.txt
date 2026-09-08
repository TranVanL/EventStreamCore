add_executable(cyclictest_runner
    cyclictest_runner.cpp
)

target_link_libraries(cyclictest_runner
    PRIVATE
        eventstream_rt
)

target_compile_features(cyclictest_runner
    PRIVATE
        cxx_std_17
)
