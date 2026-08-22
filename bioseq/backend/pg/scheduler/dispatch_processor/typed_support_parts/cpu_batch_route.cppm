/**
 * @file typed_support_parts/cpu_batch_route.cppm
 * @brief Owns generic and persisted native CPU batch execution.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.cpu_batch_route;

import std;
import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_target_materialization;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.row_adapters;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.source_provider_adapters;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.persisted_native_batch;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.generic_rows;
import bioseq.highway.dispatch_contract;
import bioseq.sequences.kmers;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.pool;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Execute the non-Vulkan batch through persisted native preflight or the
 * generic CPU and SIMD row continuation.
 */
template <class p_sequence, class p_scanner_row, class p_inmemory_row>
[[nodiscard]] auto process_cpu_batch_route(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    const minimap2_parameters& dispatch_params,
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
    p_inmemory_row& inmemory_row) -> std::map<int32_t, std::vector<match_result>> {
    std::map<int32_t, std::vector<match_result>> results;
    bool shared_target_outcome_recorded = false;
    std::optional<std::vector<minimap_sketch_data>> cached_query_minimizers;
    std::optional<pipe::minimizer_range_execution_stats> cached_query_minimizer_stats;
    auto& pool = bioseq::parallelism::global_pool::instance();
    /*
     * The persisted hash-major scanner path serializes one
     * complete query at a time because its fragmented target could not
     * enter worker code. For a shared target, authenticate every
     * query/locator/reader identity on the PostgreSQL thread, copy the
     * packed target once under the existing admission authority, then
     * fan independent queries across the task pool. Workers borrow only
     * immutable resident sequence bytes, retained query sketches, and
     * the immutable selective reader; they never call PostgreSQL.
     *
     * task_parallel_region::query_batch owns the outer parallel region,
     * so every inner align_regions/segment region observes an active
     * task_parallel_scope and executes serially within its query worker.
     * Any failed preflight or resident admission falls through to the
     * established scanner_pg row loop below.
     */
    auto generic_rows = [&batch_size,
                         &alphabet_code,
                         &storage_unit_byte_width,
                         &query_hdr,
                         &per_row_query_hdrs,
                         &target_hdrs,
                         &query_ids,
                         &dispatch_params,
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
                         &results] {
        process_generic_rows<p_sequence>(batch_size,
                                         alphabet_code,
                                         storage_unit_byte_width,
                                         query_hdr,
                                         per_row_query_hdrs,
                                         target_hdrs,
                                         query_ids,
                                         dispatch_params,
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
    const auto materialize_target = [](auto target_view,
                                       const std::uint8_t requested_width,
                                       const std::uint64_t sequence_rowno) {
        return try_materialize_target_for_segment_workers(
            target_view, requested_width, sequence_rowno);
    };
    const auto same_target_locator = [](const auto& left, const auto& right) noexcept {
        return same_dispatch_target_locator(left, right);
    };
    const auto hash_major_identity_valid = [](const auto& group,
                                               const std::uint64_t target_bp_size) noexcept {
        return hash_major_parallel_reader_identity_valid(group, target_bp_size);
    };
    const auto fragment_identity_valid = [](const auto& group,
                                             const std::uint64_t target_bp_size) noexcept {
        return fragment_parallel_reader_identity_valid(group, target_bp_size);
    };
    const auto with_pg_target = [](auto& context,
                                   const auto& locator,
                                   const std::uint8_t target_alphabet_code,
                                   const std::uint8_t target_storage_unit_byte_width,
                                   const std::uint16_t last_padding_letters,
                                   pg_sequence_read_session* active_read_session,
                                   auto&& callback) {
        with_source_provider_pg_target<p_sequence>(context,
                                                   locator,
                                                   target_alphabet_code,
                                                   target_storage_unit_byte_width,
                                                   last_padding_letters,
                                                   active_read_session,
                                                   std::forward<decltype(callback)>(callback));
    };
    const auto attach_query_stats = [](auto& collection, const auto& execution_stats) {
        attach_retained_hash_major_query_stats(collection, execution_stats);
    };
    process_persisted_native_batch<p_sequence>(batch_size,
                                               alphabet_code,
                                               storage_unit_byte_width,
                                               per_row_query_hdrs,
                                               target_hdrs,
                                               query_ids,
                                               dispatch_params,
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
