# The SQL compile-contract provider owns the smallest reusable PostgreSQL
# module closure needed by generated catalog handles, row codecs, and GUC
# facets. Both the extension and the dedicated contract OBJECT target import
# these PUBLIC BMIs; the contract objects themselves never enter the DSO.
set(BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/configuration/aligner.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/configuration/core.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/configuration/scheduler.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/configuration/storage.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/column/metadata/sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/datums.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/runtime_diagnostics.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/sql_catalog_entities.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/sql_boundary/aggregate.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/sql_boundary/guc_storage.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/sql_boundary/index.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/sql_boundary/row_codec.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/support/sql_boundary/vector.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/io/sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/manager/sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/sequences/sample_table/sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/table/sql_rows.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/vectorization/vector_types.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS})

# Compiler-owned SQL module-reachability fixtures. They are removed from
# the production source globs here and compiled only by the dedicated OBJECT
# target declared after generated provider paths are known.
set(BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/catalog_entity_handle_producer_test.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/generated_guc_producer_test.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/sql_boundary_template_producer_test.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/result_materialization_test.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/sql_entity_handles_test.cppm")
set(BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/catalog_entity_handle_consumer_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/generated_guc_consumer_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/sql_boundary_template_consumer_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/postgres/sql/active_generated_guc_consumer_test.cpp")
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})
list(REMOVE_ITEM SRC_FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})
list(REMOVE_DUPLICATES SRC_MOD_FILES)
list(REMOVE_DUPLICATES SRC_FILES)

# External CustomScan (backend/pg/external/scan.cppm), its row writer
# (row_writer.cppm), and provider-error translation (errors.cppm) import the
# external provider stack, so they compile into the extension only when the
# external API clients are enabled. The provider-free relation and path-builder
# modules stay unconditional. The option is declared here (before
# SRC_MOD_FILES is consumed by the postgresql_bioseq target); the later
# add_subdirectory(external) reuses the same cached value.
option(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
  "Configure generated external API clients and their integration tests" OFF)
set(BIOSEQ_EXTERNAL_API_OPTIONAL_PG_MODULES
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/external/errors.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/external/row_writer.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/external/scan.cppm")
if(NOT BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  foreach(_bioseq_external_api_optional_pg_module IN LISTS
      BIOSEQ_EXTERNAL_API_OPTIONAL_PG_MODULES)
    if(NOT _bioseq_external_api_optional_pg_module IN_LIST SRC_MOD_FILES)
      message(FATAL_ERROR
        "External API optional PostgreSQL module is absent from the authored "
        "inventory: ${_bioseq_external_api_optional_pg_module}")
    endif()
  endforeach()
  list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_EXTERNAL_API_OPTIONAL_PG_MODULES})
endif()
unset(_bioseq_external_api_optional_pg_module)
unset(BIOSEQ_EXTERNAL_API_OPTIONAL_PG_MODULES)

# GCC heavy dispatch owners inherit the production preset's -O3 codegen.
# Scheduling and forced-template-inlining controls bound compiler resources
# without lowering optimization or changing runtime dispatch.
set(BIOSEQ_GCC_TYPED_DISPATCH_JOBS "1" CACHE STRING
  "Ninja compile-pool width for GCC 16.1 AWS typed dispatch module units. \
0 = inherit the target compile pool.")
if(NOT BIOSEQ_GCC_TYPED_DISPATCH_JOBS MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "BIOSEQ_GCC_TYPED_DISPATCH_JOBS must be a non-negative integer; got "
    "'${BIOSEQ_GCC_TYPED_DISPATCH_JOBS}'")
endif()

set(BIOSEQ_GCC_TEMPLATE_FORCE_INLINE "ON" CACHE BOOL
  "Keep forced always_inline/flatten attributes in template dispatch modules \
for the GCC 16.1 AWS preset. OFF preserves algorithm selection but lets GCC \
avoid constructing very large inlining graphs while compiling typed dispatch.")

# Materialize only the profile-enabled scheduler coordinates. The canonical
# body remains an unattached raw-token input; dependency BMIs use the authored
# primary as their compilation-signature donor.
set(_bioseq_typed_sql_support_profile production)
if(BIOSEQ_TEST_INDEXES)
  set(_bioseq_typed_sql_support_profile test_indexes)
endif()
set(BIOSEQ_TYPED_SQL_SUPPORT_STAMP
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/profiles/${_bioseq_typed_sql_support_profile}/support.stamp")
unset(_bioseq_typed_sql_support_profile)
bioseq_add_template_flatten_codegen(
  NAME dispatch_processor_typed
  SOURCE "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.postgres.scheduler.dispatch_processor
  EMISSION_POLICY selector_api
  CONFIGURE_TOPOLOGY_MANIFEST "${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  EXPECTED_TOPOLOGY_DIGEST "${BIOSEQ_TYPED_TOPOLOGY_DIGEST}"
  ENABLED_ORDINALS ${BIOSEQ_TYPED_ENABLED_ORDINALS}
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_processor/index.cppm"
  PCM_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}/pcm"
  PREPARE_DEPENDS "${BIOSEQ_TYPED_SQL_SUPPORT_STAMP}"
  OUT_API_PARTITION BIOSEQ_TYPED_API_PARTITION
  OUT_SELECTOR_PARTITION BIOSEQ_TYPED_SELECTOR_PARTITION
  OUT_IMPLEMENTATION_UNITS BIOSEQ_TYPED_IMPLEMENTATION_UNITS
  OUT_GENERATION_TARGET BIOSEQ_TYPED_GENERATION_TARGET
  OUT_ARTIFACT_MANIFEST BIOSEQ_TYPED_ARTIFACT_MANIFEST)

if(NOT "${BIOSEQ_TYPED_PROBED_ENABLED_SOURCES}" STREQUAL
    "${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}")
  message(FATAL_ERROR
    "Template-flatten probe and generator disagree on enabled scheduler units")
endif()
set(_bioseq_typed_expected_count
  "${BIOSEQ_BUILD_CONFIG_SCHEDULER_UNIT_COUNT}")
list(LENGTH BIOSEQ_TYPED_IMPLEMENTATION_UNITS
  BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT)
if(NOT BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT EQUAL
    _bioseq_typed_expected_count)
  message(FATAL_ERROR
    "Template-flatten profile ${BIOSEQ_TEMPLATE_FLATTEN_PROFILE} requires "
    "${_bioseq_typed_expected_count} scheduler units, generated "
    "${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT}")
endif()
unset(_bioseq_typed_expected_count)

set(BIOSEQ_ALIGNMENT_HOOK_PRIVATE_PARTITIONS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_contract.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_core_state.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_core_lookup.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_core_unlink.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_core_retirement.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_core_stack.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_binding.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_run.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_callbacks.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_hook_publish.cppm")

set(BIOSEQ_SUPERVISOR_PRIVATE_PARTITIONS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/persist_queue_publish.cppm")

set(BIOSEQ_DISPATCH_BUFFER_IMPLEMENTATION_PARTITIONS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/configuration.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/join_preparation.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/hydration.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/join_execution.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/scan_execution.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/output.cppm")

set(BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/lifecycle.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_operation_stats.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/alignment_stats.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/index_admin.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/minimizer_diagnostics.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/scheduler_diagnostics.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/sequence_source_contract.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/sql_frontdoor.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_processor/implementation.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_processor/source_provider_promotion_implementation.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_buffer/api_implementation.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/runtime.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/highway_binding.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/persist.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/persist_stage.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/persist_stats.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/persist_worker.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/shader_runtime.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/direct_minimizer_materialization.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/io/index_maintenance_implementation.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/admin/index_build_runtime_implementation.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/admin/rebuild/generation_route_implementation.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/external/standard_library_link_support.cppm")
list(APPEND BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
  ${BIOSEQ_DISPATCH_BUFFER_IMPLEMENTATION_PARTITIONS}
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/sorted_runs/postgres_error.cppm")
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  if(_bioseq_pipeline_dispatch_owner IN_LIST
      BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS)
    message(FATAL_ERROR
      "Pipeline owner leaked into the PostgreSQL ordinary-unit registry: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owner)
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES}
  ${BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS})
list(APPEND SRC_MOD_FILES
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}")
list(REMOVE_DUPLICATES SRC_MOD_FILES)
list(APPEND SRC_FILES
  ${BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS}
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS})
foreach(_bioseq_alignment_hook_partition
    IN LISTS BIOSEQ_ALIGNMENT_HOOK_PRIVATE_PARTITIONS)
  set(_bioseq_alignment_hook_module_count 0)
  foreach(_bioseq_module_source IN LISTS SRC_MOD_FILES)
    if(_bioseq_module_source STREQUAL _bioseq_alignment_hook_partition)
      math(EXPR _bioseq_alignment_hook_module_count
        "${_bioseq_alignment_hook_module_count} + 1")
    endif()
  endforeach()
  if(NOT _bioseq_alignment_hook_module_count EQUAL 1)
    message(FATAL_ERROR
      "Alignment-hook private partition must occur exactly once in "
      "SRC_MOD_FILES: '${_bioseq_alignment_hook_partition}' occurs "
      "${_bioseq_alignment_hook_module_count} times")
  endif()
  if(_bioseq_alignment_hook_partition IN_LIST SRC_FILES
      OR _bioseq_alignment_hook_partition IN_LIST
        BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
      OR _bioseq_alignment_hook_partition IN_LIST
        BIOSEQ_TYPED_IMPLEMENTATION_UNITS
      OR _bioseq_alignment_hook_partition IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_MODULES)
    message(FATAL_ERROR
      "Alignment-hook private partition must be target-owned only through "
      "the CXX_MODULES file set: '${_bioseq_alignment_hook_partition}'")
  endif()
endforeach()
unset(_bioseq_alignment_hook_module_count)
unset(_bioseq_module_source)
unset(_bioseq_alignment_hook_partition)
foreach(_bioseq_supervisor_partition
    IN LISTS BIOSEQ_SUPERVISOR_PRIVATE_PARTITIONS)
  set(_bioseq_supervisor_module_count 0)
  foreach(_bioseq_module_source IN LISTS SRC_MOD_FILES)
    if(_bioseq_module_source STREQUAL _bioseq_supervisor_partition)
      math(EXPR _bioseq_supervisor_module_count
        "${_bioseq_supervisor_module_count} + 1")
    endif()
  endforeach()
  if(NOT _bioseq_supervisor_module_count EQUAL 1)
    message(FATAL_ERROR
      "Supervisor private partition must occur exactly once in "
      "SRC_MOD_FILES: '${_bioseq_supervisor_partition}' occurs "
      "${_bioseq_supervisor_module_count} times")
  endif()
  if(_bioseq_supervisor_partition IN_LIST SRC_FILES
      OR _bioseq_supervisor_partition IN_LIST
        BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
      OR _bioseq_supervisor_partition IN_LIST
        BIOSEQ_TYPED_IMPLEMENTATION_UNITS
      OR _bioseq_supervisor_partition IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_MODULES)
    message(FATAL_ERROR
      "Supervisor private partition must be target-owned only through "
      "the CXX_MODULES file set: '${_bioseq_supervisor_partition}'")
  endif()
endforeach()
unset(_bioseq_supervisor_module_count)
unset(_bioseq_module_source)
unset(_bioseq_supervisor_partition)
set_source_files_properties(
  ${BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS}
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}"
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/external/standard_library_link_support.cppm"
  PROPERTIES CXX_SCAN_FOR_MODULES OFF)
set_source_files_properties(
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}"
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
  PROPERTIES GENERATED TRUE)
message(STATUS
  "bioseq: enabled ${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT} "
  "typed dispatch processor module implementation units")

if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  set(BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS
    ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
    ${BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS})
  if(NOT BIOSEQ_GCC_TEMPLATE_FORCE_INLINE)
    set_property(SOURCE ${BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS}
      APPEND PROPERTY COMPILE_DEFINITIONS
        BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE=0)
  endif()
  if(BIOSEQ_GCC_TYPED_DISPATCH_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
      gcc_typed_dispatch_compile=${BIOSEQ_GCC_TYPED_DISPATCH_JOBS})
    set_source_files_properties(
      ${BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS}
      PROPERTIES JOB_POOL_COMPILE gcc_typed_dispatch_compile)
  endif()
  message(STATUS
    "bioseq: GCC typed dispatch module units use preset -O3, compile jobs="
    "${BIOSEQ_GCC_TYPED_DISPATCH_JOBS}, template force-inline="
    "${BIOSEQ_GCC_TEMPLATE_FORCE_INLINE}")
  unset(BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS)
endif()
