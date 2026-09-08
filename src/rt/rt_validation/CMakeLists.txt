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

add_executable(priority_inversion_demo
    priority_inversion_demo.cpp
)

target_link_libraries(priority_inversion_demo
    PRIVATE
        eventstream_rt
)

target_compile_features(priority_inversion_demo
    PRIVATE
        cxx_std_17
)
