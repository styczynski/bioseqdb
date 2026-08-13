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
  BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256
  BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR
  BIOSEQ_GCC_CANARY_APPLE_SDK_PATH
  BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT
  BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION
  BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION
  BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST)
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
_bioseq_highway_gcc_canary_verify_apple_sdk(
  CXX "${BIOSEQ_GCC_CANARY_CXX}"
  DEVELOPER_DIR "${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}"
  SDK_PATH "${BIOSEQ_GCC_CANARY_APPLE_SDK_PATH}"
  SDK_ROOT "${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"
  VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION}"
  BUILD_VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}"
  DIGEST "${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST}")

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
  "revision=2\n"
  "${BIOSEQ_GCC_CANARY_ROOT}\n"
  "${BIOSEQ_GCC_CANARY_VERSION}\n"
  "${BIOSEQ_GCC_CANARY_C_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_CXX_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_MODULES_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}\n"
  "${BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}\n"
  "${BIOSEQ_GCC_CANARY_APPLE_SDK_PATH}\n"
  "${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}\n"
  "${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION}\n"
  "${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}\n"
  "${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST}\n")
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
  "LANG=C"
  "DEVELOPER_DIR=${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}"
  "SDKROOT=${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}")

execute_process(
  COMMAND ${_allowlisted_environment}
    "${BIOSEQ_GCC_CANARY_CMAKE}"
    -S "${BIOSEQ_GCC_CANARY_SOURCE}"
    -B "${BIOSEQ_GCC_CANARY_BUILD}"
    -G Ninja
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=${BIOSEQ_GCC_CANARY_NINJA}"
    "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_GCC_CANARY_CXX}"
    "-DCMAKE_OSX_SYSROOT:PATH=${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"
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
    "-DBIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR:PATH=${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}"
    "-DBIOSEQ_GCC_CANARY_APPLE_SDK_PATH:PATH=${BIOSEQ_GCC_CANARY_APPLE_SDK_PATH}"
    "-DBIOSEQ_GCC_CANARY_APPLE_SDK_ROOT:PATH=${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"
    "-DBIOSEQ_GCC_CANARY_APPLE_SDK_VERSION:STRING=${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION}"
    "-DBIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION:STRING=${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}"
    "-DBIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST:STRING=${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST}"
  COMMAND_ERROR_IS_FATAL ANY)

_bioseq_highway_gcc_canary_verify_apple_sdk(
  CXX "${BIOSEQ_GCC_CANARY_CXX}"
  DEVELOPER_DIR "${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}"
  SDK_PATH "${BIOSEQ_GCC_CANARY_APPLE_SDK_PATH}"
  SDK_ROOT "${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"
  VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION}"
  BUILD_VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}"
  DIGEST "${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST}")

execute_process(
  COMMAND ${_allowlisted_environment}
    "${BIOSEQ_GCC_CANARY_NINJA}"
    -C "${BIOSEQ_GCC_CANARY_BUILD}"
    -t commands bioseq_gcc_canary_closure
  RESULT_VARIABLE _commands_status
  OUTPUT_VARIABLE _commands
  ERROR_VARIABLE _commands_stderr)
if(NOT _commands_status EQUAL 0 OR "${_commands}" STREQUAL "")
  message(FATAL_ERROR
    "Highway GCC canary could not audit nested build commands: "
    "status=${_commands_status}, stderr=${_commands_stderr}")
endif()
if("${_commands}" MATCHES ";")
  message(FATAL_ERROR
    "Highway GCC canary refuses an unrepresentable semicolon in nested "
    "build commands")
endif()
string(REPLACE "\r\n" "\n" _commands "${_commands}")
string(REPLACE "\n" ";" _command_lines "${_commands}")
set(_gcc_command_count 0)
foreach(_command_line IN LISTS _command_lines)
  separate_arguments(_command_tokens UNIX_COMMAND "${_command_line}")
  set(_gcc_token_count 0)
  foreach(_command_token IN LISTS _command_tokens)
    if("${_command_token}" STREQUAL "${BIOSEQ_GCC_CANARY_CXX}")
      math(EXPR _gcc_token_count "${_gcc_token_count} + 1")
    endif()
  endforeach()
  if(_gcc_token_count EQUAL 0)
    continue()
  endif()
  if(NOT _gcc_token_count EQUAL 1)
    message(FATAL_ERROR
      "Highway GCC canary nested command references the authenticated compiler "
      "more than once: ${_command_line}")
  endif()
  math(EXPR _gcc_command_count "${_gcc_command_count} + 1")
  set(_sysroot_flag_count 0)
  list(LENGTH _command_tokens _command_token_count)
  set(_command_token_index 0)
  while(_command_token_index LESS _command_token_count)
    list(GET _command_tokens ${_command_token_index} _command_token)
    if(_command_token STREQUAL "-isysroot")
      math(EXPR _sysroot_flag_count "${_sysroot_flag_count} + 1")
      math(EXPR _sysroot_value_index "${_command_token_index} + 1")
      if(_sysroot_value_index GREATER_EQUAL _command_token_count)
        message(FATAL_ERROR
          "Highway GCC canary nested GCC command ended at -isysroot: "
          "${_command_line}")
      endif()
      list(GET _command_tokens ${_sysroot_value_index} _sysroot_value)
      if(NOT "${_sysroot_value}" STREQUAL
          "${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}")
        message(FATAL_ERROR
          "Highway GCC canary nested GCC command carries a different Apple "
          "sysroot: ${_sysroot_value}")
      endif()
    elseif(_command_token MATCHES "^-isysroot.+"
        OR _command_token MATCHES "^--sysroot($|=)")
      message(FATAL_ERROR
        "Highway GCC canary nested GCC command carries a forbidden sysroot "
        "spelling: ${_command_token}")
    endif()
    math(EXPR _command_token_index "${_command_token_index} + 1")
  endwhile()
  if(NOT _sysroot_flag_count EQUAL 1)
    message(FATAL_ERROR
      "Highway GCC canary nested GCC command does not carry exactly one "
      "sealed Apple sysroot: ${_command_line}")
  endif()
endforeach()
if(_gcc_command_count LESS 2)
  message(FATAL_ERROR
    "Highway GCC canary found too few authenticated GCC commands to audit: "
    "${_gcc_command_count}")
endif()

_bioseq_highway_gcc_canary_verify_apple_sdk(
  CXX "${BIOSEQ_GCC_CANARY_CXX}"
  DEVELOPER_DIR "${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}"
  SDK_PATH "${BIOSEQ_GCC_CANARY_APPLE_SDK_PATH}"
  SDK_ROOT "${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"
  VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION}"
  BUILD_VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}"
  DIGEST "${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST}")

execute_process(
  COMMAND ${_allowlisted_environment}
    "${BIOSEQ_GCC_CANARY_CMAKE}"
    --build "${BIOSEQ_GCC_CANARY_BUILD}"
    --target bioseq_gcc_canary_closure
    --verbose
  COMMAND_ERROR_IS_FATAL ANY)

_bioseq_highway_gcc_canary_verify_apple_sdk(
  CXX "${BIOSEQ_GCC_CANARY_CXX}"
  DEVELOPER_DIR "${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}"
  SDK_PATH "${BIOSEQ_GCC_CANARY_APPLE_SDK_PATH}"
  SDK_ROOT "${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"
  VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION}"
  BUILD_VERSION "${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}"
  DIGEST "${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST}")

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
  "Highway headers=${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}; "
  "Apple SDK=${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION} "
  "(${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}, "
  "${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST})")
