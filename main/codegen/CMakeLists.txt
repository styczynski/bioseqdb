cmake_minimum_required(VERSION 4.2)

# Isolated host-tool generator project. Built by BioseqCodegen.cmake as an
# ExternalProject with the GCC 16.1 / libstdc++ toolchain so its C and legacy
# C++ dependencies never touch the main Clang module graph. Components are
# selected by BIOSEQ_CODEGEN_ENABLE_EXTERNAL / BIOSEQ_CODEGEN_ENABLE_WEBSITE.

if(NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
  else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  endif()
endif()

project(bioseq_codegen LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
  message(FATAL_ERROR
    "bioseq_codegen requires GCC 16.1 or newer; found "
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at "
    "${CMAKE_CXX_COMPILER}")
endif()

if(NOT BIOSEQ_REPOSITORY_ROOT)
  get_filename_component(BIOSEQ_REPOSITORY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
if(NOT BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY)
  set(BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT LLVM_CONFIG_BINARY)
  set(LLVM_CONFIG_BINARY "/usr/bin/llvm-config-22")
endif()

option(BIOSEQ_CODEGEN_ENABLE_EXTERNAL "Build the external-API-client component" OFF)
option(BIOSEQ_CODEGEN_ENABLE_WEBSITE "Build the website component" OFF)

if(NOT BIOSEQ_CODEGEN_ENABLE_EXTERNAL AND NOT BIOSEQ_CODEGEN_ENABLE_WEBSITE)
  message(FATAL_ERROR
    "bioseq_codegen requires at least one enabled component "
    "(BIOSEQ_CODEGEN_ENABLE_EXTERNAL / BIOSEQ_CODEGEN_ENABLE_WEBSITE)")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
include(FetchContent)

set(_codegen_src "${CMAKE_CURRENT_SOURCE_DIR}")

# Shared support footprint, always compiled.
set(_codegen_module_files
  "${_codegen_src}/support/diagnostics.cppm"
  "${_codegen_src}/support/text.cppm"
  "${_codegen_src}/support/artifact_store.cppm"
  "${_codegen_src}/support/index.cppm"
  "${_codegen_src}/cli.cppm"
  "${_codegen_src}/index.cppm"
  "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq/backend/common/hashing.cppm")
set(_codegen_base_dirs
  "${_codegen_src}"
  "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq"
  "${BIOSEQ_REPOSITORY_ROOT}/main/common"
  "${CMAKE_BINARY_DIR}/generated")
set(_codegen_link_libs "")
set(_codegen_component_imports "")
set(_codegen_component_entries "")

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
  set(CPPAST_BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(CPPAST_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(CPPAST_BUILD_TOOL OFF CACHE BOOL "" FORCE)
  set(LLVM_CONFIG_BINARY "${LLVM_CONFIG_BINARY}" CACHE FILEPATH "" FORCE)
  add_subdirectory("${BIOSEQ_REPOSITORY_ROOT}/cppast-main"
    "${CMAKE_BINARY_DIR}/cppast" EXCLUDE_FROM_ALL)
  set_target_properties(cppast PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD OFF)

  list(APPEND _codegen_module_files
    "${_codegen_src}/external/model.cppm"
    "${_codegen_src}/external/annotation_parser.cppm"
    "${_codegen_src}/external/openapi_parser.cppm"
    "${_codegen_src}/external/client_renderer.cppm"
    "${_codegen_src}/external/artifact_handler.cppm"
    "${_codegen_src}/external/incremental_state.cppm"
    "${_codegen_src}/external/builder.cppm"
    "${_codegen_src}/external/contract_tests.cppm"
    "${_codegen_src}/external/index.cppm"
    "${BIOSEQ_REPOSITORY_ROOT}/main/bioseq/external/http_validation.cppm")
  list(APPEND _codegen_link_libs cppast)
  string(APPEND _codegen_component_imports "import bioseq.codegen.external;\n")
  string(APPEND _codegen_component_entries
    "        {\"external\", &bioseq::codegen::external::self_test, "
    "&bioseq::codegen::external::run},\n")
endif()

if(BIOSEQ_CODEGEN_ENABLE_WEBSITE)
  # Website structural pass and rendering pull C and legacy-C++ dependencies.
  # Compile them without module scanning or import std so they never enter the
  # generator's C++26 module graph; contain their headers in the owning units.
  # They keep the C++26 standard (Boost.Filesystem 1.90 needs std::atomic_ref).
  set(_saved_scan "${CMAKE_CXX_SCAN_FOR_MODULES}")
  set(_saved_module_std "${CMAKE_CXX_MODULE_STD}")
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
  set(CMAKE_CXX_MODULE_STD OFF)

  # MD4C 0.5.3: link md4c (typed callbacks), never md4c-html; no md2html/shared.
  set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${BIOSEQ_REPOSITORY_ROOT}/md4c-master"
    "${CMAKE_BINARY_DIR}/md4c" EXCLUDE_FROM_ALL)

  # pugixml v1.15 pinned by hash through the normal FetchContent cache.
  if(NOT TARGET pugixml::pugixml)
    FetchContent_Declare(pugixml
      URL "https://github.com/zeux/pugixml/releases/download/v1.15/pugixml-1.15.tar.gz"
      URL_HASH
      SHA256=655ade57fa703fb421c2eb9a0113b5064bddb145d415dd1f88c79353d90d511a)
    FetchContent_MakeAvailable(pugixml)
  endif()

  # Jinja2Cpp 1.3.2 with its default Boost.JSON binding; internal deps mode
  # fetches Boost 1.90 (json/regex) and exposes Boost::json for website/json.
  set(JINJA2CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(JINJA2CPP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(JINJA2CPP_DEPS_MODE "internal" CACHE STRING "" FORCE)
  set(JINJA2CPP_WITH_JSON_BINDINGS "boost" CACHE STRING "" FORCE)
  set(JINJA2CPP_USE_REGEX "std" CACHE STRING "" FORCE)
  add_subdirectory("${BIOSEQ_REPOSITORY_ROOT}/Jinja2Cpp-master"
    "${CMAKE_BINARY_DIR}/jinja2cpp" EXCLUDE_FROM_ALL)
  # Jinja2Cpp 1.3.2 is not C++23/26-clean (its nonstd::expected aliases
  # std::expected and its polymorphic value trips a libstdc++ <optional>
  # constraint); build it and its renderer shim at their native C++17.
  set_target_properties(jinja2cpp PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_SCAN_FOR_MODULES OFF)
  add_library(bioseq_codegen_jinja_shim STATIC
    "${_codegen_src}/website/template_renderer_impl.cpp")
  set_target_properties(bioseq_codegen_jinja_shim PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
    CXX_SCAN_FOR_MODULES OFF CXX_MODULE_STD OFF POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(bioseq_codegen_jinja_shim PRIVATE jinja2cpp)

  set(CMAKE_CXX_SCAN_FOR_MODULES "${_saved_scan}")
  set(CMAKE_CXX_MODULE_STD "${_saved_module_std}")

  # libpq is discovered via pkg-config so the isolated GCC generator does not
  # depend on the extension's vcpkg libpq. Only bioseq.libpq.client includes it.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)

  list(APPEND _codegen_module_files
    "${BIOSEQ_REPOSITORY_ROOT}/main/common/libpq/index.cppm"
    "${_codegen_src}/website/model/index.cppm"
    "${_codegen_src}/website/front_matter.cppm"
    "${_codegen_src}/website/markdown_extensions.cppm"
    "${_codegen_src}/website/markdown.cppm"
    "${_codegen_src}/website/doxygen_xml.cppm"
    "${_codegen_src}/website/clang_entity_index.cppm"
    "${_codegen_src}/website/entity_index.cppm"
    "${_codegen_src}/website/json.cppm"
    "${_codegen_src}/website/search.cppm"
    "${_codegen_src}/website/template_renderer.cppm"
    "${_codegen_src}/website/version_assembler.cppm"
    "${_codegen_src}/website/postgres/executor.cppm"
    "${_codegen_src}/website/postgres/catalog.cppm"
    "${_codegen_src}/website/postgres/index.cppm"
    "${_codegen_src}/website/site_builder.cppm"
    "${_codegen_src}/website/contract_tests.cppm"
    "${_codegen_src}/website/index.cppm")
  list(APPEND _codegen_link_libs
    md4c pugixml::pugixml bioseq_codegen_jinja_shim Boost::json PkgConfig::LIBPQ)
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

add_executable(bioseq_codegen "${_codegen_src}/main.cpp")
target_sources(bioseq_codegen
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS ${_codegen_base_dirs}
    FILES ${_codegen_module_files})
if(_codegen_link_libs)
  target_link_libraries(bioseq_codegen PRIVATE ${_codegen_link_libs})
endif()
set_target_properties(bioseq_codegen PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  RUNTIME_OUTPUT_DIRECTORY "${BIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY}")
