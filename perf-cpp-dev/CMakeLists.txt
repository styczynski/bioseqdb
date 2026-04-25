cmake_minimum_required(VERSION 3.10)
project(libperf-cpp VERSION 0.13.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wconversion -Wcast-align -Wunused -Wshadow -Wold-style-cast -Wpointer-arith -Wcast-qual -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wno-missing-braces")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()
message("Building Mode                        = ${CMAKE_BUILD_TYPE}")

# Include include/ folder.
include_directories(include/)

#############################################################
#   Options                                                 #
#############################################################
option(BUILD_EXAMPLES "Build examples in examples/ directory." OFF)
option(BUILD_LIB_SHARED "Build the library as a shared library (default is static)." OFF)
option(BUILD_TESTS "Build the unit tests." OFF)
option(GEN_PROCESSOR_EVENTS "Generate processor-specific events" OFF)

message("Building as Shared Library           = ${BUILD_LIB_SHARED}")
message("Building Examples                    = ${BUILD_EXAMPLES}")
message("Building Tests                       = ${BUILD_TESTS}")
message("Generating Processor-specific Events = ${GEN_PROCESSOR_EVENTS}")

#############################################################
#   perf-cpp source files                                   #
#############################################################
set(PERF_CPP_SRC
    src/config.cpp
    src/counter_definition.cpp
    src/event_counter.cpp
    src/event_file_descriptor_parser.cpp
    src/exception.cpp
    src/hardware_info.cpp
    src/sampler.cpp
    src/counter/counter.cpp
    src/counter/event_provider.cpp
    src/counter/group.cpp
    src/counter/requested_event.cpp
    src/counter/result.cpp
    src/sample/decoder.cpp
    src/sample/trigger.cpp
    src/sample/mmap_buffer.cpp
    src/sample/recording_values.cpp
    src/sample/record_file_writer.cpp
    src/sample/result.cpp
    src/metric/expression/token.cpp
    src/metric/expression/tokenizer.cpp
    src/metric/expression/parser.cpp
    src/metric/expression/function.cpp
    src/metric/expression/expression.cpp
    src/analyzer/memory_access.cpp
    src/analyzer/flame_graph_generator.cpp
    src/util/callchain_trie.cpp
    src/util/symbol_resolver.cpp
    src/util/table.cpp
)

#############################################################
#   Generated source file with processor-specific           #
#   events.                                                 #
#############################################################
if (GEN_PROCESSOR_EVENTS)
    set(PROCESSOR_EVENT_SRC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/processor_specific_event_provider.cpp")

    # Execute the Python script to generate the file
    execute_process(
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/script/generate_processor_event_provider.py"
        --output "${PROCESSOR_EVENT_SRC_FILE}" --events "${CMAKE_CURRENT_SOURCE_DIR}/events" --verbose
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE SCRIPT_RESULT
    )

    # Verify the file was created; if yes, add the file and hint perf-cpp that this event provider
    # should be included into the default CounterDefinition.
    if(EXISTS "${PROCESSOR_EVENT_SRC_FILE}")
        set(PERF_CPP_SRC ${PERF_CPP_SRC} "${PROCESSOR_EVENT_SRC_FILE}")
        add_compile_definitions(PERFCPP_HAS_PROCESSOR_SPECIFIC_EVENTS)
    endif()
endif()

#############################################################
#   perf-cpp library definition                             #
#############################################################
if (BUILD_LIB_SHARED)
    add_library(perf-cpp SHARED ${PERF_CPP_SRC})
else ()
    add_library(perf-cpp STATIC ${PERF_CPP_SRC})
endif ()

#############################################################
#   clang-tidy integration                                  #
#############################################################
find_program(CLANG_TIDY NAMES "clang-tidy")
if(CLANG_TIDY)
    set(CLANG_TIDY_COMMAND "${CLANG_TIDY}" "--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
    set_target_properties(perf-cpp PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
else()
    message(WARNING "clang-tidy not found")
endif()

#############################################################
#   Examples definition                                     #
#############################################################
if(BUILD_EXAMPLES)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/bin)
    include(examples/CMakeLists.txt)
endif()

#############################################################
#   Unit tests definition                                   #
#############################################################
if(BUILD_TESTS)
    include(test/CMakeLists.txt)
endif()

#############################################################
#   Custom target to build documentation website            #
#############################################################
## To preview the documentation locally, run: .venv/bin/mkdocs serve
find_program(MKDOCS_EXECUTABLE mkdocs HINTS ${CMAKE_SOURCE_DIR}/.venv/bin)
if(MKDOCS_EXECUTABLE)
    add_custom_target(docs
            COMMAND ${MKDOCS_EXECUTABLE} build -f ${CMAKE_SOURCE_DIR}/mkdocs.yml -d ${CMAKE_SOURCE_DIR}/site
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Building documentation website into site/"
    )
endif()

#############################################################
#   Custom target to extract hardware events into CSV file  #
#############################################################
add_custom_target(perf-list python3 ${CMAKE_SOURCE_DIR}/script/create_perf_list.py)

#############################################################
#   Rules for installing the library                        #
#############################################################
if(PROJECT_IS_TOP_LEVEL)
    include(CPack)
    set(CMAKE_INSTALL_INCLUDEDIR include CACHE PATH "")
    set(CMAKE_INSTALL_LIBRARY_DIR bin CACHE PATH "")
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# Allow package maintainers to freely override the path for the configs
set(package perf-cpp)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT perf-cpp)
install(TARGETS perf-cpp EXPORT perf-cppTargets  DESTINATION "${CMAKE_INSTALL_LIBRARY_DIR}")

write_basic_package_version_file(
    "${package}ConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

set(
    perf-cpp_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/${package}"
    CACHE PATH "CMake package config location relative to the install prefix"
)
mark_as_advanced(perf-cpp_INSTALL_CMAKEDIR)

file(
    WRITE
    "${PROJECT_BINARY_DIR}/${package}Config.cmake"
    "include(\"${CMAKE_INSTALL_PREFIX}/${perf-cpp_INSTALL_CMAKEDIR}/${package}Targets.cmake\")"
)

install(
    FILES "${PROJECT_BINARY_DIR}/${package}Config.cmake"
    DESTINATION "${perf-cpp_INSTALL_CMAKEDIR}"
    COMPONENT perf-cpp
)

install(
    FILES "${PROJECT_BINARY_DIR}/${package}ConfigVersion.cmake"
    DESTINATION "${perf-cpp_INSTALL_CMAKEDIR}"
    COMPONENT perf-cpp
)

install(
    EXPORT perf-cppTargets
    NAMESPACE perf-cpp::
    DESTINATION "${perf-cpp_INSTALL_CMAKEDIR}"
    COMPONENT perf-cpp
)