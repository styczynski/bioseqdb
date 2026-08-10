cmake_minimum_required(VERSION 4.2)

# Isolated host-tool generator project. Built by bioseq_codegen.cmake as an
# ExternalProject with either GCC 16.1/libstdc++ or Clang 22/libc++, keeping its
# C and legacy C++ dependencies out of the main module graph. Components are
# selected by BIOSEQ_CODEGEN_ENABLE_EXTERNAL / BIOSEQ_CODEGEN_ENABLE_WEBSITE /
# BIOSEQ_CODEGEN_ENABLE_SQL / BIOSEQ_CODEGEN_ENABLE_HIGHWAY /
# BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION.

if(NOT BIOSEQ_REPOSITORY_ROOT)
  get_filename_component(BIOSEQ_REPOSITORY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_bioseq_expected_gcc_identity_variables
  BIOSEQ_EXPECTED_GCC_ROOT
  BIOSEQ_EXPECTED_GCC_C
  BIOSEQ_EXPECTED_GCC_CXX
  BIOSEQ_EXPECTED_GCC_MODULES
  BIOSEQ_EXPECTED_GCC_VERSION
  BIOSEQ_EXPECTED_GCC_C_SHA256
  BIOSEQ_EXPECTED_GCC_CXX_SHA256
  BIOSEQ_EXPECTED_GCC_MODULES_SHA256
  BIOSEQ_EXPECTED_GCC_MODULE_SOURCE_CLOSURE_SHA256)
set(_bioseq_expected_gcc_identity_count 0)
foreach(_bioseq_expected_gcc_identity_variable IN LISTS
    _bioseq_expected_gcc_identity_variables)
  if(DEFINED ${_bioseq_expected_gcc_identity_variable}
      AND NOT "${${_bioseq_expected_gcc_identity_variable}}" STREQUAL "")
    math(EXPR _bioseq_expected_gcc_identity_count
      "${_bioseq_expected_gcc_identity_count} + 1")
  endif()
endforeach()
list(LENGTH _bioseq_expected_gcc_identity_variables
  _bioseq_expected_gcc_identity_required_count)
if(_bioseq_expected_gcc_identity_count GREATER 0
    AND NOT _bioseq_expected_gcc_identity_count EQUAL
      _bioseq_expected_gcc_identity_required_count)
  message(FATAL_ERROR
    "isolated generator received an incomplete authenticated GCC identity")
endif()
set(_bioseq_codegen_gcc_only OFF)
if(_bioseq_expected_gcc_identity_count EQUAL
    _bioseq_expected_gcc_identity_required_count)
  set(_bioseq_codegen_gcc_only ON)
  include(
    "${BIOSEQ_REPOSITORY_ROOT}/cmake/bioseq_gcc_toolchain.cmake")
  foreach(_bioseq_compiler_path_pair IN ITEMS
      "CMAKE_C_COMPILER;BIOSEQ_EXPECTED_GCC_C"
      "CMAKE_CXX_COMPILER;BIOSEQ_EXPECTED_GCC_CXX"
      "CMAKE_CXX_STDLIB_MODULES_JSON;BIOSEQ_EXPECTED_GCC_MODULES")
    list(GET _bioseq_compiler_path_pair 0 _bioseq_actual_path_variable)
    list(GET _bioseq_compiler_path_pair 1 _bioseq_expected_path_variable)
    file(REAL_PATH "${${_bioseq_actual_path_variable}}"
      _bioseq_actual_path)
    file(REAL_PATH "${${_bioseq_expected_path_variable}}"
      _bioseq_expected_path)
    if(NOT _bioseq_actual_path STREQUAL _bioseq_expected_path)
      message(FATAL_ERROR
        "isolated generator compiler/inventory path diverges from "
        "the authenticated root: ${_bioseq_actual_path_variable}")
    endif()
  endforeach()
  bioseq_verify_gcc16_toolchain_identity(
    ROOT "${BIOSEQ_EXPECTED_GCC_ROOT}"
    C "${BIOSEQ_EXPECTED_GCC_C}"
    CXX "${BIOSEQ_EXPECTED_GCC_CXX}"
    MODULES "${BIOSEQ_EXPECTED_GCC_MODULES}"
    VERSION "${BIOSEQ_EXPECTED_GCC_VERSION}"
    C_SHA256 "${BIOSEQ_EXPECTED_GCC_C_SHA256}"
    CXX_SHA256 "${BIOSEQ_EXPECTED_GCC_CXX_SHA256}"
    MODULES_SHA256 "${BIOSEQ_EXPECTED_GCC_MODULES_SHA256}"
    MODULE_SOURCE_CLOSURE_SHA256
      "${BIOSEQ_EXPECTED_GCC_MODULE_SOURCE_CLOSURE_SHA256}")
endif()

option(BIOSEQ_CODEGEN_USE_MODULE_STD
  "Build and expose the selected standard library's named std module" ON)

if(BIOSEQ_CODEGEN_USE_MODULE_STD
    AND NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
  else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  endif()
endif()

project(bioseq_codegen LANGUAGES C CXX)

option(BIOSEQ_CODEGEN_RUN_POST_BUILD_SELF_TESTS
  "Run generator contracts immediately after linking the host tool" ON)

if(_bioseq_codegen_gcc_only
    AND (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU"
      OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
      OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1"))
  message(FATAL_ERROR
    "authenticated isolated generator requires GNU C/C++ >= 16.1; found "
    "C=${CMAKE_C_COMPILER_ID}, "
    "CXX=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

if((NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
   AND
   (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"
       OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "22"))
  message(FATAL_ERROR
    "bioseq_codegen requires GCC 16.1 or Clang 22 (or newer); found "
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at "
    "${CMAKE_CXX_COMPILER}")
endif()

if(NOT BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY)
  set(BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT LLVM_CONFIG_BINARY)
  set(LLVM_CONFIG_BINARY "/usr/bin/llvm-config-22")
endif()

option(BIOSEQ_CODEGEN_ENABLE_EXTERNAL "Build the external-API-client component" OFF)
option(BIOSEQ_CODEGEN_ENABLE_WEBSITE "Build the website component" OFF)
option(BIOSEQ_CODEGEN_ENABLE_SQL "Build the PostgreSQL extension SQL component" OFF)
option(BIOSEQ_CODEGEN_ENABLE_HIGHWAY "Build the Highway target-module component" OFF)
option(BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION
  "Build the generic template-flatten namespace materializer" OFF)

if(NOT BIOSEQ_CODEGEN_ENABLE_EXTERNAL
   AND NOT BIOSEQ_CODEGEN_ENABLE_WEBSITE
   AND NOT BIOSEQ_CODEGEN_ENABLE_SQL
   AND NOT BIOSEQ_CODEGEN_ENABLE_HIGHWAY
   AND NOT BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION)
  message(FATAL_ERROR
    "bioseq_codegen requires at least one enabled component "
    "(BIOSEQ_CODEGEN_ENABLE_EXTERNAL / BIOSEQ_CODEGEN_ENABLE_WEBSITE / "
    "BIOSEQ_CODEGEN_ENABLE_SQL / BIOSEQ_CODEGEN_ENABLE_HIGHWAY / "
    "BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION)")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
include(FetchContent)

set(_codegen_src "${CMAKE_CURRENT_SOURCE_DIR}")
set_source_files_properties(
  "${_codegen_src}/main.cppm"
  PROPERTIES COMPILE_OPTIONS "-x;c++")

# Shared support footprint, always compiled.
set(_codegen_module_files
  "${_codegen_src}/support/diagnostics.cppm"
  "${_codegen_src}/support/source_text.cppm"
  "${_codegen_src}/support/artifact_io.cppm"
  "${_codegen_src}/support/index.cppm"
  "${_codegen_src}/cli.cppm"
  "${_codegen_src}/index.cppm"
  "${BIOSEQ_REPOSITORY_ROOT}/bioseq/backend/common/hashing/index.cppm")
set(_codegen_base_dirs
  "${_codegen_src}"
  "${BIOSEQ_REPOSITORY_ROOT}/bioseq"
  "${BIOSEQ_REPOSITORY_ROOT}/common"
  "${CMAKE_BINARY_DIR}/generated")
set(_codegen_implementation_files "")
set(_codegen_link_libs "")
set(_codegen_component_imports "")
set(_codegen_component_entries "")

if(BIOSEQ_CODEGEN_ENABLE_SQL
    OR BIOSEQ_CODEGEN_ENABLE_WEBSITE
    OR BIOSEQ_CODEGEN_ENABLE_HIGHWAY
    OR BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION)
  # One Clang 22 AST/token/USR implementation serves SQL, website, and Highway
  # collection. Clang/LLVM types remain confined to the named module
  # implementation unit; importers see only the compiler-neutral model.
  execute_process(COMMAND "${LLVM_CONFIG_BINARY}" --includedir
    OUTPUT_VARIABLE _bioseq_llvm_incdir OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${LLVM_CONFIG_BINARY}" --libdir
    OUTPUT_VARIABLE _bioseq_llvm_libdir OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${LLVM_CONFIG_BINARY}" --bindir
    OUTPUT_VARIABLE _bioseq_llvm_bindir OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${LLVM_CONFIG_BINARY}" --prefix
    OUTPUT_VARIABLE _bioseq_llvm_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${LLVM_CONFIG_BINARY}" --version
    OUTPUT_VARIABLE _bioseq_llvm_version OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  if(_bioseq_llvm_version VERSION_LESS "22")
    message(FATAL_ERROR
      "bioseq_codegen AST tooling requires LLVM/Clang >= 22; found "
      "${_bioseq_llvm_version} at ${LLVM_CONFIG_BINARY}")
  endif()
  find_library(BIOSEQ_CODEGEN_CLANG_CPP_LIB NAMES clang-cpp
    PATHS "${_bioseq_llvm_libdir}" NO_DEFAULT_PATH REQUIRED)
  find_library(BIOSEQ_CODEGEN_LLVM_LIB NAMES LLVM LLVM-22
    PATHS "${_bioseq_llvm_libdir}" NO_DEFAULT_PATH)
  if(NOT BIOSEQ_CODEGEN_LLVM_LIB)
    # LLVM's official Linux release archive links the LLVM components into
    # libclang-cpp itself and does not ship a separate monolithic libLLVM.
    set(BIOSEQ_CODEGEN_LLVM_LIB "${BIOSEQ_CODEGEN_CLANG_CPP_LIB}")
  endif()
  file(SHA256 "${LLVM_CONFIG_BINARY}" _bioseq_llvm_config_sha256)
  file(SHA256 "${BIOSEQ_CODEGEN_CLANG_CPP_LIB}"
    _bioseq_clang_cpp_sha256)
  file(SHA256 "${BIOSEQ_CODEGEN_LLVM_LIB}"
    _bioseq_llvm_library_sha256)
  list(APPEND _codegen_module_files
    "${_codegen_src}/structural_analysis/model.cppm"
    "${_codegen_src}/structural_analysis/index.cppm")
  list(APPEND _codegen_implementation_files
    "${_codegen_src}/structural_analysis/implementation.cppm"
    "$<$<BOOL:${BIOSEQ_CODEGEN_ENABLE_SQL}>:${_codegen_src}/safety/fact_producer_implementation.cppm>")
  # Keep the authored production unit on the required .cppm path while
  # compiling it as the primary module's implementation unit. Clang otherwise
  # infers an interface solely from the extension and rejects `module M;`.
  set_source_files_properties(
    "${_codegen_src}/structural_analysis/implementation.cppm"
    "${_codegen_src}/safety/fact_producer_implementation.cppm"
    PROPERTIES COMPILE_OPTIONS "-x;c++")
  list(APPEND _codegen_link_libs "${BIOSEQ_CODEGEN_CLANG_CPP_LIB}")
  if(NOT "${BIOSEQ_CODEGEN_LLVM_LIB}" STREQUAL
      "${BIOSEQ_CODEGEN_CLANG_CPP_LIB}")
    list(APPEND _codegen_link_libs "${BIOSEQ_CODEGEN_LLVM_LIB}")
  endif()
endif()

if(BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION
    OR BIOSEQ_CODEGEN_ENABLE_HIGHWAY)
  list(APPEND _codegen_module_files
    "${_codegen_src}/namespace_materialization/model.cppm"
    "${_codegen_src}/namespace_materialization/validator.cppm"
    "${_codegen_src}/namespace_materialization/manifest.cppm"
    "${_codegen_src}/namespace_materialization/renderer.cppm"
    "${_codegen_src}/namespace_materialization/tests/contract/index.cppm"
    "${_codegen_src}/namespace_materialization/index.cppm")
  string(APPEND _codegen_component_imports
    "import bioseq.codegen.namespace_materialization;\n")
  string(APPEND _codegen_component_entries
    "        {\"namespace_materialization\", "
    "&bioseq::codegen::namespace_materialization::self_test, "
    "&bioseq::codegen::namespace_materialization::run},\n")
endif()

if(BIOSEQ_CODEGEN_ENABLE_HIGHWAY)
  list(APPEND _codegen_module_files
    "${_codegen_src}/highway/model.cppm"
    "${_codegen_src}/highway/manifest.cppm"
    "${_codegen_src}/highway/validator.cppm"
    "${_codegen_src}/highway/renderer.cppm"
    "${_codegen_src}/highway/tests/contract/index.cppm"
    "${_codegen_src}/highway/index.cppm")
  string(APPEND _codegen_component_imports
    "import bioseq.codegen.highway;\n")
  string(APPEND _codegen_component_entries
    "        {\"highway\", &bioseq::codegen::highway::self_test, "
    "&bioseq::codegen::highway::run},\n")
endif()

if(BIOSEQ_CODEGEN_ENABLE_SQL)
  if(NOT CLANG_BINARY OR NOT EXISTS "${CLANG_BINARY}")
    find_program(CLANG_BINARY
      NAMES clang++ clang++-22
      PATHS "${_bioseq_llvm_bindir}"
      NO_DEFAULT_PATH
      REQUIRED)
  endif()
  execute_process(
    COMMAND "${CLANG_BINARY}" --version
    OUTPUT_VARIABLE _bioseq_clang_version_output
    ERROR_VARIABLE _bioseq_clang_version_error
    RESULT_VARIABLE _bioseq_clang_version_status)
  string(REGEX MATCH
    "[Cc]lang version ([0-9]+(\\.[0-9]+)*)"
    _bioseq_clang_version_match
    "${_bioseq_clang_version_output}\n${_bioseq_clang_version_error}")
  set(_bioseq_clang_version "${CMAKE_MATCH_1}")
  if(NOT _bioseq_clang_version_status EQUAL 0
      OR NOT _bioseq_clang_version_match
      OR _bioseq_clang_version VERSION_LESS "22")
    message(FATAL_ERROR
      "bioseq_codegen AST parser requires Clang >= 22; found "
      "${_bioseq_clang_version_output}${_bioseq_clang_version_error}")
  endif()
  set(_bioseq_prepare_test_clang_scan_deps
    "${_bioseq_llvm_bindir}/clang-scan-deps")
  if(NOT EXISTS "${_bioseq_prepare_test_clang_scan_deps}")
    message(FATAL_ERROR
      "PREPARE contract tests require clang-scan-deps beside the linked Clang")
  endif()
  set(_bioseq_prepare_test_compiler_inventory "")
  file(GLOB _bioseq_prepare_test_multiarch_inventories
    "${_bioseq_llvm_prefix}/lib/*/c++/libc++.modules.json"
    "${_bioseq_llvm_prefix}/lib/*/libc++.modules.json")
  list(SORT _bioseq_prepare_test_multiarch_inventories)
  foreach(_bioseq_prepare_test_inventory_candidate IN ITEMS
      "${CMAKE_CXX_STDLIB_MODULES_JSON}"
      "${_bioseq_llvm_libdir}/c++/libc++.modules.json"
      "${_bioseq_llvm_prefix}/lib/c++/libc++.modules.json"
      ${_bioseq_prepare_test_multiarch_inventories}
      "${_bioseq_llvm_prefix}/lib/libc++.modules.json")
    if(EXISTS "${_bioseq_prepare_test_inventory_candidate}")
      set(_bioseq_prepare_test_compiler_inventory
        "${_bioseq_prepare_test_inventory_candidate}")
      break()
    endif()
  endforeach()
  if(NOT _bioseq_prepare_test_compiler_inventory)
    message(FATAL_ERROR
      "PREPARE contract tests require libc++.modules.json under the linked LLVM prefix")
  endif()
  file(SHA256 "${CLANG_BINARY}" _bioseq_prepare_test_clang_sha256)
  file(SHA256 "${_bioseq_prepare_test_clang_scan_deps}"
    _bioseq_prepare_test_clang_scan_deps_sha256)
  file(SHA256 "${_bioseq_prepare_test_compiler_inventory}"
    _bioseq_prepare_test_compiler_inventory_sha256)

  list(APPEND _codegen_module_files
    "${_codegen_src}/structural_analysis/tests/contract/index.cppm"
    "${_codegen_src}/safety/model.cppm"
    "${_codegen_src}/safety/json.cppm"
    "${_codegen_src}/safety/analyzer_model.cppm"
    "${_codegen_src}/safety/manifest.cppm"
    "${_codegen_src}/safety/validation.cppm"
    "${_codegen_src}/safety/analyzer.cppm"
    "${_codegen_src}/safety/serialization.cppm"
    "${_codegen_src}/safety/runtime_inventory.cppm"
    "${_codegen_src}/safety/tests/contract/runtime_inventory_test.cppm"
    "${_codegen_src}/safety/fact_producer.cppm"
    "${_codegen_src}/safety/tests/contract/fact_producer_test.cppm"
    "${_codegen_src}/safety/runtime_matrix.cppm"
    "${_codegen_src}/safety/dso.cppm"
    "${_codegen_src}/safety/tests/contract/analyzer_test.cppm"
    "${_codegen_src}/safety/tests/contract/index.cppm"
    "${_codegen_src}/safety/cli.cppm"
    "${_codegen_src}/safety/index.cppm"
    "${_codegen_src}/sql/annotation_model/entities.cppm"
    "${_codegen_src}/sql/annotation_model/options.cppm"
    "${_codegen_src}/sql/annotation_model/validation.cppm"
    "${_codegen_src}/sql/annotation_model/index.cppm"
    "${_codegen_src}/sql/signature.cppm"
    "${_codegen_src}/sql/collector/model.cppm"
    "${_codegen_src}/sql/collector/options.cppm"
    "${_codegen_src}/sql/collector/gucs.cppm"
    "${_codegen_src}/sql/collector/index.cppm"
    "${_codegen_src}/sql/graph/model.cppm"
    "${_codegen_src}/sql/graph/index.cppm"
    "${_codegen_src}/sql/renderer/model.cppm"
    "${_codegen_src}/sql/renderer/sql.cppm"
    "${_codegen_src}/sql/renderer/cpp_facets.cppm"
    "${_codegen_src}/sql/renderer/gucs.cppm"
    "${_codegen_src}/sql/renderer/fmgr.cppm"
    "${_codegen_src}/sql/renderer/tests/contract/fmgr_test.cppm"
    "${_codegen_src}/sql/renderer/tests/contract/compact_fmgr_test.cppm"
    "${_codegen_src}/sql/renderer/manifest.cppm"
    "${_codegen_src}/sql/renderer/index.cppm"
    "${_codegen_src}/sql/tests/contract/fixtures/annotation_probe.cppm"
    "${_codegen_src}/sql/model.cppm"
    "${_codegen_src}/sql/policy.cppm"
    "${_codegen_src}/sql/tests/contract/guc_test.cppm"
    "${_codegen_src}/sql/tests/contract/index.cppm"
    "${_codegen_src}/sql/index.cppm")
  string(APPEND _codegen_component_imports
    "import bioseq.codegen.safety;\n")
  string(APPEND _codegen_component_entries
    "        {\"safety\", &bioseq::codegen::safety::self_test, "
    "&bioseq::codegen::safety::run},\n")
  string(APPEND _codegen_component_imports "import bioseq.codegen.sql;\n")
  string(APPEND _codegen_component_entries
    "        {\"sql\", &bioseq::codegen::sql::self_test, "
    "&bioseq::codegen::sql::run},\n")
endif()

# yaml-cpp 0.8.0 is the sole frontmatter/config parser, shared by whichever of
# the external or website components is enabled.
if(BIOSEQ_CODEGEN_ENABLE_EXTERNAL OR BIOSEQ_CODEGEN_ENABLE_WEBSITE)
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
  if(NOT TARGET yaml-cpp::yaml-cpp)
    FetchContent_Declare(yaml-cpp
      GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
      GIT_TAG 0.8.0
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(yaml-cpp)
  endif()
  list(APPEND _codegen_link_libs yaml-cpp::yaml-cpp)
endif()

if(BIOSEQ_CODEGEN_ENABLE_EXTERNAL)
  set(LLVM_CONFIG_BINARY "${LLVM_CONFIG_BINARY}" CACHE FILEPATH "" FORCE)
  find_package(cppast CONFIG REQUIRED)

  list(APPEND _codegen_module_files
    "${_codegen_src}/external/model.cppm"
    "${_codegen_src}/external/annotation_parser.cppm"
    "${_codegen_src}/external/openapi_parser.cppm"
    "${_codegen_src}/external/client_renderer.cppm"
    "${_codegen_src}/external/artifact_handler.cppm"
    "${_codegen_src}/external/incremental_state.cppm"
    "${_codegen_src}/external/builder.cppm"
    "${_codegen_src}/external/tests/contract/index.cppm"
    "${_codegen_src}/external/index.cppm"
    "${BIOSEQ_REPOSITORY_ROOT}/bioseq/backend/common/http/validation/index.cppm")
  list(APPEND _codegen_link_libs cppast::cppast)
  string(APPEND _codegen_component_imports "import bioseq.codegen.external;\n")
  string(APPEND _codegen_component_entries
    "        {\"external\", &bioseq::codegen::external::run_self_tests, "
    "&bioseq::codegen::external::run_command},\n")
endif()

if(BIOSEQ_CODEGEN_ENABLE_WEBSITE)
  # Website structural and rendering dependencies enter only through package
  # targets; their headers remain contained in the owning module units.
  set(_saved_scan "${CMAKE_CXX_SCAN_FOR_MODULES}")
  set(_saved_module_std "${CMAKE_CXX_MODULE_STD}")
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
  set(CMAKE_CXX_MODULE_STD OFF)

  find_package(md4c CONFIG REQUIRED)

  # pugixml v1.15 pinned by hash through the normal FetchContent cache.
  if(NOT TARGET pugixml::pugixml)
    FetchContent_Declare(pugixml
      URL "https://github.com/zeux/pugixml/releases/download/v1.15/pugixml-1.15.tar.gz"
      URL_HASH
      SHA256=655ade57fa703fb421c2eb9a0113b5064bddb145d415dd1f88c79353d90d511a)
    FetchContent_MakeAvailable(pugixml)
  endif()

  # The pinned overlay package contains Jinja2Cpp's C++17 implementation and
  # publishes only its namespaced package target to current-standard consumers.
  find_package(jinja2cpp CONFIG REQUIRED)

  set(CMAKE_CXX_SCAN_FOR_MODULES "${_saved_scan}")
  set(CMAKE_CXX_MODULE_STD "${_saved_module_std}")

  # libpq is discovered via pkg-config so the isolated GCC generator does not
  # depend on the extension's vcpkg libpq. Only bioseq.libpq.client includes it.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)

  list(APPEND _codegen_module_files
    "${BIOSEQ_REPOSITORY_ROOT}/common/libpq/index.cppm"
    "${_codegen_src}/website/model/index.cppm"
    "${_codegen_src}/website/front_matter.cppm"
    "${_codegen_src}/website/markdown_directives.cppm"
    "${_codegen_src}/website/markdown.cppm"
    "${_codegen_src}/website/doxygen_reference.cppm"
    "${_codegen_src}/website/cpp_reference_index.cppm"
    "${_codegen_src}/website/reference_index.cppm"
    "${_codegen_src}/website/html_renderer.cppm"
    "${_codegen_src}/website/serialization.cppm"
    "${_codegen_src}/website/search_index.cppm"
    "${_codegen_src}/website/template_renderer.cppm"
    "${_codegen_src}/website/release_assembly.cppm"
    "${_codegen_src}/website/postgres/executor.cppm"
    "${_codegen_src}/website/postgres/catalog.cppm"
    "${_codegen_src}/website/postgres/index.cppm"
    "${_codegen_src}/website/build_pipeline.cppm"
    "${_codegen_src}/website/tests/contract/index.cppm"
    "${_codegen_src}/website/index.cppm")
  list(APPEND _codegen_link_libs
    md4c::md4c pugixml::pugixml jinja2cpp::jinja2cpp Boost::json
    PkgConfig::LIBPQ)
  string(APPEND _codegen_component_imports "import bioseq.codegen.website;\n")
  string(APPEND _codegen_component_entries
    "        {\"website\", &bioseq::codegen::website::self_test, "
    "&bioseq::codegen::website::run},\n")
endif()

# Generate the component registry the CLI dispatches through, listing exactly
# the components compiled into this binary. This keeps cli.cppm free of any
# per-component preprocessor gating.
set(BIOSEQ_CODEGEN_COMPONENT_IMPORTS "${_codegen_component_imports}")
set(BIOSEQ_CODEGEN_COMPONENT_ENTRIES "${_codegen_component_entries}")
configure_file(
  "${_codegen_src}/component_registry.cppm.in"
  "${CMAKE_BINARY_DIR}/generated/component_registry.cppm"
  @ONLY)
list(APPEND _codegen_module_files
  "${CMAKE_BINARY_DIR}/generated/component_registry.cppm")

add_executable(bioseq_codegen
  "${_codegen_src}/main.cppm"
  ${_codegen_implementation_files}
  "$<$<BOOL:${BIOSEQ_CODEGEN_ENABLE_SQL}>:${_codegen_src}/sql/tests/contract/fixtures/annotation_probe.cpp>")
target_sources(bioseq_codegen
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS ${_codegen_base_dirs}
    FILES ${_codegen_module_files})
if(_codegen_link_libs)
  target_link_libraries(bioseq_codegen PRIVATE ${_codegen_link_libs})
endif()
if(BIOSEQ_CODEGEN_ENABLE_SQL
    OR BIOSEQ_CODEGEN_ENABLE_WEBSITE
    OR BIOSEQ_CODEGEN_ENABLE_HIGHWAY
    OR BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION)
  target_include_directories(bioseq_codegen SYSTEM PRIVATE
    "${_bioseq_llvm_incdir}")
  target_link_options(bioseq_codegen PRIVATE
    "-Wl,-rpath,${_bioseq_llvm_libdir}")
  target_compile_definitions(bioseq_codegen PRIVATE
    "BIOSEQ_LINKED_LLVM_PREFIX=\"${_bioseq_llvm_prefix}\""
    "BIOSEQ_LINKED_LLVM_VERSION=\"${_bioseq_llvm_version}\""
    "BIOSEQ_LINKED_LLVM_CONFIG=\"${LLVM_CONFIG_BINARY}\""
    "BIOSEQ_LINKED_LLVM_CONFIG_SHA256=\"${_bioseq_llvm_config_sha256}\""
    "BIOSEQ_LINKED_CLANG_CPP=\"${BIOSEQ_CODEGEN_CLANG_CPP_LIB}\""
    "BIOSEQ_LINKED_CLANG_CPP_SHA256=\"${_bioseq_clang_cpp_sha256}\""
    "BIOSEQ_LINKED_LLVM_LIBRARY=\"${BIOSEQ_CODEGEN_LLVM_LIB}\""
    "BIOSEQ_LINKED_LLVM_LIBRARY_SHA256=\"${_bioseq_llvm_library_sha256}\"")
endif()

if(BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION
    OR BIOSEQ_CODEGEN_ENABLE_HIGHWAY)
  target_compile_definitions(bioseq_codegen PRIVATE
    "BIOSEQ_NAMESPACE_MATERIALIZATION_TEST_REPOSITORY_ROOT=\"${BIOSEQ_REPOSITORY_ROOT}\"")
endif()

if((BIOSEQ_CODEGEN_ENABLE_NAMESPACE_MATERIALIZATION
      OR BIOSEQ_CODEGEN_ENABLE_HIGHWAY)
    AND BIOSEQ_CODEGEN_RUN_POST_BUILD_SELF_TESTS)
  add_custom_command(TARGET bioseq_codegen POST_BUILD
    COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test namespace_materialization
    COMMENT "Running namespace-materialization code-generator contracts"
    VERBATIM)
endif()
if(BIOSEQ_CODEGEN_ENABLE_HIGHWAY
    AND BIOSEQ_CODEGEN_RUN_POST_BUILD_SELF_TESTS)
  add_custom_command(TARGET bioseq_codegen POST_BUILD
    COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test highway
    COMMENT "Running Highway target-module code-generator contracts"
    VERBATIM)
endif()
if(BIOSEQ_CODEGEN_ENABLE_SQL)
  target_compile_definitions(bioseq_codegen PRIVATE
    "BIOSEQ_PREPARE_TEST_CLANG_BINARY=\"${CLANG_BINARY}\""
    "BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS=\"${_bioseq_prepare_test_clang_scan_deps}\""
    "BIOSEQ_PREPARE_TEST_LLVM_CONFIG=\"${LLVM_CONFIG_BINARY}\""
    "BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY=\"${_bioseq_prepare_test_compiler_inventory}\""
    "BIOSEQ_PREPARE_TEST_CLANG_SHA256=\"${_bioseq_prepare_test_clang_sha256}\""
    "BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS_SHA256=\"${_bioseq_prepare_test_clang_scan_deps_sha256}\""
    "BIOSEQ_PREPARE_TEST_LLVM_CONFIG_SHA256=\"${_bioseq_llvm_config_sha256}\""
    "BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY_SHA256=\"${_bioseq_prepare_test_compiler_inventory_sha256}\""
    "BIOSEQ_PREPARE_TEST_REPOSITORY_ROOT=\"${BIOSEQ_REPOSITORY_ROOT}\"")
endif()
set_target_properties(bioseq_codegen PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD "${BIOSEQ_CODEGEN_USE_MODULE_STD}"
  RUNTIME_OUTPUT_DIRECTORY "${BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY}")

if(BIOSEQ_CODEGEN_ENABLE_SQL)
  set_source_files_properties(
    "${_codegen_src}/sql/tests/contract/fixtures/annotation_probe.cppm"
    "${_codegen_src}/sql/tests/contract/fixtures/annotation_probe.cpp"
    PROPERTIES COMPILE_OPTIONS
      "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>;$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
  if(BIOSEQ_CODEGEN_RUN_POST_BUILD_SELF_TESTS)
    add_custom_command(TARGET bioseq_codegen POST_BUILD
      COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test sql
      COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test safety
      COMMENT "Running SQL and PostgreSQL safety code-generator contracts"
      VERBATIM)
  endif()
endif()
