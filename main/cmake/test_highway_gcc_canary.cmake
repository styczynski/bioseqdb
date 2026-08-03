cmake_minimum_required(VERSION 4.2)

if(NOT DEFINED TEST_ROOT OR "${TEST_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TEST_ROOT is required")
endif()
if(NOT DEFINED SOURCE_ROOT OR "${SOURCE_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

set(_canary_module "${SOURCE_ROOT}/cmake/BioseqHighwayGccCanary.cmake")
set(_toolchain_helper "${SOURCE_ROOT}/cmake/BioseqGccToolchain.cmake")
set(_runner "${SOURCE_ROOT}/cmake/RunBioseqHighwayGccCanary.cmake")
set(_nested_project
  "${SOURCE_ROOT}/codegen/namespace_materialization/tests/module_canary/gcc_canary/CMakeLists.txt")
foreach(_checked IN ITEMS
    _canary_module _toolchain_helper _runner _nested_project)
  if(NOT EXISTS "${${_checked}}")
    message(FATAL_ERROR "missing Highway GCC canary contract file: ${${_checked}}")
  endif()
endforeach()

function(_expect_script_failure name source expected)
  set(_script "${TEST_ROOT}/${name}.cmake")
  file(WRITE "${_script}" "${source}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${_script}"
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(_status EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly succeeded")
  endif()
  set(_combined "${_stdout}\n${_stderr}")
  if(NOT _combined MATCHES "${expected}")
    message(FATAL_ERROR
      "${name} did not fail with '${expected}':\n${_combined}")
  endif()
endfunction()

set(CANARY_MODULE "${_canary_module}")
set(CMAKE_EXE "${CMAKE_COMMAND}")
set(_off_darwin [=[
include("@CANARY_MODULE@")
_bioseq_highway_gcc_canary_validate_host(
  HOST_SYSTEM Linux
  TARGET_SYSTEM Linux
  COMPILER_ID Clang
  GENERATOR Ninja
  MAKE_PROGRAM "@CMAKE_EXE@")
]=])
string(CONFIGURE "${_off_darwin}" _off_darwin @ONLY)
_expect_script_failure(off_darwin "${_off_darwin}" "native-Darwin")

set(_non_clang [=[
include("@CANARY_MODULE@")
_bioseq_highway_gcc_canary_validate_host(
  HOST_SYSTEM Darwin
  TARGET_SYSTEM Darwin
  COMPILER_ID GNU
  GENERATOR Ninja
  MAKE_PROGRAM "@CMAKE_EXE@")
]=])
string(CONFIGURE "${_non_clang}" _non_clang @ONLY)
_expect_script_failure(non_clang_host "${_non_clang}" "ordinary Clang")

set(_fake_root "${TEST_ROOT}/fake-gcc-16.1")
file(MAKE_DIRECTORY "${_fake_root}/bin" "${_fake_root}/lib")
file(WRITE "${_fake_root}/lib/std.cc" "export module std;\n")
file(WRITE "${_fake_root}/lib/std.compat.cc" "export module std.compat;\n")
set(_inventory "${_fake_root}/lib/libstdc++.modules.json")
file(WRITE "${_inventory}"
  "{\n"
  "  \"version\": 1,\n"
  "  \"modules\": [\n"
  "    {\"logical-name\":\"std\",\"source-path\":\"std.cc\",\"is-std-library\":true},\n"
  "    {\"logical-name\":\"std.compat\",\"source-path\":\"std.compat.cc\",\"is-std-library\":true}\n"
  "  ]\n"
  "}\n")
set(_fake_compiler
  "#!/bin/sh\n"
  "case \"$1\" in\n"
  "  -dumpfullversion|-dumpversion) printf '%s\\n' '16.1.0' ;;\n"
  "  -print-file-name=libstdc++.modules.json) printf '%s\\n' '${_inventory}' ;;\n"
  "  *) exit 0 ;;\n"
  "esac\n")
foreach(_compiler IN ITEMS gcc-16 g++-16)
  file(WRITE "${_fake_root}/bin/${_compiler}" "${_fake_compiler}")
  file(CHMOD "${_fake_root}/bin/${_compiler}"
    PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE
      GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE)
endforeach()

include("${_toolchain_helper}")
bioseq_select_gcc16_toolchain(
  REQUESTED_ROOT "${_fake_root}"
  REQUESTED_MODULES "${_inventory}"
  OUTPUT_ROOT _selected_root
  OUTPUT_C _selected_c
  OUTPUT_CXX _selected_cxx
  OUTPUT_MODULES _selected_modules
  OUTPUT_VERSION _selected_version
  OUTPUT_C_SHA256 _selected_c_sha256
  OUTPUT_CXX_SHA256 _selected_cxx_sha256
  OUTPUT_MODULES_SHA256 _selected_modules_sha256
  OUTPUT_MODULE_SOURCE_CLOSURE_SHA256 _selected_sources_sha256)
if(NOT _selected_version VERSION_EQUAL "16.1.0"
    OR NOT _selected_modules STREQUAL "${_inventory}"
    OR "${_selected_c_sha256}" STREQUAL ""
    OR "${_selected_cxx_sha256}" STREQUAL ""
    OR "${_selected_modules_sha256}" STREQUAL ""
    OR "${_selected_sources_sha256}" STREQUAL "")
  message(FATAL_ERROR
    "authenticated fake GCC compiler/inventory selection was incomplete")
endif()
bioseq_verify_gcc16_toolchain_identity(
  ROOT "${_selected_root}"
  C "${_selected_c}"
  CXX "${_selected_cxx}"
  MODULES "${_selected_modules}"
  VERSION "${_selected_version}"
  C_SHA256 "${_selected_c_sha256}"
  CXX_SHA256 "${_selected_cxx_sha256}"
  MODULES_SHA256 "${_selected_modules_sha256}"
  MODULE_SOURCE_CLOSURE_SHA256 "${_selected_sources_sha256}")

set(TOOLCHAIN_HELPER "${_toolchain_helper}")
set(MISSING_ROOT "${TEST_ROOT}/missing-gcc")
file(MAKE_DIRECTORY "${MISSING_ROOT}")
set(_missing_pair [=[
include("@TOOLCHAIN_HELPER@")
bioseq_select_gcc16_toolchain(
  REQUESTED_ROOT "@MISSING_ROOT@"
  OUTPUT_ROOT root
  OUTPUT_C c
  OUTPUT_CXX cxx
  OUTPUT_MODULES modules
  OUTPUT_VERSION version
  OUTPUT_C_SHA256 c_sha
  OUTPUT_CXX_SHA256 cxx_sha
  OUTPUT_MODULES_SHA256 modules_sha
  OUTPUT_MODULE_SOURCE_CLOSURE_SHA256 sources_sha)
]=])
string(CONFIGURE "${_missing_pair}" _missing_pair @ONLY)
_expect_script_failure(
  missing_gcc_pair "${_missing_pair}" "paired gcc-16/g\\+\\+-16")

file(READ "${SOURCE_ROOT}/CMakeLists.txt" _root_cmake)
string(FIND "${_root_cmake}" "option(BIOSEQ_HIGHWAY_GCC_CANARY" _option_offset)
if(_option_offset EQUAL -1)
  message(FATAL_ERROR "Highway GCC canary option is missing")
endif()
string(FIND "${_root_cmake}"
  "module boundary canary\" OFF)" _option_off_offset)
if(_option_off_offset EQUAL -1)
  message(FATAL_ERROR "Highway GCC canary must remain OFF by default")
endif()

file(READ "${_nested_project}" _nested)
foreach(_required_text IN ITEMS
    "CMAKE_CXX_MODULE_STD OFF"
    "CXX_MODULE_STD OFF"
    "typed_flat_api.cppm"
    "typed_flat_selector.cppm"
    "instance_0.cppm"
    "instance_1.cppm"
    "instance_2.cppm"
    "instance_3.cppm"
    "BIOSEQ_GCC_CANARY_PRIMARY"
    "BIOSEQ_GCC_CANARY_DRIVER"
    "BIOSEQ_GCC_CANARY_MAIN"
    "BASE_DIRS"
    "CXX_SCAN_FOR_MODULES ON")
  string(FIND "${_nested}" "${_required_text}" _required_offset)
  if(_required_offset EQUAL -1)
    message(FATAL_ERROR
      "nested GCC canary lost required closure text: ${_required_text}")
  endif()
endforeach()
string(FIND "${_nested}" "find_package(" _find_package_offset)
string(FIND "${_nested}" "target_link_libraries(" _link_libraries_offset)
if(NOT _find_package_offset EQUAL -1
    OR NOT _link_libraries_offset EQUAL -1)
  message(FATAL_ERROR
    "nested GCC canary must have no package discovery or explicit libraries")
endif()

file(READ "${_runner}" _runner_source)
foreach(_sealed_argument IN ITEMS
    "-G Ninja"
    "-DCMAKE_TOOLCHAIN_FILE:FILEPATH="
    "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING="
    "-DCMAKE_CXX_FLAGS:STRING="
    "-DCMAKE_CXX_MODULE_STD:BOOL=OFF"
    "bioseq_verify_gcc16_toolchain_identity"
    "--verbose")
  string(FIND "${_runner_source}" "${_sealed_argument}" _sealed_offset)
  if(_sealed_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary runner lost sealed argument: ${_sealed_argument}")
  endif()
endforeach()
string(FIND "${_nested}" "-fpermissive" _permissive_offset)
if(NOT _permissive_offset EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC canary must not downgrade generated module diagnostics")
endif()

message(STATUS "Highway GCC canary CMake contracts passed")
