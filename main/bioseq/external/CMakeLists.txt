include(ExternalProject)

get_filename_component(BIOSEQ_REPOSITORY_ROOT
  "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(_bioseq_external_source_root
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED BIOSEQ_EXTERNAL_GENERATED_ROOT)
  set(BIOSEQ_EXTERNAL_GENERATED_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/../generated/external")
endif()
if(NOT DEFINED BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY)
  set(BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/..")
endif()

set(BIOSEQ_EXTERNAL_CODEGEN_CXX_COMPILER "" CACHE FILEPATH
  "GCC 16 C++ compiler used by the isolated external-client generator")
if(NOT BIOSEQ_EXTERNAL_CODEGEN_CXX_COMPILER)
  if(EXISTS "/opt/gcc-16.1.0/bin/g++-16")
    set(BIOSEQ_EXTERNAL_CODEGEN_CXX_COMPILER
      "/opt/gcc-16.1.0/bin/g++-16")
  elseif(EXISTS "/usr/bin/g++-16")
    set(BIOSEQ_EXTERNAL_CODEGEN_CXX_COMPILER "/usr/bin/g++-16")
  endif()
endif()

set(BIOSEQ_EXTERNAL_CODEGEN_C_COMPILER "" CACHE FILEPATH
  "GCC 16 C compiler used by the isolated external-client generator")
if(NOT BIOSEQ_EXTERNAL_CODEGEN_C_COMPILER)
  if(EXISTS "/opt/gcc-16.1.0/bin/gcc-16")
    set(BIOSEQ_EXTERNAL_CODEGEN_C_COMPILER
      "/opt/gcc-16.1.0/bin/gcc-16")
  elseif(EXISTS "/usr/bin/gcc-16")
    set(BIOSEQ_EXTERNAL_CODEGEN_C_COMPILER "/usr/bin/gcc-16")
  endif()
endif()

set(BIOSEQ_EXTERNAL_LLVM_CONFIG "/usr/bin/llvm-config-22" CACHE FILEPATH
  "llvm-config used by cppast to locate libclang")
set(BIOSEQ_EXTERNAL_CLANG_BINARY "/usr/bin/clang++-22" CACHE FILEPATH
  "Clang parser executable passed to external-client generation")

foreach(_bioseq_required_tool IN ITEMS
    BIOSEQ_EXTERNAL_CODEGEN_CXX_COMPILER
    BIOSEQ_EXTERNAL_CODEGEN_C_COMPILER
    BIOSEQ_EXTERNAL_LLVM_CONFIG
    BIOSEQ_EXTERNAL_CLANG_BINARY)
  if(NOT EXISTS "${${_bioseq_required_tool}}")
    message(FATAL_ERROR
      "${_bioseq_required_tool} does not name an installed executable: "
      "'${${_bioseq_required_tool}}'")
  endif()
endforeach()

execute_process(
  COMMAND "${BIOSEQ_EXTERNAL_LLVM_CONFIG}" --version
  OUTPUT_VARIABLE _bioseq_external_llvm_version
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${BIOSEQ_EXTERNAL_LLVM_CONFIG}" --includedir
  OUTPUT_VARIABLE _bioseq_external_llvm_include_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${BIOSEQ_EXTERNAL_LLVM_CONFIG}" --libdir
  OUTPUT_VARIABLE _bioseq_external_llvm_library_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
find_library(_bioseq_external_libclang
  NAMES clang libclang
  PATHS "${_bioseq_external_llvm_library_dir}"
  NO_DEFAULT_PATH
  NO_CACHE
  REQUIRED)

set(BIOSEQ_EXTERNAL_CODEGEN_STDLIB_MODULES_JSON "" CACHE FILEPATH
  "libstdc++ module metadata used by the isolated GCC 16 generator")
if(NOT BIOSEQ_EXTERNAL_CODEGEN_STDLIB_MODULES_JSON)
  foreach(_bioseq_modules_json IN ITEMS
      "/opt/gcc-16.1.0/lib64/libstdc++.modules.json"
      "/opt/gcc-16.1.0/lib/gcc/x86_64-pc-linux-gnu/16.1.0/libstdc++.modules.json"
      "/opt/gcc-16.1.0/lib/gcc/x86_64-linux-gnu/16.1.0/libstdc++.modules.json"
      "/usr/lib/gcc/x86_64-linux-gnu/16/libstdc++.modules.json")
    if(EXISTS "${_bioseq_modules_json}")
      set(BIOSEQ_EXTERNAL_CODEGEN_STDLIB_MODULES_JSON
        "${_bioseq_modules_json}")
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${BIOSEQ_EXTERNAL_CODEGEN_STDLIB_MODULES_JSON}")
  message(FATAL_ERROR
    "GCC 16 libstdc++.modules.json was not found; set "
    "BIOSEQ_EXTERNAL_CODEGEN_STDLIB_MODULES_JSON explicitly")
endif()

set(_bioseq_codegen_binary_dir
  "${CMAKE_CURRENT_BINARY_DIR}/generator-build")
set(_bioseq_codegen_runtime_dir
  "${CMAKE_CURRENT_BINARY_DIR}/generator-bin")
set(_bioseq_codegen_executable
  "${_bioseq_codegen_runtime_dir}/bioseq_external_codegen")

ExternalProject_Add(bioseq_external_codegen_project
  PREFIX "${CMAKE_CURRENT_BINARY_DIR}/generator-project"
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/codegen"
  BINARY_DIR "${_bioseq_codegen_binary_dir}"
  EXCLUDE_FROM_ALL TRUE
  CMAKE_ARGS
    "-DCMAKE_BUILD_TYPE:STRING=Release"
    "-DCMAKE_C_COMPILER:FILEPATH=${BIOSEQ_EXTERNAL_CODEGEN_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_EXTERNAL_CODEGEN_CXX_COMPILER}"
    "-DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5"
    "-DCMAKE_CXX_STDLIB_MODULES_JSON:FILEPATH=${BIOSEQ_EXTERNAL_CODEGEN_STDLIB_MODULES_JSON}"
    "-DBIOSEQ_REPOSITORY_ROOT:PATH=${BIOSEQ_REPOSITORY_ROOT}"
    "-DBIOSEQ_CODEGEN_RUNTIME_OUTPUT_DIRECTORY:PATH=${_bioseq_codegen_runtime_dir}"
    "-DCLANG_BINARY:FILEPATH=${BIOSEQ_EXTERNAL_CLANG_BINARY}"
    "-DLLVM_CONFIG_BINARY:FILEPATH=${BIOSEQ_EXTERNAL_LLVM_CONFIG}"
    "-DLLVM_VERSION_EXPLICIT:STRING=${_bioseq_external_llvm_version}"
    "-DLIBCLANG_INCLUDE_DIR:PATH=${_bioseq_external_llvm_include_dir}"
    "-DLIBCLANG_LIBRARY:FILEPATH=${_bioseq_external_libclang}"
  BUILD_COMMAND
    "${CMAKE_COMMAND}" --build <BINARY_DIR>
      --target bioseq_external_codegen
  INSTALL_COMMAND ""
  TEST_COMMAND ""
  BUILD_ALWAYS TRUE
  BUILD_BYPRODUCTS "${_bioseq_codegen_executable}"
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE)

# cppast remains authoritative for parsing. This output-aware registry only
# gives CMake stable target names and byproduct contracts before generation.
include("${CMAKE_CURRENT_SOURCE_DIR}/codegen/annotation_registry.cmake")
file(GLOB_RECURSE _bioseq_external_scan_inputs CONFIGURE_DEPENDS
  "${_bioseq_external_source_root}/*.cppm")
list(SORT _bioseq_external_scan_inputs)
set(_bioseq_external_annotation_handlers
  "bioseq::api_client|api_client")
set(_bioseq_external_output_kinds)
set(_bioseq_external_descriptors)
set(_bioseq_external_descriptors_api_client)
set(_bioseq_external_descriptor_records)
foreach(_bioseq_handler IN LISTS _bioseq_external_annotation_handlers)
  bioseq_parse_annotation_handler(
    "${_bioseq_handler}" _bioseq_annotation _bioseq_output_kind)
  list(APPEND _bioseq_external_output_kinds "${_bioseq_output_kind}")
endforeach()
list(REMOVE_DUPLICATES _bioseq_external_output_kinds)
foreach(_bioseq_candidate IN LISTS _bioseq_external_scan_inputs)
  bioseq_find_registered_annotation(
    "${_bioseq_candidate}"
    _bioseq_external_annotation_handlers
    _bioseq_handler)
  if(NOT _bioseq_handler)
    continue()
  endif()
  bioseq_parse_annotation_handler(
    "${_bioseq_handler}" _bioseq_annotation _bioseq_output_kind)
  list(APPEND _bioseq_external_descriptors "${_bioseq_candidate}")
  list(APPEND
    _bioseq_external_descriptors_${_bioseq_output_kind}
    "${_bioseq_candidate}")
  list(APPEND _bioseq_external_descriptor_records
    "${_bioseq_handler}\t${_bioseq_candidate}")
endforeach()
list(SORT _bioseq_external_descriptors)
list(SORT _bioseq_external_descriptor_records)
foreach(_bioseq_output_kind IN LISTS _bioseq_external_output_kinds)
  list(SORT _bioseq_external_descriptors_${_bioseq_output_kind})
endforeach()

set(_bioseq_external_inventory_file
  "${CMAKE_CURRENT_BINARY_DIR}/annotation_inventory.txt")
set(_bioseq_external_inventory_temporary
  "${CMAKE_CURRENT_BINARY_DIR}/annotation_inventory.configure.tmp")
set(_bioseq_external_source_list_file
  "${CMAKE_CURRENT_BINARY_DIR}/annotation_sources.txt")
set(_bioseq_external_source_list_temporary
  "${CMAKE_CURRENT_BINARY_DIR}/annotation_sources.configure.tmp")
set(_bioseq_external_scan_stamp
  "${CMAKE_CURRENT_BINARY_DIR}/annotation_scan.stamp")
set(_bioseq_external_scan_depfile
  "${CMAKE_CURRENT_BINARY_DIR}/annotation_scan.d")
set(_bioseq_external_inventory_content
  "handlers=${_bioseq_external_annotation_handlers}\n")
foreach(_bioseq_descriptor_record IN LISTS _bioseq_external_descriptor_records)
  string(APPEND _bioseq_external_inventory_content
    "descriptor=${_bioseq_descriptor_record}\n")
endforeach()
file(WRITE "${_bioseq_external_inventory_temporary}"
  "${_bioseq_external_inventory_content}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${_bioseq_external_inventory_temporary}"
    "${_bioseq_external_inventory_file}"
  COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${_bioseq_external_inventory_temporary}")
string(REPLACE ";" "\n" _bioseq_external_source_list_content
  "${_bioseq_external_scan_inputs}")
if(_bioseq_external_source_list_content)
  string(APPEND _bioseq_external_source_list_content "\n")
endif()
file(WRITE "${_bioseq_external_source_list_temporary}"
  "${_bioseq_external_source_list_content}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${_bioseq_external_source_list_temporary}"
    "${_bioseq_external_source_list_file}"
  COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${_bioseq_external_source_list_temporary}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_bioseq_external_inventory_file}")

add_custom_command(
  OUTPUT "${_bioseq_external_scan_stamp}"
  BYPRODUCTS "${_bioseq_external_inventory_file}"
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_ROOT:PATH=${_bioseq_external_source_root}"
    "-DOUTPUT_FILE:FILEPATH=${_bioseq_external_inventory_file}"
    "-DSTAMP_FILE:FILEPATH=${_bioseq_external_scan_stamp}"
    "-DDEPFILE:FILEPATH=${_bioseq_external_scan_depfile}"
    "-DSOURCE_LIST_FILE:FILEPATH=${_bioseq_external_source_list_file}"
    "-DANNOTATION_HANDLERS:STRING=${_bioseq_external_annotation_handlers}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/codegen/scan_annotations.cmake"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/codegen/annotation_registry.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/codegen/scan_annotations.cmake"
    "${_bioseq_external_source_list_file}"
  DEPFILE "${_bioseq_external_scan_depfile}"
  COMMENT "Scanning Bioseq annotation graph"
  VERBATIM)
add_custom_target(bioseq_external_annotation_scan
  DEPENDS "${_bioseq_external_scan_stamp}")

set(_bioseq_generation_stamp
  "${BIOSEQ_EXTERNAL_GENERATED_ROOT}/generation.stamp")
set(_bioseq_generated_byproducts
  "${BIOSEQ_EXTERNAL_GENERATED_ROOT}/generation_state.tsv"
  "${BIOSEQ_EXTERNAL_GENERATED_ROOT}/generated_apis.cmake"
  "${BIOSEQ_EXTERNAL_GENERATED_ROOT}/generated_client.cmake")

set(_bioseq_external_client_names)
foreach(_bioseq_descriptor IN LISTS _bioseq_external_descriptors_api_client)
  get_filename_component(_bioseq_client_name "${_bioseq_descriptor}" NAME_WE)
  if(_bioseq_client_name IN_LIST _bioseq_external_client_names)
    message(FATAL_ERROR
      "External API annotation filenames must have unique stems: "
      "'${_bioseq_client_name}'")
  endif()
  list(APPEND _bioseq_external_client_names "${_bioseq_client_name}")
  set(_bioseq_client_root
    "${BIOSEQ_EXTERNAL_GENERATED_ROOT}/api/${_bioseq_client_name}")
  list(APPEND _bioseq_generated_byproducts
    "${_bioseq_client_root}/CMakeLists.txt"
    "${_bioseq_client_root}/client/index.cppm"
    "${_bioseq_client_root}/integration_tests/test_${_bioseq_client_name}_client.cpp"
    "${_bioseq_client_root}/manifest.tsv"
    "${_bioseq_client_root}/provenance.txt")
endforeach()

add_custom_command(
  OUTPUT "${_bioseq_generation_stamp}"
  BYPRODUCTS ${_bioseq_generated_byproducts}
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_EXTERNAL_GENERATED_ROOT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/generated_client.cmake"
    "${BIOSEQ_EXTERNAL_GENERATED_ROOT}/generated_client.cmake"
  COMMAND "${_bioseq_codegen_executable}" --self-test
  COMMAND "${_bioseq_codegen_executable}"
    --source-root "${_bioseq_external_source_root}"
    --output-root "${BIOSEQ_EXTERNAL_GENERATED_ROOT}"
    --clang-binary "${BIOSEQ_EXTERNAL_CLANG_BINARY}"
    --compiler-id "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    --stamp "${_bioseq_generation_stamp}"
  DEPENDS
    bioseq_external_codegen_project
    "${_bioseq_codegen_executable}"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated_client.cmake"
    "${_bioseq_external_inventory_file}"
    ${_bioseq_external_descriptors}
  WORKING_DIRECTORY "${BIOSEQ_REPOSITORY_ROOT}"
  COMMENT "Generating external API clients from Bioseq annotations"
  VERBATIM
  USES_TERMINAL)

add_custom_target(bioseq_external_codegen
  DEPENDS "${_bioseq_generation_stamp}")
add_dependencies(bioseq_external_codegen
  bioseq_external_annotation_scan)

set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OATPP_INSTALL OFF CACHE BOOL "" FORCE)
set(OATPP_LINK_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(OATPP_LINK_ATOMIC ON CACHE BOOL "" FORCE)

set(_bioseq_oatpp_root "${BIOSEQ_REPOSITORY_ROOT}/oatpp-master")
file(STRINGS "${_bioseq_oatpp_root}/src/oatpp/Environment.hpp"
  _bioseq_oatpp_version_line
  REGEX "^#define OATPP_VERSION \"1\\.4\\.[0-9]+\"$")
if(NOT _bioseq_oatpp_version_line)
  message(FATAL_ERROR
    "The repo-local oatpp-master dependency must provide Oat++ 1.4.x")
endif()

# DOC0372 — the postgresql_bioseq extension is a shared object, so every static
# archive that reaches its link (oatpp, via the provider adapters ->
# bioseq_external_https) must be position-independent. oatpp's own CMake does not
# force PIC, and its thread-local members emit R_X86_64_TPOFF32 relocations that a
# shared object rejects ("recompile with -fPIC"). Force PIC on the whole oatpp
# subtree by seeding the directory default before add_subdirectory; the standalone
# external test exec (a PIE executable) is unaffected.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT TARGET oatpp)
  add_subdirectory("${_bioseq_oatpp_root}"
    "${CMAKE_CURRENT_BINARY_DIR}/oatpp" EXCLUDE_FROM_ALL)
endif()
if(TARGET oatpp)
  set_target_properties(oatpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
if(NOT TARGET oatpp::oatpp)
  add_library(oatpp::oatpp ALIAS oatpp)
endif()

find_package(OpenSSL REQUIRED)

add_library(bioseq_external_https STATIC)
set_target_properties(bioseq_external_https PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(bioseq_external_https PRIVATE NO_POSTGRES=1)
target_sources(bioseq_external_https
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/http_validation.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/https.cppm")
target_link_libraries(bioseq_external_https PUBLIC
  oatpp::oatpp
  OpenSSL::SSL
  OpenSSL::Crypto)

include("${CMAKE_CURRENT_SOURCE_DIR}/generated_client.cmake")
foreach(_bioseq_descriptor IN LISTS _bioseq_external_descriptors_api_client)
  get_filename_component(_bioseq_client_name "${_bioseq_descriptor}" NAME_WE)
  set(_bioseq_client_root
    "${BIOSEQ_EXTERNAL_GENERATED_ROOT}/api/${_bioseq_client_name}")
  bioseq_register_generated_api_client(
    CLIENT_NAME "${_bioseq_client_name}"
    CLIENT_ROOT "${_bioseq_client_root}")
endforeach()

# ---------------------------------------------------------------------------
# DOC0372 — PostgreSQL-independent provider core (concepts + structs only). No
# Datum/tuple/palloc/SPI/PostgreSQL header, no alphabet, no libarchive edge.
# Owns bioseq.external.provider.* and links only the runtime/leaf/templating
# cores so metadata provider clients never schedule the sequence graph.
# ---------------------------------------------------------------------------
file(GLOB _bioseq_provider_core_modules CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/provider/*.cppm")
add_library(bioseq_external_provider_core STATIC)
set_target_properties(bioseq_external_provider_core PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_external_provider_core PRIVATE NO_POSTGRES=1)
target_link_libraries(bioseq_external_provider_core PUBLIC
  bioseq_runtime_core
  bioseq_sink_control_core
  bioseq_checked_arithmetic_core
  bioseq_common_templating_core
  hwy::hwy)
target_sources(bioseq_external_provider_core
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${_bioseq_provider_core_modules})

# ---------------------------------------------------------------------------
# DOC0372 — provider metadata adapters (NCBI/UCSC/UniProt/EBI) + configured pack.
# Each adapter normalizes one provider's generated GET client into the common
# owning-row model. Links the generated clients + provider core + simdjson; no
# PostgreSQL, alphabet, or transfer edge.
# ---------------------------------------------------------------------------
find_package(simdjson CONFIG REQUIRED)
file(GLOB _bioseq_provider_adapter_modules CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/providers/*.cppm")
# Classic (non-module) helper TUs for the adapters. simdjson's amalgamated
# header exposes a TU-local serializer entity that GCC refuses to write into a
# module CMI, so all simdjson use is confined to these plain translation units
# (json_dom.cpp), matching the repo's existing simdjson usage in io_bench.
file(GLOB _bioseq_provider_adapter_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/providers/*.cpp")
if(_bioseq_provider_adapter_modules)
  add_library(bioseq_external_provider_adapters STATIC)
  set_target_properties(bioseq_external_provider_adapters PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    POSITION_INDEPENDENT_CODE ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_compile_definitions(bioseq_external_provider_adapters PRIVATE NO_POSTGRES=1)
  target_link_libraries(bioseq_external_provider_adapters PUBLIC
    bioseq_external_provider_core
    bioseq_external_api_ncbi
    bioseq_external_api_ucsc
    bioseq_external_api_uniprot
    bioseq_external_api_ebi
    bioseq_external_https
    simdjson::simdjson)
  target_sources(bioseq_external_provider_adapters
    PUBLIC FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
      FILES ${_bioseq_provider_adapter_modules})
  if(_bioseq_provider_adapter_sources)
    target_sources(bioseq_external_provider_adapters
      PRIVATE ${_bioseq_provider_adapter_sources})
  endif()
endif()

# Provider-core + adapter tests run inside the existing external integration exec
# so the `external-api` MCP mode builds and runs them without a sequence/PG target.
if(TARGET bioseq_external_integration_tests_exec)
  file(GLOB _bioseq_provider_test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
  target_sources(bioseq_external_integration_tests_exec PRIVATE
    ${_bioseq_provider_test_sources})
  target_link_libraries(bioseq_external_integration_tests_exec PRIVATE
    bioseq_external_provider_core)
  if(TARGET bioseq_external_provider_adapters)
    target_link_libraries(bioseq_external_integration_tests_exec PRIVATE
      bioseq_external_provider_adapters)
  endif()
endif()
