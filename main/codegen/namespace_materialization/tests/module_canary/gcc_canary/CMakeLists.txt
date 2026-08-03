cmake_minimum_required(VERSION 4.2)

foreach(_required IN ITEMS
    BIOSEQ_GCC_CANARY_CANARY_HELPER
    BIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT
    BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT
    BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST
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
    BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256
    BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "Highway GCC nested canary requires ${_required}")
  endif()
endforeach()
foreach(_file_input IN ITEMS
    BIOSEQ_GCC_CANARY_CANARY_HELPER
    BIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER)
  if(NOT IS_ABSOLUTE "${${_file_input}}"
      OR NOT EXISTS "${${_file_input}}"
      OR IS_DIRECTORY "${${_file_input}}")
    message(FATAL_ERROR
      "Highway GCC nested canary requires an absolute regular ${_file_input}")
  endif()
endforeach()
foreach(_directory_input IN ITEMS
    BIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT
    BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT
    BIOSEQ_GCC_CANARY_GENERATED_ROOT)
  if(NOT IS_ABSOLUTE "${${_directory_input}}"
      OR NOT IS_DIRECTORY "${${_directory_input}}")
    message(FATAL_ERROR
      "Highway GCC nested canary requires an absolute ${_directory_input}")
  endif()
endforeach()
foreach(_file_input IN ITEMS
    BIOSEQ_GCC_CANARY_PRIMARY
    BIOSEQ_GCC_CANARY_DRIVER
    BIOSEQ_GCC_CANARY_MAIN)
  if(NOT IS_ABSOLUTE "${${_file_input}}"
      OR NOT EXISTS "${${_file_input}}"
      OR IS_DIRECTORY "${${_file_input}}")
    message(FATAL_ERROR
      "Highway GCC nested canary requires an absolute regular ${_file_input}")
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
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
  "Retain the exact GCC canary compile commands" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
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

include("${BIOSEQ_GCC_CANARY_CANARY_HELPER}")
_bioseq_highway_gcc_canary_driver_closure(
  _observed_driver_closure _gcc_libstdcxx _gcc_libgcc
  "${BIOSEQ_GCC_CANARY_ROOT}" "${BIOSEQ_GCC_CANARY_CXX}")
if(NOT "${_observed_driver_closure}" STREQUAL
    "${BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256}")
  message(FATAL_ERROR
    "authenticated GCC driver/frontend/header/runtime closure changed")
endif()
file(REAL_PATH "${BIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT}" _source_root)
file(REAL_PATH "${BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT}" _highway_include)
_bioseq_highway_gcc_canary_header_digest(
  _observed_highway_digest "${_highway_include}")
if(NOT "${_observed_highway_digest}" STREQUAL
    "${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}")
  message(FATAL_ERROR
    "Highway header closure changed after the authenticated outer configure: "
    "expected ${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}, "
    "observed ${_observed_highway_digest}")
endif()
foreach(_source IN ITEMS
    contract.cppm
    dispatch.cppm
    driver.cppm
    main.cpp
    registry.cppm.in
    runtime.cpp.in
    target_primary.cppm.in
    target_kernel.cppm.in
    target_binder.cppm.in
    target_attribute_shapes.cppm.in)
  if(NOT EXISTS "${_source_root}/${_source}")
    message(FATAL_ERROR
      "Highway GCC canary source closure is missing ${_source_root}/${_source}")
  endif()
endforeach()

get_filename_component(
  _canary_cmake_dir "${BIOSEQ_GCC_CANARY_CANARY_HELPER}" DIRECTORY)
include("${_canary_cmake_dir}/BioseqHighwayImportedBinderCanary.cmake")
bioseq_add_highway_imported_binder_canary(
  TARGET bioseq_highway_gcc_module_canary
  SOURCE_ROOT "${_source_root}"
  HIGHWAY_INCLUDE_ROOT "${_highway_include}"
  GENERATED_ROOT "${CMAKE_BINARY_DIR}/generated")

# Retain the original namespace-materialization GCC closure beside the real
# Highway boundary so the specialized proof does not regress generic generated
# implementation-unit/module dependency coverage.
set(_generic_generated_sources
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/typed_flat_api.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/typed_flat_selector.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_0.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_1.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_2.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_3.cppm")
foreach(_generic_source IN LISTS _generic_generated_sources)
  if(NOT IS_ABSOLUTE "${_generic_source}"
      OR NOT EXISTS "${_generic_source}"
      OR IS_DIRECTORY "${_generic_source}")
    message(FATAL_ERROR
      "generic GCC canary input is missing: ${_generic_source}")
  endif()
endforeach()
get_filename_component(
  _generic_primary_base "${BIOSEQ_GCC_CANARY_PRIMARY}" DIRECTORY)
add_executable(bioseq_namespace_materialization_gcc_module_canary)
target_sources(bioseq_namespace_materialization_gcc_module_canary
  PRIVATE
    "${BIOSEQ_GCC_CANARY_MAIN}"
    ${_generic_generated_sources}
    "${BIOSEQ_GCC_CANARY_DRIVER}"
  PRIVATE
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS "${_generic_primary_base}"
    FILES "${BIOSEQ_GCC_CANARY_PRIMARY}")
set_source_files_properties(
  ${_generic_generated_sources}
  "${BIOSEQ_GCC_CANARY_DRIVER}"
  PROPERTIES COMPILE_OPTIONS "-x;c++" CXX_SCAN_FOR_MODULES ON)
set_target_properties(
  bioseq_namespace_materialization_gcc_module_canary PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD OFF
    CXX_SCAN_FOR_MODULES ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)

foreach(_canary_target IN ITEMS
    bioseq_highway_gcc_module_canary
    bioseq_namespace_materialization_gcc_module_canary)
  get_target_property(_canary_link_libraries
    "${_canary_target}" LINK_LIBRARIES)
  if(_canary_link_libraries)
    message(FATAL_ERROR
      "Highway GCC nested canaries must not link the mixed-runtime outer "
      "libhwy, vcpkg, LLVM, Clang-Tooling, or any other explicit C++ library: "
      "${_canary_link_libraries}")
  endif()
endforeach()

add_custom_target(bioseq_gcc_canary_closure
  DEPENDS
    bioseq_highway_gcc_module_canary
    bioseq_namespace_materialization_gcc_module_canary)
