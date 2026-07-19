cmake_minimum_required(VERSION 3.10)
project(Fastest-Bloom-Filter)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Reset option to default value every time CMake is run
unset(USE_AVX512 CACHE)
option(USE_AVX512 "Enable AVX-512 optimizations" OFF)

unset(USE_AVX2 CACHE)
option(USE_AVX2 "Enable AVX2 optimizations" ON)

if(USE_AVX512)
    message(STATUS "AVX-512 optimizations enabled")
else()
    message(STATUS "AVX-512 optimizations disabled")
endif()

# Conditionally add AVX-512 compiler flags
if(USE_AVX512)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f")
endif()

# Conditionally add AVX2 compiler flags
if (USE_AVX2)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
endif()

# Set -O1 optimization for Release mode
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# Add include directories
include_directories(include)

# Create the executable
add_executable(main_benchmark src/main_benchmark.cpp)