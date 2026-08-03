include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/BioseqGccToolchain.cmake")

function(_bioseq_highway_gcc_canary_validate_host)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "HOST_SYSTEM;TARGET_SYSTEM;COMPILER_ID;GENERATOR;MAKE_PROGRAM"
    "")
  foreach(_required IN ITEMS
      HOST_SYSTEM TARGET_SYSTEM COMPILER_ID GENERATOR MAKE_PROGRAM)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "Highway GCC canary host validation requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "Highway GCC canary host validation received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT ARG_HOST_SYSTEM STREQUAL "Darwin"
      OR NOT ARG_TARGET_SYSTEM STREQUAL "Darwin")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY is a local native-Darwin proof only; "
      "host=${ARG_HOST_SYSTEM}, target=${ARG_TARGET_SYSTEM}")
  endif()
  if(NOT ARG_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY requires the ordinary Clang host generator; "
      "found ${ARG_COMPILER_ID}")
  endif()
  if(NOT ARG_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY requires the Ninja host generator; found "
      "${ARG_GENERATOR}")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_MAKE_PROGRAM}"
      OR NOT EXISTS "${ARG_MAKE_PROGRAM}")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY requires an existing absolute Ninja path; "
      "found ${ARG_MAKE_PROGRAM}")
  endif()
endfunction()

function(_bioseq_highway_gcc_canary_require_homebrew_root root)
  file(REAL_PATH "${root}" _root_real)
  set(_homebrew_cellars)
  foreach(_prefix IN ITEMS "/opt/homebrew" "/usr/local")
    if(IS_DIRECTORY "${_prefix}/Cellar")
      file(REAL_PATH "${_prefix}/Cellar" _cellar_real)
      list(APPEND _homebrew_cellars "${_cellar_real}")
    endif()
  endforeach()
  foreach(_cellar IN LISTS _homebrew_cellars)
    string(FIND "${_root_real}/" "${_cellar}/" _under_cellar)
    if(_under_cellar EQUAL 0)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "BIOSEQ_HIGHWAY_GCC_CANARY requires a Homebrew GCC prefix under an "
    "authenticated Homebrew Cellar; selected ${_root_real}")
endfunction()

function(_bioseq_highway_gcc_canary_header_digest output include_root)
  if(NOT IS_ABSOLUTE "${include_root}"
      OR NOT EXISTS "${include_root}/hwy/highway.h"
      OR NOT EXISTS "${include_root}/hwy/targets.h")
    message(FATAL_ERROR
      "Highway GCC canary requires an absolute complete Highway include root: "
      "${include_root}")
  endif()
  file(REAL_PATH "${include_root}" _include_root)
  file(GLOB_RECURSE _headers LIST_DIRECTORIES FALSE
    "${_include_root}/hwy/*.h")
  list(SORT _headers)
  if(NOT _headers)
    message(FATAL_ERROR
      "Highway GCC canary found no headers beneath ${_include_root}/hwy")
  endif()
  set(_material "bioseq-highway-gcc-canary-headers-v1\n")
  foreach(_header IN LISTS _headers)
    cmake_path(RELATIVE_PATH _header
      BASE_DIRECTORY "${_include_root}" OUTPUT_VARIABLE _relative)
    file(SHA256 "${_header}" _header_sha256)
    string(APPEND _material "${_relative}\t${_header_sha256}\n")
  endforeach()
  string(SHA256 _digest "${_material}")
  set(${output} "${_digest}" PARENT_SCOPE)
endfunction()

function(bioseq_add_highway_gcc_canary)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "TARGET;SOURCE_ROOT;HIGHWAY_INCLUDE_ROOT"
    "")
  foreach(_required IN ITEMS
      TARGET SOURCE_ROOT HIGHWAY_INCLUDE_ROOT)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "bioseq_add_highway_gcc_canary requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "bioseq_add_highway_gcc_canary received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(TARGET "${ARG_TARGET}")
    message(FATAL_ERROR
      "Highway GCC canary target already exists: ${ARG_TARGET}")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_SOURCE_ROOT}"
      OR NOT EXISTS "${ARG_SOURCE_ROOT}/contract.cppm"
      OR NOT EXISTS "${ARG_SOURCE_ROOT}/target_primary.cppm.in")
    message(FATAL_ERROR
      "Highway GCC canary SOURCE_ROOT must own the complete source/template "
      "closure: ${ARG_SOURCE_ROOT}")
  endif()
  file(REAL_PATH "${ARG_SOURCE_ROOT}" _canary_source_root)
  file(REAL_PATH "${ARG_HIGHWAY_INCLUDE_ROOT}" _highway_include_root)
  _bioseq_highway_gcc_canary_header_digest(
    _highway_header_digest "${_highway_include_root}")

  _bioseq_highway_gcc_canary_validate_host(
    HOST_SYSTEM "${CMAKE_HOST_SYSTEM_NAME}"
    TARGET_SYSTEM "${CMAKE_SYSTEM_NAME}"
    COMPILER_ID "${CMAKE_CXX_COMPILER_ID}"
    GENERATOR "${CMAKE_GENERATOR}"
    MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")

  set(_requested_root "")
  if(DEFINED ENV{BIOSEQ_GCC16_ROOT})
    set(_requested_root "$ENV{BIOSEQ_GCC16_ROOT}")
  elseif(BIOSEQ_GCC16_ROOT)
    set(_requested_root "${BIOSEQ_GCC16_ROOT}")
  endif()
  set(_requested_modules "")
  if(DEFINED ENV{BIOSEQ_LIBSTDCXX_MODULES_JSON})
    set(_requested_modules "$ENV{BIOSEQ_LIBSTDCXX_MODULES_JSON}")
  elseif(BIOSEQ_LIBSTDCXX_MODULES_JSON)
    set(_requested_modules "${BIOSEQ_LIBSTDCXX_MODULES_JSON}")
  endif()

  bioseq_select_gcc16_toolchain(
    REQUESTED_ROOT "${_requested_root}"
    REQUESTED_MODULES "${_requested_modules}"
    OUTPUT_ROOT _gcc_root
    OUTPUT_C _gcc_c
    OUTPUT_CXX _gcc_cxx
    OUTPUT_MODULES _gcc_modules
    OUTPUT_VERSION _gcc_version
    OUTPUT_C_SHA256 _gcc_c_sha256
    OUTPUT_CXX_SHA256 _gcc_cxx_sha256
    OUTPUT_MODULES_SHA256 _gcc_modules_sha256
    OUTPUT_MODULE_SOURCE_CLOSURE_SHA256 _gcc_module_sources_sha256)
  _bioseq_highway_gcc_canary_require_homebrew_root("${_gcc_root}")

  set(_nested_source
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../codegen/namespace_materialization/tests/module_canary/gcc_canary")
  cmake_path(NORMAL_PATH _nested_source)
  if(NOT EXISTS "${_nested_source}/CMakeLists.txt")
    message(FATAL_ERROR
      "Highway GCC canary nested project is missing: ${_nested_source}")
  endif()
  set(_nested_build
    "${CMAKE_CURRENT_BINARY_DIR}/highway-gcc-canary/nested")
  set(_runner
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RunBioseqHighwayGccCanary.cmake")
  set(_toolchain_helper
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BioseqGccToolchain.cmake")

  add_custom_target("${ARG_TARGET}"
    COMMAND "${CMAKE_COMMAND}"
      "-DBIOSEQ_GCC_CANARY_CMAKE=${CMAKE_COMMAND}"
      "-DBIOSEQ_GCC_CANARY_NINJA=${CMAKE_MAKE_PROGRAM}"
      "-DBIOSEQ_GCC_CANARY_SOURCE=${_nested_source}"
      "-DBIOSEQ_GCC_CANARY_BUILD=${_nested_build}"
      "-DBIOSEQ_GCC_CANARY_OUTER_BINARY=${CMAKE_BINARY_DIR}"
      "-DBIOSEQ_GCC_CANARY_CANARY_HELPER=${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
      "-DBIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT=${_canary_source_root}"
      "-DBIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT=${_highway_include_root}"
      "-DBIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST=${_highway_header_digest}"
      "-DBIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER=${_toolchain_helper}"
      "-DBIOSEQ_GCC_CANARY_ROOT=${_gcc_root}"
      "-DBIOSEQ_GCC_CANARY_C=${_gcc_c}"
      "-DBIOSEQ_GCC_CANARY_CXX=${_gcc_cxx}"
      "-DBIOSEQ_GCC_CANARY_MODULES=${_gcc_modules}"
      "-DBIOSEQ_GCC_CANARY_VERSION=${_gcc_version}"
      "-DBIOSEQ_GCC_CANARY_C_SHA256=${_gcc_c_sha256}"
      "-DBIOSEQ_GCC_CANARY_CXX_SHA256=${_gcc_cxx_sha256}"
      "-DBIOSEQ_GCC_CANARY_MODULES_SHA256=${_gcc_modules_sha256}"
      "-DBIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256=${_gcc_module_sources_sha256}"
      -P "${_runner}"
    COMMENT
      "Compiling and running the imported-binder Highway canary with isolated Homebrew GCC 16"
    VERBATIM
    USES_TERMINAL)
endfunction()
