/**
 * @file typed_support_parts/root_coordination.cppm
 * @brief Checks a typed batch and runs its selected CPU or Vulkan pipeline.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <fmt/format.h>
#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.root_coordination;

import std;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_query_route;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_target_routes;
import bioseq.highway.dispatch_contract;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.active_read_policy;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.minimap_port.match_result_contract;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
import bioseq.parallelism.pool;
#endif
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.postgres.scheduler.supervisor.vulkan;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

namespace root_std {
using std::addressof;
using std::array;
using std::exchange;
using std::forward;
using std::logic_error;
using std::map;
using std::move;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::shared_ptr;
using std::size_t;
using std::uint8_t;
using std::uint16_t;
using std::uint64_t;
using std::vector;
}  // namespace root_std

/**
 * Runs a batch using the statement's chosen algorithms and target-index policy.
 * Checks the sequence format and required index readers before dispatch. Vulkan
 * jobs keep their results and execution records until the whole batch is ready
 * to report. CPU seeding uses the supplied typed row dispatcher, including when
 * Vulkan is selected for chaining.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_sequence_header,
          class p_scanner_context,
          class p_source_view_resolver,
          class p_same_target_locator,
          class p_cpu_route>
[[nodiscard]] auto process_sequences_typed_root(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    const std::array<const p_sequence_header*, p_batch_capacity>& target_hdrs,
    const std::array<int32_t, p_batch_capacity>& query_ids,
    pipe::statement_execution_plan_owner execution_plan,
    const bool primary_invocation,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    p_scanner_context* scanner_pg_ctx,
    p_source_view_resolver& source_view_for_row,
    p_same_target_locator&,
    p_cpu_route& cpu_route) -> match_result_map {
    match_result_map results;
    const std::size_t facade_letters_per_unit = bioseq::storage::packed_letters_per_unit(
        storage_unit_byte_width, static_cast<std::uint8_t>(p_sequence::letter_bit_width));
    if (facade_letters_per_unit == 0u) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR, "typed batch facade has invalid packed storage geometry");
    }
    const bool per_row_query_mode = per_row_query_hdrs != nullptr;
    const bool record_target_operational_outcomes =
        primary_invocation &&
        (scanner_pg_ctx == nullptr || scanner_pg_ctx->record_target_operational_outcomes);
    if (!execution_plan) {
        throw std::logic_error("typed root requires its immutable statement plan");
    }
    const std::optional<std::string> chunked_scanner_guc =
        pg_guc_value_as_cpp("bioseq.testing.chunked_scanner.enabled");
    const bool chunked_scanner_forced =
        chunked_scanner_guc.has_value() && *chunked_scanner_guc == "on";
    // Test settings can select flat or chunked baseline scanners. Expanded
    // sequence expressions still use the PostgreSQL scanner so their lazy
    // views remain available.
    const std::optional<std::string> inmemory_scanner_guc =
        pg_guc_value_as_cpp("bioseq.testing.in_memory_scanner.enabled");
    const bool inmemory_scanner_forced =
        inmemory_scanner_guc.has_value() && *inmemory_scanner_guc == "on";
    bool scanner_has_recipe = false;
    if (scanner_pg_ctx != nullptr) {
        for (std::size_t row = 0u; row < batch_size && !scanner_has_recipe; ++row) {
            scanner_has_recipe = scanner_pg_ctx->target_recipe_datums[row] != Datum{0} ||
                                 scanner_pg_ctx->query_recipe_datums[row] != Datum{0};
        }
    }
    const bool use_scanner_pg =
        scanner_pg_ctx != nullptr &&
        ((!inmemory_scanner_forced && !chunked_scanner_forced) || scanner_has_recipe);
    const auto index_read_requirement = scanner_pg_ctx != nullptr
                                            ? scanner_pg_ctx->index_read_requirement
                                            : bioseq::indexing::index_read_requirement::allow;
    const bool persisted_required =
        index_read_requirement ==
        bioseq::indexing::index_read_requirement::require_compatible_persisted;
    if (scanner_pg_ctx != nullptr) {
        using policy_source_type = bioseq::storage::indexing::active_read_policy_source;
        const policy_source_type source = scanner_pg_ctx->active_index_policy_source;
        const bool relation_backed = OidIsValid(scanner_pg_ctx->user_relid);
        const bool valid_relation_source =
            relation_backed && (source == policy_source_type::configured_row ||
                                source == policy_source_type::authoritative_no_row_default);
        const bool valid_non_relation_source =
            !relation_backed && source == policy_source_type::non_relation;
        if ((!valid_relation_source && !valid_non_relation_source) ||
            scanner_pg_ctx->active_index.empty()) {
            const std::string diagnostic = fmt::format(
                "relation={} relation_backed={} source={} index={}",
                static_cast<unsigned>(scanner_pg_ctx->user_relid),
                relation_backed,
                bioseq::storage::indexing::active_read_policy_resolution_source_name(source),
                scanner_pg_ctx->active_index);
            raise_typed_dispatch_pg_error_as_cpp(ERRCODE_INTERNAL_ERROR,
                                                 "active-read policy dispatch stats are invalid",
                                                 diagnostic.c_str());
        }
    }
    if (index_read_requirement == bioseq::indexing::index_read_requirement::require_absent &&
        scanner_pg_ctx != nullptr && scanner_pg_ctx->read_index_active) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "index_read_requirement=require_absent contract violation",
            "typed processor received read_index_active=true");
    }
    const bool selected_vulkan_seed = execution_plan->uses_vulkan_seed();
    const bool selected_vulkan_chain =
        execution_plan->route == pipe::seed_chain_route::host_seed_vulkan_chain ||
        execution_plan->route == pipe::seed_chain_route::vulkan_seed_vulkan_chain;
    const auto selected_target_layout = classify_gpu_seed_target_layout(
        scanner_pg_ctx != nullptr
            ? std::string_view{scanner_pg_ctx->active_index}
            : std::string_view{bioseq::indexing::k_minimizers_per_fragment_index_name});
    const bool cpu_persisted_prepared =
        scanner_pg_ctx != nullptr && scanner_pg_ctx->cpu_prepared_target != nullptr;
    const bool gpu_mpf_source_prepared =
        scanner_pg_ctx != nullptr && scanner_pg_ctx->gpu_prepared_mpf_target != nullptr &&
        scanner_pg_ctx->gpu_prepared_mpf_target->source_ready();
    const bool gpu_mps_source_prepared =
        scanner_pg_ctx != nullptr && scanner_pg_ctx->gpu_prepared_grouped_soa_target != nullptr &&
        scanner_pg_ctx->gpu_prepared_grouped_soa_target->source_ready();
    const bool ordinary_vulkan_preparation_ready =
        scanner_pg_ctx != nullptr &&
        scanner_pg_ctx->ordinary_vulkan_target_preparation.valid();
    const auto selected_target_ownership = classify_vulkan_seed_target_ownership(
        selected_target_layout,
        gpu_mpf_source_prepared,
        gpu_mps_source_prepared,
        ordinary_vulkan_preparation_ready);
    const bool required_owner_prepared =
        !selected_vulkan_seed ? cpu_persisted_prepared
        : (selected_target_ownership == vulkan_seed_target_ownership::shared_mpf ||
           selected_target_ownership == vulkan_seed_target_ownership::shared_mps)
            ? true
        : (selected_target_ownership == vulkan_seed_target_ownership::ordinary_mpf ||
           selected_target_ownership == vulkan_seed_target_ownership::ordinary_mps)
            ? scanner_pg_ctx->read_index_active &&
                  scanner_pg_ctx->executor_read_context != nullptr
            : false;
    if (persisted_required && !required_owner_prepared) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            -1,
            selected_vulkan_seed
                ? "GPU seeding requires the active layout's exact prepared MPF or MPS owner"
                : "host seeding requires one retained native persisted reader");
    }
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: batch_processor dispatching ({},{}) query_bp_size={} batch_size={} "
        "per_row_query={} use_scanner_pg={} chunked={} inmemory_forced={} active_index={} "
        "active_index_policy_source={}",
        static_cast<unsigned>(alphabet_code),
        static_cast<unsigned>(storage_unit_byte_width),
        query_hdr != nullptr ? query_hdr->bp_size : 0,
        batch_size,
        per_row_query_mode,
        use_scanner_pg,
        chunked_scanner_forced,
        inmemory_scanner_forced,
        scanner_pg_ctx != nullptr ? std::string_view{scanner_pg_ctx->active_index}
                                  : std::string_view{"<no-scanner-context>"},
        scanner_pg_ctx != nullptr
            ? bioseq::storage::indexing::active_read_policy_resolution_source_name(
                  scanner_pg_ctx->active_index_policy_source)
            : std::string_view{"no_scanner_context"});

    /*
     * A stored-index batch may share one broadcast query sketch. Track whether
     * its work has been counted so each row can reuse it without inflating stats.
     */
    bool cached_query_minimizer_stats_accounted = false;
    bioseq::gpu::vulkan::runtime* selected_vulkan_runtime = nullptr;
    if (selected_vulkan_seed || selected_vulkan_chain) {
        auto& runtime = bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
        if (!runtime.is_usable()) {
            add_operational_counter(stats, stats.operational.backend_declines);
            bioseq::gpu::vulkan::vulkan_runtime_error err{
                "selected Vulkan runtime unavailable: " + runtime.initialization_error()};
            raise_vulkan_pg_error(selected_vulkan_seed ? "Vulkan canonical seed route"
                                                       : "Vulkan canonical chain route",
                                  err.what());
        }
        selected_vulkan_runtime = std::addressof(runtime);
    }
    if (selected_vulkan_seed) {
        if (selected_target_ownership == vulkan_seed_target_ownership::invalid) {
            add_operational_counter(stats, stats.operational.backend_declines);
            raise_vulkan_pg_error(
                "Vulkan canonical seed route",
                "selected target layout has no exclusive shared or ordinary owner topology");
        }
        if (chunked_scanner_forced) {
            add_operational_counter(stats, stats.operational.backend_declines);
            raise_vulkan_pg_error(
                "Vulkan canonical seed route",
                "the forced chunked test scanner has no canonical GPU-seed owner; "
                "host biological fallback is forbidden");
        }
        auto& runtime = *selected_vulkan_runtime;
        try {
            const auto physical_layout = select_vulkan_batch_minimizer_layout<p_sequence>(
                batch_size,
                storage_unit_byte_width,
                query_hdr,
                per_row_query_hdrs,
                target_hdrs,
                execution_plan,
                scanner_pg_ctx);
            bioseq::parallelism::memory_permit completed_suffix_rows_permit;
            std::vector<canonical_suffix_row_completion> completed_suffix_rows;
            prepare_vulkan_query_host_capacity(completed_suffix_rows,
                                               completed_suffix_rows_permit,
                                               batch_size);
            std::uint64_t completed_query_sketch_host_ns = 0u;
            ordinary_vulkan_scan_route_completion ordinary_route_completion;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            bioseq::gpu::vulkan::runtime_sketch_dispatch_stats
                completed_query_sketch_dispatch_stats{};
            std::shared_ptr<const void> completed_query_sketch_owner;
            bioseq::gpu::vulkan::device_query_sketch_authorization
                completed_mpf_query_authorization;
            bioseq::parallelism::memory_permit
                completed_mpf_query_authorizations_permit;
            std::vector<bioseq::gpu::vulkan::device_query_sketch_authorization>
                completed_mpf_query_authorizations;
            if (selected_target_ownership == vulkan_seed_target_ownership::shared_mpf ||
                selected_target_ownership == vulkan_seed_target_ownership::ordinary_mpf) {
                prepare_vulkan_query_host_capacity(
                    completed_mpf_query_authorizations,
                    completed_mpf_query_authorizations_permit,
                    batch_size);
            }
            bioseq::gpu::vulkan::runtime_pipeline_operation_token
                completed_query_operation;
            bioseq::gpu::vulkan::runtime_pipeline_operation_token
                completed_seed_operation;
            bioseq::parallelism::memory_permit completed_mpf_seed_operations_permit;
            std::vector<bioseq::gpu::vulkan::runtime_pipeline_operation_token>
                completed_mpf_seed_operations;
            if (batch_size > ((std::numeric_limits<std::size_t>::max)() - 1u) / 3u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan proof-token capacity arithmetic overflowed");
            }
            prepare_vulkan_query_host_capacity(completed_mpf_seed_operations,
                                               completed_mpf_seed_operations_permit,
                                               batch_size * 3u + 1u);
            bioseq::parallelism::memory_permit completed_chaining_permit;
            std::vector<bioseq::gpu::vulkan::runtime_pipeline_operation_token>
                completed_chaining;
            prepare_vulkan_query_host_capacity(completed_chaining,
                                               completed_chaining_permit,
                                               batch_size);
            completed_chaining.resize(batch_size);
#endif
            bioseq::minimizers::visit_minimizer_physical_layout(
                physical_layout,
                [batch_size,
                 &physical_layout,
                 alphabet_code,
                 storage_unit_byte_width,
                 query_hdr,
                 per_row_query_hdrs,
                 &target_hdrs,
                 &query_ids,
                 &execution_plan,
                 broadcast_query_ordinal,
                 scanner_pg_ctx,
                 per_row_query_mode,
                 &runtime,
                 selected_target_ownership,
                 &source_view_for_row,
                 &completed_suffix_rows,
                 &completed_query_sketch_host_ns,
                 &ordinary_route_completion
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                 , &completed_query_sketch_owner
                 , &completed_query_sketch_dispatch_stats
                 , &completed_mpf_query_authorization
                 , &completed_query_operation
                 , &completed_seed_operation
                 , &completed_mpf_seed_operations
                 , &completed_chaining
#endif
                 ]<class p_minimizer_type>() {
                    if (selected_target_ownership ==
                            vulkan_seed_target_ownership::ordinary_mpf ||
                        selected_target_ownership ==
                            vulkan_seed_target_ownership::ordinary_mps) {
                        process_ordinary_vulkan_scan_route<p_sequence,
                                                           p_batch_capacity,
                                                           p_minimizer_type>(
                            batch_size,
                            alphabet_code,
                            storage_unit_byte_width,
                            physical_layout,
                            query_hdr,
                            per_row_query_hdrs,
                            target_hdrs,
                            query_ids,
                            execution_plan,
                            broadcast_query_ordinal,
                            scanner_pg_ctx,
                            per_row_query_mode,
                            runtime,
                            selected_target_ownership,
                            source_view_for_row,
                            completed_suffix_rows,
                            ordinary_route_completion);
                        completed_query_sketch_host_ns =
                            ordinary_route_completion.query_sketch_host_ns;
                        return;
                    }
                    vulkan_query_batch_state<p_sequence,
                                             p_batch_capacity,
                                             p_minimizer_type> query_state;
                    prepare_vulkan_query_route<p_sequence,
                                               p_batch_capacity,
                                               p_minimizer_type>(batch_size,
                                                                 alphabet_code,
                                                                 storage_unit_byte_width,
                                                                 query_hdr,
                                                                 per_row_query_hdrs,
                                                                 execution_plan,
                                                                 scanner_pg_ctx,
                                                                 per_row_query_mode,
                                                                 broadcast_query_ordinal,
                                                                 source_view_for_row,
                                                                 query_state);
                    switch (selected_target_ownership) {
                    case vulkan_seed_target_ownership::shared_mpf: {
                        if (scanner_pg_ctx == nullptr ||
                            scanner_pg_ctx->gpu_prepared_mpf_target == nullptr) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "selected MPF Vulkan seed route has no GPU target owner");
                        }
                        process_vulkan_mpf_seed_route(batch_size,
                                                      alphabet_code,
                                                      physical_layout,
                                                      query_hdr,
                                                      per_row_query_hdrs,
                                                      target_hdrs,
                                                      query_ids,
                                                      execution_plan,
                                                      broadcast_query_ordinal,
                                                      scanner_pg_ctx,
                                                      per_row_query_mode,
                                                      runtime,
                                                      *scanner_pg_ctx->gpu_prepared_mpf_target,
                                                      query_state,
                                                      completed_suffix_rows
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                      , completed_chaining
                                                      , completed_mpf_seed_operations
                                                      , std::addressof(
                                                            completed_mpf_query_authorization)
#endif
                                                      );
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                        completed_query_sketch_owner =
                            std::static_pointer_cast<const void>(
                                query_state.query_sketch_owner);
                        if (!completed_query_sketch_owner) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "MPF Vulkan route lost its physical query owner");
                        }
                        completed_query_sketch_dispatch_stats =
                            query_state.query_sketch_owner->dispatch_stats;
#endif
                        completed_query_sketch_host_ns =
                            query_state.gpu_query_sketch_host_ns;
                        return;
                    }
                    case vulkan_seed_target_ownership::shared_mps: {
                        if (scanner_pg_ctx == nullptr ||
                            scanner_pg_ctx->gpu_prepared_grouped_soa_target == nullptr) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "selected MPS Vulkan seed route has no grouped-SoA target owner");
                        }
                        process_vulkan_mps_seed_route(
                            batch_size,
                            alphabet_code,
                            storage_unit_byte_width,
                            query_hdr,
                            per_row_query_hdrs,
                            target_hdrs,
                            query_ids,
                            execution_plan,
                            broadcast_query_ordinal,
                            scanner_pg_ctx,
                            per_row_query_mode,
                            runtime,
                            *scanner_pg_ctx->gpu_prepared_grouped_soa_target,
                            query_state,
                            completed_suffix_rows
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                            , std::addressof(completed_query_operation)
                            , std::addressof(completed_seed_operation)
                            , completed_chaining
#endif
                            );
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                        completed_query_sketch_owner =
                            std::static_pointer_cast<const void>(
                                query_state.query_sketch_owner);
                        if (!completed_query_sketch_owner) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "MPS Vulkan route lost its physical query owner");
                        }
                        completed_query_sketch_dispatch_stats =
                            query_state.query_sketch_owner->dispatch_stats;
#endif
                        completed_query_sketch_host_ns =
                            query_state.gpu_query_sketch_host_ns;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                        if (!query_state.query_sketch_owner ||
                            !completed_query_operation.is_valid_for_query_sketch() ||
                            !completed_seed_operation
                                 .is_valid_for_grouped_soa_seed_anchor_roundtrip()) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                "MPS Vulkan seed route returned incomplete publication receipts");
                        }
#endif
                        return;
                    }
                    case vulkan_seed_target_ownership::ordinary_mpf:
                    case vulkan_seed_target_ownership::ordinary_mps:
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "ordinary target ownership escaped its row-local branch");
                    case vulkan_seed_target_ownership::invalid:
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "active index policy is neither MPF nor MPS");
                    }
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "unreachable Vulkan target-layout selection");
                });
            if (completed_suffix_rows.size() != batch_size) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan seed wave did not retain one completed suffix per submitted job");
            }
            for (std::size_t index = 0u; index < completed_suffix_rows.size(); ++index) {
                const auto& completed = completed_suffix_rows[index];
                if (completed.row != index || completed.row >= batch_size ||
                    completed.query_id != query_ids[completed.row]) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan seed wave changed canonical row or query-id order");
                }
            }
            // Allocate all result-map entries before reporting GPU execution. If an
            // allocation fails, none of this batch's one-time records have been
            // consumed yet.
            for (auto& completed : completed_suffix_rows) {
                append_canonical_results(results, completed.query_id, completed.collection);
            }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            const bool ordinary_route =
                selected_target_ownership == vulkan_seed_target_ownership::ordinary_mpf ||
                selected_target_ownership == vulkan_seed_target_ownership::ordinary_mps;
            if (ordinary_route) {
                if (completed_query_sketch_owner ||
                    completed_mpf_query_authorization.is_valid() ||
                    completed_query_operation.audit().kind !=
                        bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid ||
                    completed_seed_operation.audit().kind !=
                        bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid ||
                    !completed_mpf_seed_operations.empty() ||
                    ordinary_route_completion.row_proofs.size() != batch_size) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "ordinary Vulkan wave escaped into the shared-target proof set");
                }
                for (std::size_t index = 0u; index < batch_size; ++index) {
                    const auto& proof = ordinary_route_completion.row_proofs[index];
                    const bool chain_dispatched =
                        completed_suffix_rows[index].collection.partial_vulkan_stats
                            .chain_score_dispatches == 1u;
                    const bool chain_receipt =
                        proof.chaining_operation.is_valid_for_chaining_tiled_dp();
                    const bool common_identity =
                        proof.query_sketch_owner && proof.target_owner &&
                        proof.ownership == selected_target_ownership && proof.row == index &&
                        proof.target_rowno == scanner_pg_ctx->target_rownos[index] &&
                        proof.target_bp_size != 0u && proof.target_position_offset == 0u &&
                        proof.target_operation_identity != 0u &&
                        proof.target_persisted == scanner_pg_ctx->read_index_active &&
                        ordinary_persisted_read_receipt_matches_source(
                            proof.persisted_read_stats, proof.target_persisted) &&
                        completed_suffix_rows[index].collection.partial_vulkan_stats
                                .chain_score_dispatches <= 1u &&
                        chain_dispatched == chain_receipt;
                    const bool mpf_identity =
                        selected_target_ownership ==
                            vulkan_seed_target_ownership::ordinary_mpf &&
                        proof.mpf_query_authorization.is_valid() &&
                        proof.query_operation.audit().kind ==
                            bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid &&
                        (proof.seed_operation.audit().kind ==
                             bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid ||
                         proof.seed_operation.is_valid_for_mpf_seed_anchor_roundtrip(
                             runtime.features().max_workgroup_count_x)) &&
                        proof.target_cold_load_identity == 0u &&
                        proof.target_persisted ==
                            (proof.target_generation_id != 0u &&
                             proof.target_source_identity != 0u);
                    const bool mps_identity =
                        selected_target_ownership ==
                            vulkan_seed_target_ownership::ordinary_mps &&
                        !proof.mpf_query_authorization.is_valid() &&
                        proof.query_operation.is_valid_for_query_sketch() &&
                        proof.seed_operation
                            .is_valid_for_grouped_soa_seed_anchor_roundtrip() &&
                        proof.target_generation_id != 0u &&
                        proof.target_source_identity != 0u &&
                        proof.target_cold_load_identity != 0u;
                    if (!common_identity || (!mpf_identity && !mps_identity)) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "ordinary Vulkan wave lost row, offset, owner, or receipt identity");
                    }
                }
            } else {
                if (!completed_query_sketch_owner) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan seed wave has no retained physical query owner");
                }
                const bool mpf_receipts =
                    selected_target_ownership == vulkan_seed_target_ownership::shared_mpf &&
                    completed_mpf_query_authorization.is_valid() &&
                    completed_query_operation.audit().kind ==
                        bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid &&
                    completed_seed_operation.audit().kind ==
                        bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid &&
                    std::all_of(
                        completed_mpf_seed_operations.begin(),
                        completed_mpf_seed_operations.end(),
                        [maximum_workgroup_count_x =
                             runtime.features().max_workgroup_count_x](const auto& operation) {
                            return operation.is_valid_for_mpf_seed_anchor_roundtrip(
                                maximum_workgroup_count_x);
                        });
                const bool mps_receipts =
                    selected_target_ownership == vulkan_seed_target_ownership::shared_mps &&
                    !completed_mpf_query_authorization.is_valid() &&
                    completed_mpf_seed_operations.empty() &&
                    completed_query_operation.is_valid_for_query_sketch() &&
                    completed_seed_operation.is_valid_for_grouped_soa_seed_anchor_roundtrip();
                if (!mpf_receipts && !mps_receipts) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan seed wave returned an incomplete or cross-route proof set");
                }
                for (std::size_t index = 0u; index < completed_suffix_rows.size(); ++index) {
                    const bool chain_dispatched =
                        completed_suffix_rows[index].collection.partial_vulkan_stats
                            .chain_score_dispatches == 1u;
                    const bool chain_receipt =
                        completed_chaining[index].is_valid_for_chaining_tiled_dp();
                    if (completed_suffix_rows[index].collection.partial_vulkan_stats
                                .chain_score_dispatches > 1u ||
                        chain_dispatched != chain_receipt) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "Vulkan seed wave lost a per-job completed chaining receipt");
                    }
                }
            }
            const bool shared_mpf_route =
                selected_target_ownership == vulkan_seed_target_ownership::shared_mpf;
            const bool shared_mps_route =
                selected_target_ownership == vulkan_seed_target_ownership::shared_mps;
            const bool ordinary_mpf_route =
                selected_target_ownership == vulkan_seed_target_ownership::ordinary_mpf;
            const bool ordinary_mps_route =
                selected_target_ownership == vulkan_seed_target_ownership::ordinary_mps;
            std::optional<vulkan_seed_target_execution> shared_target_execution;
            bool publish_mpf_target_stats = false;
            bool publish_mps_target_producer_stats = false;
            bool publish_mps_target_materialization_stats = false;
            std::uint64_t completed_queue_roundtrips = 0u;
            std::uint64_t completed_target_materializations = 0u;
            std::uint64_t completed_persisted_target_ingestions = 0u;
            const auto add_completed_count =
                [&completed_queue_roundtrips](const std::uint64_t incoming) {
                    const auto combined = bioseq::common::checked_add(
                        completed_queue_roundtrips, incoming);
                    if (!combined) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "Vulkan seed wave physical counter overflowed");
                    }
                    completed_queue_roundtrips = *combined;
                };
            const auto add_materializations =
                [&completed_target_materializations](const std::uint64_t incoming) {
                    const auto combined = bioseq::common::checked_add(
                        completed_target_materializations, incoming);
                    if (!combined) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "Vulkan target materialization counter overflowed");
                    }
                    completed_target_materializations = *combined;
                };
            const auto add_persisted_ingestions =
                [&completed_persisted_target_ingestions](const std::uint64_t incoming) {
                    const auto combined = bioseq::common::checked_add(
                        completed_persisted_target_ingestions, incoming);
                    if (!combined) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "Vulkan target ingestion counter overflowed");
                    }
                    completed_persisted_target_ingestions = *combined;
                };
            const auto append_additional_operation =
                [&completed_mpf_seed_operations](
                    bioseq::gpu::vulkan::runtime_pipeline_operation_token& operation) {
                    if (completed_mpf_seed_operations.size() ==
                        completed_mpf_seed_operations.capacity()) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "Vulkan proof token exceeded admitted publication capacity");
                    }
                    completed_mpf_seed_operations.push_back(std::move(operation));
                };
            const auto append_mpf_query_authorization =
                [&completed_mpf_query_authorizations](
                    bioseq::gpu::vulkan::device_query_sketch_authorization& authorization) {
                    if (!authorization.is_valid() ||
                        completed_mpf_query_authorizations.size() ==
                            completed_mpf_query_authorizations.capacity()) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "MPF query authority exceeded admitted publication capacity");
                    }
                    completed_mpf_query_authorizations.push_back(std::move(authorization));
                };

            if (ordinary_route) {
                for (std::size_t index = 0u; index < batch_size; ++index) {
                    auto& proof = ordinary_route_completion.row_proofs[index];
                    const auto& frontend = completed_suffix_rows[index].seed_frontend;
                    if (!proof.query_sketch_dispatch_stats.is_internally_consistent() ||
                        proof.query_sketch_dispatch_stats.materialization() !=
                            bioseq::gpu::vulkan::vulkan_sketch_host_materialization::device_only ||
                        (ordinary_mps_route &&
                         proof.query_operation.audit().committed_submissions !=
                             proof.query_sketch_dispatch_stats.completed_roundtrips()) ||
                        !proof.target_execution.is_internally_consistent() ||
                        proof.target_execution.completion != frontend.target_completion ||
                        proof.target_execution.target_operation_identity !=
                            frontend.target_operation_identity ||
                        proof.target_execution.target_len != frontend.target_len ||
                        proof.target_execution.persisted_generation_id !=
                            frontend.persisted_generation_id ||
                        proof.target_execution.persisted_source_identity !=
                            frontend.persisted_source_identity ||
                        !frontend.matches_runtime_operation(
                            proof.seed_operation,
                            runtime.features().max_workgroup_count_x)) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "ordinary Vulkan row changed its query, target, or seed proof");
                    }
                    if (ordinary_mpf_route &&
                        frontend.query_authority_identity !=
                            proof.mpf_query_authorization.identity()) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "ordinary MPF seed changed its query authority");
                    }
                    add_completed_count(
                        proof.query_sketch_dispatch_stats.completed_roundtrips());
                    if (proof.seed_operation.audit().kind !=
                        bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid) {
                        add_completed_count(
                            proof.seed_operation.audit().committed_submissions);
                    }
                    if (proof.chaining_operation.is_valid_for_chaining_tiled_dp()) {
                        add_completed_count(
                            proof.chaining_operation.audit().committed_submissions);
                    }
                    add_completed_count(proof.target_execution.producer_roundtrips);
                    add_materializations(
                        proof.target_execution.resident_materializations);
                    if (proof.target_execution.target_source() ==
                        ::bioseq::pipeline_stats::target_index_source::persisted) {
                        add_persisted_ingestions(
                            proof.target_execution.resident_ingestions);
                    }
                }
            } else {
                if (!completed_query_sketch_owner ||
                    !completed_query_sketch_dispatch_stats.is_internally_consistent() ||
                    completed_query_sketch_dispatch_stats.materialization() !=
                        bioseq::gpu::vulkan::vulkan_sketch_host_materialization::device_only) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan seed wave has no completed device-only query sketch");
                }
                if (shared_mpf_route) {
                    auto& target = *scanner_pg_ctx->gpu_prepared_mpf_target;
                    shared_target_execution.emplace(make_mpf_target_execution(target));
                    publish_mpf_target_stats = !target.target_physical_stats_published;
                } else if (shared_mps_route) {
                    auto& target = *scanner_pg_ctx->gpu_prepared_grouped_soa_target;
                    shared_target_execution.emplace(make_mps_target_execution(target));
                    publish_mps_target_producer_stats =
                        !target.persisted && !target.transient_generation_stats_published;
                    publish_mps_target_materialization_stats =
                        !target.resident_materialization_stats_published;
                } else {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan seed wave has no selected target proof");
                }
                std::size_t matched_mpf_seed_operations = 0u;
                for (std::size_t index = 0u; index < batch_size; ++index) {
                    const auto& frontend = completed_suffix_rows[index].seed_frontend;
                    const auto& target = *shared_target_execution;
                    const bool target_identity = frontend.is_internally_consistent() &&
                        frontend.target_completion == target.completion &&
                        frontend.target_operation_identity ==
                            target.target_operation_identity &&
                        frontend.target_len == target.target_len &&
                        frontend.target_position_offset == 0u &&
                        frontend.persisted_generation_id ==
                            target.persisted_generation_id &&
                        frontend.persisted_source_identity ==
                            target.persisted_source_identity;
                    bool seed_identity = false;
                    if (shared_mpf_route) {
                        seed_identity = frontend.completion !=
                            vulkan_seed_frontend_completion_kind::mpf_runtime;
                        for (const auto& operation : completed_mpf_seed_operations) {
                            if (operation.audit().operation_identity !=
                                frontend.operation_identity) {
                                continue;
                            }
                            if (seed_identity || !frontend.matches_runtime_operation(
                                    operation,
                                    runtime.features().max_workgroup_count_x)) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                                    "MPF Vulkan seed wave duplicated or changed an operation");
                            }
                            seed_identity = true;
                            ++matched_mpf_seed_operations;
                        }
                    } else {
                        seed_identity = frontend.matches_runtime_operation(
                            completed_seed_operation,
                            runtime.features().max_workgroup_count_x);
                    }
                    if (!target_identity || !seed_identity) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "Vulkan seed wave changed its target or seed identity");
                    }
                }
                if (shared_mpf_route && matched_mpf_seed_operations !=
                                                completed_mpf_seed_operations.size()) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "MPF Vulkan seed wave retained an unbound operation receipt");
                }
                add_completed_count(
                    completed_query_sketch_dispatch_stats.completed_roundtrips());
                if (shared_mpf_route) {
                    for (const auto& operation : completed_mpf_seed_operations) {
                        add_completed_count(operation.audit().committed_submissions);
                    }
                } else {
                    if (completed_query_operation.audit().committed_submissions !=
                        completed_query_sketch_dispatch_stats.completed_roundtrips()) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error(
                            "MPS query receipt disagrees with its physical dispatch stats");
                    }
                    add_completed_count(
                        completed_seed_operation.audit().committed_submissions);
                }
                if (publish_mpf_target_stats ||
                    publish_mps_target_producer_stats) {
                    add_completed_count(
                        shared_target_execution->producer_roundtrips);
                }
                if (publish_mpf_target_stats ||
                    publish_mps_target_materialization_stats) {
                    add_materializations(
                        shared_target_execution->resident_materializations);
                }
                if (publish_mps_target_materialization_stats &&
                    shared_target_execution->completion ==
                        vulkan_seed_target_completion_kind::mps_persisted) {
                    add_persisted_ingestions(
                        shared_target_execution->resident_ingestions);
                } else if (publish_mpf_target_stats &&
                           shared_target_execution->completion ==
                               vulkan_seed_target_completion_kind::mpf_persisted) {
                    add_persisted_ingestions(
                        shared_target_execution->resident_ingestions);
                }
            }
            for (const auto& chain : completed_chaining) {
                if (chain.is_valid_for_chaining_tiled_dp()) {
                    add_completed_count(chain.audit().committed_submissions);
                }
            }

            // Check the completed jobs and counters before gathering their one-use
            // execution records for publication as a batch.
            if (ordinary_mpf_route) {
                for (auto& proof : ordinary_route_completion.row_proofs) {
                    append_mpf_query_authorization(proof.mpf_query_authorization);
                    if (proof.seed_operation.audit().kind !=
                        bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid) {
                        append_additional_operation(proof.seed_operation);
                    }
                    if (proof.chaining_operation.is_valid_for_chaining_tiled_dp()) {
                        append_additional_operation(proof.chaining_operation);
                    }
                }
            } else if (ordinary_mps_route) {
                for (std::size_t index = 0u; index < batch_size; ++index) {
                    auto& proof = ordinary_route_completion.row_proofs[index];
                    if (index == 0u) {
                        completed_query_operation = std::move(proof.query_operation);
                    } else {
                        append_additional_operation(proof.query_operation);
                    }
                    append_additional_operation(proof.seed_operation);
                    if (proof.chaining_operation.is_valid_for_chaining_tiled_dp()) {
                        append_additional_operation(proof.chaining_operation);
                    }
                }
            } else {
                if (shared_mpf_route) {
                    append_mpf_query_authorization(completed_mpf_query_authorization);
                }
                if (shared_mps_route) {
                    append_additional_operation(completed_seed_operation);
                }
                for (auto& chain : completed_chaining) {
                    if (chain.is_valid_for_chaining_tiled_dp()) {
                        append_additional_operation(chain);
                    }
                }
            }
            const std::size_t expected_mpf_query_authorizations =
                ordinary_mpf_route ? batch_size : shared_mpf_route ? 1u : 0u;
            if (completed_mpf_query_authorizations.size() !=
                expected_mpf_query_authorizations) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "MPF query-authority transaction has incomplete wave coverage");
            }

            const auto requested_source = requested_target_index_source(scanner_pg_ctx);
            const auto expected_attempted_jobs = bioseq::common::checked_add(
                stats.attempted_alignment_jobs,
                static_cast<std::uint64_t>(batch_size));
            if (!expected_attempted_jobs) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan seed wave job counter overflowed before publication");
            }
            struct wave_publication_validation {
                ::bioseq::pipeline_stats::pipeline_execution_stats staged{};
                const pipe::statement_execution_plan_owner* execution_plan = nullptr;
                p_scanner_context* scanner_pg_ctx = nullptr;
                std::vector<canonical_suffix_row_completion>* completed_rows = nullptr;
                std::vector<ordinary_vulkan_row_proof>* ordinary_proofs = nullptr;
                const vulkan_seed_target_execution* shared_target = nullptr;
                ::bioseq::pipeline_stats::target_index_source requested_source =
                    ::bioseq::pipeline_stats::target_index_source::none;
                std::uint64_t query_sketch_host_ns = 0u;
                std::uint64_t queue_roundtrips = 0u;
                std::uint64_t target_materializations = 0u;
                std::uint64_t persisted_target_ingestions = 0u;
                std::size_t expected_jobs = 0u;
                std::uint64_t expected_attempted_jobs = 0u;
                std::uint64_t broadcast_query_ordinal = 0u;
                bool per_row_query_mode = false;
                bool record_target_outcomes = false;
            } publication{
                .staged = stats,
                .execution_plan = std::addressof(execution_plan),
                .scanner_pg_ctx = scanner_pg_ctx,
                .completed_rows = std::addressof(completed_suffix_rows),
                .ordinary_proofs = ordinary_route
                    ? std::addressof(ordinary_route_completion.row_proofs) : nullptr,
                .shared_target = shared_target_execution
                    ? std::addressof(*shared_target_execution) : nullptr,
                .requested_source = requested_source,
                .query_sketch_host_ns = completed_query_sketch_host_ns,
                .queue_roundtrips = completed_queue_roundtrips,
                .target_materializations = completed_target_materializations,
                .persisted_target_ingestions = completed_persisted_target_ingestions,
                .expected_jobs = batch_size,
                .expected_attempted_jobs = *expected_attempted_jobs,
                .broadcast_query_ordinal = broadcast_query_ordinal,
                .per_row_query_mode = per_row_query_mode,
                .record_target_outcomes = record_target_operational_outcomes,
            };
            const auto validate_publication = +[](
                void* context,
                const bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot& binding,
                const bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& cache_audit) {
                auto& pending = *static_cast<wave_publication_validation*>(context);
                const std::size_t effective_workers =
                    bioseq::parallelism::global_pool::instance().maximum_concurrency();
                auto environment =
                    ::bioseq::pipeline_stats::make_persisted_grouped_soa_gpu_environment_snapshot(
                        binding, effective_workers);
                const auto cache_delta =
                    ::bioseq::pipeline_stats::make_persisted_grouped_soa_gpu_cache_metrics_delta(
                        cache_audit);
                if (!environment || !cache_delta || pending.execution_plan == nullptr ||
                    !*pending.execution_plan || pending.completed_rows == nullptr ||
                    pending.scanner_pg_ctx == nullptr ||
                    pending.expected_jobs == 0u ||
                    pending.completed_rows->size() != pending.expected_jobs ||
                    ((pending.shared_target == nullptr) ==
                     (pending.ordinary_proofs == nullptr)) ||
                    (pending.ordinary_proofs != nullptr &&
                     pending.ordinary_proofs->size() != pending.expected_jobs)) {
                    throw std::logic_error{
                        "Vulkan seed wave has no publishable execution environment"};
                }
                const auto& params = (*pending.execution_plan)->frozen_parameters;
                environment->record_highway_target(params.highway_dispatch,
                                                   params.highway_executed_target);
                add_stage_host_time(
                    pending.staged,
                    ::bioseq::pipeline_stats::pipeline_stage::query_sketch,
                    pending.query_sketch_host_ns);
                add_operational_counter(pending.staged,
                                        pending.staged.operational.gpu_queue_submits,
                                        pending.queue_roundtrips);
                add_operational_counter(pending.staged,
                                        pending.staged.operational.gpu_host_waits,
                                        pending.queue_roundtrips);
                add_operational_counter(pending.staged,
                                        pending.staged.operational.gpu_buffer_creates,
                                        pending.target_materializations);
                add_operational_counter(pending.staged,
                                        pending.staged.operational.gpu_memory_allocations,
                                        pending.target_materializations);
                add_operational_counter(
                    pending.staged,
                    pending.staged.operational.gpu_persisted_resident_uploads,
                    pending.persisted_target_ingestions);
                bool ignored_cached_query_stats = false;
                for (std::size_t index = 0u; index < pending.expected_jobs; ++index) {
                    auto& completed = (*pending.completed_rows)[index];
                    const auto* ordinary_proof = pending.ordinary_proofs != nullptr
                        ? std::addressof((*pending.ordinary_proofs)[index]) : nullptr;
                    const auto* target = pending.shared_target;
                    if (target == nullptr && ordinary_proof != nullptr) {
                        target = std::addressof(ordinary_proof->target_execution);
                    }
                    if (target == nullptr || !target->is_internally_consistent() ||
                        target->device_uuid != binding.gpu_device_uuid ||
                        target->target_source() != completed.actual_source ||
                        (ordinary_proof != nullptr &&
                         !ordinary_persisted_read_receipt_matches_source(
                             ordinary_proof->persisted_read_stats,
                             ordinary_proof->target_persisted))) {
                        throw std::logic_error{
                            "Vulkan seed target differs from the completed GPU environment"};
                    }
                    if (ordinary_proof != nullptr && ordinary_proof->target_persisted) {
                        auto& hook = pending.scanner_pg_ctx
                                         ->ordinary_vulkan_target_preparation;
                        if (!hook.valid()) {
                            throw std::logic_error{
                                "ordinary Vulkan persisted read lost its source mapper"};
                        }
                        hook.map_persisted_read_stats(
                            pending.staged.operational,
                            ordinary_proof->persisted_read_stats);
                    }
                    if (pending.record_target_outcomes) {
                        record_target_row(pending.staged);
                        record_target_index_outcome(
                            pending.staged,
                            pending.requested_source,
                            completed.actual_source);
                    }
                    record_unattested_job(
                        pending.staged,
                        *pending.execution_plan,
                        pending.requested_source,
                        completed.actual_source,
                        pending.scanner_pg_ctx,
                        completed.row,
                        pending.per_row_query_mode,
                        pending.broadcast_query_ordinal,
                        std::addressof(completed.collection),
                        /*consumed_shared_cached_query_stats=*/false,
                        ignored_cached_query_stats,
                        std::addressof(completed.seed_frontend),
                        target,
                        std::addressof(*environment));
                }
                pending.staged.merge_operational(*cache_delta);
                if (pending.staged.attempted_alignment_jobs !=
                        pending.expected_attempted_jobs ||
                    !::bioseq::pipeline_stats::pipeline_execution_stats::
                         can_retain_full_attestation(pending.staged)) {
                    throw std::logic_error{
                        "Vulkan seed wave did not stage complete per-job attestation"};
                }
            };
            const bool finalize_mpf_authorization = shared_mpf_route || ordinary_mpf_route;
            auto finalized = finalize_mpf_authorization
                ? runtime.finalize_selected_query_sketches(
                      std::span<bioseq::gpu::vulkan::device_query_sketch_authorization>{
                          completed_mpf_query_authorizations},
                      validate_publication,
                      std::addressof(publication),
                      std::span<bioseq::gpu::vulkan::runtime_pipeline_operation_token>{
                          completed_mpf_seed_operations})
                : runtime.finalize_selected_pipeline_operation(
                      std::move(completed_query_operation),
                      validate_publication,
                      std::addressof(publication),
                      std::span<bioseq::gpu::vulkan::runtime_pipeline_operation_token>{
                          completed_mpf_seed_operations});
            static_assert(std::is_nothrow_copy_assignable_v<
                          ::bioseq::pipeline_stats::pipeline_execution_stats>);
            stats = publication.staged;
            finalized.second.consume();
            if (shared_mpf_route && publish_mpf_target_stats) {
                scanner_pg_ctx->gpu_prepared_mpf_target->target_physical_stats_published = true;
            }
            if (shared_mps_route && publish_mps_target_producer_stats) {
                scanner_pg_ctx->gpu_prepared_grouped_soa_target
                    ->transient_generation_stats_published = true;
            }
            if (shared_mps_route && publish_mps_target_materialization_stats) {
                scanner_pg_ctx->gpu_prepared_grouped_soa_target
                    ->resident_materialization_stats_published = true;
            }
#else
            add_stage_host_time(stats,
                                ::bioseq::pipeline_stats::pipeline_stage::query_sketch,
                                completed_query_sketch_host_ns);
            const auto requested_source = requested_target_index_source(scanner_pg_ctx);
#if defined(BIOSEQ_TEST_INDEXES) && !defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            const bool qualify_mpf_dispatch_counters =
                selected_target_ownership == vulkan_seed_target_ownership::shared_mpf ||
                selected_target_ownership == vulkan_seed_target_ownership::ordinary_mpf;
#endif
            for (std::size_t index = 0u; index < completed_suffix_rows.size(); ++index) {
                auto& completed = completed_suffix_rows[index];
                if (record_target_operational_outcomes) {
                    record_target_row(stats);
                    record_target_index_outcome(
                        stats, requested_source, completed.actual_source);
                }
#if defined(BIOSEQ_TEST_INDEXES) && !defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                if (qualify_mpf_dispatch_counters) {
                    add_operational_counter(stats,
                                            stats.operational.gpu_seed_dispatches,
                                            completed.seed_lookup_dispatches);
                    add_operational_counter(
                        stats,
                        stats.operational.gpu_chain_dispatches,
                        completed.collection.partial_vulkan_stats.chain_score_dispatches);
                }
#endif
                record_unattested_job(stats,
                                      execution_plan,
                                      requested_source,
                                      completed.actual_source,
                                      scanner_pg_ctx,
                                      completed.row,
                                      per_row_query_mode,
                                      broadcast_query_ordinal,
                                      &completed.collection,
                                      /*consumed_shared_cached_query_stats=*/false,
                                      cached_query_minimizer_stats_accounted);
            }
#endif
            return results;
        } catch (const bioseq::storage::native_packed_materialization_unavailable&) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "Vulkan query input requires identity-oriented native packed I/O",
                "the selected query facade cannot supply its packed representation without "
                "host rank decoding or repacking",
                "Use an untransformed persisted source/flat query; host biological fallback is "
                "forbidden for the selected GPU seed stage.");
        } catch (const std::invalid_argument& err) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "Vulkan canonical seed route has no registered physical representation",
                err.what(),
                "Use a compiled alphabet/storage shape and one supported runtime k/w pair; "
                "the selected seed stage is never coerced to another record or backend.");
        } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
            add_operational_counter(stats, stats.operational.backend_declines);
            raise_vulkan_pg_error("Vulkan canonical seed route", err.what());
        }
    }

    /* Host seeding uses the row dispatcher, including the host-seed/Vulkan-chain
     * combination. A GPU-seed request must have completed in the branch above. */
    if (selected_vulkan_seed) {
        add_operational_counter(stats, stats.operational.backend_declines);
        raise_vulkan_pg_error(
            "Vulkan canonical seed route",
            "selected GPU seeding did not publish canonical anchors; host biological fallback "
            "is forbidden");
    }

    return cpu_route(batch_size,
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
                     facade_letters_per_unit,
                     per_row_query_mode,
                     record_target_operational_outcomes,
                     chunked_scanner_forced,
                     use_scanner_pg,
                     persisted_required,
                     cached_query_minimizer_stats_accounted);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
