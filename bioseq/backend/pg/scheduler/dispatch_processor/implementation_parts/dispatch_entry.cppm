/**
 * @file implementation_parts/dispatch_entry.cppm
 * @brief Owns batch dispatch entry, prepared-group lookup, and pair parameter scaling.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <stdexcept>
#include <vector>

module bioseq.postgres.scheduler.dispatch_processor;
import bioseq.postgres.scheduler.dispatch_processor.effective_minimap_parameters;

import :highway_registry;
import :typed_flat_selector;
import bioseq.common.logging;
import bioseq.highway.dispatch_contract;
import bioseq.parallelism.pool;

namespace bioseq::simd {

/**
 * @brief Selects one Highway target and executes a typed alignment batch.
 *
 * Only the first `batch_size` target and identifier slots participate. Observer state is
 * restored on both success and failure, while processing errors propagate to the caller.
 *
 * @param[in] batch_size Number of live rows in the fixed-capacity arrays.
 * @param[in] alphabet_code Shared alphabet code for every participating sequence.
 * @param[in] storage_unit_byte_width Shared packed storage width in bytes.
 * @param[in] query_hdr Broadcast query, or `nullptr` in per-row query mode.
 * @param[in] per_row_query_hdrs Optional fixed-capacity array of per-row queries.
 * @param[in] target_hdrs Fixed-capacity target array with a live prefix of `batch_size`.
 * @param[in] query_ids Stable row identifiers parallel to `target_hdrs`.
 * @param[in] base_params Caller-selected minimap2 parameters.
 * @param[in] invocation_role Broadcast or per-row scheduling role.
 * @param[in] broadcast_query_ordinal Stable ordinal for broadcast-query accounting.
 * @param[in,out] stats Batch and pipeline counters accumulated by the typed route.
 * @param[in,out] scanner_pg_ctx Optional scan-owned preparation and observer context.
 * @return Matches grouped by the corresponding query identifier.
 */
[[nodiscard]] std::map<int32_t, std::vector<match_result>> batch_processor::process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    const minimap2_parameters& base_params,
    batch_invocation_role invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx) {
    std::map<int32_t, std::vector<match_result>> results;
    const bool per_row_query_mode = per_row_query_hdrs != nullptr;

    if (!per_row_query_mode && query_hdr == nullptr) {
        bioseq_log<logging_level::debug>("[bioseq] pg: batch_processor::process_sequences received "
                                         "null query_hdr; returning empty map");
        return results;
    }
    if (!per_row_query_mode && (query_hdr->alphabet_code != alphabet_code ||
                                query_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: batch_processor::process_sequences query_hdr typmod ({},{}) disagrees "
            "with context ({},{}); refusing to dispatch",
            static_cast<unsigned>(query_hdr->alphabet_code),
            static_cast<unsigned>(query_hdr->storage_unit_byte_width),
            static_cast<unsigned>(alphabet_code),
            static_cast<unsigned>(storage_unit_byte_width));
        return results;
    }

    const auto highway_plan = ::bioseq::simd_dispatch::registered_highway_dispatch_plan();
    const auto& highway_binding = ::bioseq::simd_dispatch::select_highway_target_once(highway_plan);
    if (highway_binding.target == 0 || highway_binding.kernels == nullptr ||
        !highway_binding.kernels->is_complete() ||
        highway_binding.target != highway_plan.planned_target) {
        throw std::logic_error{"Highway runtime binding disagrees with the generated target plan"};
    }
    if (scanner_pg_ctx != nullptr && !scanner_pg_ctx->highway_preparation.empty() &&
        !scanner_pg_ctx->highway_preparation.valid()) {
        throw std::logic_error{"Highway preparation hook is only partially configured"};
    }
    minimap2_parameters dispatched_params = base_params;
    dispatched_params.highway_kernels = highway_binding.kernels;
    dispatched_params.highway_dispatch = highway_plan;
    dispatched_params.highway_executed_target = highway_binding.target;
    dispatched_params.alphabet_code = alphabet_code;

    auto run_pipeline_body = [batch_size,
                              alphabet_code,
                              storage_unit_byte_width,
                              query_hdr,
                              per_row_query_hdrs,
                              &target_hdrs,
                              &query_ids,
                              &dispatched_params,
                              invocation_role,
                              broadcast_query_ordinal,
                              &stats,
                              scanner_pg_ctx,
                              &results] {
        results = dispatch_process_sequences_typed(batch_size,
                                                   alphabet_code,
                                                   storage_unit_byte_width,
                                                   query_hdr,
                                                   per_row_query_hdrs,
                                                   target_hdrs,
                                                   query_ids,
                                                   dispatched_params,
                                                   invocation_role,
                                                   broadcast_query_ordinal,
                                                   stats,
                                                   scanner_pg_ctx);
    };

    const auto inherited_observer = bioseq::parallelism::current_tracked_job_observer();
    const auto requested_observer = scanner_pg_ctx != nullptr && scanner_pg_ctx->async_job_observer
                                        ? scanner_pg_ctx->async_job_observer
                                        : inherited_observer;
    const auto previous_observer =
        bioseq::parallelism::exchange_tracked_job_observer(requested_observer);
    try {
        run_pipeline_body();
    } catch (...) {
        bioseq::parallelism::restore_tracked_job_observer(previous_observer);
        throw;
    }
    bioseq::parallelism::restore_tracked_job_observer(previous_observer);

    return results;
}

/**
 * @brief Finds a prepared CPU index group matching one minimizer configuration.
 * @param[in] kmer_size Minimizer k-mer length in bases.
 * @param[in] window_size Minimizer window length in bases.
 * @param[in] max_occ Maximum retained minimizer occurrence count.
 * @param[in] sketch_backend Sketch implementation used to build the group.
 * @return Borrowed group pointer, or `nullptr` when no prepared group matches.
 */
batch_processor::cpu_prepared_index_group* batch_processor::cpu_prepared_target::find_group(
    const std::int32_t kmer_size,
    const std::int32_t window_size,
    const std::int32_t max_occ,
    const minimizer_sketch_backend sketch_backend) noexcept {
    for (cpu_prepared_index_group& group : groups) {
        if (group.kmer_size == kmer_size && group.window_size == window_size &&
            group.max_occ == max_occ && group.sketch_backend == sketch_backend) {
            return &group;
        }
    }
    return nullptr;
}

/**
 * @brief Scales untouched minimizer defaults for short query-target pairs.
 * @param[in] base Requested parameters, preserved when any tuned field is nondefault.
 * @param[in] min_bp Shorter operand length in base pairs.
 * @return Effective parameters for the pair.
 */
minimap2_parameters batch_processor::effective_params_for_pair(const minimap2_parameters& base,
                                                               const std::size_t min_bp) noexcept {
    return effective_minimap_parameters_for_pair(base, min_bp);
}

} // namespace bioseq::simd
