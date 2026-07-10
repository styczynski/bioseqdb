cmake_minimum_required(VERSION 4.2)

if(NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
  else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  endif()
endif()

project(bioseq_external_codegen LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
  message(FATAL_ERROR
    "bioseq_external_codegen requires GCC 16.1 or newer; found "
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at "
    "${CMAKE_CXX_COMPILER}")
endif()

if(NOT BIOSEQ_REPOSITORY_ROOT)
  get_filename_component(BIOSEQ_REPOSITORY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
endif()
if(NOT BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY)
  set(BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT LLVM_CONFIG_BINARY)
  set(LLVM_CONFIG_BINARY "/usr/bin/llvm-config-22")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

include(FetchContent)

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
FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(yaml-cpp)

set(_bioseq_codegen_entry
  "${CMAKE_CURRENT_BINARY_DIR}/generator_main.cpp")
file(GENERATE OUTPUT "${_bioseq_codegen_entry}" CONTENT [=[
import bioseq.external.codegen;

int main(int argc, char** argv) {
    return bioseq::external::codegen::run(argc, argv);
}
]=])

add_executable(bioseq_external_codegen "${_bioseq_codegen_entry}")
target_sources(bioseq_external_codegen
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/generator.cppm"
      "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq/backend/common/hashing.cppm")
target_link_libraries(bioseq_external_codegen PRIVATE
  cppast
  yaml-cpp::yaml-cpp)
set_target_properties(bioseq_external_codegen PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  RUNTIME_OUTPUT_DIRECTORY "${BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY}")
