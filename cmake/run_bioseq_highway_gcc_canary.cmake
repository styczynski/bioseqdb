cmake_minimum_required(VERSION 4.2)

set(_required_inputs
  BIOSEQ_GCC_CANARY_CMAKE
  BIOSEQ_GCC_CANARY_NINJA
  BIOSEQ_GCC_CANARY_SOURCE
  BIOSEQ_GCC_CANARY_BUILD
  BIOSEQ_GCC_CANARY_OUTER_BINARY
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
foreach(_input IN LISTS _required_inputs)
  if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
    message(FATAL_ERROR "Highway GCC canary runner requires ${_input}")
  endif()
endforeach()

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

if(NOT IS_ABSOLUTE "${BIOSEQ_GCC_CANARY_BUILD}"
    OR NOT IS_ABSOLUTE "${BIOSEQ_GCC_CANARY_OUTER_BINARY}")
  message(FATAL_ERROR
    "Highway GCC canary build and outer binary roots must be absolute")
endif()
cmake_path(
  NORMAL_PATH BIOSEQ_GCC_CANARY_BUILD OUTPUT_VARIABLE _nested_build_normal)
cmake_path(
  NORMAL_PATH BIOSEQ_GCC_CANARY_OUTER_BINARY OUTPUT_VARIABLE _outer_normal)
set(_expected_nested_build
  "${_outer_normal}/codegen/namespace_materialization/tests/integration/module_canary/highway-gcc-canary/nested")
if(NOT _nested_build_normal STREQUAL _expected_nested_build)
  message(FATAL_ERROR
    "refusing to recreate an unscoped Highway GCC canary build tree: "
    "${_nested_build_normal}")
endif()

set(_identity_material
  "bioseq-highway-gcc-canary\n"
  "revision=1\n"
  "${BIOSEQ_GCC_CANARY_ROOT}\n"
  "${BIOSEQ_GCC_CANARY_VERSION}\n"
  "${BIOSEQ_GCC_CANARY_C_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_CXX_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_MODULES_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}\n"
  "${BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256}\n")
string(SHA256 _identity "${_identity_material}")
set(_identity_file "${BIOSEQ_GCC_CANARY_BUILD}/.bioseq-toolchain-identity")
# Recreate the nested closure on every invocation so no same-compiler CMake
# cache, project hook, or flag injection can survive.
file(REMOVE_RECURSE "${_nested_build_normal}")
file(MAKE_DIRECTORY "${BIOSEQ_GCC_CANARY_BUILD}")
file(WRITE "${_identity_file}" "${_identity}\n")

set(_allowlisted_environment
  "/usr/bin/env"
  -i
  "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
  "LC_ALL=C"
  "LANG=C")

execute_process(
  COMMAND ${_allowlisted_environment}
    "${BIOSEQ_GCC_CANARY_CMAKE}"
    -S "${BIOSEQ_GCC_CANARY_SOURCE}"
    -B "${BIOSEQ_GCC_CANARY_BUILD}"
    -G Ninja
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=${BIOSEQ_GCC_CANARY_NINJA}"
    "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_GCC_CANARY_CXX}"
    "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING="
    "-DCMAKE_CXX_FLAGS:STRING="
    "-DCMAKE_EXE_LINKER_FLAGS:STRING="
    "-DCMAKE_BUILD_TYPE:STRING=Release"
    "-DCMAKE_TOOLCHAIN_FILE:FILEPATH="
    "-DCMAKE_PROJECT_INCLUDE:FILEPATH="
    "-DCMAKE_PROJECT_INCLUDE_BEFORE:FILEPATH="
    "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES:STRING="
    "-DCMAKE_PROJECT_bioseq_highway_gcc_module_canary_INCLUDE:FILEPATH="
    "-DCMAKE_PROJECT_bioseq_highway_gcc_module_canary_INCLUDE_BEFORE:FILEPATH="
    "-DCMAKE_PREFIX_PATH:STRING="
    "-DCMAKE_CXX_MODULE_STD:BOOL=OFF"
    "-DBIOSEQ_GCC_CANARY_CANARY_HELPER:FILEPATH=${BIOSEQ_GCC_CANARY_CANARY_HELPER}"
    "-DBIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT:PATH=${BIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT}"
    "-DBIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT:PATH=${BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT}"
    "-DBIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST:STRING=${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}"
    "-DBIOSEQ_GCC_CANARY_GENERATED_ROOT:PATH=${BIOSEQ_GCC_CANARY_GENERATED_ROOT}"
    "-DBIOSEQ_GCC_CANARY_PRIMARY:FILEPATH=${BIOSEQ_GCC_CANARY_PRIMARY}"
    "-DBIOSEQ_GCC_CANARY_DRIVER:FILEPATH=${BIOSEQ_GCC_CANARY_DRIVER}"
    "-DBIOSEQ_GCC_CANARY_MAIN:FILEPATH=${BIOSEQ_GCC_CANARY_MAIN}"
    "-DBIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER:FILEPATH=${BIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER}"
    "-DBIOSEQ_GCC_CANARY_ROOT:PATH=${BIOSEQ_GCC_CANARY_ROOT}"
    "-DBIOSEQ_GCC_CANARY_C:FILEPATH=${BIOSEQ_GCC_CANARY_C}"
    "-DBIOSEQ_GCC_CANARY_CXX:FILEPATH=${BIOSEQ_GCC_CANARY_CXX}"
    "-DBIOSEQ_GCC_CANARY_MODULES:FILEPATH=${BIOSEQ_GCC_CANARY_MODULES}"
    "-DBIOSEQ_GCC_CANARY_VERSION:STRING=${BIOSEQ_GCC_CANARY_VERSION}"
    "-DBIOSEQ_GCC_CANARY_C_SHA256:STRING=${BIOSEQ_GCC_CANARY_C_SHA256}"
    "-DBIOSEQ_GCC_CANARY_CXX_SHA256:STRING=${BIOSEQ_GCC_CANARY_CXX_SHA256}"
    "-DBIOSEQ_GCC_CANARY_MODULES_SHA256:STRING=${BIOSEQ_GCC_CANARY_MODULES_SHA256}"
    "-DBIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256:STRING=${BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256}"
    "-DBIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256:STRING=${BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256}"
  COMMAND_ERROR_IS_FATAL ANY)

execute_process(
  COMMAND ${_allowlisted_environment}
    "${BIOSEQ_GCC_CANARY_CMAKE}"
    --build "${BIOSEQ_GCC_CANARY_BUILD}"
    --target bioseq_gcc_canary_closure
    --verbose
  COMMAND_ERROR_IS_FATAL ANY)

_bioseq_highway_gcc_canary_driver_closure(
  _postlink_driver_closure _gcc_libstdcxx _gcc_libgcc
  "${BIOSEQ_GCC_CANARY_ROOT}" "${BIOSEQ_GCC_CANARY_CXX}")
_bioseq_highway_gcc_canary_header_digest(
  _postlink_highway_headers "${BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT}")
if(NOT "${_postlink_driver_closure}" STREQUAL
      "${BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256}"
    OR NOT "${_postlink_highway_headers}" STREQUAL
      "${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}")
  message(FATAL_ERROR
    "GCC or Highway authenticated closure changed during nested linking")
endif()

function(_bioseq_highway_gcc_audit_macho executable)
  if(NOT IS_ABSOLUTE "${executable}"
      OR NOT EXISTS "${executable}"
      OR IS_DIRECTORY "${executable}")
    message(FATAL_ERROR
      "Highway GCC canary linked no regular executable at ${executable}")
  endif()
  file(REAL_PATH "${_gcc_libstdcxx}" _expected_libstdcxx)
  file(REAL_PATH "${_gcc_libgcc}" _expected_libgcc)
  file(SHA256 "${_expected_libstdcxx}" _expected_libstdcxx_sha256)
  file(SHA256 "${_expected_libgcc}" _expected_libgcc_sha256)
  set(_expected_runtime_paths
    "${_expected_libstdcxx}" "${_expected_libgcc}")
  set(_expected_runtime_rows
    "${_expected_libstdcxx}\t${_expected_libstdcxx_sha256}"
    "${_expected_libgcc}\t${_expected_libgcc_sha256}")
  list(REMOVE_DUPLICATES _expected_runtime_paths)
  list(REMOVE_DUPLICATES _expected_runtime_rows)
  list(SORT _expected_runtime_paths)
  list(SORT _expected_runtime_rows)

  _bioseq_highway_gcc_canary_collect_macho_closure(
    _observed_runtime_rows _observed_runtime_paths "${executable}")
  if(NOT "${_observed_runtime_paths}" STREQUAL
        "${_expected_runtime_paths}"
      OR NOT "${_observed_runtime_rows}" STREQUAL
        "${_expected_runtime_rows}")
    message(FATAL_ERROR
      "Highway GCC canary executable has an unexpected, missing, or changed "
      "non-system transitive dependency: ${executable}; expected="
      "${_expected_runtime_rows}; observed=${_observed_runtime_rows}")
  endif()
endfunction()

foreach(_linked_executable IN ITEMS
    "${BIOSEQ_GCC_CANARY_BUILD}/bin/bioseq_highway_gcc_module_canary"
    "${BIOSEQ_GCC_CANARY_BUILD}/bin/bioseq_namespace_materialization_gcc_module_canary")
  _bioseq_highway_gcc_audit_macho("${_linked_executable}")
endforeach()

message(STATUS
  "bioseq-highway-gcc-canary: linked isolated GCC ${BIOSEQ_GCC_CANARY_VERSION} "
  "Highway and generic module canaries with sealed dyld runtimes; "
  "toolchain identity=${_identity}; "
  "Highway headers=${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}")
