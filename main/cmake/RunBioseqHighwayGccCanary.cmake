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
  "${_outer_normal}/codegen/namespace_materialization/tests/module_canary/highway-gcc-canary/nested")
if(NOT _nested_build_normal STREQUAL _expected_nested_build)
  message(FATAL_ERROR
    "refusing to recreate an unscoped Highway GCC canary build tree: "
    "${_nested_build_normal}")
endif()

set(_identity_material
  "bioseq-highway-gcc-canary/v1\n"
  "${BIOSEQ_GCC_CANARY_ROOT}\n"
  "${BIOSEQ_GCC_CANARY_VERSION}\n"
  "${BIOSEQ_GCC_CANARY_C_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_CXX_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_MODULES_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256}\n"
  "${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}\n")
string(SHA256 _identity "${_identity_material}")
set(_identity_file "${BIOSEQ_GCC_CANARY_BUILD}/.bioseq-toolchain-identity")
# The closure is deliberately tiny. Recreate it on every invocation so no
# same-compiler CMake cache, project hook, or flag injection can survive.
file(REMOVE_RECURSE "${_nested_build_normal}")
file(MAKE_DIRECTORY "${BIOSEQ_GCC_CANARY_BUILD}")
file(WRITE "${_identity_file}" "${_identity}\n")

set(_sanitized_environment
  --unset=CC
  --unset=CXX
  --unset=CFLAGS
  --unset=CXXFLAGS
  --unset=CPPFLAGS
  --unset=LDFLAGS
  --unset=CPATH
  --unset=C_INCLUDE_PATH
  --unset=CPLUS_INCLUDE_PATH
  --unset=OBJC_INCLUDE_PATH
  --unset=LIBRARY_PATH
  --unset=COMPILER_PATH
  --unset=GCC_EXEC_PREFIX
  --unset=SDKROOT
  --unset=MACOSX_DEPLOYMENT_TARGET
  --unset=CMAKE_PREFIX_PATH
  --unset=CMAKE_TOOLCHAIN_FILE
  --unset=VCPKG_ROOT
  --unset=VCPKG_DEFAULT_TRIPLET
  --unset=PKG_CONFIG_PATH
  --unset=DYLD_LIBRARY_PATH
  --unset=DYLD_FALLBACK_LIBRARY_PATH
  --unset=DYLD_FRAMEWORK_PATH
  --unset=DYLD_FALLBACK_FRAMEWORK_PATH
  --unset=DYLD_INSERT_LIBRARIES
  "PATH=/usr/bin:/bin:/usr/sbin:/sbin")

execute_process(
  COMMAND "${BIOSEQ_GCC_CANARY_CMAKE}" -E env
    ${_sanitized_environment}
    --
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
  COMMAND_ERROR_IS_FATAL ANY)

execute_process(
  COMMAND "${BIOSEQ_GCC_CANARY_CMAKE}" -E env
    ${_sanitized_environment}
    --
    "${BIOSEQ_GCC_CANARY_CMAKE}"
    --build "${BIOSEQ_GCC_CANARY_BUILD}"
    --target bioseq_highway_gcc_module_canary
    --verbose
  COMMAND_ERROR_IS_FATAL ANY)

set(_canary_executable
  "${BIOSEQ_GCC_CANARY_BUILD}/bin/bioseq_highway_gcc_module_canary")
if(NOT EXISTS "${_canary_executable}")
  message(FATAL_ERROR
    "Highway GCC canary linked no executable at ${_canary_executable}")
endif()
execute_process(
  COMMAND "${BIOSEQ_GCC_CANARY_CMAKE}" -E env
    ${_sanitized_environment}
    --
    "${_canary_executable}"
  OUTPUT_VARIABLE _canary_output
  ERROR_VARIABLE _canary_error
  COMMAND_ERROR_IS_FATAL ANY)
string(STRIP "${_canary_output}" _canary_output)
if(NOT _canary_output MATCHES
    "^bioseq-highway-gcc-canary: first=[A-Z0-9_]+ second=EMU128 checksum=976$")
  message(FATAL_ERROR
    "Highway GCC canary returned malformed selection evidence: "
    "stdout='${_canary_output}' stderr='${_canary_error}'")
endif()

message(STATUS
  "bioseq-highway-gcc-canary: linked isolated GCC ${BIOSEQ_GCC_CANARY_VERSION} "
  "module canary; ${_canary_output}; toolchain identity=${_identity}; "
  "Highway headers=${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}")
