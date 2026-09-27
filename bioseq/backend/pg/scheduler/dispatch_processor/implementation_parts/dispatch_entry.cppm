/**
 * @file implementation_parts/dispatch_entry.cppm
 * @brief Run alignment batches with  the statement's selected CPU kernels.
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
import bioseq.pipeline.pipe;

namespace bioseq::simd {

/**
 * @brief Run a btch using the statement's selected Highway kernels.
 *
 * Only the first batch_size array entries participate. Use query_hdr for a
 * shared query, or per_row_query_hdrs for separate queries. The scanner context
 * supplies deferred sequences and preparation callbacks. Add execution counters
 * to stats and restore the caller's job observer on success or failure
 *
 * @return Matches keyed by query_ids.
 */
[[nodiscard]] match_result_map batch_processor::process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    pipe::statement_execution_plan_owner execution_plan,
    batch_invocation_role invocation_role,
    std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx) {
    if (!execution_plan) {
        throw std::logic_error("batch dispatch requires its immutable statement plan");
    }
    const auto& base_params = execution_plan->frozen_parameters;
    if (base_params.alphabet_code != alphabet_code) {
        throw std::logic_error("batch alphabet disagrees with its frozen statement plan");
    }
    if (scanner_pg_ctx != nullptr) {
        if (scanner_pg_ctx->execution_plan &&
            (scanner_pg_ctx->execution_plan.get() != execution_plan.get() ||
             scanner_pg_ctx->execution_plan.owner_before(execution_plan) ||
             execution_plan.owner_before(scanner_pg_ctx->execution_plan))) {
            throw std::logic_error("batch dispatch changed its statement-plan control block");
        }
        scanner_pg_ctx->execution_plan = execution_plan;
        if (scanner_pg_ctx->downstream_active_wall != base_params.downstream_active_wall) {
            throw std::logic_error("batch timing owner disagrees with its statement plan");
        }
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (base_params.downstream_active_wall != nullptr &&
        !base_params.downstream_active_wall->attest_benchmark_e2e_statement_plan_checkpoint(
            execution_plan,
            benchmark_e2e_statement_plan_checkpoint::scan_dispatch)) {
        throw std::logic_error{"batch dispatch changed its guarded statement-plan owner"};
    }
#endif
    match_result_map results;
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

    if (base_params.highway_executed_target == 0 || base_params.highway_kernels == nullptr ||
        !base_params.highway_kernels->is_complete() ||
        base_params.highway_executed_target != base_params.highway_dispatch.planned_target) {
        throw std::logic_error{"Highway runtime binding disagrees with the generated target plan"};
    }
    if (scanner_pg_ctx != nullptr && !scanner_pg_ctx->highway_preparation.empty() &&
        !scanner_pg_ctx->highway_preparation.valid()) {
        throw std::logic_error{"Highway preparation hook is only partially configured"};
    }
    auto run_pipeline_body = [batch_size,
                              alphabet_code,
                              storage_unit_byte_width,
                              query_hdr,
                              per_row_query_hdrs,
                              &target_hdrs,
                              &query_ids,
                              execution_plan,
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
                                                   execution_plan,
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
 * @brief Find a prepared index with the same minimizer settings and sketch implementation.
 * @return A borrowed group, or nullptr when none matches.
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
 * @brief Adjust default minimizer and chaining settings for the shorter sequence.
 * @param[in] base Requested parameters. User changes to  the adjusted fields are preserved.
 * @param[in] min_bp Length of the shorter sequence in bases.
 * @return The parameters to use for this pair.
 */
minimap2_parameters batch_processor::effective_params_for_pair(const minimap2_parameters& base,
                                                               const std::size_t min_bp) noexcept {
    return effective_minimap_parameters_for_pair(base, min_bp);
}

} // namespace bioseq::simd
