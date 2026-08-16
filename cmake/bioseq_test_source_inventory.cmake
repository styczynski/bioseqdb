find_package(Catch2 3 REQUIRED)

# Bench-prefilter correctness metrics are also unit-tested. Keep their named
# module in one reusable owner instead of recompiling it in the benchmark and
# unit-test targets.
add_library(bioseq_bench_prefilter_metrics STATIC)
set_target_properties(bioseq_bench_prefilter_metrics PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_bench_prefilter_metrics
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../bench/prefilter_pipeline_metrics.cppm")

# Indexing storage fakes and the libpq integration client each have one reusable
# test-support owner. Test executables link these targets instead of recompiling
# the same providers in every consumer.
bioseq_collect_authored_sources(
  BIOSEQ_UNIT_TEST_SUPPORT_MODULES
  BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests"
  RECURSE
  INCLUDE_PATTERNS
    "support/*.cppm"
    "unit/*.cppm"
  EXPECTED_COUNT 12)
add_library(bioseq_testing_support OBJECT)
set_target_properties(bioseq_testing_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_testing_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${BIOSEQ_UNIT_TEST_SUPPORT_MODULES})
unset(BIOSEQ_UNIT_TEST_SUPPORT_MODULES)
target_link_libraries(bioseq_testing_support
  PUBLIC
    bioseq_modules_nopg
    bioseq_highway_test_support
    libassert::assert
  PRIVATE
    Catch2::Catch2)

# Safety-runtime harness modules form a separate executable/runtime closure.
# Keep those six exact providers out of this reusable libpq/Catch support owner.
bioseq_collect_authored_sources(
  BIOSEQ_INTEGRATION_TEST_SUPPORT_MODULES
  BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support"
  INCLUDE_PATTERNS "*.cppm"
  EXCLUDE_PATHS
    "safety_runtime_control.cppm"
    "safety_runtime_harness_case_support.cppm"
    "safety_runtime_harness_core_support.cppm"
    "safety_runtime_harness_manifest_support.cppm"
    "safety_runtime_harness_scenario_support.cppm"
    "safety_runtime_harness_support.cppm"
  EXPECTED_COUNT 8)
add_library(bioseq_integration_test_support STATIC)
set_target_properties(bioseq_integration_test_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_integration_test_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${BIOSEQ_INTEGRATION_TEST_SUPPORT_MODULES})
unset(BIOSEQ_INTEGRATION_TEST_SUPPORT_MODULES)
target_include_directories(bioseq_integration_test_support PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/include")
target_link_libraries(bioseq_integration_test_support
  PUBLIC
    fmt
    libassert::assert
    PostgreSQL::PostgreSQL
  PRIVATE
    Catch2::Catch2)

# Catch source ownership and order stay explicit because object/link order fixes
# the existing Catch registration order. The repository-level ownership audit
# discovers *_test.cpp only to reject omissions or duplicate owners; discovery
# never feeds these target source lists.
# The event listener is a unit-target-private ordinary source, not a test case.
set(BIOSEQ_UNIT_TEST_SUPPORT_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/test_event_listener.cpp")
set(BIOSEQ_UNIT_TEST_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/alignment/block_aligner_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/alignment/block_aligner/traceback_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/alignment/block_aligner/routing_stats_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/alignment/edit_distance_accounting_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/chaining/simd_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/chaining/vulkan_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/common/logging_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/gpu/vulkan_shader_cache_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/gpu/vulkan_shader_cache/sealed_seed_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/gpu/vulkan_shader_cache/archive_cache_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/gpu/vulkan_shader_cache/runtime_receipts_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/boundary_parity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/compact_hash32_direct_index_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/compact_hash32_direct_index/read_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/compact_hash32_direct_index/write_planning_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/gpu_minimizer_index_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/gpu_minimizer_index/resident_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/gpu_minimizer_index/source_direct_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/index_chunk_io_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/merge_runs_multicursor_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_fragment_compact_hash32_adapter_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/hash_major_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/generation_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/fragment_producer_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/fragment_producer/replay_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/grouped_soa_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/grouped_soa/reader_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/anchor_planner/01_contracts_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/reader_codec/02_grouped_reader_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/anchor_planner/03_query_waves_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/reader_codec/04_matcher_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/anchor_planner/05_occurrence_capacity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/reader_codec/06_page_advice_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/anchor_planner/07_query_identity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/reader_codec/08_direct_execution_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/anchor_planner/09_admission_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct/reader_codec/10_payload_corruption_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_sequence_index_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/persisted_minimizer_image_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/persisted_minimizer_image/layout_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/persisted_minimizer_image/validation_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/storage_concept_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/symbol_counts_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/io/file_kind_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/io/identifier_parser/formats_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/io/identifier_parser/identifier_parser_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/highway_minimizer_sink_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/kmers_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/lazy_minimizer_parity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/minimizer_collector_admission_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/nt4_minimizer_parity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/two_stack_exact_checked_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/two_stack_exact_continuation_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/two_stack_exact_output_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/two_stack_minimizers_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_memory_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_batch_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_parity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_index_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_upload_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_segment_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/parallelism/pool_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/error_admission_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/statistics_receipts_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/statistics_receipts/execution_receipts_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/statistics_receipts/aggregation_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/plan_geometry_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/plan_geometry/dispatch_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/scalar_highway_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/segmented_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/pipeline/pipe/vulkan_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/prefilter/gatekeeper_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/prefilter/gatekeeper_vulkan_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/prefilter/sneaky_snake_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/seeding/vulkan_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/sequences/edit_distance_threshold_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/sequences/packed_sequence_split_units_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/sequences/packed_sequence_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/sequences/rank_pack_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/sequences/sequence_generator_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/simd/highway_dispatch_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/simd/highway_fixed_target_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/simd/sorting_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/simd/vector_stream_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/storage/batch_sort_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/storage/layout_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/storage/lazy_aligner_parity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/storage/lazy_pipeline_parity_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/storage/lazy_sequence_test.cpp")

set(BIOSEQ_INTEGRATION_TEST_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/admin_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/btree_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/chunked_storage_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/e2e_smoke_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/explain_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/explain_highway_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/explain_materialization_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/explain_runtime_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/gpu_mmsearch_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/index_admin_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/index_admin_generation_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/index_admin_hash_major_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/index_admin_update_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/index_invariance_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/index_mixed_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/index_table_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/insert_hook_multifragment_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/io/reader_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/join_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/join_transformed_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/join_epq_aggregate_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/large_table_alignment_smoke_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lazy_sequence_expression_property_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lazy_sequence_slicing_acceptance_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lazy_sequence_concat_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lazy_sequence_comparison_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lazy_sequence_storage_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lazy_sequence_lifetime_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lazy_sequence_slicing_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/load_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minimizers_per_sequence_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mmsearch_base_statistics_planner_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mmsearch_persisted_index_routes_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mmsearch_slice_transformed_ownership_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mmsearch_edit_options_policy_vulkan_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mmsearch_async_index_policy_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mmsearch_aligner_options_backend_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mmsearch_projection_helpers_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/reindex_scanner_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/row_mask_write_state_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sample_table_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/scalar_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sequence_basic_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sequence_fragment_storage_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sequence_typmod_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/source_domain_lifetime_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/source_domain_transaction_lifetime_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/source_domain_parallel_policy_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sql_boundary_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sql_boundary_maintenance_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sql_boundary_bpchar_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sql_boundary_generated_registry_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sql_row_codec_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/storage_buckets_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/substring_property_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/symbol_count_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/typmod_coalesce_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/wire_test.cpp")

if(NOT BIOSEQ_TEST_INDEXES)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/source_domain_transaction_lifetime_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/source_domain_parallel_policy_test.cpp"
    PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

list(REMOVE_ITEM SRC_FILES ${BIOSEQ_UNIT_TEST_SOURCES})
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_UNIT_TEST_SOURCES})

# Remove modules owned by the shared bioseq_modules_nopg OBJECT library from
# postgresql_bioseq's private module set. The `SHARED_MODULE_LIBS
# bioseq_modules_nopg` argument below supplies the PUBLIC BMIs, so the
# domain-neutral module set has one build owner.
#
# The shared BMIs are compiled with private NO_POSTGRES=1. The domain-neutral
# modules that branch on NO_POSTGRES therefore use their standalone contracts:
#
#   - sequences/simd_implementation.cppm  ::stdc_compare contract-violation paths use
#     DEBUG_ASSERT. Well-formed callers exclude mixed-alphabet values and
#     cross-storage-width non-bit-castable pairs.
#   - common/mlogging.cppm remains PG-header-free. PG-target logging is
#     delivered through the sink registered by _PG_init(); without a registered
#     sink it falls back to BIOSEQ_LOG_FILE or
#     /tmp/bioseq_loglog.log.
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_NOPG_MODULES})
