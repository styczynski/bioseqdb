# The isolated code generator is provisioned once at the repository root via
# bioseq_add_codegen (cmake/bioseq_codegen.cmake). This directory only
# consumes the imported bioseq_codegen executable and the bioseq_codegen_project
# build target; it never creates another ExternalProject or discovers the GCC/
# Clang toolchain itself.

get_filename_component(BIOSEQ_REPOSITORY_ROOT
  "${CMAKE_SOURCE_DIR}" ABSOLUTE)
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

if(NOT TARGET bioseq_codegen)
  message(FATAL_ERROR
    "bioseq/external requires the root-provisioned bioseq_codegen target; "
    "ensure BIOSEQ_BUILD_EXTERNAL_API_CLIENTS is set at the repository root "
    "before add_subdirectory(bioseq)")
endif()

# gives CMake stable target names and byproduct contracts before generation.
include("${BIOSEQ_REPOSITORY_ROOT}/codegen/external/cmake/annotation_registry.cmake")
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
    -P "${BIOSEQ_REPOSITORY_ROOT}/codegen/external/cmake/scan_annotations.cmake"
  DEPENDS
    "${BIOSEQ_REPOSITORY_ROOT}/codegen/external/cmake/annotation_registry.cmake"
    "${BIOSEQ_REPOSITORY_ROOT}/codegen/external/cmake/scan_annotations.cmake"
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
    "${_bioseq_client_root}/tests/integration/${_bioseq_client_name}_client_test.cpp"
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
  COMMAND "${BIOSEQ_CODEGEN_EXECUTABLE}" self_test external
  COMMAND "${BIOSEQ_CODEGEN_EXECUTABLE}"
    external generate
    --generator-binary "${BIOSEQ_CODEGEN_EXECUTABLE}"
    --source-root "${_bioseq_external_source_root}"
    --output-root "${BIOSEQ_EXTERNAL_GENERATED_ROOT}"
    --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    --compiler-id "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    --stamp "${_bioseq_generation_stamp}"
  DEPENDS
    bioseq_codegen_project
    "${BIOSEQ_CODEGEN_EXECUTABLE}"
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

# Every static archive that reaches the extension shared object must be PIC.
# Seed the directory default before the pinned Oat++ source is added.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
FetchContent_Declare(bioseq_oatpp
  GIT_REPOSITORY "https://github.com/oatpp/oatpp.git"
  GIT_TAG "f83d648fd82dc222ef88aabbafb68efbd7d7bf50"
  GIT_SHALLOW FALSE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(bioseq_oatpp)

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
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/https.cppm")
target_link_libraries(bioseq_external_https PUBLIC
  bioseq_http_validation_core
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
# PostgreSQL-independent provider core (concepts + structs only). No
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
# Provider metadata adapters (NCBI/UCSC/UniProt/EBI) + configured pack.
# Each adapter normalizes one provider's generated GET client into the common
# owning-row model. Links the generated clients + provider core + simdjson; no
# PostgreSQL, alphabet, or transfer edge.
# ---------------------------------------------------------------------------
find_package(simdjson CONFIG REQUIRED)
file(GLOB _bioseq_provider_adapter_modules CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/providers/*.cppm")
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
    bioseq_external_https)
  target_link_libraries(bioseq_external_provider_adapters PRIVATE
    simdjson::simdjson)
  target_sources(bioseq_external_provider_adapters
    PUBLIC FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
      FILES
        ${_bioseq_provider_adapter_modules}
        "${CMAKE_CURRENT_SOURCE_DIR}/json/index.cppm")
  target_sources(bioseq_external_provider_adapters PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/json/simdjson.cppm")
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/json/simdjson.cppm"
    PROPERTIES COMPILE_OPTIONS "-x;c++")
endif()

# ---------------------------------------------------------------------------
# External streaming IO/transfer bundle. Owns the libcurl pull source,
# the locator SSRF firewall, the serial gzip decode source, the bounded channel
# glue, and the provider-neutral sequence streaming executor. It links the
# non-PostgreSQL IO modules (io_source concept + shared FASTA/stream parser) and
# curl/c-ares/libarchive/zlib. It never links PostgreSQL; the PG custom scan calls
# its `run_external_sequence_stream` entry point for sequence targets.
# ---------------------------------------------------------------------------
find_package(CURL REQUIRED)
find_package(c-ares CONFIG REQUIRED)
find_package(LibArchive REQUIRED)

add_library(bioseq_external_io STATIC)
set_target_properties(bioseq_external_io PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_external_io PRIVATE NO_POSTGRES=1)
target_sources(bioseq_external_io
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/transport/locator.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/transport/remote_source.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/transport/transfer_service.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/io/gzip_decode_source.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/io/sequence_stream.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/io/archive_source/index.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/io/datasets_stream.cppm")
target_sources(bioseq_external_io PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/io/archive_source/openssl_md5.cppm")
set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/io/archive_source/openssl_md5.cppm"
  PROPERTIES COMPILE_OPTIONS "-x;c++")
target_link_libraries(bioseq_external_io PUBLIC
  bioseq_external_provider_core
  bioseq_modules_nopg
  CURL::libcurl
  c-ares::cares
  LibArchive::LibArchive
  ZLIB::ZLIB)
target_link_libraries(bioseq_external_io PRIVATE
  OpenSSL::Crypto)

# Provider-core + adapter tests run inside the existing external integration exec
# so the `external-api` MCP mode builds and runs them without a sequence/PG target.
if(TARGET bioseq_external_api_integration_tests)
  # This target keeps the reusable rank-pack core, checked fixed-span writer,
  # and admitted-region adapter outside PostgreSQL. Keep the checked closure
  # explicit so an unrelated PostgreSQL-facing source cannot weaken the
  # denylist.
  set(_bioseq_pg_free_pack_sources
    "${_bioseq_external_source_root}/backend/sequences/rank_pack_core.cppm"
    "${_bioseq_external_source_root}/backend/sequences/rank_pack.cppm"
    "${_bioseq_external_source_root}/backend/storage/lazy_sequence.cppm")
  set(_bioseq_forbidden_pack_source_regexes
    "#[ \t]*include[ \t]*[<\"](postgres(\\.h)?|fmgr\\.h|headers/pg_compat\\.h|pg_compat\\.h|access/|catalog/|commands/|executor/|nodes/|utils/)"
    "(^|[^A-Za-z0-9_])(ereport|elog|palloc|palloc0|pfree)[ \t]*\\("
    "(^|[^A-Za-z0-9_])(Datum|PG_FUNCTION_ARGS)([^A-Za-z0-9_]|$)"
    "(^|[ \t])import[ \t]+bioseq\\.[^;\r\n]*pg[^;\r\n]*;")
  foreach(_bioseq_pg_free_pack_source IN LISTS _bioseq_pg_free_pack_sources)
    file(READ "${_bioseq_pg_free_pack_source}"
      _bioseq_pg_free_pack_source_contents)
    foreach(_bioseq_forbidden_pack_source_regex IN LISTS
            _bioseq_forbidden_pack_source_regexes)
      string(REGEX MATCH "${_bioseq_forbidden_pack_source_regex}"
        _bioseq_forbidden_pack_source_match
        "${_bioseq_pg_free_pack_source_contents}")
      if(NOT "${_bioseq_forbidden_pack_source_match}" STREQUAL "")
        message(FATAL_ERROR
          "PostgreSQL dependency '${_bioseq_pg_free_pack_source_match}' "
          "entered PG-free packing source ${_bioseq_pg_free_pack_source}")
      endif()
    endforeach()
  endforeach()
  target_compile_definitions(bioseq_external_api_integration_tests PRIVATE
    BIOSEQ_PG_PACK_DENYLIST_ENABLED=1)

  set(_bioseq_provider_test_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/io/archive_source_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/io/fasta_stream_benchmark_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/io/gzip_decode_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/io/lazy_adapter_no_pg_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/io/sequence_stream_live_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/io/streaming_live_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/provider/core_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/provider/rate_limit_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/provider/run_live_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/provider/ucsc_live_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/transport/transfer_service_test.cpp")
  target_sources(bioseq_external_api_integration_tests PRIVATE
    ${_bioseq_provider_test_sources})
  target_link_libraries(bioseq_external_api_integration_tests PRIVATE
    bioseq_external_provider_core
    bioseq_external_io)
  if(TARGET bioseq_external_provider_adapters)
    target_link_libraries(bioseq_external_api_integration_tests PRIVATE
      bioseq_external_provider_adapters)
  endif()
endif()
