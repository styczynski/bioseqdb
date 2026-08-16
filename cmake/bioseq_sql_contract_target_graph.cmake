# The raw compile database names the generated core entity-handle providers and
# the active-profile row-codec providers before their source files exist on a
# fresh tree.  Template-flatten PREPARE reaches both while recursively scanning
# the scheduler primary.  Namespace materialization selects production compiler
# authority, so a test-indexes raw-database row is retargeted to its production
# sibling and both the active SUPPORT stage and production SUPPORT stage must be
# complete before the scan.  Do not wait for either FINAL profile: those much
# wider closures are not scheduler inputs.  Append the concrete stamps as file
# dependencies because target-only edges would allow Ninja to run the custom
# commands concurrently.
add_custom_command(
  OUTPUT
    "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}/pcm/metadata/prepare_compile_commands.json"
  APPEND
  DEPENDS
    "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}")

set(BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY
  "${BIOSEQ_SQL_CODEGEN_DIR}/required-target-dyndeps.tsv")

# PREPARE consumes every C++ module row in CMake's raw compile database.
# Depending only on the extension dyndep therefore leaves unrelated, unique
# response-file classes (bench, cache, and compiler-contract fixtures)
# unmaterialized in a clean tree.  These top-level dyndep anchors are the
# bounded minimum that covers every current source+standard equivalence class
# through CMake's generated CXXModules dependencies.
#
# Resolve paths from target BINARY_DIR only after this directory has declared
# its benchmark targets. File dependencies materialize scans/modmaps without
# making compile targets depend on SQL validation; the extension link
# validation remains a link-only dependency below.
function(_bioseq_finalize_sql_target_dyndep_anchors)
  set(_bioseq_sql_target_dyndep_anchors
    bioseq_benchmark
    bioseq_benchmark_e2e_contract_tests
    bioseq_sql_annotation_probe
    bioseq_sql_gcc_compile_contract_objects
    postgresql_bioseq)
  if(TARGET bioseq_sql_row_codec_runtime_test_support_compile_donor)
    list(APPEND _bioseq_sql_target_dyndep_anchors
      bioseq_sql_row_codec_runtime_test_support_compile_donor)
  endif()
  if(BIOSEQ_BUILD_CACHE)
    list(APPEND _bioseq_sql_target_dyndep_anchors
      postgresql_bioseq_cache)
  endif()
  list(SORT _bioseq_sql_target_dyndep_anchors)

  string(CONCAT _bioseq_sql_target_dyndep_inventory
    "schema\tbioseq_sql_target_dyndep_inventory\n"
    "target\tdyndep\n")
  set(_bioseq_sql_target_dyndep_paths)
  foreach(_bioseq_sql_target IN LISTS
      _bioseq_sql_target_dyndep_anchors)
    if(NOT TARGET "${_bioseq_sql_target}")
      message(FATAL_ERROR
        "Required SQL PREPARE dyndep anchor target is absent: "
        "${_bioseq_sql_target}")
    endif()
    get_target_property(_bioseq_sql_target_binary_dir
      "${_bioseq_sql_target}" BINARY_DIR)
    if(NOT _bioseq_sql_target_binary_dir
       OR _bioseq_sql_target_binary_dir MATCHES "-NOTFOUND$")
      message(FATAL_ERROR
        "Required SQL PREPARE dyndep anchor has no BINARY_DIR: "
        "${_bioseq_sql_target}")
    endif()
    get_filename_component(_bioseq_sql_target_binary_dir
      "${_bioseq_sql_target_binary_dir}" ABSOLUTE)
    set(_bioseq_sql_target_dyndep
      "${_bioseq_sql_target_binary_dir}/CMakeFiles/${_bioseq_sql_target}.dir/CXX.dd")
    list(APPEND _bioseq_sql_target_dyndep_paths
      "${_bioseq_sql_target_dyndep}")
    string(APPEND _bioseq_sql_target_dyndep_inventory
      "${_bioseq_sql_target}\t${_bioseq_sql_target_dyndep}\n")
  endforeach()

  file(MAKE_DIRECTORY "${BIOSEQ_SQL_CODEGEN_DIR}")
  file(CONFIGURE
    OUTPUT "${BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY}"
    CONTENT "${_bioseq_sql_target_dyndep_inventory}"
    @ONLY
    NEWLINE_STYLE UNIX)
  foreach(_bioseq_sql_prepare_output IN ITEMS
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PREPARE_STAMP}")
    if("${_bioseq_sql_prepare_output}" STREQUAL "")
      message(FATAL_ERROR
        "SQL PREPARE output is unavailable during dyndep finalization")
    endif()
    add_custom_command(
      OUTPUT "${_bioseq_sql_prepare_output}"
      APPEND
      DEPENDS
        "${BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY}"
        ${_bioseq_sql_target_dyndep_paths})
  endforeach()
endfunction()
cmake_language(DEFER CALL
  _bioseq_finalize_sql_target_dyndep_anchors)

# Reusable provider closure for generated SQL C++ facets. The extension links
# these objects and PUBLIC BMIs, while the test-only OBJECT target below only
# consumes their BMIs to compile independent importers.
set(BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_ROW_CODECS
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_METADATA}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_TABLE}")
set(BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_PROVIDERS
  ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
  ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_ROW_CODECS}
  ${BIOSEQ_SQL_GUC_ACTIVE_MODULES})
set(BIOSEQ_SQL_POSTGRES_ROW_CODEC_MODULES
  ${BIOSEQ_SQL_ROW_CODEC_MODULES})
list(REMOVE_ITEM BIOSEQ_SQL_POSTGRES_ROW_CODEC_MODULES
  ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_ROW_CODECS})
set_source_files_properties(
  ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_PROVIDERS}
  PROPERTIES GENERATED TRUE)

add_library(bioseq_sql_compile_contract_providers OBJECT)
target_sources(bioseq_sql_compile_contract_providers
  PUBLIC
    FILE_SET bioseq_sql_contract_authored_providers
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS}
    FILE_SET bioseq_sql_contract_generated_providers
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    FILES ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_PROVIDERS})
target_include_directories(bioseq_sql_compile_contract_providers PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend"
  "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/safestringlib/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/fmtlog"
  "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/fmt"
  "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/seqan3/include")
target_compile_definitions(bioseq_sql_compile_contract_providers PUBLIC
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})
target_compile_options(bioseq_sql_compile_contract_providers PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
if(BIOSEQ_TEST_INDEXES)
  target_compile_definitions(bioseq_sql_compile_contract_providers
    PUBLIC BIOSEQ_TEST_INDEXES=1)
endif()
target_link_libraries(bioseq_sql_compile_contract_providers PUBLIC
  bioseq_modules_nopg
  bioseq_pg_support
  bioseq_pg_sql_boundary
  bioseq_sql_annotation_model)
set_target_properties(bioseq_sql_compile_contract_providers PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
# These objects compile only active-profile SUPPORT providers and
# profile-neutral CORE handles. Keep those exact generation prerequisites, but
# do not serialize compilation behind the FINAL SQL validation closures. FINAL
# generation and dual-profile validation remain ordinary DSO/link gates.
add_dependencies(
  bioseq_sql_compile_contract_providers
  bioseq_sql_support_${_bioseq_sql_active_profile}
  bioseq_sql_core_entity_handles)

# A production configure still prepares the independent test_indexes Clang
# profile. Give PREPARE one non-linking compile-command owner for the
# test-profile-only row-codec provider without attaching it to the production
# extension. In a BIOSEQ_TEST_INDEXES build the extension above is the owner,
# so this donor is deliberately absent.
if(NOT BIOSEQ_TEST_INDEXES)
  add_library(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    OBJECT EXCLUDE_FROM_ALL)
  target_sources(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    PRIVATE
      FILE_SET bioseq_sql_row_codec_runtime_test_support_compile_donor_modules
      TYPE CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
      FILES ${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES})
  target_link_libraries(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    PRIVATE bioseq_sql_compile_contract_providers)
  set_target_properties(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON
      CXX_MODULE_STD ON
      POSITION_INDEPENDENT_CODE ON
      INTERPROCEDURAL_OPTIMIZATION FALSE)
  add_dependencies(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    bioseq_sql_compile_contract_providers)
endif()

add_library(bioseq_sql_gcc_compile_contract_objects OBJECT EXCLUDE_FROM_ALL
  ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})
target_sources(bioseq_sql_gcc_compile_contract_objects
  PRIVATE
    FILE_SET bioseq_sql_gcc_contract_modules
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})
target_include_directories(bioseq_sql_gcc_compile_contract_objects PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend")
target_link_libraries(bioseq_sql_gcc_compile_contract_objects PRIVATE
  bioseq_sql_compile_contract_providers)
set_target_properties(bioseq_sql_gcc_compile_contract_objects PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
add_dependencies(
  bioseq_sql_gcc_compile_contract_objects
  bioseq_sql_compile_contract_providers)

# Static provenance and two-profile `.core.` descriptor gate for the
# single-compiler compile fixtures above. The ordinary target compiles the
# producer/consumer graph with whichever compiler owns this build; this Python
# step independently rejects authored specializations/access and proves both
# generated profiles describe the same invariant handle modules.
set(BIOSEQ_SQL_GCC_CONTRACT_VERIFIER
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/test_gcc_only_graph.py")
set(BIOSEQ_SQL_GCC_CONTRACT_NEGATIVE_FIXTURES
  ${BIOSEQ_SQL_NEGATIVE_FIXTURES})
set(BIOSEQ_SQL_GCC_CONTRACT_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/gcc-only-compile-contracts.stamp")
file(GLOB_RECURSE BIOSEQ_SQL_GCC_CONTRACT_PROVENANCE_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/*.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/**/*.cppm")
# Helper edits must invalidate this contract just as edits to the former
# monolithic bioseq/CMakeLists.txt did.
set(BIOSEQ_SQL_GCC_CONTRACT_SOURCE_INPUTS
  "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_source_inventory.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_source_guards.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_highway_target_graph.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_test_source_inventory.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_test_target_graph.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_postgres_source_graph.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_sql_generation_target_graph.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_sql_contract_target_graph.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_postgresql_target_graph.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_postgres_error_safety_target_graph.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_directory_target_finalization.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/CMakeLists.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/safety/postgres_error_safety.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_codegen.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_sql_codegen.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bioseq_gcc_toolchain.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/gcc_hwy_header_unit_launcher.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/collector/index.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/collector/options.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/index.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cppm"
  ${BIOSEQ_SQL_GCC_CONTRACT_NEGATIVE_FIXTURES}
  ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES}
  ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES}
  ${BIOSEQ_SQL_GCC_CONTRACT_PROVENANCE_SOURCES})
set(_bioseq_sql_gcc_contract_generated_args)
foreach(_bioseq_sql_gcc_contract_generated_source IN LISTS
    BIOSEQ_SQL_ENTITY_HANDLE_MODULES)
  list(APPEND _bioseq_sql_gcc_contract_generated_args
    --allowed-core-generated-source
    "${_bioseq_sql_gcc_contract_generated_source}")
endforeach()
foreach(_bioseq_sql_gcc_contract_generated_source IN LISTS
    BIOSEQ_SQL_GUC_production_MODULES)
  list(APPEND _bioseq_sql_gcc_contract_generated_args
    --allowed-production-guc-generated-source
    "${_bioseq_sql_gcc_contract_generated_source}")
endforeach()
foreach(_bioseq_sql_gcc_contract_generated_source IN LISTS
    BIOSEQ_SQL_GUC_test_indexes_MODULES)
  list(APPEND _bioseq_sql_gcc_contract_generated_args
    --allowed-test-indexes-guc-generated-source
    "${_bioseq_sql_gcc_contract_generated_source}")
endforeach()
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_GCC_CONTRACT_STAMP}"
  COMMAND "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_GCC_CONTRACT_VERIFIER}"
    --repository-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
    --production-manifest
      "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
    --test-indexes-manifest
      "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
    --core-manifest "${BIOSEQ_SQL_CORE_MANIFEST}"
    --core-seal "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    --generated-root "${BIOSEQ_SQL_PROFILE_MODULE_ROOT}"
    ${_bioseq_sql_gcc_contract_generated_args}
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_GCC_CONTRACT_STAMP}"
  DEPENDS
    bioseq_sql_final
    "${BIOSEQ_SQL_GCC_CONTRACT_VERIFIER}"
    ${BIOSEQ_SQL_GCC_CONTRACT_SOURCE_INPUTS}
    ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
    ${BIOSEQ_SQL_GUC_GENERATED_MODULES}
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
    "${BIOSEQ_SQL_CORE_MANIFEST}"
    "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
  COMMENT
    "Validating GCC-only SQL module/provenance and profile-equal CORE contracts"
  VERBATIM)
add_custom_target(bioseq_sql_gcc_compile_contracts
  DEPENDS "${BIOSEQ_SQL_GCC_CONTRACT_STAMP}")
add_dependencies(
  bioseq_sql_gcc_compile_contracts
  bioseq_sql_gcc_compile_contract_objects
  bioseq_sql_negative_contracts)

# PostgreSQL ERROR-safety is derived from a second, post-FINAL Clang closure.
# FINAL first publishes the generated fmgr entrypoint/registry modules; the
# safety collector then audits those exact modules together with every natural
# FINAL owner.  Each profile owns independent compile, PCM, graph, fact, and
# ledger artifacts so neither profile can discharge the other's obligations.
set(BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT
  "${BIOSEQ_SQL_CODEGEN_DIR}/safety")
set(BIOSEQ_SQL_SAFETY_GENERATION_CONTRACT
  "bioseq-postgres-error-safety")
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
  "${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}")
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_safety_profile_root
    "${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}/${_bioseq_sql_profile}")
  file(MAKE_DIRECTORY "${_bioseq_sql_safety_profile_root}")
  set(_bioseq_sql_safety_sources
    ${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_FINAL_SOURCES}
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINT_MODULES})
  list(SORT _bioseq_sql_safety_sources)
  list(REMOVE_DUPLICATES _bioseq_sql_safety_sources)
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_SOURCE_LIST
    "${_bioseq_sql_safety_profile_root}/sources.txt")
  _bioseq_sql_write_registry_file(
    "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_SOURCE_LIST}"
    ${_bioseq_sql_safety_sources})

  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_DATABASE
    "${_bioseq_sql_safety_profile_root}/clang/compile_commands.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_ROOT
    "${_bioseq_sql_safety_profile_root}/clang/pcm")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_MANIFEST
    "${_bioseq_sql_safety_profile_root}/clang/pcm-manifest.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_DEPFILE
    "${_bioseq_sql_safety_profile_root}/clang/prepare.d")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_STAMP
    "${_bioseq_sql_safety_profile_root}/clang/prepare.stamp")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH
    "${_bioseq_sql_safety_profile_root}/canonical-compile-graph.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS
    "${_bioseq_sql_safety_profile_root}/compiler-facts.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_FACTS_STAMP
    "${_bioseq_sql_safety_profile_root}/facts.stamp")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER
    "${_bioseq_sql_safety_profile_root}/postgres-error-ledger.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER_STAMP
    "${_bioseq_sql_safety_profile_root}/ledger.stamp")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY
    "${_bioseq_sql_safety_profile_root}/runtime-probe-inventory.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY_STAMP
    "${_bioseq_sql_safety_profile_root}/runtime-probe-inventory.stamp")

  set(_bioseq_sql_safety_facts_target
    "bioseq_sql_postgres_error_safety_facts_${_bioseq_sql_profile}")
  set(_bioseq_sql_safety_profile_target
    "bioseq_sql_postgres_error_safety_${_bioseq_sql_profile}")
  set(_bioseq_sql_safety_inventory_target
    "bioseq_sql_postgres_error_safety_inventory_${_bioseq_sql_profile}")
  bioseq_add_postgres_error_safety_facts_profile(
    TARGET "${_bioseq_sql_safety_facts_target}"
    PROFILE "${_bioseq_sql_profile}"
    FINAL_TARGET "bioseq_sql_final_${_bioseq_sql_profile}"
    SOURCE_LIST
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_SOURCE_LIST}"
    RAW_COMPILE_DATABASE "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    SQL_MANIFEST
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    PROVENANCE "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    OUTPUT_COMPILE_DATABASE
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_DATABASE}"
    OUTPUT_PCM_ROOT
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_ROOT}"
    OUTPUT_PCM_MANIFEST
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_MANIFEST}"
    OUTPUT_PREPARE_DEPFILE
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_DEPFILE}"
    OUTPUT_PREPARE_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_STAMP}"
    OUTPUT_COMPILE_GRAPH
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH}"
    OUTPUT_FACTS
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS}"
    OUTPUT_FACTS_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_FACTS_STAMP}"
    SOURCES ${_bioseq_sql_safety_sources}
    DEPENDS
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP}")
  bioseq_add_postgres_error_safety_profile(
    TARGET "${_bioseq_sql_safety_profile_target}"
    PROFILE "${_bioseq_sql_profile}"
    SQL_MANIFEST
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    COMPILER_FACTS
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS}"
    COMPILE_GRAPH
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH}"
    PROVENANCE "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    GENERATOR_REVISION "${BIOSEQ_SQL_SAFETY_GENERATION_CONTRACT}"
    OUTPUT_LEDGER
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER}"
    OUTPUT_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER_STAMP}"
    DEPENDS "${_bioseq_sql_safety_facts_target}")
  bioseq_add_postgres_error_safety_inventory(
    TARGET "${_bioseq_sql_safety_inventory_target}"
    PROFILE "${_bioseq_sql_profile}"
    LEDGER
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER}"
    COMPILER_FACTS
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS}"
    COMPILE_GRAPH
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH}"
    SQL_MANIFEST
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    PRODUCER_EXECUTABLE "${BIOSEQ_CODEGEN_EXECUTABLE}"
    OUTPUT_INVENTORY
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY}"
    OUTPUT_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY_STAMP}"
    DEPENDS "${_bioseq_sql_safety_profile_target}")
endforeach()

bioseq_add_postgres_error_safety_pair(
  TARGET bioseq_sql_postgres_error_safety
  PRODUCTION_TARGET bioseq_sql_postgres_error_safety_production
  TEST_INDEXES_TARGET bioseq_sql_postgres_error_safety_test_indexes)
add_dependencies(
  bioseq_sql_postgres_error_safety
  bioseq_sql_postgres_error_safety_inventory_production
  bioseq_sql_postgres_error_safety_inventory_test_indexes)

set(BIOSEQ_SQL_EXTERNAL_FEATURE_CONTRACT_SCRIPT
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/test_external_feature.py")
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  set(_bioseq_sql_external_feature_expect enabled)
else()
  set(_bioseq_sql_external_feature_expect disabled)
endif()
set(_bioseq_sql_external_feature_contract_stamps)
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_external_feature_contract_stamp
    "${BIOSEQ_SQL_PROFILE_ARTIFACT_ROOT}/${_bioseq_sql_profile}/external-feature-contract.stamp")
  add_custom_command(
    OUTPUT "${_bioseq_sql_external_feature_contract_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${Python3_EXECUTABLE}"
      "${BIOSEQ_SQL_EXTERNAL_FEATURE_CONTRACT_SCRIPT}"
      --install-sql
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL}"
      --manifest
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
      --expect "${_bioseq_sql_external_feature_expect}"
      --stamp "${_bioseq_sql_external_feature_contract_stamp}"
    DEPENDS
      "${BIOSEQ_SQL_EXTERNAL_FEATURE_CONTRACT_SCRIPT}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    COMMENT
      "Verifying ${_bioseq_sql_profile} external SQL feature contract"
    VERBATIM)
  list(APPEND _bioseq_sql_external_feature_contract_stamps
    "${_bioseq_sql_external_feature_contract_stamp}")
endforeach()
add_custom_target(bioseq_sql_external_feature_contract
  DEPENDS ${_bioseq_sql_external_feature_contract_stamps})

set(_bioseq_sql_generation_contract_outputs
  ${_bioseq_sql_final_stamps}
  ${_bioseq_sql_external_feature_contract_stamps})
# The full post-FINAL Clang facts validation is opt-in. Keep its static report
# available without pulling it into ordinary generated-SQL or extension-link
# gates.
if(NOT "${_bioseq_sql_safety_review_inputs}" STREQUAL "")
  set(BIOSEQ_SQL_SAFETY_VALIDATION_REPORT
    "${BIOSEQ_SQL_CODEGEN_DIR}/bioseq-safety-static-validation.json")
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_SAFETY_VALIDATION_REPORT}"
    COMMAND "${Python3_EXECUTABLE}"
      "${BIOSEQ_SQL_VALIDATOR}"
      --static-only
      --production-ledger
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
      --test-indexes-ledger
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
      --reviewed-production-ledger
        "${BIOSEQ_SQL_SAFETY_REVIEWED_PRODUCTION_LEDGER}"
      --reviewed-test-indexes-ledger
        "${BIOSEQ_SQL_SAFETY_REVIEWED_TEST_INDEXES_LEDGER}"
      --production-source-list
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_SOURCE_LIST}"
      --test-indexes-source-list
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_SOURCE_LIST}"
      --production-compile-database
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_DATABASE}"
      --test-indexes-compile-database
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_DATABASE}"
      --production-compile-graph
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_GRAPH}"
      --test-indexes-compile-graph
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_GRAPH}"
      --production-pcm-manifest
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_PCM_MANIFEST}"
      --test-indexes-pcm-manifest
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PCM_MANIFEST}"
      --production-compiler-facts
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILER_FACTS}"
      --test-indexes-compiler-facts
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILER_FACTS}"
      --production-compiler-facts-producer-executable
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
      --test-indexes-compiler-facts-producer-executable
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
      --provenance
        "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
      --production-sql-manifest
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
      --test-indexes-sql-manifest
        "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
      ${_bioseq_sql_completion_args}
      --report "${BIOSEQ_SQL_SAFETY_VALIDATION_REPORT}"
    DEPENDS
      bioseq_sql_postgres_error_safety
      "${BIOSEQ_SQL_VALIDATOR}"
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_SOURCE_LIST}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_SOURCE_LIST}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_DATABASE}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_DATABASE}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_GRAPH}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_GRAPH}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PCM_MANIFEST}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PCM_MANIFEST}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILER_FACTS}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILER_FACTS}"
      "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
      "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
      ${_bioseq_sql_safety_review_inputs}
    COMMENT
      "Validating current PostgreSQL ERROR-safety ledgers against reviewed inputs"
    VERBATIM)
  add_custom_target(bioseq_sql_postgres_error_safety_static_evidence
    DEPENDS "${BIOSEQ_SQL_SAFETY_VALIDATION_REPORT}")
endif()
add_custom_target(bioseq_sql_generation_contract
  DEPENDS ${_bioseq_sql_generation_contract_outputs})
add_dependencies(
  bioseq_sql_generation_contract
  bioseq_sql_validator_self_test
  bioseq_sql_external_feature_contract)
