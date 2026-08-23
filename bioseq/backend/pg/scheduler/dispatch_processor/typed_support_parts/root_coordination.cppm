/**
 * @file typed_support_parts/root_coordination.cppm
 * @brief Coordinates one typed scheduler batch while preserving generated row identities.
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

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.root_coordination;

import std;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_query_route;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_target_routes;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.active_read_policy;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
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
using std::size_t;
using std::uint8_t;
using std::uint16_t;
using std::uint64_t;
using std::vector;
}  // namespace root_std

/**
 * Executes the typed root batch around compile-time scanner and in-memory row
 * callables supplied by the generated coordinate owner.
 *
 * The callables preserve generated source identities without introducing
 * runtime dispatch. PostgreSQL exception conversion, memory permits, worker
 * unwinding, and Vulkan publication remain in their original lexical regions.
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
    const minimap2_parameters& base_params,
    const bool primary_invocation,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    p_scanner_context* scanner_pg_ctx,
    p_source_view_resolver& source_view_for_row,
    p_same_target_locator& same_target_locator,
    p_cpu_route& cpu_route) -> std::map<int32_t, std::vector<match_result>> {
    std::map<int32_t, std::vector<match_result>> results;
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
    const minimap2_parameters& dispatch_params = base_params;

    const std::optional<std::string> chunked_scanner_guc =
        pg_guc_value_as_cpp("bioseq.testing.chunked_scanner.enabled");
    const bool chunked_scanner_forced =
        chunked_scanner_guc.has_value() && *chunked_scanner_guc == "on";
    // Test parity knob. When on, the dispatch
    // loop forces the in-memory baseline (with_lazy_in_memory). Otherwise
    // (default) and given a non-null scanner_pg_context, the loop uses
    // the canonical PostgreSQL-backed facade (for stub targets,
    // in-memory for the query literal / inline targets).
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
    const bool selected_vulkan = selected_vulkan_execution(dispatch_params);
    const bool cpu_persisted_prepared = scanner_pg_ctx != nullptr &&
                                        scanner_pg_ctx->cpu_prepared_target != nullptr &&
                                        scanner_pg_ctx->gpu_prepared_grouped_soa_target == nullptr;
    const bool gpu_persisted_prepared =
        scanner_pg_ctx != nullptr && scanner_pg_ctx->cpu_prepared_target == nullptr &&
        scanner_pg_ctx->gpu_prepared_grouped_soa_target != nullptr &&
        scanner_pg_ctx->gpu_prepared_grouped_soa_target->ready();
    if (persisted_required &&
        (selected_vulkan ? !gpu_persisted_prepared : !cpu_persisted_prepared)) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            -1,
            selected_vulkan ? "GPU execution requires one retained authenticated grouped-SoA "
                              "target and forbids a CPU prepared target"
                            : "ordinal 2 CPU execution requires one retained native persisted "
                              "reader and forbids a GPU prepared target");
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
     * Query minimizers are computed once per batch for the
     * prebuilt-index path (every row reuses the same query).
     * Recompute-path rows use minimap_port_raw_pg_checked(), which does its
     * own sketching; the extra allocation here is avoided
     * for those rows via the optional nested-scope.
     */
    bool cached_query_minimizer_stats_accounted = false;
    constexpr std::size_t gpu_batch_minimum_rows = 2;
    bool persisted_transaction_active = false;

    if (selected_vulkan_execution(dispatch_params) &&
        !pipe::complete_all_gpu_tuple_selected(dispatch_params)) {
        add_operational_counter(stats, stats.operational.backend_declines);
        bioseq::gpu::vulkan::vulkan_runtime_error err{
            "requested Vulkan components do not form the complete "
            "supported all-GPU tuple; generic CPU/SIMD biological "
            "fallback is forbidden"};
        raise_vulkan_pg_error("Vulkan complete-tuple admission", err.what());
    }

    if (selected_vulkan_execution(dispatch_params) && batch_size < gpu_batch_minimum_rows) {
        add_operational_counter(stats, stats.operational.backend_declines);
        bioseq::gpu::vulkan::vulkan_runtime_error err{
            "selected Vulkan execution received a one-job wave; "
            "silent CPU/SIMD fallthrough is forbidden"};
        raise_vulkan_pg_error("Vulkan batch cardinality", err.what());
    }

    if (selected_vulkan_execution(dispatch_params) && batch_size >= gpu_batch_minimum_rows &&
        !chunked_scanner_forced) {
        const bool selected_vulkan_bounded_cigar =
            dispatch_params.do_cigar && dispatch_params.vulkan_extend_enabled &&
            selected_vulkan_coordinate_prereqs_active(dispatch_params);
        if ((!pipe::is_gpu_coordinate_only_execution_active(dispatch_params) &&
             !selected_vulkan_bounded_cigar) ||
            !pipe::is_gpu_target_index_build_active(dispatch_params)) {
            add_operational_counter(stats, stats.operational.backend_declines);
            bioseq::gpu::vulkan::vulkan_runtime_error err{
                "selected Vulkan batch requires coordinate-only projection "
                "or bounded GPU CIGAR extension, "
                "Vulkan minimizers, Vulkan tiled-DP chaining, fused frontend, "
                "and GPU target-index build"};
            raise_vulkan_pg_error("Vulkan batched resident coordinate", err.what());
        }

        auto& runtime = bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
        if (!runtime.is_usable()) {
            add_operational_counter(stats, stats.operational.backend_declines);
            bioseq::gpu::vulkan::vulkan_runtime_error err{
                "selected Vulkan batch runtime unavailable: " + runtime.initialization_error()};
            raise_vulkan_pg_error("Vulkan batched resident coordinate", err.what());
        }
        if (gpu_persisted_prepared &&
            (dispatch_params.do_cigar ||
             !pipe::is_gpu_coordinate_only_execution_active(dispatch_params))) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "persisted GPU grouped-SoA execution does not yet support CIGAR projection",
                "the direct path can execute query sketching, persisted Shape lookup, "
                "GPU chaining, backtracking, and coordinate projection, but no genuine "
                "GPU target-window reader is wired for alignment extension",
                "Use coordinate-only projection; CPU target hydration or SIMD/CPU "
                "alignment fallback is intentionally forbidden.");
        }

        try {
            persisted_transaction_active = gpu_persisted_prepared;
            vulkan_query_batch_state<p_sequence, p_batch_capacity> query_state;
            prepare_vulkan_query_route<p_sequence>(batch_size,
                                                   alphabet_code,
                                                   storage_unit_byte_width,
                                                   query_hdr,
                                                   per_row_query_hdrs,
                                                   dispatch_params,
                                                   scanner_pg_ctx,
                                                   per_row_query_mode,
                                                   source_view_for_row,
                                                   query_state);

            /* A restart-cold persisted GPU target arrives as the
             * authenticated PostgreSQL-owned grouped-SoA state. */
            auto* const prepared_gpu_target = scanner_pg_ctx != nullptr
                                                  ? scanner_pg_ctx->gpu_prepared_grouped_soa_target
                                                  : nullptr;
            if (prepared_gpu_target != nullptr) {
                process_vulkan_persisted_route<p_sequence>(batch_size,
                                                           query_ids,
                                                           dispatch_params,
                                                           broadcast_query_ordinal,
                                                           stats,
                                                           scanner_pg_ctx,
                                                           per_row_query_mode,
                                                           record_target_operational_outcomes,
                                                           gpu_persisted_prepared,
                                                           runtime,
                                                           prepared_gpu_target,
                                                           query_state,
                                                           results);
            } else {
                process_vulkan_resident_route<p_sequence>(batch_size,
                                                          alphabet_code,
                                                          storage_unit_byte_width,
                                                          facade_letters_per_unit,
                                                          query_hdr,
                                                          per_row_query_hdrs,
                                                          target_hdrs,
                                                          query_ids,
                                                          dispatch_params,
                                                          broadcast_query_ordinal,
                                                          stats,
                                                          scanner_pg_ctx,
                                                          same_target_locator,
                                                          per_row_query_mode,
                                                          record_target_operational_outcomes,
                                                          cached_query_minimizer_stats_accounted,
                                                          runtime,
                                                          query_state,
                                                          results);
            }
            return results;
        } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
            /* The persisted grouped-SoA lane is transactional. Its results,
             * stats, source identities, and lifecycle remain local on
             * every failure; do not leak a diagnostic mutation into the caller
             * while converting the exception to PostgreSQL. */
            if (!persisted_transaction_active) {
                add_operational_counter(stats, stats.operational.backend_declines);
            }
            raise_vulkan_pg_error("Vulkan batched resident coordinate", err.what());
        }
    }

    /* A biological Vulkan request may never continue into the generic
     * in-memory/chunked/scanner row dispatcher. That dispatcher is allowed to
     * compose CPU stages and is therefore not a fallback for a selected GPU
     * statement. The dedicated branch above returns on every successful run;
     * reaching this guard proves that its admission was bypassed (most notably
     * by the integration-only forced-chunked route). */
    if (selected_vulkan_execution(dispatch_params)) {
        if (!persisted_transaction_active) {
            add_operational_counter(stats, stats.operational.backend_declines);
        }
        const std::string detail =
            chunked_scanner_forced ? "selected Vulkan execution cannot use the forced chunked "
                                     "scanner; generic CPU/SIMD biological fallback is forbidden"
            : pipe::complete_all_gpu_tuple_selected(dispatch_params)
                ? "complete all-GPU tuple did not enter the dedicated resident route; generic "
                  "CPU/SIMD biological fallback is forbidden"
                : "requested Vulkan components do not form a complete supported all-GPU tuple; "
                  "generic CPU/SIMD biological fallback is forbidden";
        raise_vulkan_pg_error("Vulkan dedicated route admission", detail);
    }

    return cpu_route(batch_size,
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
                     facade_letters_per_unit,
                     per_row_query_mode,
                     record_target_operational_outcomes,
                     chunked_scanner_forced,
                     use_scanner_pg,
                     persisted_required,
                     cached_query_minimizer_stats_accounted);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
