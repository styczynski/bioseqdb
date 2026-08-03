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

function(_bioseq_highway_gcc_canary_query_driver
    output compiler description)
  if(NOT EXISTS "/usr/bin/env")
    message(FATAL_ERROR
      "Highway GCC canary requires /usr/bin/env for empty-environment queries")
  endif()
  execute_process(
    COMMAND "/usr/bin/env" -i
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
      "LC_ALL=C"
      "LANG=C"
      "${compiler}" ${ARGN}
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  string(STRIP "${_stdout}" _stdout)
  if(NOT _status EQUAL 0 OR "${_stdout}" STREQUAL "")
    message(FATAL_ERROR
      "Highway GCC canary could not query ${description}: "
      "status=${_status}, stderr=${_stderr}")
  endif()
  set(${output} "${_stdout}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_driver_closure
    output_digest output_libstdcxx output_libgcc root compiler)
  file(REAL_PATH "${root}" _root_real)
  file(REAL_PATH "${compiler}" _compiler_real)
  string(FIND "${_compiler_real}/" "${_root_real}/" _compiler_owned)
  if(NOT _compiler_owned EQUAL 0)
    message(FATAL_ERROR
      "Highway GCC canary compiler escaped its authenticated prefix")
  endif()

  _bioseq_highway_gcc_canary_query_driver(
    _cc1plus "${_compiler_real}" "cc1plus" -print-prog-name=cc1plus)
  _bioseq_highway_gcc_canary_query_driver(
    _collect2 "${_compiler_real}" "collect2" -print-prog-name=collect2)
  _bioseq_highway_gcc_canary_query_driver(
    _libstdcxx "${_compiler_real}" "libstdc++.dylib"
    -print-file-name=libstdc++.dylib)
  _bioseq_highway_gcc_canary_query_driver(
    _libstdcxx_static "${_compiler_real}" "libstdc++.a"
    -print-file-name=libstdc++.a)
  _bioseq_highway_gcc_canary_query_driver(
    _libgcc "${_compiler_real}" "libgcc_s.1.1.dylib"
    -print-file-name=libgcc_s.1.1.dylib)
  _bioseq_highway_gcc_canary_query_driver(
    _libgcc_static "${_compiler_real}" "libgcc.a"
    -print-file-name=libgcc.a)
  _bioseq_highway_gcc_canary_query_driver(
    _libgcc_eh "${_compiler_real}" "libgcc_eh.a"
    -print-file-name=libgcc_eh.a)
  _bioseq_highway_gcc_canary_query_driver(
    _specs "${_compiler_real}" "effective GCC specs" -dumpspecs)
  _bioseq_highway_gcc_canary_query_driver(
    _search_dirs "${_compiler_real}" "effective GCC search directories"
    -print-search-dirs)
  string(SHA256 _specs_sha256 "${_specs}")
  string(SHA256 _search_dirs_sha256 "${_search_dirs}")

  set(_artifact_labels
    compiler cc1plus collect2 libstdcxx_shared libstdcxx_static
    libgcc_shared libgcc_static libgcc_eh)
  set(_artifact_paths
    "${_compiler_real}" "${_cc1plus}" "${_collect2}" "${_libstdcxx}"
    "${_libstdcxx_static}" "${_libgcc}" "${_libgcc_static}"
    "${_libgcc_eh}")
  list(LENGTH _artifact_labels _artifact_count)
  math(EXPR _artifact_last "${_artifact_count} - 1")
  set(_artifact_rows)
  foreach(_artifact_index RANGE 0 ${_artifact_last})
    list(GET _artifact_labels ${_artifact_index} _artifact_label)
    list(GET _artifact_paths ${_artifact_index} _artifact_path)
    if(NOT IS_ABSOLUTE "${_artifact_path}"
        OR NOT EXISTS "${_artifact_path}"
        OR IS_DIRECTORY "${_artifact_path}")
      message(FATAL_ERROR
        "Highway GCC canary ${_artifact_label} is not an absolute regular "
        "driver artifact: ${_artifact_path}")
    endif()
    file(REAL_PATH "${_artifact_path}" _artifact_real)
    string(FIND "${_artifact_real}/" "${_root_real}/" _artifact_owned)
    if(NOT _artifact_owned EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary ${_artifact_label} escaped the selected prefix: "
        "${_artifact_real}")
    endif()
    file(SHA256 "${_artifact_real}" _artifact_sha256)
    list(APPEND _artifact_rows
      "${_artifact_label}\t${_artifact_real}\t${_artifact_sha256}")
    if(_artifact_label STREQUAL "libstdcxx_shared")
      set(_libstdcxx_real "${_artifact_real}")
    elseif(_artifact_label STREQUAL "libgcc_shared")
      set(_libgcc_real "${_artifact_real}")
    endif()
  endforeach()

  # Hashing the complete libstdc++ include tree is a conservative superset of
  # every textual standard-library header reachable by the canary sources.
  file(GLOB_RECURSE _libstdcxx_headers LIST_DIRECTORIES FALSE
    "${_root_real}/include/c++/*")
  list(SORT _libstdcxx_headers)
  if(NOT _libstdcxx_headers)
    message(FATAL_ERROR
      "Highway GCC canary found no libstdc++ textual header closure")
  endif()
  set(_header_material "bioseq-gcc-libstdcxx-headers/v1\n")
  foreach(_header IN LISTS _libstdcxx_headers)
    file(REAL_PATH "${_header}" _header_real)
    string(FIND "${_header_real}/" "${_root_real}/" _header_owned)
    if(NOT _header_owned EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary libstdc++ header escaped the selected prefix: "
        "${_header_real}")
    endif()
    cmake_path(RELATIVE_PATH _header
      BASE_DIRECTORY "${_root_real}" OUTPUT_VARIABLE _header_relative)
    file(SHA256 "${_header_real}" _header_sha256)
    string(APPEND _header_material
      "${_header_relative}\t${_header_sha256}\n")
  endforeach()
  string(SHA256 _header_closure_sha256 "${_header_material}")

  list(SORT _artifact_rows)
  string(CONCAT _closure_material
    "bioseq-highway-gcc-driver-closure/v1\n"
    "specs\t${_specs_sha256}\n"
    "search-dirs\t${_search_dirs_sha256}\n"
    "libstdcxx-headers\t${_header_closure_sha256}\n")
  foreach(_artifact_row IN LISTS _artifact_rows)
    string(APPEND _closure_material "${_artifact_row}\n")
  endforeach()
  string(SHA256 _closure_sha256 "${_closure_material}")
  set(${output_digest} "${_closure_sha256}" PARENT_SCOPE)
  set(${output_libstdcxx} "${_libstdcxx_real}" PARENT_SCOPE)
  set(${output_libgcc} "${_libgcc_real}" PARENT_SCOPE)
endfunction()

function(bioseq_add_highway_gcc_canary)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "TARGET;SOURCE_ROOT;HIGHWAY_INCLUDE_ROOT;GENERATION_TARGET;GENERATED_ROOT;PRIMARY;DRIVER;MAIN"
    "")
  foreach(_required IN ITEMS
      TARGET SOURCE_ROOT HIGHWAY_INCLUDE_ROOT GENERATION_TARGET GENERATED_ROOT
      PRIMARY DRIVER MAIN)
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
  if(NOT TARGET "${ARG_GENERATION_TARGET}")
    message(FATAL_ERROR
      "Highway GCC canary generation target does not exist: "
      "${ARG_GENERATION_TARGET}")
  endif()
  foreach(_checked_source IN ITEMS PRIMARY DRIVER MAIN)
    if(NOT IS_ABSOLUTE "${ARG_${_checked_source}}"
        OR NOT EXISTS "${ARG_${_checked_source}}"
        OR IS_DIRECTORY "${ARG_${_checked_source}}")
      message(FATAL_ERROR
        "Highway GCC canary ${_checked_source} must be an existing absolute "
        "regular path: ${ARG_${_checked_source}}")
    endif()
  endforeach()
  if(NOT IS_ABSOLUTE "${ARG_GENERATED_ROOT}")
    message(FATAL_ERROR
      "Highway GCC canary GENERATED_ROOT must be absolute")
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
  _bioseq_highway_gcc_canary_driver_closure(
    _gcc_driver_closure_sha256 _gcc_libstdcxx _gcc_libgcc
    "${_gcc_root}" "${_gcc_cxx}")

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
    COMMAND "/usr/bin/env" -i
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
      "LC_ALL=C"
      "LANG=C"
      "${CMAKE_COMMAND}"
      "-DBIOSEQ_GCC_CANARY_CMAKE=${CMAKE_COMMAND}"
      "-DBIOSEQ_GCC_CANARY_NINJA=${CMAKE_MAKE_PROGRAM}"
      "-DBIOSEQ_GCC_CANARY_SOURCE=${_nested_source}"
      "-DBIOSEQ_GCC_CANARY_BUILD=${_nested_build}"
      "-DBIOSEQ_GCC_CANARY_OUTER_BINARY=${CMAKE_BINARY_DIR}"
      "-DBIOSEQ_GCC_CANARY_CANARY_HELPER=${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
      "-DBIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT=${_canary_source_root}"
      "-DBIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT=${_highway_include_root}"
      "-DBIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST=${_highway_header_digest}"
      "-DBIOSEQ_GCC_CANARY_GENERATED_ROOT=${ARG_GENERATED_ROOT}"
      "-DBIOSEQ_GCC_CANARY_PRIMARY=${ARG_PRIMARY}"
      "-DBIOSEQ_GCC_CANARY_DRIVER=${ARG_DRIVER}"
      "-DBIOSEQ_GCC_CANARY_MAIN=${ARG_MAIN}"
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
      "-DBIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256=${_gcc_driver_closure_sha256}"
      -P "${_runner}"
    COMMENT
      "Compiling and linking GCC Highway/module canaries with an empty environment"
    VERBATIM
    USES_TERMINAL)
  add_dependencies("${ARG_TARGET}" "${ARG_GENERATION_TARGET}")
endfunction()
