cmake_minimum_required(VERSION 3.14)

project(simdparse VERSION 0.1)

option(SIMDPARSE_USE_AVX2 "Use AVX2 instruction set" ON)

# library target configuration
file(GLOB SIMDPARSE_LIBRARY_SOURCES
    ${CMAKE_SOURCE_DIR}/test/*.cpp
)
file(GLOB SIMDPARSE_LIBRARY_HEADERS
    ${CMAKE_SOURCE_DIR}/include/simdparse/*.hpp
)
add_library(simdparse INTERFACE)

# compiler options
target_compile_features(simdparse INTERFACE cxx_std_17)
set_property(TARGET simdparse PROPERTY VISIBILITY_INLINES_HIDDEN ON)
set_property(TARGET simdparse PROPERTY PUBLIC_HEADER ${SIMDPARSE_LIBRARY_HEADERS})

if(SIMDPARSE_USE_AVX2)
    if(MSVC)
        set(SIMDPARSE_AVX2_COMPILE "/arch:AVX2")
    else()
        set(SIMDPARSE_AVX2_COMPILE "-mavx2")
    endif()
else()
    set(SIMDPARSE_AVX2_COMPILE "")
endif()

if(MSVC)
    target_compile_definitions(simdparse INTERFACE _CRT_SECURE_NO_WARNINGS)
    # set warning level 4 and treat all warnings as errors
    target_compile_options(simdparse INTERFACE ${SIMDPARSE_AVX2_COMPILE} /permissive- /W4 /WX /Zc:__cplusplus,enumTypes,lambda,referenceBinding,rvalueCast,strictStrings,ternary)
else()
    # enable lots of warnings and treat all warnings as errors
    target_compile_options(simdparse INTERFACE ${SIMDPARSE_AVX2_COMPILE} -Wall -Wextra -pedantic -Werror -Wfatal-errors)
endif()

# include directories
target_include_directories(simdparse INTERFACE ${CMAKE_SOURCE_DIR}/include)

# build configuration
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_result)
if(ipo_result)
    set_property(TARGET simdparse PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# test target configuration
add_executable(simdparse-tests ${SIMDPARSE_LIBRARY_SOURCES})
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT simdparse-tests)
target_link_libraries(simdparse-tests simdparse)

# install configuration
include(GNUInstallDirs)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/simdparse
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)
