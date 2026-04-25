cmake_minimum_required(VERSION 3.15)
project(test_perf_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(perf-cpp REQUIRED CONFIG)

add_executable(test_perf_cpp src/example.cpp)
target_link_libraries(test_perf_cpp PRIVATE perf-cpp::perf-cpp)
