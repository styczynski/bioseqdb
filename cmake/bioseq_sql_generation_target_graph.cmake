target_link_libraries(bioseq_modules_nopg
  PUBLIC bioseq_sql_annotation_model)

add_library(bioseq_sql_annotation_probe OBJECT
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cpp")
target_sources(bioseq_sql_annotation_probe
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../codegen"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cppm")
target_link_libraries(bioseq_sql_annotation_probe
  PRIVATE bioseq_sql_annotation_model)
target_compile_options(bioseq_sql_annotation_probe
  PRIVATE
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
    "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
set_target_properties(bioseq_sql_annotation_probe PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON)

# The collector receives the configured backend module closure and ignores
# modules without bioseq_sql annotations.
set(BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULE
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/row_codec_runtime_test_support.cppm")
set(BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES
  "${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/contract_core.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/catalog_lifecycle.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/toast_faults.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/codec_fixtures.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES})
# The test-indexes extension imports and owns this provider. Production keeps
# it out of the DSO; a dedicated excluded target declared after the generated
# SQL provider closure supplies its raw compile command to dual-profile PREPARE.
if(BIOSEQ_TEST_INDEXES)
  list(APPEND SRC_MOD_FILES
    ${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES})
endif()
set(BIOSEQ_SQL_CODEGEN_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql")
set(BIOSEQ_SQL_PROFILE_ARTIFACT_ROOT
  "${BIOSEQ_SQL_CODEGEN_DIR}/profiles")
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_profile_artifact_dir
    "${BIOSEQ_SQL_PROFILE_ARTIFACT_ROOT}/${_bioseq_sql_profile}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL
    "${_bioseq_sql_profile_artifact_dir}/bioseq--1.0.sql")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST
    "${_bioseq_sql_profile_artifact_dir}/bioseq-sql-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP
    "${_bioseq_sql_profile_artifact_dir}/bioseq-sql.stamp")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST
    "${_bioseq_sql_profile_artifact_dir}/support-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP
    "${_bioseq_sql_profile_artifact_dir}/support.stamp")
endforeach()
if(BIOSEQ_TEST_INDEXES)
  set(_bioseq_expected_typed_sql_support_stamp
    "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}")
else()
  set(_bioseq_expected_typed_sql_support_stamp
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}")
endif()
if(NOT "${BIOSEQ_TYPED_SQL_SUPPORT_STAMP}" STREQUAL
    "${_bioseq_expected_typed_sql_support_stamp}")
  message(FATAL_ERROR
    "Template-flatten SQL support dependency does not match the active profile")
endif()
unset(_bioseq_expected_typed_sql_support_stamp)
set(BIOSEQ_SQL_CORE_DESCRIPTOR_HASH
  "${BIOSEQ_SQL_CODEGEN_DIR}/core/core-descriptor.sha256")
set(BIOSEQ_SQL_CORE_MANIFEST
  "${BIOSEQ_SQL_CODEGEN_DIR}/core/core-manifest.json")
set(BIOSEQ_SQL_CORE_GENERATED_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/core/core.stamp")
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_SQL_GENERATED_INSTALL
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_INSTALL}")
  set(BIOSEQ_SQL_GENERATED_MANIFEST
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}")
  set(BIOSEQ_SQL_GENERATED_STAMP
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_STAMP}")
else()
  set(BIOSEQ_SQL_GENERATED_INSTALL
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}")
  set(BIOSEQ_SQL_GENERATED_MANIFEST
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}")
  set(BIOSEQ_SQL_GENERATED_STAMP
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_STAMP}")
endif()
set(BIOSEQ_SQL_VALIDATOR
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/safety/validate_postgres_error_safety.py")
set(BIOSEQ_SQL_SAFETY_PROVENANCE
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/safety/tests/contract/fixtures/pg17_provenance.json")
set(BIOSEQ_SQL_SAFETY_RUNTIME_POLICY
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/safety/policy/postgres_error_safety_policy.json")
set(BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE "" CACHE FILEPATH
  "Reviewed profile-neutral PostgreSQL runtime case template")
set(BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR "" CACHE PATH
  "Inverse-profile runtime-instrumented Bioseq build tree")
set(BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE "isolated" CACHE STRING
  "Runtime validation cluster relationship (isolated only)")
set_property(CACHE BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE
  PROPERTY STRINGS isolated)
set(BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS "600" CACHE STRING
  "Per-child timeout for PostgreSQL runtime validation")
mark_as_advanced(
  BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE
  BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR
  BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE
  BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS)
set(BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/validator-self-test.stamp")
set(BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/structural_analysis/tests/integration/test_prepare_incremental.py")
set(BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/prepare-incremental-self-test.stamp")
if(NOT EXISTS "${BIOSEQ_SQL_VALIDATOR}")
  message(FATAL_ERROR
    "Missing PostgreSQL safety validator: ${BIOSEQ_SQL_VALIDATOR}")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST)
  if(NOT EXISTS "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}")
    message(FATAL_ERROR
      "Missing PREPARE incremental fixture: "
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}")
  endif()
  find_program(BIOSEQ_SQL_PREPARE_NINJA
    NAMES ninja ninja-build
    REQUIRED)
  mark_as_advanced(BIOSEQ_SQL_PREPARE_NINJA)
endif()
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_SQL_CODEGEN_DIR}"
  COMMAND "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_VALIDATOR}" --self-test
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}"
  DEPENDS
    "${BIOSEQ_SQL_VALIDATOR}"
    "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
  COMMENT "Running PostgreSQL safety validator self-tests"
  VERBATIM)
add_custom_target(bioseq_sql_validator_self_test
  DEPENDS "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}")
set(BIOSEQ_SQL_NEGATIVE_FIXTURES
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_nested_annotation_namespace.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_class_head_placement.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_guc_enumerator_placement.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_mixed_guc_alias_initialization.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_catalog_handle_specialization.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_guc_registration_access.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_catalog_handle_namespace_alias.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_guc_access_using_declaration.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_guc_registration_type_alias.cppm")
set(BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/negative-contracts.stamp")
if(BIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST)
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test sql
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      ${BIOSEQ_SQL_NEGATIVE_FIXTURES}
      "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/collector/index.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/collector/options.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/sql/tests/contract/index.cppm"
    COMMENT
      "Running real structural SQL negative/provenance contract fixtures"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    COMMENT "Skipping disabled maintainer SQL negative/provenance fixtures"
    VERBATIM)
endif()
add_custom_target(bioseq_sql_negative_contracts
  DEPENDS "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}")
if(BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST)
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "${Python3_EXECUTABLE}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}" run
      --codegen "$<TARGET_FILE:bioseq_codegen>"
      --ninja "${BIOSEQ_SQL_PREPARE_NINJA}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256
        "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    COMMENT "Running bounded Ninja PREPARE incremental contract"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    COMMENT "Skipping disabled maintainer PREPARE incremental contract"
    VERBATIM)
endif()
add_custom_target(bioseq_sql_prepare_incremental_self_test
  DEPENDS "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}")
set(BIOSEQ_SQL_ENTITY_HANDLES_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bioseq/sql/core/owners")
set(BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE
  "${BIOSEQ_SQL_ENTITY_HANDLES_DIR}/storage_metadata_core.entity_handles.cppm")
set(BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED
  "${BIOSEQ_SQL_ENTITY_HANDLES_DIR}/storage_indexing_pg_chunked_storage.entity_handles.cppm")
set(BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE
  "${BIOSEQ_SQL_ENTITY_HANDLES_DIR}/storage_manager_sequence_storage_manager.entity_handles.cppm")
set(BIOSEQ_SQL_ENTITY_HANDLE_MODULES
  "${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE}"
  "${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED}"
  "${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE}")
set(BIOSEQ_SQL_PROFILE_MODULE_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bioseq/sql")
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_row_codecs_dir
    "${BIOSEQ_SQL_PROFILE_MODULE_ROOT}/${_bioseq_sql_profile}/owners")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA
    "${_bioseq_sql_row_codecs_dir}/storage_metadata.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG
    "${_bioseq_sql_row_codecs_dir}/storage_indexing_pg.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG
    "${_bioseq_sql_row_codecs_dir}/storage_load_pg.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER
    "${_bioseq_sql_row_codecs_dir}/storage_manager.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE
    "${_bioseq_sql_row_codecs_dir}/table.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODEC_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE}")

  set(_bioseq_sql_fmgr_entrypoints_dir
    "${BIOSEQ_SQL_PROFILE_MODULE_ROOT}/${_bioseq_sql_profile}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD
    "${_bioseq_sql_fmgr_entrypoints_dir}/mod.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT
    "${_bioseq_sql_fmgr_entrypoints_dir}/highway_target_test_support.entrypoints.cppm")
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    set(BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT
      "${_bioseq_sql_fmgr_entrypoints_dir}/highway_ksw_target_test_support.entrypoints.cppm")
  endif()
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER
    "${_bioseq_sql_fmgr_entrypoints_dir}/storage_manager_sequence_storage_manager.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS
    "${_bioseq_sql_fmgr_entrypoints_dir}/table_access_hooks.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG
    "${_bioseq_sql_fmgr_entrypoints_dir}/storage_load_pg.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF
    "${_bioseq_sql_fmgr_entrypoints_dir}/sequences_sample_table_udf.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE
    "${_bioseq_sql_fmgr_entrypoints_dir}/postgres_types_compact_sequence.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_types_integer.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_types_date.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_types_aggregates.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_comparison_registry.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_aggregate_registry.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINT_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}")
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    list(APPEND BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES
      "${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT}")
  endif()
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_FMGR_ENTRYPOINT_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES}")
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    list(APPEND BIOSEQ_SQL_TEST_INDEXES_FINAL_FMGR_ENTRYPOINT_MODULES
      "${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT}")
  endif()
endforeach()
if(DEFINED BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT)
  message(FATAL_ERROR
    "KSW boundary FMGR output must not exist in the production SQL profile")
endif()
set(_bioseq_ksw_test_fmgr_occurrences 0)
foreach(_bioseq_fmgr_module
    IN LISTS BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES)
  if(_bioseq_fmgr_module STREQUAL
      BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT)
    math(EXPR _bioseq_ksw_test_fmgr_occurrences
      "${_bioseq_ksw_test_fmgr_occurrences} + 1")
  endif()
endforeach()
if(NOT _bioseq_ksw_test_fmgr_occurrences EQUAL 1
    OR BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT
      IN_LIST BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES)
  message(FATAL_ERROR
    "KSW boundary FMGR must occur exactly once in test_indexes and never "
    "in the production generated-module list")
endif()
unset(_bioseq_fmgr_module)
unset(_bioseq_ksw_test_fmgr_occurrences)
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_SQL_ROW_CODEC_MODULES
    ${BIOSEQ_SQL_TEST_INDEXES_ROW_CODEC_MODULES})
  set(BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES
    ${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES})
  set(_bioseq_sql_active_profile test_indexes)
  set(_bioseq_sql_active_profile_key TEST_INDEXES)
else()
  set(BIOSEQ_SQL_ROW_CODEC_MODULES
    ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES})
  set(BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES
    ${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES})
  set(_bioseq_sql_active_profile production)
  set(_bioseq_sql_active_profile_key PRODUCTION)
endif()
foreach(_bioseq_sql_output_name IN ITEMS
    ROW_CODECS_STORAGE_METADATA
    ROW_CODECS_STORAGE_INDEXING_PG
    ROW_CODECS_STORAGE_LOAD_PG
    ROW_CODECS_STORAGE_MANAGER
    ROW_CODECS_TABLE
    FMGR_ENTRYPOINTS_MOD
    FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT
    FMGR_ENTRYPOINTS_STORAGE_MANAGER
    FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS
    FMGR_ENTRYPOINTS_STORAGE_LOAD_PG
    FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF
    FMGR_ENTRYPOINTS_COMPACT_SEQUENCE
    FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER
    FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE
    FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES
    VECTOR_COMPARISON_REGISTRY
    VECTOR_AGGREGATE_REGISTRY)
  set(BIOSEQ_SQL_${_bioseq_sql_output_name}
    "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_${_bioseq_sql_output_name}}")
endforeach()
set(_bioseq_sql_completion_args
  --require-complete-codegen)
set(_bioseq_sql_completion_target_option
  REQUIRE_COMPLETE_CODEGEN)
set(BIOSEQ_SQL_SAFETY_EVIDENCE_DIR "" CACHE PATH
  "Reviewed production/test-index PostgreSQL safety validation bundle")
mark_as_advanced(BIOSEQ_SQL_SAFETY_EVIDENCE_DIR)
set(_bioseq_sql_safety_review_inputs)
if(NOT "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}" STREQUAL "")
  get_filename_component(BIOSEQ_SQL_SAFETY_EVIDENCE_DIR
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}" ABSOLUTE)
  set(BIOSEQ_SQL_SAFETY_PRODUCTION_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/production-ledger.json")
  set(BIOSEQ_SQL_SAFETY_TEST_INDEXES_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/test-indexes-ledger.json")
  set(BIOSEQ_SQL_SAFETY_REVIEWED_PRODUCTION_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/reviewed/production-ledger.json")
  set(BIOSEQ_SQL_SAFETY_REVIEWED_TEST_INDEXES_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/reviewed/test-indexes-ledger.json")
  set(BIOSEQ_SQL_SAFETY_RUNTIME_MATRIX
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/runtime-matrix.json")
  set(BIOSEQ_SQL_SAFETY_REVIEWED_RUNTIME_MATRIX
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/reviewed/runtime-matrix.json")
  set(_bioseq_sql_safety_review_inputs
    "${BIOSEQ_SQL_SAFETY_PRODUCTION_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_PRODUCTION_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_TEST_INDEXES_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_MATRIX}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_RUNTIME_MATRIX}")
  foreach(_bioseq_sql_safety_input
      IN LISTS _bioseq_sql_safety_review_inputs)
    if(NOT EXISTS "${_bioseq_sql_safety_input}")
      message(FATAL_ERROR
        "Incomplete PostgreSQL safety validation bundle: missing "
        "${_bioseq_sql_safety_input}")
    endif()
  endforeach()
endif()
set(BIOSEQ_SQL_GUC_OUTPUT_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bioseq/sql/gucs")
set(BIOSEQ_SQL_GUC_GENERATED_MODULES)
foreach(_bioseq_guc_profile IN ITEMS production test_indexes)
  set(_bioseq_guc_profile_modules)
  foreach(_bioseq_guc_owner IN ITEMS bioseq scheduler storage aligner)
    list(APPEND _bioseq_guc_profile_modules
      "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/${_bioseq_guc_profile}/owners/${_bioseq_guc_owner}.gucs.cppm")
  endforeach()
  list(APPEND _bioseq_guc_profile_modules
    "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/${_bioseq_guc_profile}/guc_registry.cppm")
  set(BIOSEQ_SQL_GUC_${_bioseq_guc_profile}_MODULES
    "${_bioseq_guc_profile_modules}")
  list(APPEND BIOSEQ_SQL_GUC_GENERATED_MODULES
    ${_bioseq_guc_profile_modules})
endforeach()
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_SQL_GUC_ACTIVE_PROFILE test_indexes)
  set(BIOSEQ_SQL_GUC_ACTIVE_MODULES
    ${BIOSEQ_SQL_GUC_test_indexes_MODULES})
else()
  set(BIOSEQ_SQL_GUC_ACTIVE_PROFILE production)
  set(BIOSEQ_SQL_GUC_ACTIVE_MODULES
    ${BIOSEQ_SQL_GUC_production_MODULES})
endif()

# SQL ownership is an explicit configure-time contract.  These registries are
# the only inputs accepted by the collector; ordinary target source lists are
# intentionally not consulted.  SUPPORT sources are dependency-only
# declaration modules, CORE is profile-neutral authority, and FINAL is the full
# profile closure consumed by final SQL and boundary generation.
bioseq_sql_begin_owner_registry(
  ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
  PROFILES production test_indexes
  AUDIT_ROOTS backend)

bioseq_register_sql_owner(
  OWNER_KEY aligner_guc
  MODULE bioseq.postgres.configuration.aligner
  SOURCES backend/pg/configuration/aligner.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS aligner)
bioseq_register_sql_owner(
  OWNER_KEY scheduler_guc
  MODULE bioseq.postgres.configuration.scheduler
  SOURCES backend/pg/configuration/scheduler.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS scheduler)
bioseq_register_sql_owner(
  OWNER_KEY extension_guc
  MODULE bioseq.postgres.configuration.core
  SOURCES backend/pg/configuration/core.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS bioseq)
bioseq_register_sql_owner(
  OWNER_KEY storage_guc
  MODULE bioseq.postgres.configuration.storage
  SOURCES backend/pg/configuration/storage.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS storage)

bioseq_register_sql_owner(
  OWNER_KEY alignment_rows
  MODULE bioseq.postgres.extension.alignment.sql_rows
  SOURCES backend/pg/extension/alignment_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY storage_metadata_rows
  MODULE bioseq.postgres.storage.metadata.sql_rows
  SOURCES backend/pg/column/metadata/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_metadata)
bioseq_register_sql_owner(
  OWNER_KEY mod_rows
  MODULE bioseq.postgres.extension.sql_rows
  SOURCES backend/pg/extension/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY sequences_sample_table_udf_rows
  MODULE bioseq.postgres.sequences.sample_table.sql_rows
  SOURCES backend/pg/sequences/sample_table/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ANNOTATION_FREE_RESULT_ROWS sequences_sample_table_udf)
bioseq_register_sql_owner(
  OWNER_KEY sql_function_rows
  MODULE bioseq.postgres.sql.functions.sql_rows
  SOURCES backend/pg/sql/functions_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY storage_indexing_rows
  MODULE bioseq.postgres.storage.indexing.sql_rows
  SOURCES backend/pg/storage/indexing/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_indexing_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_load_rows
  MODULE bioseq.postgres.storage.io.sql_rows
  SOURCES backend/pg/storage/io/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_load_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_rows
  MODULE bioseq.postgres.storage.manager.sql_rows
  SOURCES backend/pg/storage/manager/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY table_rows
  MODULE bioseq.postgres.table.sql_rows
  SOURCES backend/pg/table/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS table)

bioseq_register_sql_owner(
  OWNER_KEY storage_metadata_core
  MODULE bioseq.postgres.storage.metadata.sql_entities
  SOURCES backend/pg/column/metadata/sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_metadata_core)
bioseq_register_sql_owner(
  OWNER_KEY mod
  MODULE bioseq.postgres.extension
  SOURCES backend/pg/extension/index.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS mod)
bioseq_register_sql_owner(
  OWNER_KEY highway_target_test_support
  MODULE bioseq.highway_target_test_support
  SOURCES backend/pg/highway_target_test_support.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS highway_target_test_support)
bioseq_register_sql_owner(
  OWNER_KEY highway_ksw_target_test_support
  MODULE bioseq.highway_ksw_target_test_support
  SOURCES backend/pg/highway_ksw_target_test_support.cppm
  PROFILES test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS highway_ksw_target_test_support)
bioseq_register_sql_owner(
  OWNER_KEY sequences_sample_table_udf
  MODULE bioseq.postgres.sequences.sample_table
  SOURCES backend/pg/sequences/sample_table/index.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS sequences_sample_table_udf)
bioseq_register_sql_owner(
  OWNER_KEY sql_bootstrap
  MODULE bioseq.postgres.sql.bootstrap
  SOURCES backend/pg/sql/bootstrap.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY sql_catalog_views
  MODULE bioseq.postgres.sql.catalog_views
  SOURCES backend/pg/sql/catalog_views.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY sql_functions
  MODULE bioseq.postgres.sql.functions
  SOURCES backend/pg/sql/functions.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY external_relation_fmgr_export
  MODULE bioseq.postgres.external.relation
  SOURCES backend/pg/external/relation.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  HANDWRITTEN_FMGR_EXPORTS bioseq_external_handler)
bioseq_register_sql_owner(
  OWNER_KEY external_sql_catalog
  MODULE bioseq.postgres.external.sql_catalog
  SOURCES backend/pg/external/sql_catalog.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY storage_indexing_pg_chunked_storage
  MODULE bioseq.postgres.storage.indexing.chunked_storage.sql_entities
  SOURCES backend/pg/storage/indexing/chunked_storage/sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_indexing_pg_chunked_storage)
bioseq_register_sql_owner(
  OWNER_KEY storage_load_pg
  MODULE bioseq.postgres.storage.io
  SOURCES backend/pg/storage/io/index.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS storage_load_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_sequence_storage_manager_core
  MODULE bioseq.postgres.storage.manager.sequence_storage.sql_entities
  SOURCES backend/pg/storage/manager/sequence_storage_sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_manager_sequence_storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_sequence_storage_manager
  MODULE bioseq.postgres.storage.manager:sequence_storage
  SOURCES backend/pg/storage/manager/sequence_storage.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS storage_manager_sequence_storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY table
  MODULE bioseq.postgres.table
  SOURCES
    backend/pg/table/access_hooks.cppm
    backend/pg/table/access_method.cppm
    backend/pg/table/custom_scan/index.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS table_access_hooks)

# Compact-sequence implementation partitions remain ordinary target-owned
# modules. SQL annotations stay on the two public owner partitions below, so
# admitting these implementation-only units to the SQL registry would invent a
# second annotation authority for the same ABI.
file(GLOB _bioseq_compact_sequence_annotation_free_partitions
  CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/types/compact_sequence/packed_output_parts/*.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/types/compact_sequence/tree_ops_parts/*.cppm")
list(SORT _bioseq_compact_sequence_annotation_free_partitions)
set(_bioseq_compact_sequence_unique_annotation_free_partitions
  ${_bioseq_compact_sequence_annotation_free_partitions})
list(REMOVE_DUPLICATES
  _bioseq_compact_sequence_unique_annotation_free_partitions)
list(LENGTH _bioseq_compact_sequence_annotation_free_partitions
  _bioseq_compact_sequence_annotation_free_partition_count)
list(LENGTH _bioseq_compact_sequence_unique_annotation_free_partitions
  _bioseq_compact_sequence_unique_annotation_free_partition_count)
if(NOT _bioseq_compact_sequence_annotation_free_partition_count EQUAL
    _bioseq_compact_sequence_unique_annotation_free_partition_count)
  message(FATAL_ERROR
    "Compact-sequence annotation-free partition inventory contains duplicates")
endif()
if(NOT _bioseq_compact_sequence_annotation_free_partition_count EQUAL 8)
  message(FATAL_ERROR
    "Compact-sequence annotation-free partition inventory must contain "
    "exactly 8 sources; got "
    "${_bioseq_compact_sequence_annotation_free_partition_count}")
endif()
foreach(_bioseq_compact_sequence_annotation_free_partition IN LISTS
    _bioseq_compact_sequence_annotation_free_partitions)
  if(NOT EXISTS
      "${_bioseq_compact_sequence_annotation_free_partition}")
    message(FATAL_ERROR
      "Compact-sequence annotation-free partition does not exist: "
      "${_bioseq_compact_sequence_annotation_free_partition}")
  endif()
  file(READ "${_bioseq_compact_sequence_annotation_free_partition}"
    _bioseq_compact_sequence_annotation_free_partition_text)
  if(_bioseq_compact_sequence_annotation_free_partition_text MATCHES
      "bioseq_sql::")
    message(FATAL_ERROR
      "Compact-sequence implementation partition gained SQL authority and "
      "must be reviewed for explicit registry ownership: "
      "${_bioseq_compact_sequence_annotation_free_partition}")
  endif()
  set(_bioseq_compact_sequence_partition_target_owner_count 0)
  foreach(_bioseq_compact_sequence_target_source IN LISTS SRC_MOD_FILES)
    if(_bioseq_compact_sequence_target_source STREQUAL
        _bioseq_compact_sequence_annotation_free_partition)
      math(EXPR _bioseq_compact_sequence_partition_target_owner_count
        "${_bioseq_compact_sequence_partition_target_owner_count} + 1")
    endif()
  endforeach()
  if(NOT _bioseq_compact_sequence_partition_target_owner_count EQUAL 1)
    message(FATAL_ERROR
      "Compact-sequence annotation-free partition must remain target-owned "
      "exactly once: ${_bioseq_compact_sequence_annotation_free_partition} "
      "has ${_bioseq_compact_sequence_partition_target_owner_count} owners")
  endif()
endforeach()
unset(_bioseq_compact_sequence_target_source)
unset(_bioseq_compact_sequence_partition_target_owner_count)
unset(_bioseq_compact_sequence_annotation_free_partition_text)
unset(_bioseq_compact_sequence_annotation_free_partition)
unset(_bioseq_compact_sequence_unique_annotation_free_partition_count)
unset(_bioseq_compact_sequence_annotation_free_partition_count)
unset(_bioseq_compact_sequence_unique_annotation_free_partitions)
unset(_bioseq_compact_sequence_annotation_free_partitions)

bioseq_register_sql_owner(
  OWNER_KEY postgres_types_compact_sequence
  MODULE bioseq.postgres.types.compact_sequence
  SOURCES
    backend/pg/types/compact_sequence/array_string.cppm
    backend/pg/types/compact_sequence/concat_reverse.cppm
    backend/pg/types/compact_sequence/content.cppm
    backend/pg/types/compact_sequence/flat_abi.cppm
    backend/pg/types/compact_sequence/io.cppm
    backend/pg/types/compact_sequence/letter.cppm
    backend/pg/types/compact_sequence/packed_output.cppm
    backend/pg/types/compact_sequence/sql_catalog.cppm
    backend/pg/types/compact_sequence/subscript.cppm
    backend/pg/types/compact_sequence/tree_ops.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS postgres_types_compact_sequence)
bioseq_register_sql_owner(
  OWNER_KEY vector_types
  MODULE bioseq.postgres.vectorization.types
  SOURCES
    backend/pg/vectorization/types/aggregates.cppm
    backend/pg/vectorization/types/date.cppm
    backend/pg/vectorization/types/int.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS
    vector_types_integer
    vector_types_date
    vector_types_aggregates
    vector_comparison_registry
    vector_aggregate_registry
  FMGR_OWNER_MODULES
    vector_types_integer=bioseq.postgres.vectorization.types.integer
    vector_types_date=bioseq.postgres.vectorization.types.date
    vector_types_aggregates=bioseq.postgres.vectorization.types.aggregates)

set(BIOSEQ_SQL_REGISTRY_DIR
  "${BIOSEQ_SQL_CODEGEN_DIR}/registries")
bioseq_finalize_sql_owner_registry(
  OUTPUT_DIR "${BIOSEQ_SQL_REGISTRY_DIR}"
  EXPECTED_ROW_CODECS
    storage_metadata
    storage_indexing_pg
    storage_load_pg
    storage_manager
    table
  EXPECTED_ENTITY_HANDLES
    storage_metadata_core
    storage_indexing_pg_chunked_storage
    storage_manager_sequence_storage_manager
  EXPECTED_FMGR_ENTRYPOINTS
    mod
    highway_target_test_support
    highway_ksw_target_test_support
    storage_manager_sequence_storage_manager
    table_access_hooks
    storage_load_pg
    sequences_sample_table_udf
    postgres_types_compact_sequence
    vector_types_integer
    vector_types_date
    vector_types_aggregates
    vector_comparison_registry
    vector_aggregate_registry
  EXPECTED_GUC_OWNERS
    bioseq
    scheduler
    storage
    aligner
  EXPECTED_ANNOTATION_FREE_RESULT_ROWS
    sequences_sample_table_udf
  EXPECTED_HANDWRITTEN_FMGR_EXPORTS
    bioseq_external_handler)

get_property(_bioseq_sql_entity_handle_owner_records GLOBAL
  PROPERTY BIOSEQ_SQL_BIOSEQ_ENTITY_HANDLE_OWNER_RECORDS)
list(SORT _bioseq_sql_entity_handle_owner_records)
set(_bioseq_sql_entity_handle_owner_args)
foreach(_bioseq_sql_entity_handle_owner_record IN LISTS
    _bioseq_sql_entity_handle_owner_records)
  list(APPEND _bioseq_sql_entity_handle_owner_args
    --entity-handle-owner-module
    "${_bioseq_sql_entity_handle_owner_record}")
endforeach()

# Clang collection never consumes a GCC BMI or the main target's compiler
# cache.  Each authority/profile prepare command derives a normalized,
# profile-checked Clang database from CMake's raw database and populates only
# its content-keyed PCM root.  SUPPORT and CORE can therefore bootstrap a clean
# tree before FINAL owners import their generated modules.
set(BIOSEQ_SQL_RAW_COMPILE_DATABASE
  "${CMAKE_BINARY_DIR}/compile_commands.json"
  CACHE FILEPATH
  "Raw CMake compile database used to prepare owned SQL Clang profiles")
mark_as_advanced(BIOSEQ_SQL_RAW_COMPILE_DATABASE)
set(BIOSEQ_SQL_CLANG_CONTEXT_ROOT
  "${BIOSEQ_SQL_CODEGEN_DIR}/clang")
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}")

foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_support_registry
    "${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_SUPPORT_FILE}")
  set(_bioseq_sql_support_sources
    ${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_SUPPORT_SOURCES})
  set(_bioseq_sql_pcm_root
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/pcm")
  set(_bioseq_sql_support_compile_database
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/compile_commands.json")
  set(_bioseq_sql_support_prepare_stamp
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/prepare.stamp")
  set(_bioseq_sql_support_prepare_depfile
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/prepare.d")
  set(_bioseq_sql_support_pcm_manifest
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/pcm-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_PCM_ROOT
    "${_bioseq_sql_pcm_root}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_COMPILE_DATABASE
    "${_bioseq_sql_support_compile_database}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_PREPARE_STAMP
    "${_bioseq_sql_support_prepare_stamp}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_PCM_MANIFEST
    "${_bioseq_sql_support_pcm_manifest}")

  add_custom_command(
    OUTPUT "${_bioseq_sql_support_prepare_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_support_compile_database}"
      "${_bioseq_sql_support_pcm_manifest}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${_bioseq_sql_pcm_root}"
    COMMAND
      "$<TARGET_FILE:bioseq_codegen>" sql prepare
      --authority support
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_support_registry}"
      --raw-compile-database "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --compile-database-output
        "${_bioseq_sql_support_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_support_pcm_manifest}"
      --depfile "${_bioseq_sql_support_prepare_depfile}"
      --stamp "${_bioseq_sql_support_prepare_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
      "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      "${_bioseq_sql_support_registry}"
      "${BIOSEQ_SQL_OWNER_REGISTRY_METADATA}"
      ${_bioseq_sql_support_sources}
    COMMENT
      "Preparing ${_bioseq_sql_profile} Clang SQL support closure"
    DEPFILE "${_bioseq_sql_support_prepare_depfile}"
    VERBATIM)
  add_custom_target(
    bioseq_sql_prepare_support_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_support_prepare_stamp}")

  set(_bioseq_sql_row_codec_modules
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODEC_MODULES})
  get_property(_bioseq_sql_row_codec_owner_records GLOBAL
    PROPERTY
      "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_ROW_CODEC_OWNER_RECORDS")
  list(SORT _bioseq_sql_row_codec_owner_records)
  set(_bioseq_sql_row_codec_owner_args)
  foreach(_bioseq_sql_row_codec_owner_record IN LISTS
      _bioseq_sql_row_codec_owner_records)
    list(APPEND _bioseq_sql_row_codec_owner_args
      --row-codec-owner-module
      "${_bioseq_sql_row_codec_owner_record}")
  endforeach()
  set(_bioseq_sql_guc_modules
    ${BIOSEQ_SQL_GUC_${_bioseq_sql_profile}_MODULES})
  set(_bioseq_sql_support_vector_registry_modules
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES})
  set(_bioseq_sql_support_manifest
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST}")
  set(_bioseq_sql_support_stamp
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}")
  add_custom_command(
    OUTPUT "${_bioseq_sql_support_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_support_manifest}"
      ${_bioseq_sql_row_codec_modules}
      ${_bioseq_sql_guc_modules}
      ${_bioseq_sql_support_vector_registry_modules}
    COMMAND "${CMAKE_COMMAND}" -E env
      "BIOSEQ_REVIEWED_REPOSITORY_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/.."
      "$<TARGET_FILE:bioseq_codegen>" sql generate
      --stage support
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_support_registry}"
      --compile-database "${_bioseq_sql_support_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_support_pcm_manifest}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --manifest "${_bioseq_sql_support_manifest}"
      --stamp "${_bioseq_sql_support_stamp}"
      --row-codecs-output
        "storage_metadata=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA}"
      --row-codecs-output
        "storage_indexing_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
      --row-codecs-output
        "storage_load_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
      --row-codecs-output
        "storage_manager=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
      --row-codecs-output
        "table=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE}"
      ${_bioseq_sql_row_codec_owner_args}
      --fmgr-entrypoints-output
        "vector_comparison_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
      --fmgr-entrypoints-output
        "vector_aggregate_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}"
      --guc-output-root "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}"
      --guc-active-profile "${_bioseq_sql_profile}"
      --generated-module-prefix bioseq.postgres.sql.generated
      --extension-schema public
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${_bioseq_sql_support_prepare_stamp}"
      "${_bioseq_sql_support_compile_database}"
      "${_bioseq_sql_support_pcm_manifest}"
      "${_bioseq_sql_support_registry}"
      ${_bioseq_sql_support_sources}
    COMMENT
      "Generating ${_bioseq_sql_profile} SQL support modules"
    VERBATIM)
  add_custom_target(
    bioseq_sql_support_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_support_stamp}")
  add_dependencies(
    bioseq_sql_support_${_bioseq_sql_profile}
    bioseq_sql_prepare_support_${_bioseq_sql_profile})
endforeach()

set(BIOSEQ_SQL_CORE_PCM_ROOT
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/pcm")
set(BIOSEQ_SQL_CORE_COMPILE_DATABASE
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/compile_commands.json")
set(BIOSEQ_SQL_CORE_PREPARE_STAMP
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/prepare.stamp")
set(BIOSEQ_SQL_CORE_PREPARE_DEPFILE
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/prepare.d")
set(BIOSEQ_SQL_CORE_PCM_MANIFEST
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/pcm-manifest.json")
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_CORE_PREPARE_STAMP}"
  BYPRODUCTS
    "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_SQL_CORE_PCM_ROOT}"
  COMMAND
    "$<TARGET_FILE:bioseq_codegen>" sql prepare
    --authority core
    --profile production
    --source-list "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    --raw-compile-database "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    --compiler-module-inventory
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
    --clang-scan-deps-sha256
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
    --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
    --compiler-module-inventory-sha256
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
    --compile-database-output "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    --pcm-root "${BIOSEQ_SQL_CORE_PCM_ROOT}"
    --pcm-manifest "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
    --depfile "${BIOSEQ_SQL_CORE_PREPARE_DEPFILE}"
    --stamp "${BIOSEQ_SQL_CORE_PREPARE_STAMP}"
  DEPENDS
    bioseq_codegen_project
    "${BIOSEQ_CODEGEN_EXECUTABLE}"
    "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    "${BIOSEQ_SQL_OWNER_REGISTRY_METADATA}"
    ${BIOSEQ_SQL_REGISTRY_CORE_SOURCES}
  COMMENT "Preparing profile-neutral Clang SQL core closure"
  DEPFILE "${BIOSEQ_SQL_CORE_PREPARE_DEPFILE}"
  VERBATIM)
add_custom_target(bioseq_sql_prepare_core
  DEPENDS "${BIOSEQ_SQL_CORE_PREPARE_STAMP}")
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
  BYPRODUCTS
    "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    "${BIOSEQ_SQL_CORE_MANIFEST}"
    ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
  COMMAND "${CMAKE_COMMAND}" -E env
    "BIOSEQ_REVIEWED_REPOSITORY_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/.."
    "$<TARGET_FILE:bioseq_codegen>" sql generate
    --stage core
    --profile production
    --source-list "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    --compile-database "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    --pcm-root "${BIOSEQ_SQL_CORE_PCM_ROOT}"
    --pcm-manifest "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
    --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    --compiler-module-inventory
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
    --clang-scan-deps-sha256
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
    --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
    --compiler-module-inventory-sha256
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
    --core-hash-output "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    --core-manifest "${BIOSEQ_SQL_CORE_MANIFEST}"
    --stamp "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
    --entity-handles-output
      "storage_metadata_core=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE}"
    --entity-handles-output
      "storage_indexing_pg_chunked_storage=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED}"
    --entity-handles-output
      "storage_manager_sequence_storage_manager=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE}"
    ${_bioseq_sql_entity_handle_owner_args}
    --generated-module-prefix bioseq.postgres.sql.generated
    --extension-schema public
  DEPENDS
    bioseq_codegen_project
    "${BIOSEQ_CODEGEN_EXECUTABLE}"
    "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    "${BIOSEQ_SQL_CORE_PREPARE_STAMP}"
    "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
    "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    ${BIOSEQ_SQL_REGISTRY_CORE_SOURCES}
  COMMENT "Generating profile-neutral SQL entity handles"
  VERBATIM)
add_custom_target(bioseq_sql_core_entity_handles
  DEPENDS "${BIOSEQ_SQL_CORE_GENERATED_STAMP}")
add_dependencies(
  bioseq_sql_core_entity_handles
  bioseq_sql_prepare_core)

set(_bioseq_sql_final_stamps)
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_final_registry
    "${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_FINAL_FILE}")
  set(_bioseq_sql_final_sources
    ${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_FINAL_SOURCES})
  set(_bioseq_sql_pcm_root
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_PCM_ROOT}")
  set(_bioseq_sql_final_compile_database
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/compile_commands.json")
  set(_bioseq_sql_final_prepare_stamp
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/prepare.stamp")
  set(_bioseq_sql_final_prepare_depfile
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/prepare.d")
  set(_bioseq_sql_final_pcm_manifest
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/pcm-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_COMPILE_DATABASE
    "${_bioseq_sql_final_compile_database}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_PREPARE_STAMP
    "${_bioseq_sql_final_prepare_stamp}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_PCM_MANIFEST
    "${_bioseq_sql_final_pcm_manifest}")

  add_custom_command(
    OUTPUT "${_bioseq_sql_final_prepare_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_final_compile_database}"
      "${_bioseq_sql_final_pcm_manifest}"
    COMMAND
      "$<TARGET_FILE:bioseq_codegen>" sql prepare
      --authority final
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_final_registry}"
      --raw-compile-database "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --compile-database-output
        "${_bioseq_sql_final_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_final_pcm_manifest}"
      --depfile "${_bioseq_sql_final_prepare_depfile}"
      --stamp "${_bioseq_sql_final_prepare_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
      "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      "${_bioseq_sql_final_registry}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}"
      ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES}
      "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
      ${_bioseq_sql_final_sources}
    COMMENT
      "Preparing ${_bioseq_sql_profile} Clang SQL final closure"
    DEPFILE "${_bioseq_sql_final_prepare_depfile}"
    VERBATIM)
  add_custom_target(
    bioseq_sql_prepare_final_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_final_prepare_stamp}")
  add_dependencies(
    bioseq_sql_prepare_final_${_bioseq_sql_profile}
    bioseq_sql_support_${_bioseq_sql_profile}
    bioseq_sql_core_entity_handles)

  set(_bioseq_sql_generated_install
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL}")
  set(_bioseq_sql_generated_manifest
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}")
  set(_bioseq_sql_generated_stamp
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP}")
  set(_bioseq_sql_fmgr_modules
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_FMGR_ENTRYPOINT_MODULES})
  get_property(_bioseq_sql_row_codec_owner_records GLOBAL
    PROPERTY
      "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_ROW_CODEC_OWNER_RECORDS")
  list(SORT _bioseq_sql_row_codec_owner_records)
  set(_bioseq_sql_row_codec_owner_args)
  foreach(_bioseq_sql_row_codec_owner_record IN LISTS
      _bioseq_sql_row_codec_owner_records)
    list(APPEND _bioseq_sql_row_codec_owner_args
      --row-codec-owner-module
      "${_bioseq_sql_row_codec_owner_record}")
  endforeach()
  set(_bioseq_sql_profile_only_fmgr_args)
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    list(APPEND _bioseq_sql_profile_only_fmgr_args
      --fmgr-entrypoints-output
      "highway_ksw_target_test_support=${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT}")
  endif()
  get_property(_bioseq_sql_fmgr_owner_records GLOBAL
    PROPERTY
      "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_FMGR_OWNER_RECORDS")
  list(SORT _bioseq_sql_fmgr_owner_records)
  set(_bioseq_sql_fmgr_owner_args)
  foreach(_bioseq_sql_fmgr_owner_record IN LISTS
      _bioseq_sql_fmgr_owner_records)
    list(APPEND _bioseq_sql_fmgr_owner_args
      --fmgr-owner-module "${_bioseq_sql_fmgr_owner_record}")
  endforeach()

  add_custom_command(
    OUTPUT "${_bioseq_sql_generated_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_generated_install}"
      "${_bioseq_sql_generated_manifest}"
      ${_bioseq_sql_fmgr_modules}
    COMMAND "${CMAKE_COMMAND}" -E env
      "BIOSEQ_REVIEWED_REPOSITORY_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/.."
      "$<TARGET_FILE:bioseq_codegen>" sql generate
      --stage final
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_final_registry}"
      --compile-database "${_bioseq_sql_final_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_final_pcm_manifest}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --output "${_bioseq_sql_generated_install}"
      --manifest "${_bioseq_sql_generated_manifest}"
      --stamp "${_bioseq_sql_generated_stamp}"
      --support-manifest
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST}"
      --support-seal
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}"
      --production-support-manifest
        "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
      --production-support-seal
        "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
      --test-indexes-support-manifest
        "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_MANIFEST}"
      --test-indexes-support-seal
        "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}"
      --expected-core-hash "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
      --core-manifest "${BIOSEQ_SQL_CORE_MANIFEST}"
      --entity-handles-output
        "storage_metadata_core=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE}"
      --entity-handles-output
        "storage_indexing_pg_chunked_storage=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED}"
      --entity-handles-output
        "storage_manager_sequence_storage_manager=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE}"
      ${_bioseq_sql_entity_handle_owner_args}
      --row-codecs-output
        "storage_metadata=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA}"
      --row-codecs-output
        "storage_indexing_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
      --row-codecs-output
        "storage_load_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
      --row-codecs-output
        "storage_manager=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
      --row-codecs-output
        "table=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE}"
      ${_bioseq_sql_row_codec_owner_args}
      --fmgr-entrypoints-output
        "mod=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD}"
      --fmgr-entrypoints-output
        "highway_target_test_support=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT}"
      --fmgr-entrypoints-output
        "storage_manager_sequence_storage_manager=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER}"
      --fmgr-entrypoints-output
        "table_access_hooks=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS}"
      --fmgr-entrypoints-output
        "storage_load_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG}"
      --fmgr-entrypoints-output
        "sequences_sample_table_udf=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF}"
      --fmgr-entrypoints-output
        "postgres_types_compact_sequence=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
      --fmgr-entrypoints-output
        "vector_types_integer=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER}"
      --fmgr-entrypoints-output
        "vector_types_date=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE}"
      --fmgr-entrypoints-output
        "vector_types_aggregates=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES}"
      --fmgr-entrypoints-output
        "vector_comparison_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
      --fmgr-entrypoints-output
        "vector_aggregate_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}"
      ${_bioseq_sql_profile_only_fmgr_args}
      ${_bioseq_sql_fmgr_owner_args}
      --guc-output-root "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}"
      --guc-active-profile "${_bioseq_sql_profile}"
      ${_bioseq_sql_completion_args}
      --generated-module-prefix bioseq.postgres.sql.generated
      --extension-schema public
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${_bioseq_sql_final_prepare_stamp}"
      "${_bioseq_sql_final_compile_database}"
      "${_bioseq_sql_final_pcm_manifest}"
      "${_bioseq_sql_final_registry}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST}"
      "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
      "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
      "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}"
      "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_MANIFEST}"
      "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
      "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
      "${BIOSEQ_SQL_CORE_MANIFEST}"
      ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
      ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODEC_MODULES}
      ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES}
      ${BIOSEQ_SQL_GUC_production_MODULES}
      ${BIOSEQ_SQL_GUC_test_indexes_MODULES}
      "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}"
      ${_bioseq_sql_final_sources}
    COMMENT
      "Generating ${_bioseq_sql_profile} PostgreSQL extension SQL"
    VERBATIM)
  add_custom_target(
    bioseq_sql_final_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_generated_stamp}")
  add_dependencies(
    bioseq_sql_final_${_bioseq_sql_profile}
    bioseq_sql_prepare_final_${_bioseq_sql_profile})
  list(APPEND _bioseq_sql_final_stamps
    "${_bioseq_sql_generated_stamp}")
endforeach()

add_custom_target(bioseq_sql_final
  DEPENDS ${_bioseq_sql_final_stamps})
add_dependencies(
  bioseq_sql_final
  bioseq_sql_final_production
  bioseq_sql_final_test_indexes)
