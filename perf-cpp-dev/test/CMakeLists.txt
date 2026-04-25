Include(FetchContent)
FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.14.0
)
FetchContent_MakeAvailable(Catch2)

set(PERF_CPP_TEST
        test/access_benchmark.cpp
        test/config.cpp
        test/counter_definition.cpp
        test/counter_result.cpp
        test/metric.cpp
        test/event_counter.cpp
        test/event_counter_multi_thread.cpp
        test/sampler.cpp
        test/sampler_multi_thread.cpp
        test/hardware_info.cpp
        test/requested_event.cpp
)

add_executable(tests ${PERF_CPP_TEST})
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain perf-cpp)
target_include_directories(tests PRIVATE Catch2::Catch2WithMain test)
set_target_properties(tests
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)