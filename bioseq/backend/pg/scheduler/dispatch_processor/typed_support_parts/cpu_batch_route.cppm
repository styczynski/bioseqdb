/**
 * @file typed_support_parts/cpu_batch_route.cppm
 * @brief Chooses CPU batch execution with a stored index or ordinary row processing.
 */
module;

#include <cstddef>
#include <cstdint>
#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.cpu_batch_route;

import std;
import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.persisted_native_batch;
import bioseq.postgres.types.compact_sequence;
import bioseq.parallelism.pool;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Runs a CPU batch, using parallel queries over a shared stored index when
 * possible. The row callback handles batches that cannot use that path.
 */
template <class p_sequence,
          class p_scanner_row,
          class p_inmemory_row,
          class p_materialize_target,
          class p_same_target_locator,
          class p_hash_major_identity_valid,
          class p_fragment_identity_valid,
          class p_with_pg_target,
          class p_attach_query_stats,
          class p_process_generic_rows>
[[nodiscard]] auto process_cpu_batch_route(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    pipe::statement_execution_plan_owner execution_plan,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    const std::size_t facade_letters_per_unit,
    const bool per_row_query_mode,
    const bool record_target_operational_outcomes,
    const bool chunked_scanner_forced,
    const bool use_scanner_pg,
    const bool persisted_required,
    bool& cached_query_minimizer_stats_accounted,
    p_scanner_row& scanner_row,
    p_inmemory_row& inmemory_row,
    const p_materialize_target& materialize_target,
    const p_same_target_locator& same_target_locator,
    const p_hash_major_identity_valid& hash_major_identity_valid,
    const p_fragment_identity_valid& fragment_identity_valid,
    const p_with_pg_target& with_pg_target,
    const p_attach_query_stats& attach_query_stats,
    const p_process_generic_rows& process_generic_rows_callback)
    -> match_result_map {
    match_result_map results;
    bool shared_target_outcome_recorded = false;
    std::shared_ptr<const std::vector<minimap_sketch_data>> cached_query_minimizers;
    std::optional<pipe::minimizer_range_execution_stats> cached_query_minimizer_stats;
    auto& pool = bioseq::parallelism::global_pool::instance();
    /*
     * Shared-target queries can run in parallel after the PostgreSQL thread checks
     * their identities and copies the target into memory. Workers read that copy
     * and the prepared index without calling PostgreSQL. Ineligible batches and
     * targets that cannot reserve memory use generic_rows. Inner alignment tasks
     * stay serial while the query batch runs in parallel.
     */
    auto generic_rows = [&batch_size,
                         &alphabet_code,
                         &storage_unit_byte_width,
                         &query_hdr,
                         &per_row_query_hdrs,
                         &target_hdrs,
                         &query_ids,
                         execution_plan,
                         &broadcast_query_ordinal,
                         &stats,
                         &scanner_pg_ctx,
                         &per_row_query_mode,
                         &record_target_operational_outcomes,
                         &shared_target_outcome_recorded,
                         &chunked_scanner_forced,
                         &use_scanner_pg,
                         &cached_query_minimizers,
                         &cached_query_minimizer_stats,
                         &cached_query_minimizer_stats_accounted,
                         &pool,
                         &scanner_row,
                         &inmemory_row,
                         &process_generic_rows_callback,
                         &results] {
        process_generic_rows_callback(batch_size,
                                      alphabet_code,
                                      storage_unit_byte_width,
                                      query_hdr,
                                      per_row_query_hdrs,
                                      target_hdrs,
                                      query_ids,
                                      execution_plan,
                                      broadcast_query_ordinal,
                                      stats,
                                      scanner_pg_ctx,
                                      per_row_query_mode,
                                      record_target_operational_outcomes,
                                      shared_target_outcome_recorded,
                                      chunked_scanner_forced,
                                      use_scanner_pg,
                                      cached_query_minimizers,
                                      cached_query_minimizer_stats,
                                      cached_query_minimizer_stats_accounted,
                                      pool,
                                      scanner_row,
                                      inmemory_row,
                                      results);
    };
    process_persisted_native_batch<p_sequence>(batch_size,
                                               alphabet_code,
                                               storage_unit_byte_width,
                                               per_row_query_hdrs,
                                               target_hdrs,
                                               query_ids,
                                               execution_plan,
                                               broadcast_query_ordinal,
                                               stats,
                                               scanner_pg_ctx,
                                               facade_letters_per_unit,
                                               per_row_query_mode,
                                               record_target_operational_outcomes,
                                               use_scanner_pg,
                                               persisted_required,
                                               cached_query_minimizer_stats_accounted,
                                               pool,
                                               generic_rows,
                                               materialize_target,
                                               same_target_locator,
                                               hash_major_identity_valid,
                                               fragment_identity_valid,
                                               with_pg_target,
                                               attach_query_stats,
                                               results);
    return results;

}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
