include_guard(GLOBAL)
include(ExternalProject)
include("${CMAKE_CURRENT_LIST_DIR}/BioseqGccToolchain.cmake")

function(_bioseq_codegen_reject_divergent_path variable expected)
  if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
    if(NOT EXISTS "${${variable}}")
      message(FATAL_ERROR
        "GCC-only isolated codegen has stale ${variable}: "
        "${${variable}}")
    endif()
    file(REAL_PATH "${${variable}}" _actual_real)
    file(REAL_PATH "${expected}" _expected_real)
    if(NOT _actual_real STREQUAL _expected_real)
      message(FATAL_ERROR
        "GCC-only isolated codegen rejects divergent "
        "${variable}=${${variable}}; authenticated root requires "
        "${_expected_real}")
    endif()
  endif()
endfunction()

function(_bioseq_codegen_require_identity label actual expected)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "GCC-only isolated codegen rejects divergent ${label}: "
      "root=${expected}, isolated=${actual}")
  endif()
endfunction()

# bioseq_add_codegen(
#   TARGET bioseq_codegen
#   PROJECT_TARGET bioseq_codegen_project
#   BINARY_DIR "<root-build>/codegen"
#   COMPONENTS external website sql)
#
# Provisions ONE isolated host-tool generator as an ExternalProject and an
# imported executable handle. It prefers GCC 16.1/libstdc++ when that toolchain
# is installed and otherwise reuses the already selected Clang 22/libc++
# toolchain. Root CMake computes the component union once and calls this exactly
# once; external/website consumers depend on the returned targets and never
# create another ExternalProject.
function(bioseq_add_codegen)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    "" "TARGET;PROJECT_TARGET;BINARY_DIR" "COMPONENTS")

  if(NOT ARG_TARGET OR NOT ARG_PROJECT_TARGET OR NOT ARG_BINARY_DIR)
    message(FATAL_ERROR
      "bioseq_add_codegen requires TARGET, PROJECT_TARGET and BINARY_DIR")
  endif()
  if(NOT ARG_COMPONENTS)
    return()
  endif()

  set(_enable_external OFF)
  set(_enable_website OFF)
  set(_enable_sql OFF)
  foreach(_component IN LISTS ARG_COMPONENTS)
    if(_component STREQUAL "external")
      set(_enable_external ON)
    elseif(_component STREQUAL "website")
      set(_enable_website ON)
    elseif(_component STREQUAL "sql")
      set(_enable_sql ON)
    else()
      message(FATAL_ERROR "bioseq_add_codegen: unknown component '${_component}'")
    endif()
  endforeach()

  get_filename_component(_bioseq_repo_root "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)

  # ---- toolchain discovery (moved from bioseq/external/CMakeLists.txt:17-97) --
  set(_bioseq_codegen_gcc_only OFF)
  set(_bioseq_codegen_forced_gcc_modules "")
  if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
    set(_bioseq_codegen_gcc_only ON)
    bioseq_select_gcc16_toolchain(
      REQUESTED_ROOT "${BIOSEQ_GCC16_ROOT}"
      REQUESTED_MODULES "${CMAKE_CXX_STDLIB_MODULES_JSON}"
      OUTPUT_ROOT _bioseq_codegen_authenticated_root
      OUTPUT_C _bioseq_codegen_authenticated_c
      OUTPUT_CXX _bioseq_codegen_authenticated_cxx
      OUTPUT_MODULES _bioseq_codegen_authenticated_modules
      OUTPUT_VERSION _bioseq_codegen_authenticated_version
      OUTPUT_C_SHA256 _bioseq_codegen_authenticated_c_sha256
      OUTPUT_CXX_SHA256 _bioseq_codegen_authenticated_cxx_sha256
      OUTPUT_MODULES_SHA256
        _bioseq_codegen_authenticated_modules_sha256
      OUTPUT_MODULE_SOURCE_CLOSURE_SHA256
        _bioseq_codegen_authenticated_module_source_closure_sha256)
    _bioseq_codegen_reject_divergent_path(
      CMAKE_C_COMPILER "${_bioseq_codegen_authenticated_c}")
    _bioseq_codegen_reject_divergent_path(
      CMAKE_CXX_COMPILER "${_bioseq_codegen_authenticated_cxx}")
    _bioseq_codegen_reject_divergent_path(
      CMAKE_CXX_STDLIB_MODULES_JSON
      "${_bioseq_codegen_authenticated_modules}")
    _bioseq_codegen_reject_divergent_path(
      BIOSEQ_CODEGEN_C_COMPILER "${_bioseq_codegen_authenticated_c}")
    _bioseq_codegen_reject_divergent_path(
      BIOSEQ_CODEGEN_CXX_COMPILER "${_bioseq_codegen_authenticated_cxx}")
    _bioseq_codegen_reject_divergent_path(
      BIOSEQ_CODEGEN_STDLIB_MODULES_JSON
      "${_bioseq_codegen_authenticated_modules}")
    _bioseq_codegen_require_identity(
      "GCC version"
      "${_bioseq_codegen_authenticated_version}"
      "${BIOSEQ_GCC16_VERSION}")
    _bioseq_codegen_require_identity(
      "GCC C compiler SHA-256"
      "${_bioseq_codegen_authenticated_c_sha256}"
      "${BIOSEQ_GCC16_C_SHA256}")
    _bioseq_codegen_require_identity(
      "GCC C++ compiler SHA-256"
      "${_bioseq_codegen_authenticated_cxx_sha256}"
      "${BIOSEQ_GCC16_CXX_SHA256}")
    _bioseq_codegen_require_identity(
      "libstdc++ module inventory SHA-256"
      "${_bioseq_codegen_authenticated_modules_sha256}"
      "${BIOSEQ_GCC16_MODULES_SHA256}")
    _bioseq_codegen_require_identity(
      "libstdc++ module-source closure SHA-256"
      "${_bioseq_codegen_authenticated_module_source_closure_sha256}"
      "${BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256}")
    bioseq_verify_gcc16_toolchain_identity(
      ROOT "${_bioseq_codegen_authenticated_root}"
      C "${_bioseq_codegen_authenticated_c}"
      CXX "${_bioseq_codegen_authenticated_cxx}"
      MODULES "${_bioseq_codegen_authenticated_modules}"
      VERSION "${_bioseq_codegen_authenticated_version}"
      C_SHA256 "${_bioseq_codegen_authenticated_c_sha256}"
      CXX_SHA256 "${_bioseq_codegen_authenticated_cxx_sha256}"
      MODULES_SHA256 "${_bioseq_codegen_authenticated_modules_sha256}"
      MODULE_SOURCE_CLOSURE_SHA256
        "${_bioseq_codegen_authenticated_module_source_closure_sha256}")
    set(BIOSEQ_CODEGEN_C_COMPILER
      "${_bioseq_codegen_authenticated_c}" CACHE FILEPATH
      "C compiler used by the isolated bioseq_codegen generator" FORCE)
    set(BIOSEQ_CODEGEN_CXX_COMPILER
      "${_bioseq_codegen_authenticated_cxx}" CACHE FILEPATH
      "C++ compiler used by the isolated bioseq_codegen generator" FORCE)
    set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON
      "${_bioseq_codegen_authenticated_modules}" CACHE FILEPATH
      "standard-library module metadata used by the isolated generator"
      FORCE)
    set(_bioseq_codegen_forced_gcc_modules
      "${_bioseq_codegen_authenticated_modules}")
    set(BIOSEQ_CODEGEN_C_COMPILER_LAUNCHER
      "${BIOSEQ_GCC16_C_COMPILER_LAUNCHER}" CACHE STRING
      "Authenticated launcher for isolated generator C compilation" FORCE)
    set(BIOSEQ_CODEGEN_CXX_COMPILER_LAUNCHER
      "${BIOSEQ_GCC16_CXX_COMPILER_LAUNCHER}" CACHE STRING
      "Authenticated launcher for isolated generator C++ compilation" FORCE)
  endif()

  set(BIOSEQ_CODEGEN_CXX_COMPILER "" CACHE FILEPATH
    "C++ compiler used by the isolated bioseq_codegen generator")
  if(BIOSEQ_CODEGEN_CXX_COMPILER
      AND NOT EXISTS "${BIOSEQ_CODEGEN_CXX_COMPILER}")
    set(BIOSEQ_CODEGEN_CXX_COMPILER "")
    set(BIOSEQ_CODEGEN_CXX_COMPILER "" CACHE FILEPATH
      "C++ compiler used by the isolated bioseq_codegen generator"
      FORCE)
  endif()
  if(NOT BIOSEQ_CODEGEN_CXX_COMPILER)
    if(EXISTS "/opt/gcc-16.1.0/bin/g++-16")
      set(BIOSEQ_CODEGEN_CXX_COMPILER "/opt/gcc-16.1.0/bin/g++-16")
    elseif(EXISTS "/usr/bin/g++-16")
      set(BIOSEQ_CODEGEN_CXX_COMPILER "/usr/bin/g++-16")
    elseif(CMAKE_CXX_COMPILER AND EXISTS "${CMAKE_CXX_COMPILER}")
      set(BIOSEQ_CODEGEN_CXX_COMPILER "${CMAKE_CXX_COMPILER}")
    endif()
  endif()

  set(BIOSEQ_CODEGEN_C_COMPILER "" CACHE FILEPATH
    "C compiler used by the isolated bioseq_codegen generator")
  if(BIOSEQ_CODEGEN_C_COMPILER
      AND NOT EXISTS "${BIOSEQ_CODEGEN_C_COMPILER}")
    set(BIOSEQ_CODEGEN_C_COMPILER "")
    set(BIOSEQ_CODEGEN_C_COMPILER "" CACHE FILEPATH
      "C compiler used by the isolated bioseq_codegen generator"
      FORCE)
  endif()
  if(NOT BIOSEQ_CODEGEN_C_COMPILER)
    if(EXISTS "/opt/gcc-16.1.0/bin/gcc-16")
      set(BIOSEQ_CODEGEN_C_COMPILER "/opt/gcc-16.1.0/bin/gcc-16")
    elseif(EXISTS "/usr/bin/gcc-16")
      set(BIOSEQ_CODEGEN_C_COMPILER "/usr/bin/gcc-16")
    elseif(CMAKE_C_COMPILER AND EXISTS "${CMAKE_C_COMPILER}")
      set(BIOSEQ_CODEGEN_C_COMPILER "${CMAKE_C_COMPILER}")
    endif()
  endif()

  set(BIOSEQ_CODEGEN_LLVM_CONFIG "" CACHE FILEPATH
    "llvm-config used by cppast/Clang Tooling to locate libclang")
  set(BIOSEQ_CODEGEN_CLANG_BINARY "" CACHE FILEPATH
    "Clang parser executable passed to external-client generation")
  if(BIOSEQ_CODEGEN_LLVM_CONFIG
      AND NOT EXISTS "${BIOSEQ_CODEGEN_LLVM_CONFIG}")
    set(BIOSEQ_CODEGEN_LLVM_CONFIG "")
    set(BIOSEQ_CODEGEN_LLVM_CONFIG "" CACHE FILEPATH
      "llvm-config used by cppast/Clang Tooling to locate libclang"
      FORCE)
  endif()
  if(BIOSEQ_CODEGEN_CLANG_BINARY
      AND NOT EXISTS "${BIOSEQ_CODEGEN_CLANG_BINARY}")
    set(BIOSEQ_CODEGEN_CLANG_BINARY "")
    set(BIOSEQ_CODEGEN_CLANG_BINARY "" CACHE FILEPATH
      "Clang parser executable passed to external-client generation"
      FORCE)
  endif()
  get_filename_component(
    _bioseq_codegen_compiler_bin "${BIOSEQ_CODEGEN_CXX_COMPILER}" DIRECTORY)
  if(NOT BIOSEQ_CODEGEN_LLVM_CONFIG)
    foreach(_llvm_config IN ITEMS
        "${_bioseq_codegen_compiler_bin}/llvm-config"
        "${_bioseq_codegen_compiler_bin}/llvm-config-22"
        "/usr/bin/llvm-config-22"
        "/usr/lib/llvm-22/bin/llvm-config"
        "/opt/llvm-22/bin/llvm-config"
        "/opt/homebrew/opt/llvm@22/bin/llvm-config"
        "/usr/local/opt/llvm/bin/llvm-config")
      if(EXISTS "${_llvm_config}")
        set(BIOSEQ_CODEGEN_LLVM_CONFIG "${_llvm_config}")
        break()
      endif()
    endforeach()
  endif()
  if(NOT BIOSEQ_CODEGEN_CLANG_BINARY)
    foreach(_clang_binary IN ITEMS
        "${_bioseq_codegen_compiler_bin}/clang++"
        "${_bioseq_codegen_compiler_bin}/clang++-22"
        "/usr/bin/clang++-22"
        "/usr/lib/llvm-22/bin/clang++"
        "/opt/llvm-22/bin/clang++"
        "/opt/homebrew/opt/llvm@22/bin/clang++"
        "/usr/local/opt/llvm/bin/clang++")
      if(EXISTS "${_clang_binary}")
        set(BIOSEQ_CODEGEN_CLANG_BINARY "${_clang_binary}")
        break()
      endif()
    endforeach()
  endif()

  foreach(_required_tool IN ITEMS
      BIOSEQ_CODEGEN_CXX_COMPILER
      BIOSEQ_CODEGEN_C_COMPILER
      BIOSEQ_CODEGEN_LLVM_CONFIG
      BIOSEQ_CODEGEN_CLANG_BINARY)
    if(NOT EXISTS "${${_required_tool}}")
      message(FATAL_ERROR
        "${_required_tool} does not name an installed executable: "
        "'${${_required_tool}}'")
    endif()
  endforeach()
  get_filename_component(BIOSEQ_CODEGEN_CXX_COMPILER
    "${BIOSEQ_CODEGEN_CXX_COMPILER}" ABSOLUTE)
  get_filename_component(BIOSEQ_CODEGEN_C_COMPILER
    "${BIOSEQ_CODEGEN_C_COMPILER}" ABSOLUTE)
  get_filename_component(BIOSEQ_CODEGEN_LLVM_CONFIG
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}" ABSOLUTE)
  get_filename_component(BIOSEQ_CODEGEN_CLANG_BINARY
    "${BIOSEQ_CODEGEN_CLANG_BINARY}" ABSOLUTE)
  # SQL and safety consumers expand these names after this function returns.
  # Keep both the cache and the caller's normal-variable scope authoritative.
  set(BIOSEQ_CODEGEN_CXX_COMPILER
    "${BIOSEQ_CODEGEN_CXX_COMPILER}" CACHE FILEPATH
    "C++ compiler used by the isolated bioseq_codegen generator"
    FORCE)
  set(BIOSEQ_CODEGEN_C_COMPILER
    "${BIOSEQ_CODEGEN_C_COMPILER}" CACHE FILEPATH
    "C compiler used by the isolated bioseq_codegen generator"
    FORCE)
  set(BIOSEQ_CODEGEN_LLVM_CONFIG
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}" CACHE FILEPATH
    "llvm-config used by cppast/Clang Tooling to locate libclang"
    FORCE)
  set(BIOSEQ_CODEGEN_CLANG_BINARY
    "${BIOSEQ_CODEGEN_CLANG_BINARY}" CACHE FILEPATH
    "Clang parser executable passed to external-client generation"
    FORCE)
  set(BIOSEQ_CODEGEN_CXX_COMPILER
    "${BIOSEQ_CODEGEN_CXX_COMPILER}" PARENT_SCOPE)
  set(BIOSEQ_CODEGEN_C_COMPILER
    "${BIOSEQ_CODEGEN_C_COMPILER}" PARENT_SCOPE)
  set(BIOSEQ_CODEGEN_LLVM_CONFIG
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}" PARENT_SCOPE)
  set(BIOSEQ_CODEGEN_CLANG_BINARY
    "${BIOSEQ_CODEGEN_CLANG_BINARY}" PARENT_SCOPE)

  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_LLVM_CONFIG}" --version
    OUTPUT_VARIABLE _llvm_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  if(_llvm_version VERSION_LESS "22")
    message(FATAL_ERROR
      "isolated generator AST tooling requires LLVM >= 22; found "
      "${_llvm_version} at ${BIOSEQ_CODEGEN_LLVM_CONFIG}")
  endif()
  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_CLANG_BINARY}" --version
    OUTPUT_VARIABLE _bioseq_codegen_clang_version_output
    ERROR_VARIABLE _bioseq_codegen_clang_version_error
    RESULT_VARIABLE _bioseq_codegen_clang_version_status)
  if(NOT _bioseq_codegen_clang_version_status EQUAL 0)
    message(FATAL_ERROR
      "cannot identify selected Clang AST parser: "
      "${BIOSEQ_CODEGEN_CLANG_BINARY}")
  endif()
  string(REGEX MATCH
    "[Cc]lang version ([0-9]+(\\.[0-9]+)*)"
    _bioseq_codegen_clang_version_match
    "${_bioseq_codegen_clang_version_output}\n${_bioseq_codegen_clang_version_error}")
  set(_bioseq_codegen_clang_version
    "${CMAKE_MATCH_1}")
  if(NOT _bioseq_codegen_clang_version_match
      OR _bioseq_codegen_clang_version VERSION_LESS "22")
    message(FATAL_ERROR
      "isolated generator AST tooling requires Clang >= 22; found "
      "${_bioseq_codegen_clang_version_output}"
      "${_bioseq_codegen_clang_version_error}")
  endif()
  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_LLVM_CONFIG}" --includedir
    OUTPUT_VARIABLE _llvm_include_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_LLVM_CONFIG}" --libdir
    OUTPUT_VARIABLE _llvm_library_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_LLVM_CONFIG}" --bindir
    OUTPUT_VARIABLE _llvm_binary_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_LLVM_CONFIG}" --prefix
    OUTPUT_VARIABLE _llvm_prefix
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  file(REAL_PATH "${_llvm_binary_dir}" _llvm_binary_dir_real)
  file(REAL_PATH "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    _bioseq_codegen_real_clang)
  get_filename_component(_bioseq_codegen_real_clang_bin
    "${_bioseq_codegen_real_clang}" DIRECTORY)
  if(NOT "${_bioseq_codegen_real_clang_bin}"
      STREQUAL "${_llvm_binary_dir_real}")
    set(BIOSEQ_CODEGEN_CLANG_BINARY "")
    set(BIOSEQ_CODEGEN_CLANG_BINARY "" CACHE FILEPATH
      "Clang parser executable passed to external-client generation"
      FORCE)
    foreach(_clang_binary IN ITEMS
        "${_llvm_binary_dir}/clang++"
        "${_llvm_binary_dir}/clang++-22")
      if(EXISTS "${_clang_binary}")
        set(BIOSEQ_CODEGEN_CLANG_BINARY "${_clang_binary}")
        break()
      endif()
    endforeach()
    if(NOT EXISTS "${BIOSEQ_CODEGEN_CLANG_BINARY}")
      message(FATAL_ERROR
        "selected Clang is not paired with llvm-config --bindir and no "
        "paired clang++ executable was found")
    endif()
    get_filename_component(BIOSEQ_CODEGEN_CLANG_BINARY
      "${BIOSEQ_CODEGEN_CLANG_BINARY}" ABSOLUTE)
    set(BIOSEQ_CODEGEN_CLANG_BINARY
      "${BIOSEQ_CODEGEN_CLANG_BINARY}" CACHE FILEPATH
      "Clang parser executable passed to external-client generation"
      FORCE)
    set(BIOSEQ_CODEGEN_CLANG_BINARY
      "${BIOSEQ_CODEGEN_CLANG_BINARY}" PARENT_SCOPE)
    file(REAL_PATH "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      _bioseq_codegen_real_clang)
    get_filename_component(_bioseq_codegen_real_clang_bin
      "${_bioseq_codegen_real_clang}" DIRECTORY)
  endif()
  set(_bioseq_codegen_clang_scan_deps
    "${_llvm_binary_dir}/clang-scan-deps")
  if(NOT EXISTS "${_bioseq_codegen_clang_scan_deps}")
    get_filename_component(
      _bioseq_codegen_real_clang
      "${BIOSEQ_CODEGEN_CLANG_BINARY}" REALPATH)
    get_filename_component(
      _bioseq_codegen_real_clang_bin
      "${_bioseq_codegen_real_clang}" DIRECTORY)
    set(_bioseq_codegen_clang_scan_deps
      "${_bioseq_codegen_real_clang_bin}/clang-scan-deps")
  endif()
  if(NOT EXISTS "${_bioseq_codegen_clang_scan_deps}")
    set(BIOSEQ_CODEGEN_CLANG_SCAN_DEPS "" CACHE FILEPATH
      "clang-scan-deps paired with the selected LibTooling build"
      FORCE)
    message(FATAL_ERROR
      "clang-scan-deps was not found in llvm-config --bindir or beside "
      "the canonical selected Clang binary")
  endif()
  get_filename_component(BIOSEQ_CODEGEN_CLANG_SCAN_DEPS
    "${_bioseq_codegen_clang_scan_deps}" ABSOLUTE)
  set(BIOSEQ_CODEGEN_CLANG_SCAN_DEPS
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}" CACHE FILEPATH
    "clang-scan-deps paired with the selected LibTooling build"
    FORCE)
  set(BIOSEQ_CODEGEN_CLANG_SCAN_DEPS
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}" PARENT_SCOPE)

  set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON "" CACHE FILEPATH
    "libc++ module metadata paired with the selected Clang")
  if(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
      AND NOT EXISTS
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}")
    set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON "")
    set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON ""
      CACHE FILEPATH
      "libc++ module metadata paired with the selected Clang"
      FORCE)
  endif()
  if(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
      AND EXISTS "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}")
    file(REAL_PATH "${_llvm_prefix}" _llvm_prefix_real)
    file(REAL_PATH "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      _clang_modules_json_real)
    string(FIND "${_clang_modules_json_real}/"
      "${_llvm_prefix_real}/" _clang_modules_under_prefix)
    get_filename_component(_clang_modules_json_name
      "${_clang_modules_json_real}" NAME)
    if(NOT _clang_modules_under_prefix EQUAL 0
        OR NOT _clang_modules_json_name STREQUAL
          "libc++.modules.json")
      set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON "")
      set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON ""
        CACHE FILEPATH
        "libc++ module metadata paired with the selected Clang"
        FORCE)
    endif()
  endif()
  if(NOT BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON)
    foreach(_clang_modules_json IN ITEMS
        "${_llvm_library_dir}/c++/libc++.modules.json"
        "${_llvm_library_dir}/x86_64-unknown-linux-gnu/c++/libc++.modules.json"
        "${_llvm_prefix}/lib/c++/libc++.modules.json"
        "${_llvm_prefix}/lib/x86_64-unknown-linux-gnu/c++/libc++.modules.json"
        "${_llvm_prefix}/lib/libc++.modules.json")
      if(_clang_modules_json
          AND EXISTS "${_clang_modules_json}")
        set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
          "${_clang_modules_json}")
        break()
      endif()
    endforeach()
  endif()
  if(NOT EXISTS "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}")
    message(FATAL_ERROR
      "selected-Clang libc++.modules.json was not found; set "
      "BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON explicitly")
  endif()
  get_filename_component(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}" ABSOLUTE)
  set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}" CACHE FILEPATH
    "libc++ module metadata paired with the selected Clang"
    FORCE)
  set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}" PARENT_SCOPE)
  file(SHA256 "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    _bioseq_codegen_clang_sha256)
  file(SHA256 "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    _bioseq_codegen_clang_scan_deps_sha256)
  file(SHA256 "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    _bioseq_codegen_llvm_config_sha256)
  file(SHA256 "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    _bioseq_codegen_clang_stdlib_modules_sha256)
  set(BIOSEQ_CODEGEN_CLANG_SHA256
    "${_bioseq_codegen_clang_sha256}" CACHE INTERNAL
    "Configure-time selected Clang binary identity"
    FORCE)
  set(BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256
    "${_bioseq_codegen_clang_scan_deps_sha256}" CACHE INTERNAL
    "Configure-time selected clang-scan-deps binary identity"
    FORCE)
  set(BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256
    "${_bioseq_codegen_llvm_config_sha256}" CACHE INTERNAL
    "Configure-time selected llvm-config binary identity"
    FORCE)
  set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256
    "${_bioseq_codegen_clang_stdlib_modules_sha256}" CACHE INTERNAL
    "Configure-time selected libc++ module inventory identity"
    FORCE)
  set(BIOSEQ_CODEGEN_CLANG_SHA256
    "${_bioseq_codegen_clang_sha256}" PARENT_SCOPE)
  set(BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256
    "${_bioseq_codegen_clang_scan_deps_sha256}" PARENT_SCOPE)
  set(BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256
    "${_bioseq_codegen_llvm_config_sha256}" PARENT_SCOPE)
  set(BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256
    "${_bioseq_codegen_clang_stdlib_modules_sha256}" PARENT_SCOPE)
  find_library(_bioseq_codegen_libclang
    NAMES clang libclang
    PATHS "${_llvm_library_dir}"
    NO_DEFAULT_PATH
    NO_CACHE
    REQUIRED)

  set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON "" CACHE FILEPATH
    "standard-library module metadata used by the isolated generator")
  set(_bioseq_codegen_configured_stdlib_modules
    "${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}")
  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_CXX_COMPILER}" --version
    OUTPUT_VARIABLE _bioseq_codegen_cxx_version_output
    ERROR_VARIABLE _bioseq_codegen_cxx_version_error
    RESULT_VARIABLE _bioseq_codegen_cxx_version_status)
  if(NOT _bioseq_codegen_cxx_version_status EQUAL 0)
    message(FATAL_ERROR
      "cannot identify isolated generator C++ compiler: "
      "${BIOSEQ_CODEGEN_CXX_COMPILER}")
  endif()
  string(TOLOWER
    "${_bioseq_codegen_cxx_version_output}\n${_bioseq_codegen_cxx_version_error}"
    _bioseq_codegen_cxx_version_identity)
  set(_bioseq_codegen_host_stdlib_modules "")
  if(_bioseq_codegen_cxx_version_identity MATCHES "clang")
    file(REAL_PATH "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      _bioseq_codegen_expected_clang_modules)
    if(_bioseq_codegen_configured_stdlib_modules
        AND EXISTS "${_bioseq_codegen_configured_stdlib_modules}")
      file(REAL_PATH "${_bioseq_codegen_configured_stdlib_modules}"
        _bioseq_codegen_configured_stdlib_modules_real)
      if(NOT "${_bioseq_codegen_configured_stdlib_modules_real}"
          STREQUAL "${_bioseq_codegen_expected_clang_modules}")
        set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON "")
        set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON "" CACHE FILEPATH
          "standard-library module metadata used by the isolated generator"
          FORCE)
      endif()
    endif()
    set(_bioseq_codegen_host_stdlib_modules
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}")
  elseif(_bioseq_codegen_cxx_version_identity
      MATCHES "gcc|g\\+\\+|free software foundation")
    execute_process(
      COMMAND "${BIOSEQ_CODEGEN_CXX_COMPILER}" -dumpfullversion
      OUTPUT_VARIABLE _bioseq_codegen_gcc_version
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
    if(NOT _bioseq_codegen_gcc_version)
      execute_process(
        COMMAND "${BIOSEQ_CODEGEN_CXX_COMPILER}" -dumpversion
        OUTPUT_VARIABLE _bioseq_codegen_gcc_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    endif()
    string(REGEX MATCH "^[0-9]+"
      _bioseq_codegen_gcc_major
      "${_bioseq_codegen_gcc_version}")
    file(REAL_PATH "${BIOSEQ_CODEGEN_CXX_COMPILER}"
      _bioseq_codegen_cxx_compiler_real)
    get_filename_component(_bioseq_codegen_cxx_compiler_bin_real
      "${_bioseq_codegen_cxx_compiler_real}" DIRECTORY)
    get_filename_component(_bioseq_codegen_cxx_prefix
      "${_bioseq_codegen_cxx_compiler_bin_real}/.." ABSOLUTE)
    file(REAL_PATH "${_bioseq_codegen_cxx_prefix}"
      _bioseq_codegen_cxx_prefix_real)
    execute_process(
      COMMAND "${BIOSEQ_CODEGEN_CXX_COMPILER}"
        -print-file-name=libstdc++.modules.json
      OUTPUT_VARIABLE _bioseq_codegen_gcc_printed_modules
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB _bioseq_codegen_gcc_module_candidates
      LIST_DIRECTORIES FALSE
      "${_bioseq_codegen_cxx_prefix}/lib/gcc/*/${_bioseq_codegen_gcc_version}/libstdc++.modules.json"
      "${_bioseq_codegen_cxx_prefix}/lib/gcc/*/${_bioseq_codegen_gcc_major}/libstdc++.modules.json")
    list(APPEND _bioseq_codegen_gcc_module_candidates
      "${_bioseq_codegen_gcc_printed_modules}"
      "${_bioseq_codegen_cxx_prefix}/lib64/libstdc++.modules.json")
    list(REMOVE_DUPLICATES _bioseq_codegen_gcc_module_candidates)
    set(_bioseq_codegen_valid_gcc_modules)
    foreach(_bioseq_codegen_gcc_modules
        IN LISTS _bioseq_codegen_gcc_module_candidates)
      if(NOT EXISTS "${_bioseq_codegen_gcc_modules}")
        continue()
      endif()
      file(REAL_PATH "${_bioseq_codegen_gcc_modules}"
        _bioseq_codegen_gcc_modules_real)
      get_filename_component(_bioseq_codegen_gcc_modules_name
        "${_bioseq_codegen_gcc_modules_real}" NAME)
      string(FIND "${_bioseq_codegen_gcc_modules_real}/"
        "${_bioseq_codegen_cxx_prefix_real}/"
        _bioseq_codegen_gcc_modules_prefix)
      if("${_bioseq_codegen_gcc_modules_name}"
            STREQUAL "libstdc++.modules.json"
          AND _bioseq_codegen_gcc_modules_prefix EQUAL 0)
        list(APPEND _bioseq_codegen_valid_gcc_modules
          "${_bioseq_codegen_gcc_modules_real}")
      endif()
    endforeach()
    list(SORT _bioseq_codegen_valid_gcc_modules)
    if(_bioseq_codegen_configured_stdlib_modules
        AND EXISTS "${_bioseq_codegen_configured_stdlib_modules}")
      file(REAL_PATH "${_bioseq_codegen_configured_stdlib_modules}"
        _bioseq_codegen_configured_stdlib_modules_real)
      list(FIND _bioseq_codegen_valid_gcc_modules
        "${_bioseq_codegen_configured_stdlib_modules_real}"
        _bioseq_codegen_configured_gcc_modules_index)
      if(NOT _bioseq_codegen_configured_gcc_modules_index EQUAL -1)
        set(_bioseq_codegen_host_stdlib_modules
          "${_bioseq_codegen_configured_stdlib_modules_real}")
      endif()
    endif()
    if(NOT _bioseq_codegen_host_stdlib_modules
        AND _bioseq_codegen_valid_gcc_modules)
      list(GET _bioseq_codegen_valid_gcc_modules 0
        _bioseq_codegen_host_stdlib_modules)
    endif()
  else()
    message(FATAL_ERROR
      "isolated generator compiler is neither supported GCC nor Clang: "
      "${BIOSEQ_CODEGEN_CXX_COMPILER}")
  endif()
  if(NOT EXISTS "${_bioseq_codegen_host_stdlib_modules}")
    set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON "" CACHE FILEPATH
      "standard-library module metadata used by the isolated generator"
      FORCE)
    message(FATAL_ERROR
      "standard-library module metadata matching the selected isolated "
      "generator compiler was not found")
  endif()
  get_filename_component(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON
    "${_bioseq_codegen_host_stdlib_modules}" ABSOLUTE)
  if(_bioseq_codegen_gcc_only)
    file(REAL_PATH "${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}"
      _bioseq_codegen_selected_modules_real)
    file(REAL_PATH "${_bioseq_codegen_forced_gcc_modules}"
      _bioseq_codegen_forced_modules_real)
    if(NOT _bioseq_codegen_selected_modules_real
        STREQUAL _bioseq_codegen_forced_modules_real)
      message(FATAL_ERROR
        "GCC-only isolated codegen selected a libstdc++ inventory "
        "different from the authenticated root pair")
    endif()
  endif()
  set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON
    "${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}" CACHE FILEPATH
    "standard-library module metadata used by the isolated generator"
    FORCE)
  set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON
    "${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}" PARENT_SCOPE)

  set(_runtime_dir "${ARG_BINARY_DIR}/generator-bin")
  set(_executable "${_runtime_dir}/bioseq_codegen")
  set(_bioseq_codegen_gcc_cmake_args)
  if(_bioseq_codegen_gcc_only)
    string(REPLACE ";" "|"
      _bioseq_codegen_c_launcher_argument
      "${BIOSEQ_CODEGEN_C_COMPILER_LAUNCHER}")
    string(REPLACE ";" "|"
      _bioseq_codegen_cxx_launcher_argument
      "${BIOSEQ_CODEGEN_CXX_COMPILER_LAUNCHER}")
    list(APPEND _bioseq_codegen_gcc_cmake_args
      "-DCMAKE_C_COMPILER_LAUNCHER:STRING=${_bioseq_codegen_c_launcher_argument}"
      "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=${_bioseq_codegen_cxx_launcher_argument}"
      "-DBIOSEQ_EXPECTED_GCC_ROOT:PATH=${_bioseq_codegen_authenticated_root}"
      "-DBIOSEQ_EXPECTED_GCC_C:FILEPATH=${_bioseq_codegen_authenticated_c}"
      "-DBIOSEQ_EXPECTED_GCC_CXX:FILEPATH=${_bioseq_codegen_authenticated_cxx}"
      "-DBIOSEQ_EXPECTED_GCC_MODULES:FILEPATH=${_bioseq_codegen_authenticated_modules}"
      "-DBIOSEQ_EXPECTED_GCC_VERSION:STRING=${_bioseq_codegen_authenticated_version}"
      "-DBIOSEQ_EXPECTED_GCC_C_SHA256:STRING=${_bioseq_codegen_authenticated_c_sha256}"
      "-DBIOSEQ_EXPECTED_GCC_CXX_SHA256:STRING=${_bioseq_codegen_authenticated_cxx_sha256}"
      "-DBIOSEQ_EXPECTED_GCC_MODULES_SHA256:STRING=${_bioseq_codegen_authenticated_modules_sha256}"
      "-DBIOSEQ_EXPECTED_GCC_MODULE_SOURCE_CLOSURE_SHA256:STRING=${_bioseq_codegen_authenticated_module_source_closure_sha256}")
  endif()

  ExternalProject_Add(${ARG_PROJECT_TARGET}
    PREFIX "${ARG_BINARY_DIR}/generator-project"
    SOURCE_DIR "${_bioseq_repo_root}/main/codegen"
    BINARY_DIR "${ARG_BINARY_DIR}/generator-build"
    EXCLUDE_FROM_ALL TRUE
    LIST_SEPARATOR "|"
    CMAKE_ARGS
      "-DCMAKE_BUILD_TYPE:STRING=Release"
      "-DBUILD_TESTING:BOOL=OFF"
      "-DCMAKE_C_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_CXX_COMPILER}"
      "-DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5"
      "-DCMAKE_CXX_STDLIB_MODULES_JSON:FILEPATH=${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}"
      "-DBIOSEQ_REPOSITORY_ROOT:PATH=${_bioseq_repo_root}"
      "-DBIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY:PATH=${_runtime_dir}"
      "-DBIOSEQ_CODEGEN_ENABLE_EXTERNAL:BOOL=${_enable_external}"
      "-DBIOSEQ_CODEGEN_ENABLE_WEBSITE:BOOL=${_enable_website}"
      "-DBIOSEQ_CODEGEN_ENABLE_SQL:BOOL=${_enable_sql}"
      "-DCLANG_BINARY:FILEPATH=${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "-DLLVM_CONFIG_BINARY:FILEPATH=${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "-DLLVM_VERSION_EXPLICIT:STRING=${_llvm_version}"
      "-DLIBCLANG_INCLUDE_DIR:PATH=${_llvm_include_dir}"
      "-DLIBCLANG_LIBRARY:FILEPATH=${_bioseq_codegen_libclang}"
      ${_bioseq_codegen_gcc_cmake_args}
    BUILD_COMMAND
      "${CMAKE_COMMAND}" --build <BINARY_DIR> --target bioseq_codegen
    INSTALL_COMMAND ""
    TEST_COMMAND ""
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${_executable}"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE)

  # Imported handle for the built executable. Consumers order their generation
  # custom commands via DEPENDS on ${ARG_PROJECT_TARGET} plus the byproduct
  # path; add_dependencies() is not valid on an imported target.
  add_executable(${ARG_TARGET} IMPORTED GLOBAL)
  set_target_properties(${ARG_TARGET} PROPERTIES
    IMPORTED_LOCATION "${_executable}")

  set(BIOSEQ_CODEGEN_EXECUTABLE "${_executable}" CACHE INTERNAL
    "Path to the built bioseq_codegen host-tool executable"
    FORCE)
  set(BIOSEQ_CODEGEN_EXECUTABLE "${_executable}" PARENT_SCOPE)
endfunction()
