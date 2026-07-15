include_guard(GLOBAL)
include(ExternalProject)

# bioseq_add_codegen(
#   TARGET bioseq_codegen
#   PROJECT_TARGET bioseq_codegen_project
#   BINARY_DIR "<root-build>/codegen"
#   COMPONENTS external website)
#
# Provisions ONE isolated host-tool generator (GCC 16.1 / libstdc++) as an
# ExternalProject and an imported executable handle. Root CMake computes the
# component union once and calls this exactly once; external/website consumers
# depend on the returned targets and never create another ExternalProject.
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
  foreach(_component IN LISTS ARG_COMPONENTS)
    if(_component STREQUAL "external")
      set(_enable_external ON)
    elseif(_component STREQUAL "website")
      set(_enable_website ON)
    else()
      message(FATAL_ERROR "bioseq_add_codegen: unknown component '${_component}'")
    endif()
  endforeach()

  get_filename_component(_bioseq_repo_root "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)

  # ---- toolchain discovery (moved from bioseq/external/CMakeLists.txt:17-97) --
  set(BIOSEQ_CODEGEN_CXX_COMPILER "" CACHE FILEPATH
    "GCC 16.1 C++ compiler used by the isolated bioseq_codegen generator")
  if(NOT BIOSEQ_CODEGEN_CXX_COMPILER)
    if(EXISTS "/opt/gcc-16.1.0/bin/g++-16")
      set(BIOSEQ_CODEGEN_CXX_COMPILER "/opt/gcc-16.1.0/bin/g++-16")
    elseif(EXISTS "/usr/bin/g++-16")
      set(BIOSEQ_CODEGEN_CXX_COMPILER "/usr/bin/g++-16")
    endif()
  endif()

  set(BIOSEQ_CODEGEN_C_COMPILER "" CACHE FILEPATH
    "GCC 16.1 C compiler used by the isolated bioseq_codegen generator")
  if(NOT BIOSEQ_CODEGEN_C_COMPILER)
    if(EXISTS "/opt/gcc-16.1.0/bin/gcc-16")
      set(BIOSEQ_CODEGEN_C_COMPILER "/opt/gcc-16.1.0/bin/gcc-16")
    elseif(EXISTS "/usr/bin/gcc-16")
      set(BIOSEQ_CODEGEN_C_COMPILER "/usr/bin/gcc-16")
    endif()
  endif()

  set(BIOSEQ_CODEGEN_LLVM_CONFIG "/usr/bin/llvm-config-22" CACHE FILEPATH
    "llvm-config used by cppast/Clang Tooling to locate libclang")
  set(BIOSEQ_CODEGEN_CLANG_BINARY "/usr/bin/clang++-22" CACHE FILEPATH
    "Clang parser executable passed to external-client generation")

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

  execute_process(
    COMMAND "${BIOSEQ_CODEGEN_LLVM_CONFIG}" --version
    OUTPUT_VARIABLE _llvm_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
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
  find_library(_bioseq_codegen_libclang
    NAMES clang libclang
    PATHS "${_llvm_library_dir}"
    NO_DEFAULT_PATH
    NO_CACHE
    REQUIRED)

  set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON "" CACHE FILEPATH
    "libstdc++ module metadata used by the isolated GCC 16 generator")
  if(NOT BIOSEQ_CODEGEN_STDLIB_MODULES_JSON)
    foreach(_modules_json IN ITEMS
        "/opt/gcc-16.1.0/lib64/libstdc++.modules.json"
        "/opt/gcc-16.1.0/lib/gcc/x86_64-pc-linux-gnu/16.1.0/libstdc++.modules.json"
        "/opt/gcc-16.1.0/lib/gcc/x86_64-linux-gnu/16.1.0/libstdc++.modules.json"
        "/usr/lib/gcc/x86_64-linux-gnu/16/libstdc++.modules.json")
      if(EXISTS "${_modules_json}")
        set(BIOSEQ_CODEGEN_STDLIB_MODULES_JSON "${_modules_json}")
        break()
      endif()
    endforeach()
  endif()
  if(NOT EXISTS "${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}")
    message(FATAL_ERROR
      "GCC 16 libstdc++.modules.json was not found; set "
      "BIOSEQ_CODEGEN_STDLIB_MODULES_JSON explicitly")
  endif()

  set(_runtime_dir "${ARG_BINARY_DIR}/generator-bin")
  set(_executable "${_runtime_dir}/bioseq_codegen")

  ExternalProject_Add(${ARG_PROJECT_TARGET}
    PREFIX "${ARG_BINARY_DIR}/generator-project"
    SOURCE_DIR "${_bioseq_repo_root}/main/codegen"
    BINARY_DIR "${ARG_BINARY_DIR}/generator-build"
    EXCLUDE_FROM_ALL TRUE
    CMAKE_ARGS
      "-DCMAKE_BUILD_TYPE:STRING=Release"
      "-DCMAKE_C_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_CXX_COMPILER}"
      "-DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5"
      "-DCMAKE_CXX_STDLIB_MODULES_JSON:FILEPATH=${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}"
      "-DBIOSEQ_REPOSITORY_ROOT:PATH=${_bioseq_repo_root}"
      "-DBIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY:PATH=${_runtime_dir}"
      "-DBIOSEQ_CODEGEN_ENABLE_EXTERNAL:BOOL=${_enable_external}"
      "-DBIOSEQ_CODEGEN_ENABLE_WEBSITE:BOOL=${_enable_website}"
      "-DCLANG_BINARY:FILEPATH=${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "-DLLVM_CONFIG_BINARY:FILEPATH=${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "-DLLVM_VERSION_EXPLICIT:STRING=${_llvm_version}"
      "-DLIBCLANG_INCLUDE_DIR:PATH=${_llvm_include_dir}"
      "-DLIBCLANG_LIBRARY:FILEPATH=${_bioseq_codegen_libclang}"
    BUILD_COMMAND
      "${CMAKE_COMMAND}" --build <BINARY_DIR> --target bioseq_codegen
    INSTALL_COMMAND ""
    TEST_COMMAND ""
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${_executable}"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE)

  add_executable(${ARG_TARGET} IMPORTED GLOBAL)
  set_target_properties(${ARG_TARGET} PROPERTIES
    IMPORTED_LOCATION "${_executable}")
  add_dependencies(${ARG_TARGET} ${ARG_PROJECT_TARGET})

  set(BIOSEQ_CODEGEN_EXECUTABLE "${_executable}" CACHE INTERNAL
    "Path to the built bioseq_codegen host-tool executable")
endfunction()
