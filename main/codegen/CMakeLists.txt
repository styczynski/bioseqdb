cmake_minimum_required(VERSION 4.2)

# Isolated host-tool generator project. Built by BioseqCodegen.cmake as an
# ExternalProject with the GCC 16.1 / libstdc++ toolchain so its C and legacy
# C++ dependencies never touch the main Clang module graph. Components are
# selected by BIOSEQ_CODEGEN_ENABLE_EXTERNAL / BIOSEQ_CODEGEN_ENABLE_WEBSITE.

if(NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
  else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  endif()
endif()

project(bioseq_codegen LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
  message(FATAL_ERROR
    "bioseq_codegen requires GCC 16.1 or newer; found "
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at "
    "${CMAKE_CXX_COMPILER}")
endif()

if(NOT BIOSEQ_REPOSITORY_ROOT)
  get_filename_component(BIOSEQ_REPOSITORY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
if(NOT BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY)
  set(BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT LLVM_CONFIG_BINARY)
  set(LLVM_CONFIG_BINARY "/usr/bin/llvm-config-22")
endif()

option(BIOSEQ_CODEGEN_ENABLE_EXTERNAL "Build the external-API-client component" OFF)
option(BIOSEQ_CODEGEN_ENABLE_WEBSITE "Build the website component" OFF)

if(NOT BIOSEQ_CODEGEN_ENABLE_EXTERNAL AND NOT BIOSEQ_CODEGEN_ENABLE_WEBSITE)
  message(FATAL_ERROR
    "bioseq_codegen requires at least one enabled component "
    "(BIOSEQ_CODEGEN_ENABLE_EXTERNAL / BIOSEQ_CODEGEN_ENABLE_WEBSITE)")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
include(FetchContent)

set(_codegen_src "${CMAKE_CURRENT_SOURCE_DIR}")

# Shared support footprint, always compiled.
set(_codegen_module_files
  "${_codegen_src}/support/diagnostics.cppm"
  "${_codegen_src}/support/text.cppm"
  "${_codegen_src}/support/artifact_store.cppm"
  "${_codegen_src}/support/index.cppm"
  "${_codegen_src}/cli.cppm"
  "${_codegen_src}/index.cppm"
  "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq/backend/common/hashing.cppm")
set(_codegen_base_dirs
  "${_codegen_src}"
  "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq")
set(_codegen_compile_defs "")
set(_codegen_link_libs "")

if(BIOSEQ_CODEGEN_ENABLE_EXTERNAL)
  set(CPPAST_BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(CPPAST_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(CPPAST_BUILD_TOOL OFF CACHE BOOL "" FORCE)
  set(LLVM_CONFIG_BINARY "${LLVM_CONFIG_BINARY}" CACHE FILEPATH "" FORCE)
  add_subdirectory("${BIOSEQ_REPOSITORY_ROOT}/cppast-main"
    "${CMAKE_BINARY_DIR}/cppast" EXCLUDE_FROM_ALL)
  set_target_properties(cppast PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD OFF)

  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
  if(NOT TARGET yaml-cpp::yaml-cpp)
    FetchContent_Declare(yaml-cpp
      GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
      GIT_TAG 0.8.0
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(yaml-cpp)
  endif()

  list(APPEND _codegen_module_files
    "${_codegen_src}/external/model.cppm"
    "${_codegen_src}/external/annotation_parser.cppm"
    "${_codegen_src}/external/openapi_parser.cppm"
    "${_codegen_src}/external/client_renderer.cppm"
    "${_codegen_src}/external/artifact_handler.cppm"
    "${_codegen_src}/external/incremental_state.cppm"
    "${_codegen_src}/external/builder.cppm"
    "${_codegen_src}/external/contract_tests.cppm"
    "${_codegen_src}/external/index.cppm"
    "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq/external/http_validation.cppm")
  list(APPEND _codegen_compile_defs BIOSEQ_CODEGEN_ENABLE_EXTERNAL)
  list(APPEND _codegen_link_libs cppast yaml-cpp::yaml-cpp)
endif()

if(BIOSEQ_CODEGEN_ENABLE_WEBSITE)
  # Website component sources/dependencies are wired in a later phase.
  list(APPEND _codegen_compile_defs BIOSEQ_CODEGEN_ENABLE_WEBSITE)
endif()

add_executable(bioseq_codegen "${_codegen_src}/main.cpp")
target_sources(bioseq_codegen
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS ${_codegen_base_dirs}
    FILES ${_codegen_module_files})
target_compile_definitions(bioseq_codegen PRIVATE ${_codegen_compile_defs})
if(_codegen_link_libs)
  target_link_libraries(bioseq_codegen PRIVATE ${_codegen_link_libs})
endif()
set_target_properties(bioseq_codegen PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  RUNTIME_OUTPUT_DIRECTORY "${BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY}")
