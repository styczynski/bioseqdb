cmake_minimum_required(VERSION 4.2)

foreach(_required IN ITEMS
    BIOSEQ_GCC_CANARY_GENERATED_ROOT
    BIOSEQ_GCC_CANARY_PRIMARY
    BIOSEQ_GCC_CANARY_DRIVER
    BIOSEQ_GCC_CANARY_MAIN
    BIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER
    BIOSEQ_GCC_CANARY_ROOT
    BIOSEQ_GCC_CANARY_C
    BIOSEQ_GCC_CANARY_CXX
    BIOSEQ_GCC_CANARY_MODULES
    BIOSEQ_GCC_CANARY_VERSION
    BIOSEQ_GCC_CANARY_C_SHA256
    BIOSEQ_GCC_CANARY_CXX_SHA256
    BIOSEQ_GCC_CANARY_MODULES_SHA256
    BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "Highway GCC nested canary requires ${_required}")
  endif()
endforeach()
if(CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
    "Highway GCC nested canary forbids CMAKE_TOOLCHAIN_FILE/vcpkg")
endif()
foreach(_forbidden IN ITEMS
    VCPKG_CHAINLOAD_TOOLCHAIN_FILE VCPKG_TARGET_TRIPLET VCPKG_MANIFEST_DIR)
  if(DEFINED ${_forbidden} AND NOT "${${_forbidden}}" STREQUAL "")
    message(FATAL_ERROR
      "Highway GCC nested canary forbids ${_forbidden}")
  endif()
endforeach()

set(CMAKE_CXX_COMPILER "${BIOSEQ_GCC_CANARY_CXX}"
  CACHE FILEPATH "Authenticated Homebrew g++-16" FORCE)
set(CMAKE_CXX_MODULE_STD OFF CACHE BOOL
  "The canary intentionally does not import the standard-library module" FORCE)
set(CMAKE_CXX_SCAN_FOR_MODULES ON CACHE BOOL
  "Scan the isolated generated module closure" FORCE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)

project(bioseq_highway_gcc_module_canary LANGUAGES CXX)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
  message(FATAL_ERROR "Highway GCC nested canary requires Ninja")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR
    "Highway GCC nested canary supports native Darwin only")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
  message(FATAL_ERROR
    "Highway GCC nested canary requires GNU g++ >= 16.1; found "
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()
file(REAL_PATH "${CMAKE_CXX_COMPILER}" _configured_cxx)
file(REAL_PATH "${BIOSEQ_GCC_CANARY_CXX}" _authenticated_cxx)
if(NOT _configured_cxx STREQUAL _authenticated_cxx)
  message(FATAL_ERROR
    "Highway GCC nested compiler differs from the authenticated g++-16")
endif()
if(CMAKE_CXX_MODULE_STD)
  message(FATAL_ERROR
    "Highway GCC nested canary must keep CMAKE_CXX_MODULE_STD=OFF")
endif()
if(CMAKE_CXX_STDLIB_MODULES_JSON)
  message(FATAL_ERROR
    "Highway GCC nested canary authenticates but does not import std modules")
endif()

include("${BIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER}")
bioseq_verify_gcc16_toolchain_identity(
  ROOT "${BIOSEQ_GCC_CANARY_ROOT}"
  C "${BIOSEQ_GCC_CANARY_C}"
  CXX "${BIOSEQ_GCC_CANARY_CXX}"
  MODULES "${BIOSEQ_GCC_CANARY_MODULES}"
  VERSION "${BIOSEQ_GCC_CANARY_VERSION}"
  C_SHA256 "${BIOSEQ_GCC_CANARY_C_SHA256}"
  CXX_SHA256 "${BIOSEQ_GCC_CANARY_CXX_SHA256}"
  MODULES_SHA256 "${BIOSEQ_GCC_CANARY_MODULES_SHA256}"
  MODULE_SOURCE_CLOSURE_SHA256
    "${BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256}")

set(_generated_sources
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/typed_flat_api.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/typed_flat_selector.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_0.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_1.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_2.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_3.cppm")
foreach(_source IN LISTS _generated_sources
    ITEMS
      "${BIOSEQ_GCC_CANARY_PRIMARY}"
      "${BIOSEQ_GCC_CANARY_DRIVER}"
      "${BIOSEQ_GCC_CANARY_MAIN}")
  if(NOT IS_ABSOLUTE "${_source}" OR NOT EXISTS "${_source}")
    message(FATAL_ERROR
      "Highway GCC nested canary input is missing or non-absolute: ${_source}")
  endif()
endforeach()

add_executable(bioseq_highway_gcc_module_canary)
target_sources(bioseq_highway_gcc_module_canary
  PRIVATE
    "${BIOSEQ_GCC_CANARY_MAIN}"
    ${_generated_sources}
    "${BIOSEQ_GCC_CANARY_DRIVER}"
  PRIVATE
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    FILES "${BIOSEQ_GCC_CANARY_PRIMARY}")
set_source_files_properties(
  ${_generated_sources}
  "${BIOSEQ_GCC_CANARY_DRIVER}"
  PROPERTIES COMPILE_OPTIONS "-x;c++" CXX_SCAN_FOR_MODULES ON)
target_compile_options(bioseq_highway_gcc_module_canary PRIVATE
  -fpermissive
  -fabi-version=0
  -Wno-expose-global-module-tu-local
  -Wno-external-tu-local
  -Wno-template-names-tu-local
  -Wno-global-module
  -Wno-template-body)
set_target_properties(bioseq_highway_gcc_module_canary PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD OFF
  CXX_SCAN_FOR_MODULES ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
get_target_property(_canary_link_libraries
  bioseq_highway_gcc_module_canary LINK_LIBRARIES)
if(_canary_link_libraries)
  message(FATAL_ERROR
    "Highway GCC nested canary must not link vcpkg, LLVM, Clang-Tooling, "
    "or any other explicit C++ library: ${_canary_link_libraries}")
endif()
