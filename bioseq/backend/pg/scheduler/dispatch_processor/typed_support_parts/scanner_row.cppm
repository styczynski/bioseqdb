/**
 * @file typed_support_parts/scanner_row.cppm
 * @brief Defines the complete PostgreSQL-backed typed scanner-row algorithm.
 */
module;

#include <stdint.h>

extern "C" {
#include "postgres_ext.h"

#define PGSIXBIT(ch) (((ch) - '0') & 0x3F)
#define MAKE_SQLSTATE(ch1, ch2, ch3, ch4, ch5)                                                     \
    (PGSIXBIT(ch1) + (PGSIXBIT(ch2) << 6) + (PGSIXBIT(ch3) << 12) + (PGSIXBIT(ch4) << 18) +        \
     (PGSIXBIT(ch5) << 24))
#include "utils/errcodes.h"

typedef uintptr_t Datum;
typedef int16_t AttrNumber;
}

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.scanner_row;

import std;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.transient_storage;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_targets;
import bioseq.highway.dispatch_contract;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.postgres.scheduler.dispatch_processor.hash_major_diagnostic_identity;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.residency;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.pipeline.runtime_contract;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.scheduler.dispatch_processor.fragment_storage_opener;
import bioseq.postgres.storage.scanner.lazy_binding_contract;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

using scanner_attr_number = AttrNumber;
using scanner_datum = Datum;

namespace scanner_std {
using std::forward;
using std::move;
using std::optional;
using std::shared_ptr;
using std::size_t;
using std::uint16_t;
using std::uint8_t;
using std::vector;
} // namespace scanner_std

/**
 * Aligns one typed scanner batch row through PostgreSQL lazy query and target views.
 *
 * Selects a target access path from locator-backed lazy binding, an optional
 * transformed target recipe, or an inline @p target_hdr header. Index routes
 * include native persisted readers on a CPU prepared target, assembled fragment
 * or minimizer-sequence indexes from stored chunks, opportunistic RAM-first
 * builds, transient minimizer-sequence streaming, resident segment workers, and
 * raw or segmented minimap ports. Route choice follows @p scanner_pg_ctx read,
 * recompute, async-persist, and @c index_read_requirement gates.
 *
 * Query minimizers are sketched per row or taken from @p cached_query_minimizers
 * when @p per_row_query_mode is false. Every route borrows the statement's
 * frozen index and algorithm configuration; host query batches retain real owners.
 *
 * @tparam p_sequence Compact sequence type used for PostgreSQL lazy target binding.
 * @tparam p_scanner_context Scanner batch context owning per-row locators, prepared
 *     targets, index policy flags, and payload allocation observers.
 * @tparam p_fragment_storage_factory Callable that opens the concrete fragment storage handle.
 *
 * @param query_view Lazy query sequence for this row. Payload allocations are
 *     recorded through the query observer on @p scanner_pg_ctx.
 * @param target_recipe_view Optional transformed target view. Mutually exclusive with
 *     locator storage and @p target_hdr. Raises on corruption when combined.
 * @param target_hdr Inline PostgreSQL seq header when the target is not locator-backed.
 *     Unused when @p target_recipe_view or a locator is present.
 * @param row_index Batch row index into @p scanner_pg_ctx per-row arrays.
 * @param target_bp_size Target length in base pairs for segmentation thresholds and
 *     CPU prepared-target identity checks.
 * @param min_bp Legacy caller hint; the immutable statement plan already owns chain thresholds.
 * @param execution_plan Shared immutable statement selection, retained by execution closures.
 * @param per_row_query_mode When true, sketch query minimizers into row-local storage.
 *     When false, share @p cached_query_minimizers across rows in the batch.
 * @param cached_query_minimizers Batch-shared query sketch cache. Populated on the first
 *     persisted-index consumer when @p per_row_query_mode is false.
 * @param cached_query_minimizer_stats Execution stats paired with @p cached_query_minimizers.
 * @param[out] consumed_shared_cached_query_stats Set true when this row applies batch-shared
 *     sketch execution stats to the returned collection so the caller counts them once.
 * @param alphabet_code Column alphabet code for locator-backed target binding.
 * @param storage_unit_byte_width Packed storage byte width for letters-per-unit geometry.
 * @param scanner_pg_ctx Scanner context that must outlive the call. Updated with
 *     @c target_index_sources, persisted generation ids, and source identities on index routes.
 * @param pool Global parallelism pool consulted for resident segment worker eligibility.
 * @param active_read_session Optional PostgreSQL read session passed into locator target
 *     materialization.
 * @param open_fragment_storage_fn Statically bound fragment storage opener.
 *
 * @return Match results and route execution stats for the row. Query sketch host time is
 *     attached when a fresh sketch ran on this row.
 *
 * @note Raises PostgreSQL errors on policy violations, prepared-target identity mismatch,
 *     missing or duplicated persisted readers, absent query stats after sketch consumption,
 *     duplicate query stats on the collection, Vulkan on CPU-prepared paths, query length
 *     outside the selected generated minimizer position domain, invalid packed geometry, and index
 *     admission failure.
 */
// Keep the PG-backed scanner row path as a separate function so GCC does not
// compile the whole typed scheduler as one very large control-flow graph.
template <class p_sequence,
          class p_scanner_context,
          class p_fragment_storage_factory,
          class p_with_pg_target>
[[nodiscard]] match_result_collection process_scanner_pg_row_typed(
    bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type> query_view,
    std::optional<bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type>> target_recipe_view,
    const bioseq::pg_types::seq* target_hdr,
    const std::size_t row_index,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    pipe::statement_execution_plan_owner execution_plan,
    const bool per_row_query_mode,
    std::shared_ptr<const std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>& cached_query_minimizer_stats,
    bool& consumed_shared_cached_query_stats,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    p_scanner_context* scanner_pg_ctx,
    bioseq::parallelism::global_pool& pool,
    pg_sequence_read_session* active_read_session,
    p_fragment_storage_factory& open_fragment_storage_fn,
    p_with_pg_target& with_pg_target_fn) {
    if (!execution_plan) {
        throw std::invalid_argument("scanner row requires a statement plan owner");
    }
    (void)min_bp;
    using query_alphabet =
        typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    using query_storage_unit =
        typename compact_sequence_range<p_sequence>::traits::storage_unit;
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    const bool selected_vulkan_seed = execution_plan->uses_vulkan_seed();
    match_result_collection collection;
    // Recompute-owned and per-row sketches are independent operations. A
    // broadcast persisted-query sketch is shared across rows: every consuming
    // job receives its stats for route acceptance, while the caller accounts
    // the shared operation only once on the first stats-valid consumer.
    consumed_shared_cached_query_stats = false;
    bool row_consumed_prepared_query_sketch = false;
    std::uint64_t query_sketch_host_wall_ns = 0;
    const auto cpu_prepared_target = scanner_pg_ctx->cpu_prepared_target;
    if (cpu_prepared_target != nullptr) {
        if (selected_vulkan_seed) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "GPU seeding cannot consume this host query-preparation path");
        }
        if (!cpu_prepared_target->unions_ready ||
            cpu_prepared_target->target_rowno != scanner_pg_ctx->target_rownos[row_index] ||
            cpu_prepared_target->target_bp_size != static_cast<std::uint64_t>(target_bp_size)) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq batch join CPU prepared-target identity is invalid");
        }
    }
    scanner_pg_ctx->target_index_sources[row_index] =
        ::bioseq::pipeline_stats::target_index_source::recomputed;
    std::shared_ptr<const std::vector<minimap_sketch_data>> row_query_minimizers;
    std::optional<pipe::minimizer_range_execution_stats> row_query_minimizer_stats;
    /** Element type of retained query sketches stored on a CPU prepared-target group. */
    using cpu_prepared_query_sketch =
        std::remove_cvref_t<decltype(scanner_pg_ctx->cpu_prepared_target->groups.front()
                                         .retained_query_sketches.front())>;
    const cpu_prepared_query_sketch* retained_prepared_query_sketch = nullptr;
    const sequence_size_t query_bp_size = query_view.bp_size();
    const auto unobserved_query_view = query_view;
    const auto lazy_query_view = unobserved_query_view.with_payload_allocation_observer(
        std::addressof(scanner_pg_ctx->mmsearch_query_payload_observer));
    const auto fq_pg_view = lazy_query_view;
    const std::size_t target_letters_per_unit = bioseq::storage::packed_letters_per_unit(
        storage_unit_byte_width, static_cast<std::uint8_t>(p_sequence::letter_bit_width));
    if (target_letters_per_unit == 0u) {
        raise_typed_dispatch_pg_error_as_cpp(ERRCODE_INTERNAL_ERROR,
                                             "PG lazy facade has invalid packed storage geometry");
    }
    const auto& loc_opt = scanner_pg_ctx->target_locators[row_index];
    const bool persisted_required =
        scanner_pg_ctx->index_read_requirement ==
        bioseq::indexing::index_read_requirement::require_compatible_persisted;
    if (persisted_required &&
        (cpu_prepared_target == nullptr || selected_vulkan_seed)) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[row_index],
            "the live persisted reader is available only through the retained "
            "CPU prepared-target lifecycle; GPU and ad-hoc row fallback are forbidden");
    }
    if (persisted_required && !loc_opt.has_value()) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[row_index],
            "target payload is inline; persisted minimizer storage requires "
            "a locator-backed target");
    }
    if (persisted_required && scanner_pg_ctx->target_rownos[row_index] < 0) {
        raise_required_persisted_index_error(scanner_pg_ctx,
                                             scanner_pg_ctx->target_rownos[row_index],
                                             "logical target row identity is unavailable");
    }
    if (target_recipe_view.has_value()) {
        if (loc_opt.has_value() || target_hdr != nullptr) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED, "target recipe is not exclusive with flat/locator storage");
        }
        if (persisted_required) {
            raise_required_persisted_index_error(
                scanner_pg_ctx,
                scanner_pg_ctx->target_rownos[row_index],
                "a transformed target has no compatible persisted base-row index");
        }
        const auto observed_target = target_recipe_view->with_payload_allocation_observer(
            std::addressof(scanner_pg_ctx->mmsearch_target_payload_observer));
        collection =
            target_bp_size > postgres_segmentation_threshold_bp
                ? minimap_port_segmented_pg_checked(lazy_query_view, observed_target, execution_plan)
                : minimap_port_raw_pg_checked(lazy_query_view, observed_target, execution_plan);
    } else if (loc_opt.has_value()) {
        // Compute last-fragment padding from the canonical facade
        // geometry. For
        // total_bp = N, padding = ceil(N/lpu)*lpu - N.
        const std::uint64_t _remainder = loc_opt->logical_length % target_letters_per_unit;
        const std::size_t _padding =
            static_cast<std::size_t>(_remainder == 0u ? 0u : target_letters_per_unit - _remainder);
        with_pg_target_fn(
            *scanner_pg_ctx,
            *loc_opt,
            alphabet_code,
            storage_unit_byte_width,
            static_cast<std::uint16_t>(_padding),
            active_read_session,
            [scanner_pg_ctx,
             cpu_prepared_target,
             execution_plan,
             &params,
             per_row_query_mode,
             persisted_required,
             &row_query_minimizers,
             &cached_query_minimizers,
             &row_query_minimizer_stats,
             &cached_query_minimizer_stats,
             query_bp_size,
             lazy_query_view,
             fq_pg_view,
             target_bp_size,
             alphabet_code,
             storage_unit_byte_width,
             row_index,
             &loc_opt,
             _padding,
             &pool,
             target_letters_per_unit,
             &collection,
             &row_consumed_prepared_query_sketch,
             &retained_prepared_query_sketch,
             &open_fragment_storage_fn,
             &query_sketch_host_wall_ns](auto ft_pg_view, pg_sequence_read_session*) {
                ft_pg_view = ft_pg_view.with_payload_allocation_observer(
                    std::addressof(scanner_pg_ctx->mmsearch_target_payload_observer));
                // Assemble from the stored
                // per-fragment minimizer index if present
                // (deserialise + globalise, no re-collection).
                // Otherwise fall back to recompute.
                bool assembled = false;
                /*
                 * Persisted-payload reads and opportunistic recompute persistence are
                 * independent policies.  In particular, require_absent deliberately
                 * clears read_index_active, but index.recompute_async_persistence.enabled must
                 * still be able to build and enqueue the default minimizer-fragment index from the
                 * source sequence. Keep the minimizer-fragment RAM-first path reachable when both
                 * async write gates are captured for this scan. Every prepared,
                 * sidecar, and assembled-cache read below remains guarded by
                 * read_index_active.
                 */
                const auto requested_target_backend =
                    target_backend_from_params_for_recompute(params);
                const bool stored_minimizer_scheme_compatible =
                    requested_target_backend !=
                    bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd;
                if (scanner_pg_ctx->read_index_active && !stored_minimizer_scheme_compatible) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] index: persisted read skipped for inapplicable storage profile "
                        "row={} "
                        "backend=bounded_nthash_simd reason=payload_has_no_scheme_identity",
                        static_cast<unsigned long long>(scanner_pg_ctx->target_rownos[row_index]));
                }
                const bool opportunistic_minimizer_fragment_recompute_requested =
                    !scanner_pg_ctx->read_index_active &&
                    scanner_pg_ctx->recompute_async_persist_active &&
                    scanner_pg_ctx->index_async_persist_active &&
                    stored_minimizer_scheme_compatible &&
                    scanner_pg_ctx->active_index == minimizers_per_fragment_index_name;
                const bool transient_minimizer_generation_requested =
                    !persisted_required &&
                    (scanner_pg_ctx->active_index == minimizers_per_fragment_index_name ||
                     scanner_pg_ctx->active_index == minimizers_per_sequence_index_name);
                const minimap2_parameters& idx_params = execution_plan->frozen_parameters;
                const bool generic_raw_optional_scalar =
                    pipe::use_generic_raw_optional_scalar_minimizer_route<query_alphabet>(
                        idx_params, scanner_pg_ctx->index_read_requirement);
                const bool generated_minimizer_route_requested =
                    !generic_raw_optional_scalar &&
                    (persisted_required ||
                     (scanner_pg_ctx->read_index_active && stored_minimizer_scheme_compatible) ||
                     opportunistic_minimizer_fragment_recompute_requested ||
                     transient_minimizer_generation_requested);
                std::optional<::bioseq::minimizers::minimizer_physical_layout_selection>
                    selected_query_execution_layout;
                bool legacy_query_destination_selected = false;
                if (generated_minimizer_route_requested &&
                    scanner_pg_ctx->user_relid != InvalidOid) {
                    if (idx_params.kmer_size <= 0 || idx_params.window_size <= 0 ||
                        static_cast<std::uint64_t>(idx_params.kmer_size) >
                            (std::numeric_limits<std::uint16_t>::max)() ||
                        static_cast<std::uint64_t>(idx_params.window_size) >
                            (std::numeric_limits<std::uint16_t>::max)()) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_FEATURE_NOT_SUPPORTED,
                            "persisted query minimizer k/w is outside the generated registry");
                    }
                    const bool strict_generated_route =
                        persisted_required ||
                        scanner_pg_ctx->index_read_requirement ==
                            bioseq::indexing::index_read_requirement::require_absent;
                    if (strict_generated_route) {
                        selected_query_execution_layout =
                            pipe::select_minimizer_execution_layout_prefer_native<query_alphabet>(
                                idx_params.minimizer_algorithm,
                                static_cast<std::uint64_t>(query_bp_size),
                                static_cast<std::uint16_t>(idx_params.kmer_size),
                                static_cast<std::uint16_t>(idx_params.window_size),
                                static_cast<std::uint8_t>(sizeof(query_storage_unit) * 8u));
                    } else {
                        selected_query_execution_layout =
                            pipe::try_select_legacy_minimap_sketch_data_execution_layout<
                                query_alphabet>(
                                idx_params.minimizer_algorithm,
                                static_cast<std::uint64_t>(query_bp_size),
                                static_cast<std::uint16_t>(idx_params.kmer_size),
                                static_cast<std::uint16_t>(idx_params.window_size),
                                static_cast<std::uint8_t>(sizeof(query_storage_unit) * 8u));
                        if (selected_query_execution_layout) {
                            legacy_query_destination_selected = true;
                        } else {
                            /* A canonical registered shape whose record is not
                             * minimap_sketch_data (for example scalar K20)
                             * keeps its exact generated physical route. */
                            selected_query_execution_layout =
                                pipe::select_minimizer_execution_layout_prefer_native<
                                    query_alphabet>(
                                    idx_params.minimizer_algorithm,
                                    static_cast<std::uint64_t>(query_bp_size),
                                    static_cast<std::uint16_t>(idx_params.kmer_size),
                                    static_cast<std::uint16_t>(idx_params.window_size),
                                    static_cast<std::uint8_t>(sizeof(query_storage_unit) * 8u));
                        }
                    }
                }
                if (selected_query_execution_layout) {
                    const bool want_seq_recompute =
                        scanner_pg_ctx->active_index == minimizers_per_sequence_index_name;
                    const bool want_seq = want_seq_recompute;
                    const bool want_fragment =
                        scanner_pg_ctx->active_index == minimizers_per_fragment_index_name;
                    auto with_fragment_storage =
                        [&open_fragment_storage_fn, scanner_pg_ctx, &loc_opt, row_index]<
                            class p_callback>(std::string_view index_name,
                                              p_callback&& callback) {
                            using callback_type = std::remove_reference_t<p_callback>;
                            open_fragment_storage_fn(
                                scanner_pg_ctx->user_relid,
                                scanner_pg_ctx->db_oid,
                                scanner_pg_ctx->tablespace_oid,
                                scanner_pg_ctx->main_relfilenode,
                                scanner_pg_ctx->mmsearch_seq_attno,
                                index_name,
                                *loc_opt,
                                static_cast<std::uint64_t>(
                                    scanner_pg_ctx->target_rownos[row_index]),
                                scanner_pg_ctx->caches,
                                scanner_pg_ctx->parent_mctx,
                                std::addressof(callback),
                                +[](void* callback_context,
                                    typed_dispatch_fragment_read_view& storage) {
                                    (*static_cast<callback_type*>(callback_context))(storage);
                                });
                        };
                    const auto& query_execution_layout = *selected_query_execution_layout;
                    const auto selected_query_backend =
                        pipe::minimizer_sketch_backend_for_effective_algorithm(
                            query_execution_layout.effective_algorithm);
                    if (!selected_query_backend ||
                        *selected_query_backend == minimizer_sketch_backend::vulkan_segment) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_FEATURE_NOT_SUPPORTED,
                            "scanner host-seed route has no generated host minimizer "
                            "implementation; GPU target ownership is external");
                    }
                    std::optional<bioseq::indexing::gpu_minimizer_index_cache> pg_gpu_index_cache;
                    if (pg_vulkan_seed_lookup_selected(idx_params)) {
                        pg_gpu_index_cache.emplace(pg_gpu_index_resident_limit_bytes());
                    }
                    auto* pg_gpu_index_cache_ptr =
                        pg_gpu_index_cache ? &*pg_gpu_index_cache : nullptr;
                    const std::uint64_t target_rowno =
                        static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]);
                    auto& query_minimizers =
                        per_row_query_mode ? row_query_minimizers : cached_query_minimizers;
                    auto& query_minimizer_stats = per_row_query_mode ? row_query_minimizer_stats
                                                                     : cached_query_minimizer_stats;
                    auto ensure_q = [&query_minimizers,
                                     &query_minimizer_stats,
                                     query_bp_size,
                                     lazy_query_view,
                                     &query_execution_layout,
                                     selected_query_backend,
                                     execution_plan,
                                     &idx_params,
                                     &row_consumed_prepared_query_sketch,
                                     &query_sketch_host_wall_ns] {
                        if (!query_minimizers) {
                            if (!pipe::minimizer_execution_layout_matches_record<
                                    minimap_sketch_data,
                                    query_alphabet>(
                                    query_execution_layout,
                                    static_cast<std::uint16_t>(idx_params.kmer_size),
                                    static_cast<std::uint16_t>(idx_params.window_size)) ||
                                !is_minimizer_query_length_representable<minimap_sketch_data>(
                                    static_cast<sequence_size_t>(query_bp_size))) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_FEATURE_NOT_SUPPORTED,
                                    "legacy query sketch storage cannot represent the selected "
                                    "generated minimizer record");
                            }
                            const std::uint64_t query_sketch_started_ns = dispatch_host_now_ns();
                            if (execution_plan->uses_vulkan_seed()) {
                                raise_vulkan_pg_error(
                                    "persisted query minimizer sketch",
                                    "persisted Vulkan query sketch cannot use the CPU "
                                    "prepared-index path");
                            }
                            sketch_collector query_collector;
                            const minimizer_sketch_backend sketch_backend = *selected_query_backend;
                            auto sketch_stats = pipe::minimizer_dispatch_range<minimap_sketch_data>(
                                query_collector,
                                lazy_query_view,
                                0,
                                query_bp_size,
                                static_cast<std::size_t>(idx_params.window_size),
                                static_cast<std::size_t>(idx_params.kmer_size),
                                sketch_backend,
                                pipe::exact_simd_range_route::require_continuation,
                                idx_params.highway_kernels,
                                pipe::exact_simd_input_storage::preserve_native_width,
                                nullptr,
                                &query_execution_layout);
                            if (!pipe::minimizer_range_stats_matches_effective_algorithm(
                                    query_execution_layout.effective_algorithm,
                                    sketch_stats,
                                    /*require_nonempty_vector_work=*/false,
                                    pipe::exact_simd_range_route::require_continuation)) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_INTERNAL_ERROR,
                                    "persisted query sketch stats do not match the selected "
                                    "minimizer backend");
                            }
                            query_minimizers =
                                std::make_shared<const std::vector<minimap_sketch_data>>(
                                    std::move(query_collector).extract_sketches_data());
                            query_minimizer_stats.emplace(std::move(sketch_stats));
                            query_sketch_host_wall_ns =
                                elapsed_dispatch_host_ns(query_sketch_started_ns);
                        } else if (!query_minimizer_stats.has_value()) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "cached persisted query minimizers have no operation stats");
                        }
                        row_consumed_prepared_query_sketch = true;
                    };
                    auto* prepared_group =
                        cpu_prepared_target == nullptr
                            ? nullptr
                            : cpu_prepared_target->find_group(
                                  idx_params.kmer_size,
                                  idx_params.window_size,
                                  idx_params.max_occ,
                                  *selected_query_backend);
                    if (cpu_prepared_target != nullptr && prepared_group == nullptr) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_DATA_CORRUPTED,
                            "bioseq batch join prepared query group is missing");
                    }
                    if (prepared_group != nullptr) {
                        const auto& prepared_layout = prepared_group->execution_layout;
                        const bool same_generated_identity =
                            prepared_layout.requested_algorithm ==
                                query_execution_layout.requested_algorithm &&
                            prepared_layout.effective_algorithm ==
                                query_execution_layout.effective_algorithm &&
                            prepared_layout.alphabet_id == query_execution_layout.alphabet_id &&
                            prepared_layout.physical_kmer_size ==
                                query_execution_layout.physical_kmer_size &&
                            prepared_layout.hash_width == query_execution_layout.hash_width &&
                            prepared_layout.packed_storage_bits ==
                                query_execution_layout.packed_storage_bits &&
                            prepared_layout.runtime_kmer_size ==
                                query_execution_layout.runtime_kmer_size &&
                            prepared_layout.runtime_window_size ==
                                query_execution_layout.runtime_window_size &&
                            prepared_layout.record_position_log2 >=
                                query_execution_layout.record_position_log2;
                        if (!same_generated_identity) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "bioseq batch join prepared query group has another generated "
                                "minimizer identity");
                        }
                    }
                    const bool prepared_physical_query_destination =
                        prepared_group != nullptr &&
                        prepared_group->query_hash_union.capacity() == 0u &&
                        !prepared_group->retained_query_sketches.empty();
                    const bool prepared_generated_query_route =
                        !persisted_required && prepared_physical_query_destination &&
                        transient_minimizer_generation_requested;
                    const bool legacy_optional_index_read_active =
                        scanner_pg_ctx->read_index_active &&
                        stored_minimizer_scheme_compatible &&
                        legacy_query_destination_selected &&
                        !prepared_physical_query_destination;
                    if (prepared_generated_query_route) {
                        if (prepared_group == nullptr || cpu_prepared_target == nullptr ||
                            !scanner_pg_ctx->explicit_query_ordinals ||
                            static_cast<unsigned>(want_fragment) +
                                    static_cast<unsigned>(want_seq) !=
                                1u) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "generated host route has no exclusive prepared target group");
                        }
                        retained_prepared_query_sketch =
                            prepared_group->find_retained_query_sketch(
                                scanner_pg_ctx->query_ordinals[row_index]);
                        if (retained_prepared_query_sketch == nullptr ||
                            retained_prepared_query_sketch->query_bp_size !=
                                static_cast<std::uint64_t>(query_bp_size) ||
                            !retained_prepared_query_sketch->minimizers.valid() ||
                            retained_prepared_query_sketch->minimizers.layout() !=
                                prepared_group->execution_layout ||
                            !pipe::minimizer_range_stats_matches_effective_algorithm(
                                prepared_group->execution_layout.effective_algorithm,
                                retained_prepared_query_sketch->execution_stats,
                                /*require_nonempty_vector_work=*/false,
                                pipe::exact_simd_range_route::require_continuation)) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "no-index retained query owner does not match its generated "
                                "layout");
                        }
                        if (want_seq) {
                            collection =
                                build_and_align_prepared_transient_minimizer_sequence(
                                    fq_pg_view,
                                    ft_pg_view,
                                    execution_plan,
                                    static_cast<sequence_size_t>(loc_opt->logical_length),
                                    alphabet_code,
                                    storage_unit_byte_width,
                                    loc_opt->source_epoch,
                                    *retained_prepared_query_sketch,
                                    *prepared_group,
                                    *cpu_prepared_target);
                        } else {
                            collection =
                                build_and_align_prepared_transient_minimizer_fragment(
                                    fq_pg_view,
                                    ft_pg_view,
                                    execution_plan,
                                    static_cast<sequence_size_t>(loc_opt->logical_length),
                                    alphabet_code,
                                    storage_unit_byte_width,
                                    *retained_prepared_query_sketch,
                                    *prepared_group,
                                    *cpu_prepared_target);
                        }
                        row_consumed_prepared_query_sketch = true;
                        assembled = true;
                        scanner_pg_ctx->target_index_sources[row_index] =
                            ::bioseq::pipeline_stats::target_index_source::recomputed;
                    }
                    if (persisted_required) {
                        const bool has_fragment_reader =
                            prepared_group != nullptr &&
                            prepared_group->persisted_fragment_reader.has_value();
                        const bool has_fragment_mapping_guard =
                            prepared_group != nullptr &&
                            prepared_group->persisted_fragment_mapping_guard.has_value() &&
                            static_cast<bool>(*prepared_group->persisted_fragment_mapping_guard);
                        const bool has_grouped_soa_reader =
                            prepared_group != nullptr &&
                            prepared_group->persisted_grouped_soa_reader.has_value();
                        const bool has_hash_major_reader =
                            prepared_group != nullptr &&
                            prepared_group->persisted_hash_major_reader.has_value();
                        const bool has_full_hash_major_source_identity =
                            prepared_group != nullptr &&
                            prepared_group->persisted_hash_major_source_identity_present;
                        const bool hash_major_identity_valid =
                            prepared_group == nullptr ||
                            (!has_hash_major_reader
                                 ? !has_full_hash_major_source_identity &&
                                       prepared_group->persisted_hash_major_source_identity ==
                                           std::array<std::byte, 16>{}
                                 : has_full_hash_major_source_identity &&
                                       prepared_group->persisted_source_identity ==
                                           hash_major_source_diagnostic_token(
                                               prepared_group
                                                   ->persisted_hash_major_source_identity));
                        const bool canonical_owner_exclusive =
                            prepared_group == nullptr ||
                            prepared_group->query_hash_union.capacity() == 0u;
                        bool hash_major_read_receipt_valid = !has_hash_major_reader;
                        if (has_hash_major_reader) {
                            const auto& receipt = prepared_group->persisted_read_stats.hash_major;
                            const auto& memory =
                                prepared_group->persisted_hash_major_reader->memory_stats();
                            const std::uint64_t stage_peak = std::max({
                                receipt.directory_stage_peak_bytes,
                                receipt.control_stage_peak_bytes,
                                receipt.posting_stage_peak_bytes,
                            });
                            hash_major_read_receipt_valid =
                                receipt.layout_revision ==
                                    bioseq::indexing::k_hash_major_minimizer_layout_revision &&
                                receipt.sessions == 1u &&
                                receipt.generation_id == prepared_group->persisted_generation_id &&
                                receipt.source_identity ==
                                    prepared_group->persisted_hash_major_source_identity &&
                                receipt.directory_row_crc_receipts == receipt.directory_rows &&
                                receipt.control_row_crc_receipts == receipt.control_rows &&
                                receipt.posting_row_crc_receipts == receipt.posting_rows &&
                                receipt.control_block_crc_receipts ==
                                    receipt.selected_block_count &&
                                receipt.control_semantic_receipts == receipt.selected_block_count &&
                                receipt.posting_ranges_delivered ==
                                    receipt.posting_semantic_receipts &&
                                receipt.posting_range_crc_receipts <=
                                    receipt.posting_ranges_delivered &&
                                receipt.query_count == memory.query_count &&
                                receipt.accepted_hash_count == memory.accepted_hash_count &&
                                receipt.retained_position_count == memory.retained_position_count &&
                                receipt.peak_admitted_bytes == stage_peak &&
                                receipt.retained_owner_bytes == receipt.admitted_bytes &&
                                receipt.admitted_bytes ==
                                    prepared_group->persisted_hash_major_reader->admitted_bytes() &&
                                receipt.generic_source_fallback_calls == 0u &&
                                receipt.failures == 0u && !receipt.saturated;
                        }
                        if (prepared_group == nullptr ||
                            static_cast<unsigned>(has_fragment_reader) +
                                    static_cast<unsigned>(has_grouped_soa_reader) +
                                    static_cast<unsigned>(has_hash_major_reader) !=
                                1u ||
                            has_fragment_reader != has_fragment_mapping_guard ||
                            (has_fragment_reader &&
                             !prepared_group->persisted_fragment_reader->ready()) ||
                            (has_grouped_soa_reader &&
                             !prepared_group->persisted_grouped_soa_reader->ready()) ||
                            prepared_group->target_index ||
                            prepared_group->target_index_source !=
                                ::bioseq::pipeline_stats::target_index_source::persisted ||
                            prepared_group->persisted_generation_id == 0 ||
                            prepared_group->persisted_source_identity == 0 ||
                            !hash_major_identity_valid || !canonical_owner_exclusive ||
                            !hash_major_read_receipt_valid) {
                            raise_required_persisted_index_error(
                                scanner_pg_ctx,
                                scanner_pg_ctx->target_rownos[row_index],
                                "the exact READY native persisted reader is absent, incomplete, "
                                "duplicated, or "
                                "mixed with a storage-profile-transcoded assembled index");
                        }
                        if (!scanner_pg_ctx->explicit_query_ordinals) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "persisted native query execution has no prepared-query ordinal");
                        }
                        retained_prepared_query_sketch =
                            prepared_group->find_retained_query_sketch(
                                scanner_pg_ctx->query_ordinals[row_index]);
                        if (retained_prepared_query_sketch == nullptr ||
                            retained_prepared_query_sketch->query_bp_size !=
                                static_cast<std::uint64_t>(query_bp_size) ||
                            !retained_prepared_query_sketch->minimizers.valid() ||
                            retained_prepared_query_sketch->minimizers.layout() !=
                                prepared_group->execution_layout ||
                            !pipe::minimizer_range_stats_matches_effective_algorithm(
                                prepared_group->execution_layout.effective_algorithm,
                                retained_prepared_query_sketch->execution_stats,
                                /*require_nonempty_vector_work=*/false,
                                pipe::exact_simd_range_route::require_continuation)) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "retained native query owner does not match the selected generated "
                                "minimizer identity");
                        }
                        row_consumed_prepared_query_sketch = true;
                        const auto& alignment_query_minimizers =
                            retained_prepared_query_sketch->minimizers;
                        if (has_hash_major_reader) {
                            collection = minimap_port_hash_major_pg_checked(
                                fq_pg_view,
                                ft_pg_view,
                                alignment_query_minimizers,
                                *prepared_group->persisted_hash_major_reader,
                                execution_plan,
                                /*gpu_index_cache=*/nullptr,
                                bioseq::indexing::gpu_index_cache_identity{});
                        } else if (has_fragment_reader) {
                            collection = run_aligner_pg_checked(
                                "fragment-native single-query anchor preparation",
                                [retained_prepared_query_sketch,
                                 query_bp_size,
                                 execution_plan,
                                 &idx_params,
                                 prepared_group,
                                 fq_pg_view,
                                 ft_pg_view]() -> match_result_collection {
                                    const std::uint64_t prepare_started_ns = dispatch_host_now_ns();
                                    auto& admission = bioseq::parallelism::
                                        memory_admission_controller::instance();
                                    auto scratch_result =
                                        bioseq::indexing::query_wave_scratch::create(admission);
                                    if (!scratch_result) {
                                        throw bioseq::indexing::persisted_minimizer_lookup_error(
                                            "fragment-native query scratch was not "
                                            "admitted; wire_error=" +
                                            std::to_string(
                                                static_cast<unsigned>(scratch_result.error())));
                                    }
                                    auto scratch = std::move(*scratch_result);
                                    const bioseq::indexing::fragment_seed_query fragment_query{
                                        .minimizers =
                                            retained_prepared_query_sketch->minimizers.view(),
                                        .query_length = static_cast<sequence_size_t>(query_bp_size),
                                        .kmer_size = idx_params.kmer_size,
                                    };
                                    fragment_anchor_wave_completion wave_completion{scratch};
                                    auto prepared_seed_batch =
                                        prepared_group->persisted_fragment_reader
                                            ->prepare_fragment_anchor_batch(
                                                admission,
                                                std::span<
                                                    const bioseq::indexing::fragment_seed_query>{
                                                    &fragment_query, 1u},
                                                idx_params.max_occ,
                                                scratch,
                                                /*baseline_query_plan_matcher_for_testing=*/false,
                                                /*verify_compositional_order_for_testing=*/false,
                                                prepared_group->persisted_fragment_lookup_route);
                                    const auto prepared_query_index =
                                        prepared_seed_batch.query_view(0u);
                                    const std::uint64_t prepare_host_wall_ns =
                                        elapsed_dispatch_host_ns(prepare_started_ns);
                                    match_result_collection match_collection =
                                        minimap_port_fragment_query_owned(
                                            fq_pg_view,
                                            ft_pg_view,
                                            retained_prepared_query_sketch->minimizers,
                                            prepared_query_index,
                                            execution_plan,
                                            /*gpu_index_cache=*/nullptr,
                                            bioseq::indexing::gpu_index_cache_identity{});
                                    match_collection.record_seed_lookup_host_time(
                                        prepare_host_wall_ns);
                                    match_collection.record_fragment_native_query_stats(
                                        prepared_seed_batch.stats());
                                    return match_collection;
                                });
                        } else {
                            collection = minimap_port_grouped_soa_pg_checked(
                                fq_pg_view,
                                ft_pg_view,
                                alignment_query_minimizers,
                                *prepared_group->persisted_grouped_soa_reader,
                                execution_plan,
                                /*gpu_index_cache=*/nullptr,
                                bioseq::indexing::gpu_index_cache_identity{});
                        }
                        if (prepared_group->persisted_reader_queries == 0u &&
                            prepared_group->persisted_reader_load_host_wall_ns != 0u) {
                            collection.record_persisted_index_decode_host_time(
                                prepared_group->persisted_reader_load_host_wall_ns);
                        }
                        ++prepared_group->persisted_reader_queries;
                        assembled = true;
                        scanner_pg_ctx->target_index_sources[row_index] =
                            ::bioseq::pipeline_stats::target_index_source::persisted;
                        scanner_pg_ctx->persisted_generation_ids[row_index] =
                            prepared_group->persisted_generation_id;
                        scanner_pg_ctx->persisted_source_identities[row_index] =
                            prepared_group->persisted_source_identity;
                    }
                    if (legacy_optional_index_read_active && !persisted_required &&
                        prepared_group != nullptr && prepared_group->target_index) {
                        ensure_q();
                        collection = minimap_port_fragment_index_vector_pg_checked(
                            fq_pg_view,
                            ft_pg_view,
                            query_minimizers,
                            *prepared_group->target_index,
                            execution_plan,
                            /*gpu_index_cache=*/nullptr,
                            bioseq::indexing::gpu_index_cache_identity{
                                .assembly_identity = prepared_group->key_set_checksum});
                        ++prepared_group->target_index_reuses;
                        assembled = true;
                        scanner_pg_ctx->target_index_sources[row_index] =
                            prepared_group->target_index_source;
                    }
                    auto make_cache_key = [scanner_pg_ctx,
                                           &query_minimizers,
                                           selected_query_backend,
                                           &idx_params,
                                           row_index](std::string_view index_name,
                                                      sequence_size_t clip_lo_bp,
                                                      sequence_size_t clip_hi_bp) {
                        const auto query_span = std::span<const minimap_sketch_data>(
                            query_minimizers->data(), query_minimizers->size());
                        const auto sketch_backend = *selected_query_backend;
                        return bioseq::storage::indexing::assembled_index_key{
                            .db_oid = scanner_pg_ctx->db_oid,
                            .user_relid = scanner_pg_ctx->user_relid,
                            .tablespace_oid = scanner_pg_ctx->tablespace_oid,
                            .main_relfilenode = scanner_pg_ctx->main_relfilenode,
                            .column_attno = scanner_pg_ctx->mmsearch_seq_attno,
                            .index_name_hash = bioseq::storage::indexing::index_name_hash(
                                scanner_pg_ctx->db_oid,
                                scanner_pg_ctx->tablespace_oid,
                                scanner_pg_ctx->main_relfilenode,
                                scanner_pg_ctx->mmsearch_seq_attno,
                                index_name),
                            .sketch_backend = sketch_backend,
                            .minimizer_scheme =
                                bioseq::indexing::minimizer_scheme_identity_for_backend(
                                    sketch_backend),
                            .query_key = static_cast<std::uint64_t>(
                                scanner_pg_ctx->target_rownos[row_index]),
                            .query_signature = bioseq::storage::indexing::query_minimizer_signature(
                                query_span,
                                static_cast<std::uint32_t>(idx_params.kmer_size),
                                static_cast<std::uint32_t>(idx_params.window_size),
                                static_cast<std::uint32_t>(idx_params.max_occ)),
                            .clip_lo_bp = clip_lo_bp,
                            .clip_hi_bp = clip_hi_bp,
                        };
                    };
                    auto run_fragment_backed_index = [&ensure_q,
                                                      &loc_opt,
                                                      execution_plan,
                                     &idx_params,
                                                      query_bp_size,
                                                      &query_minimizers,
                                                      &collection,
                                                      &assembled,
                                                      &make_cache_key,
                                                      fq_pg_view,
                                                      ft_pg_view,
                                                      target_letters_per_unit,
                                                      pg_gpu_index_cache_ptr,
                                                      prepared_group,
                                                      cpu_prepared_target](
                                                         typed_dispatch_fragment_read_view&
                                                             fragment_store,
                                                         std::string_view cache_index_name) {
                        ensure_q();
                        const sequence_size_t total_bp = loc_opt->logical_length;
                        const std::size_t a_lpu = target_letters_per_unit;
                        constexpr sequence_size_t frag_bp = static_cast<sequence_size_t>(
                            bioseq::storage::postgres_lazy_sequence_binding<
                                p_sequence>::fragment_bp_capacity());
                        static_assert(frag_bp != 0u);
                        const std::size_t size_u =
                            static_cast<std::size_t>((total_bp + a_lpu - 1) / a_lpu);
                        bioseq::indexing::minimizer_assembly_scratch scratch;
                        const auto& assembly_minimizers = prepared_group == nullptr
                                                              ? *query_minimizers
                                                              : prepared_group->query_hash_union;
                        auto assemble_one = [a_lpu,
                                             total_bp,
                                             frag_bp,
                                             &fragment_store,
                                             &scratch,
                                             &assembly_minimizers](std::size_t cs_u,
                                                                   std::size_t ce_u) {
                            const sequence_size_t cs_bp =
                                static_cast<sequence_size_t>(cs_u) * a_lpu;
                            const sequence_size_t ce_bp =
                                std::min(static_cast<sequence_size_t>(ce_u) * a_lpu, total_bp);
                            const std::size_t frag_lo = static_cast<std::size_t>(cs_bp / frag_bp);
                            const std::size_t frag_hi =
                                static_cast<std::size_t>(bioseq::common::ceil_div(ce_bp, frag_bp));
                            const auto query_span = std::span<const minimap_sketch_data>(
                                assembly_minimizers.data(), assembly_minimizers.size());
                            return fragment_store.assemble_fragment_segment_for_query(query_span,
                                                                                      frag_lo,
                                                                                      frag_hi,
                                                                                      frag_bp,
                                                                                      total_bp,
                                                                                      cs_bp,
                                                                                      ce_bp,
                                                                                      scratch);
                        };
                        auto run_with_segment_index = [&make_cache_key,
                                                       &assemble_one,
                                                       fq_pg_view,
                                                       ft_pg_view,
                                                       &query_minimizers,
                                                       execution_plan,
                                     &idx_params,
                                                       cache_index_name,
                                                       pg_gpu_index_cache_ptr,
                                                       prepared_group,
                                                       cpu_prepared_target,
                                                       total_bp](sequence_size_t clip_lo_bp,
                                                                 sequence_size_t clip_hi_bp,
                                                                 std::size_t cs_u,
                                                                 std::size_t ce_u) {
                            const auto cache_key =
                                make_cache_key(cache_index_name, clip_lo_bp, clip_hi_bp);
                            const auto gpu_identity = pg_gpu_index_identity(cache_key);
                            if (prepared_group == nullptr)
                                if (auto cached_index_entry =
                                        bioseq::storage::indexing::lookup_assembled_index(
                                            cache_key)) {
                                    return minimap_port_fragment_index_vector_pg_checked(
                                        fq_pg_view,
                                        ft_pg_view,
                                        query_minimizers,
                                        *cached_index_entry->index,
                                        execution_plan,
                                        pg_gpu_index_cache_ptr,
                                        gpu_identity);
                                }
                            if (prepared_group != nullptr &&
                                !prepared_group->target_index_permit.has_value()) {
                                const std::uint64_t admission_bytes =
                                    bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
                                        static_cast<std::uint64_t>(total_bp));
                                auto permit =
                                    bioseq::parallelism::memory_admission_controller::instance()
                                        .try_reserve(admission_bytes);
                                if (!permit) {
                                    raise_typed_dispatch_pg_error_as_cpp(
                                        ERRCODE_OUT_OF_MEMORY,
                                        "bioseq batch join prepared persisted target "
                                        "index cannot be admitted");
                                }
                                prepared_group->target_index_permit.emplace(std::move(*permit));
                            }
                            auto assembled_index = assemble_one(cs_u, ce_u);
                            if (prepared_group != nullptr) {
                                if (prepared_group->target_index) {
                                    ++prepared_group->duplicate_build_attempts;
                                } else {
                                    prepared_group->target_index = std::make_shared<
                                        bioseq::indexing::minimizer_hashmap_fragment_index>(
                                        std::move(assembled_index));
                                    prepared_group->target_index_source =
                                        ::bioseq::pipeline_stats::target_index_source::persisted;
                                    ++prepared_group->target_index_builds;
                                    prepared_group->build_set_checksum_present = true;
                                    prepared_group->build_set_checksum =
                                        prepared_group->key_set_checksum;
                                    ++cpu_prepared_target->physical_target_index_reads;
                                }
                                return minimap_port_fragment_index_vector_pg_checked(
                                    fq_pg_view,
                                    ft_pg_view,
                                    query_minimizers,
                                    *prepared_group->target_index,
                                    execution_plan,
                                    /*gpu_index_cache=*/nullptr,
                                    bioseq::indexing::gpu_index_cache_identity{
                                        .assembly_identity = prepared_group->key_set_checksum});
                            }
                            match_result_collection match_collection =
                                minimap_port_fragment_index_vector_pg_checked(
                                    fq_pg_view,
                                    ft_pg_view,
                                    query_minimizers,
                                    assembled_index,
                                    execution_plan,
                                    pg_gpu_index_cache_ptr,
                                    gpu_identity);
                            bioseq::storage::indexing::store_assembled_index(
                                cache_key, std::move(assembled_index));
                            return match_collection;
                        };
                        if (prepared_group != nullptr) {
                            /* One full-target union-filtered index is the
                             * scan-retained object reused by every wave. */
                            collection = run_with_segment_index(0, total_bp, 0, size_u);
                        } else if (total_bp <= postgres_segmentation_threshold_bp) {
                            collection = run_with_segment_index(0, total_bp, 0, size_u);
                        } else {
                            match_result_collection merged_collection;
                            auto& merged = merged_collection.results;
                            const sequence_size_t query_len = query_bp_size;
                            for (const auto segment_range :
                                 pg_segment_unit_ranges(total_bp, query_len, a_lpu, idx_params)) {
                                const sequence_size_t clip_lo_bp =
                                    static_cast<sequence_size_t>(segment_range.cs_u) * a_lpu;
                                const sequence_size_t clip_hi_bp = std::min(
                                    static_cast<sequence_size_t>(segment_range.ce_u) * a_lpu,
                                    total_bp);
                                match_result_collection segment_collection = run_with_segment_index(
                                    clip_lo_bp, clip_hi_bp, segment_range.cs_u, segment_range.ce_u);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                merged_collection.prepare_result_append(
                                    segment_collection.results.size());
#endif
                                merged_collection.combine_execution_stats(segment_collection);
                                for (auto& match : segment_collection.results) {
                                    if (match.target_start >= segment_range.core_lo_bp &&
                                        match.target_start < segment_range.core_hi_bp) {
                                        merged.push_back(std::move(match));
                                    }
                                }
                            }
                            collection = std::move(merged_collection);
                        }
                        assembled = true;
                    };
                    // Route by the table's ACTIVE read index.
                    // (set_active_read_index / resolve_active_read_index, default
                    // minimizers_per_fragment). Per-row presence still gates: an
                    // active index not materialised for THIS row recomputes.
                    if (legacy_optional_index_read_active && !persisted_required && !assembled) {
                        if (want_seq) {
                            with_fragment_storage(
                                minimizers_per_sequence_index_name,
                                [&assembled,
                                 &collection,
                                 &cpu_prepared_target,
                                 &ensure_q,
                                 &fq_pg_view,
                                 &ft_pg_view,
                                 &row_index,
                                 execution_plan,
                                     &idx_params,
                                 &loc_opt,
                                 &make_cache_key,
                                 &pg_gpu_index_cache_ptr,
                                 &prepared_group,
                                 &query_bp_size,
                                 &query_minimizers,
                                 &run_fragment_backed_index,
                                 &scanner_pg_ctx,
                                 &target_letters_per_unit](
                                    typed_dispatch_fragment_read_view& seq_store) {
                                    if (seq_store.number_of_chunks(
                                            bioseq::indexing::k_directory_sub_key) >= 1) {
                                        const std::size_t dir_bytes = seq_store.chunk_byte_size(
                                            bioseq::indexing::k_directory_sub_key, 0);
                                        if (dir_bytes == sizeof(std::int64_t)) {
                                            run_fragment_backed_index(
                                                seq_store, minimizers_per_sequence_index_name);
                                            bioseq_log<logging_level::debug>(
                                                "[bioseq] index: logical fragment-backed read "
                                                "index=minimizers_per_sequence row={} target_bp={}",
                                                static_cast<unsigned long long>(
                                                    scanner_pg_ctx->target_rownos[row_index]),
                                                static_cast<unsigned long long>(
                                                    loc_opt->logical_length));
                                        } else {
                                            ensure_q();
                                            const sequence_size_t total_bp =
                                                loc_opt->logical_length;
                                            const std::size_t a_lpu = target_letters_per_unit;
                                            const auto query_vec = query_minimizers;
                                            const std::vector<minimap_sketch_data>&
                                                assembly_query_vec =
                                                    prepared_group == nullptr
                                                        ? *query_vec
                                                        : prepared_group->query_hash_union;
                                            const auto query_span =
                                                std::span<const minimap_sketch_data>(
                                                    assembly_query_vec.data(),
                                                    assembly_query_vec.size());
                                            bioseq::indexing::minimizer_assembly_scratch sq_scratch;
                                            auto run_minimizer_sequence_index = [&make_cache_key,
                                                                                 &seq_store,
                                                                                 fq_pg_view,
                                                                                 ft_pg_view,
                                                                                 execution_plan,
                                     &idx_params,
                                                                                 query_vec,
                                                                                 query_span,
                                                                                 total_bp,
                                                                                 &sq_scratch,
                                                                                 pg_gpu_index_cache_ptr,
                                                                                 prepared_group,
                                                                                 cpu_prepared_target](
                                                                                    sequence_size_t
                                                                                        clip_lo_bp,
                                                                                    sequence_size_t
                                                                                        clip_hi_bp) {
                                                const auto cache_key = make_cache_key(
                                                    minimizers_per_sequence_index_name,
                                                    clip_lo_bp,
                                                    clip_hi_bp);
                                                const auto gpu_identity =
                                                    pg_gpu_index_identity(cache_key);
                                                if (prepared_group == nullptr)
                                                    if (auto cached_index_entry =
                                                            bioseq::storage::indexing::
                                                                lookup_assembled_index(cache_key)) {
                                                        return minimap_port_fragment_index_vector_pg_checked(
                                                            fq_pg_view,
                                                            ft_pg_view,
                                                            query_vec,
                                                            *cached_index_entry->index,
                                                            execution_plan,
                                                            pg_gpu_index_cache_ptr,
                                                            gpu_identity);
                                                    }
                                                if (prepared_group != nullptr &&
                                                    !prepared_group->target_index_permit
                                                         .has_value()) {
                                                    const std::uint64_t admission_bytes =
                                                        bioseq::parallelism::
                                                            minimizer_index_peak_bytes_for_bp(
                                                                static_cast<std::uint64_t>(
                                                                    total_bp));
                                                    auto permit =
                                                        bioseq::parallelism::
                                                            memory_admission_controller::instance()
                                                                .try_reserve(admission_bytes);
                                                    if (!permit) {
                                                        raise_typed_dispatch_pg_error_as_cpp(
                                                            ERRCODE_OUT_OF_MEMORY,
                                                            "bioseq batch join prepared persisted "
                                                            "target "
                                                            "index cannot be admitted");
                                                    }
                                                    prepared_group->target_index_permit.emplace(
                                                        std::move(*permit));
                                                }
                                                auto assembled_index =
                                                    seq_store
                                                        .assemble_sequence_segment_for_query(
                                                            query_span,
                                                            total_bp,
                                                            idx_params.max_occ,
                                                            clip_lo_bp,
                                                            clip_hi_bp,
                                                            sq_scratch);
                                                if (prepared_group != nullptr) {
                                                    if (prepared_group->target_index) {
                                                        ++prepared_group->duplicate_build_attempts;
                                                    } else {
                                                        prepared_group
                                                            ->target_index = std::make_shared<
                                                            bioseq::indexing::
                                                                minimizer_hashmap_fragment_index>(
                                                            std::move(assembled_index));
                                                        prepared_group->target_index_source =
                                                            ::bioseq::pipeline_stats::
                                                                target_index_source::persisted;
                                                        ++prepared_group->target_index_builds;
                                                        prepared_group->build_set_checksum_present =
                                                            true;
                                                        prepared_group->build_set_checksum =
                                                            prepared_group->key_set_checksum;
                                                        ++cpu_prepared_target
                                                              ->physical_target_index_reads;
                                                    }
                                                    return minimap_port_fragment_index_vector_pg_checked(
                                                        fq_pg_view,
                                                        ft_pg_view,
                                                        query_vec,
                                                        *prepared_group->target_index,
                                                        execution_plan,
                                                        /*gpu_index_cache=*/nullptr,
                                                        bioseq::indexing::gpu_index_cache_identity{
                                                            .assembly_identity =
                                                                prepared_group->key_set_checksum});
                                                }
                                                match_result_collection match_collection =
                                                    minimap_port_fragment_index_vector_pg_checked(
                                                        fq_pg_view,
                                                        ft_pg_view,
                                                        query_vec,
                                                        assembled_index,
                                                        execution_plan,
                                                        pg_gpu_index_cache_ptr,
                                                        gpu_identity);
                                                bioseq::storage::indexing::store_assembled_index(
                                                    cache_key, std::move(assembled_index));
                                                return match_collection;
                                            };
                                            if (prepared_group != nullptr) {
                                                collection =
                                                    run_minimizer_sequence_index(0, total_bp);
                                            } else if (total_bp <=
                                                       postgres_segmentation_threshold_bp) {
                                                collection =
                                                    run_minimizer_sequence_index(0, total_bp);
                                            } else {
                                                match_result_collection merged_collection;
                                                auto& merged = merged_collection.results;
                                                const sequence_size_t query_len = query_bp_size;
                                                for (const auto segment_range :
                                                     pg_segment_unit_ranges(
                                                         total_bp, query_len, a_lpu, idx_params)) {
                                                    const sequence_size_t clip_lo_bp =
                                                        static_cast<sequence_size_t>(
                                                            segment_range.cs_u) *
                                                        a_lpu;
                                                    const sequence_size_t clip_hi_bp =
                                                        std::min(static_cast<sequence_size_t>(
                                                                     segment_range.ce_u) *
                                                                     a_lpu,
                                                                 total_bp);
                                                    match_result_collection segment_collection =
                                                        run_minimizer_sequence_index(clip_lo_bp,
                                                                                     clip_hi_bp);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                    merged_collection.prepare_result_append(
                                                        segment_collection.results.size());
#endif
                                                    merged_collection.combine_execution_stats(
                                                        segment_collection);
                                                    for (auto& match : segment_collection.results) {
                                                        if (match.target_start >=
                                                                segment_range.core_lo_bp &&
                                                            match.target_start <
                                                                segment_range.core_hi_bp) {
                                                            merged.push_back(std::move(match));
                                                        }
                                                    }
                                                }
                                                collection = std::move(merged_collection);
                                            }
                                            assembled = true;
                                        }
                                    }
                                });
                        } else {
                            with_fragment_storage(
                                minimizers_per_fragment_index_name,
                                [&run_fragment_backed_index](
                                    typed_dispatch_fragment_read_view& frag_store) {
                                    if (frag_store.number_of_chunks(
                                            bioseq::indexing::k_directory_sub_key) >= 1) {
                                        run_fragment_backed_index(
                                            frag_store, minimizers_per_fragment_index_name);
                                    }
                                });
                        }
                    }
                    if (assembled) {
                        // The prepared group owns provenance; only an ordinary
                        // stored-chunk assembly has no prepared-group owner.
                        const auto assembled_source =
                            prepared_group != nullptr
                                ? prepared_group->target_index_source
                                : ::bioseq::pipeline_stats::target_index_source::persisted;
                        if (assembled_source !=
                                ::bioseq::pipeline_stats::target_index_source::persisted &&
                            assembled_source !=
                                ::bioseq::pipeline_stats::target_index_source::recomputed) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "assembled target has no concrete index source");
                        }
                        scanner_pg_ctx->target_index_sources[row_index] =
                            assembled_source;
                    }
                    if (persisted_required && !assembled) {
                        raise_required_persisted_index_error(
                            scanner_pg_ctx,
                            scanner_pg_ctx->target_rownos[row_index],
                            "the active persisted minimizer index has no compatible "
                            "directory/chunks for this target row");
                    }
                    // Missing/forbidden persisted storage is rebuilt from the target through the
                    // statement-selected generated producer. The synchronous host MPF and MPS
                    // routes themselves never open or publish a sidecar; explicitly enabled later
                    // asynchronous staging is separate. A generation failure cannot fall through
                    // to the raw aligner.
                    if (!assembled && want_seq) {
                        const auto rowno =
                            static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]);
                        collection = build_and_align_transient_minimizer_sequence(
                            fq_pg_view,
                            ft_pg_view,
                            execution_plan,
                            static_cast<sequence_size_t>(loc_opt->logical_length),
                            target_letters_per_unit,
                            alphabet_code,
                            static_cast<std::uint8_t>(sizeof(query_storage_unit)),
                            storage_unit_byte_width,
                            loc_opt->source_epoch);
                        const bool async_staged =
                            stage_recompute_minimizer_sequence_build_if_selected(
                                *scanner_pg_ctx,
                                *loc_opt,
                                rowno,
                                alphabet_code,
                                storage_unit_byte_width,
                                static_cast<std::uint16_t>(_padding),
                                execution_plan,
                                "generated_minimizer_sequence_noindex");
                        bioseq_log<logging_level::debug>(
                            "[bioseq] index: generated transient minimizer-sequence row={} "
                            "target_bp={} async_staged={} persisted=false backend={}",
                            static_cast<unsigned long long>(rowno),
                            static_cast<unsigned long long>(loc_opt->logical_length),
                            async_staged,
                            bioseq::minimizers::minimizer_sketch_backend_name(
                                *selected_query_backend));
                        assembled = true;
                    }
                    if (!assembled && want_fragment) {
                        collection = build_and_align_transient_minimizer_fragment(
                            fq_pg_view,
                            ft_pg_view,
                            execution_plan,
                            static_cast<sequence_size_t>(loc_opt->logical_length),
                            target_letters_per_unit,
                            alphabet_code,
                            static_cast<std::uint8_t>(sizeof(query_storage_unit)),
                            storage_unit_byte_width);
                        bioseq_log<logging_level::debug>(
                            "[bioseq] index: generated transient minimizer-fragment row={} "
                            "target_bp={} persisted=false backend={}",
                            static_cast<unsigned long long>(
                                scanner_pg_ctx->target_rownos[row_index]),
                            static_cast<unsigned long long>(loc_opt->logical_length),
                            bioseq::minimizers::minimizer_sketch_backend_name(
                                *selected_query_backend));
                        assembled = true;
                    }
                }
                if (!assembled) {
                    const bool try_resident_segment_workers =
                        target_bp_size > postgres_segmentation_threshold_bp &&
                        !params.enable_profiling &&
                        bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
                        pool.maximum_concurrency() > 1;
                    if (try_resident_segment_workers) {
                        auto resident = try_materialize_target_for_segment_workers(
                            ft_pg_view,
                            storage_unit_byte_width,
                            static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]));
                        if (resident.has_value()) {
                            const auto resident_target_view = resident->owner.view();
                            const bool can_defer_recompute_persist =
                                !generic_raw_optional_scalar &&
                                can_defer_recompute_minimizer_persist(*scanner_pg_ctx, params);
                            const auto deferred_persist_choice =
                                can_defer_recompute_persist
                                    ? choose_deferred_minimizer_persist_index(*scanner_pg_ctx)
                                    : std::optional<deferred_minimizer_persist_choice>{};
                            if (deferred_persist_choice.has_value()) {
                                const auto rowno = static_cast<std::uint64_t>(
                                    scanner_pg_ctx->target_rownos[row_index]);
                                collection = run_recompute_with_deferred_minimizer_persist(
                                    lazy_query_view,
                                    resident_target_view,
                                    execution_plan,
                                    *scanner_pg_ctx,
                                    *loc_opt,
                                    rowno,
                                    alphabet_code,
                                    static_cast<std::uint8_t>(sizeof(query_storage_unit)),
                                    storage_unit_byte_width,
                                    static_cast<std::uint16_t>(_padding),
                                    *deferred_persist_choice);
                            } else {
                                collection = minimap_port_segmented_pg_checked(
                                    lazy_query_view, resident_target_view, execution_plan);
                                if (scanner_pg_ctx->recompute_async_persist_active &&
                                    (params.kmer_size != 15 || params.window_size != 10)) {
                                    bioseq_log<logging_level::debug>(
                                        "[bioseq] index: recompute async-persist skipped row={} "
                                        "target_bp={} because active params are k={} w={} but "
                                        "persisted minimizer indexes are pinned to k=15 w=10",
                                        static_cast<unsigned long long>(
                                            scanner_pg_ctx->target_rownos[row_index]),
                                        static_cast<unsigned long long>(loc_opt->logical_length),
                                        params.kmer_size,
                                        params.window_size);
                                } else if (can_defer_recompute_persist) {
                                    bioseq_log<logging_level::debug>(
                                        "[bioseq] index: recompute async-persist skipped row={} "
                                        "target_bp={} because no minimizer index is enabled for "
                                        "UPDATE_ON_INSERT",
                                        static_cast<unsigned long long>(
                                            scanner_pg_ctx->target_rownos[row_index]),
                                        static_cast<unsigned long long>(loc_opt->logical_length));
                                }
                            }
                        } else {
                            collection =
                                minimap_port_segmented_pg_checked(fq_pg_view, ft_pg_view, execution_plan);
                            if (!generic_raw_optional_scalar) {
                                stage_recompute_minimizer_sequence_build_if_selected(
                                    *scanner_pg_ctx,
                                    *loc_opt,
                                    static_cast<std::uint64_t>(
                                        scanner_pg_ctx->target_rownos[row_index]),
                                    alphabet_code,
                                    storage_unit_byte_width,
                                    static_cast<std::uint16_t>(_padding),
                                    execution_plan,
                                    "resident_materialize_failed");
                            }
                        }
                    } else {
                        collection =
                            (target_bp_size > postgres_segmentation_threshold_bp)
                                ? minimap_port_segmented_pg_checked(fq_pg_view, ft_pg_view, execution_plan)
                                : minimap_port_raw_pg_checked(fq_pg_view, ft_pg_view, execution_plan);
                        if (!generic_raw_optional_scalar) {
                            stage_recompute_minimizer_sequence_build_if_selected(
                                *scanner_pg_ctx,
                                *loc_opt,
                                static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]),
                                alphabet_code,
                                storage_unit_byte_width,
                                static_cast<std::uint16_t>(_padding),
                                execution_plan,
                                "streaming_recompute");
                        }
                    }
                }
            });
    } else {
        compact_sequence_range<p_sequence> target_seq{*target_hdr};
        const auto target_view = bioseq::storage::make_lazy_sequence_view(target_seq);
        collection = (target_bp_size > postgres_segmentation_threshold_bp)
                         ? minimap_port_segmented_pg_checked(lazy_query_view, target_view, execution_plan)
                         : minimap_port_raw_pg_checked(lazy_query_view, target_view, execution_plan);
    }

    if (query_sketch_host_wall_ns != 0u) {
        collection.record_query_sketch_host_time(query_sketch_host_wall_ns);
    }
    if (row_consumed_prepared_query_sketch) {
        const pipe::minimizer_range_execution_stats* completed_query_stats =
            retained_prepared_query_sketch != nullptr
                ? &retained_prepared_query_sketch->execution_stats
            : per_row_query_mode ? row_query_minimizer_stats ? &*row_query_minimizer_stats : nullptr
            : cached_query_minimizer_stats ? &*cached_query_minimizer_stats
                                           : nullptr;
        if (completed_query_stats == nullptr) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "consumed prepared query minimizers have no operation stats");
        }
        const bool exact_query_stats_absent =
            !collection.exact_simd_query_execution_stats_available &&
            collection.exact_simd_query_execution_stats ==
                decltype(collection.exact_simd_query_execution_stats){};
        const bool scalar_query_stats_absent =
            collection.scalar_pipeline_stats.query_minimizers ==
            decltype(collection.scalar_pipeline_stats.query_minimizers){};
        const bool bounded_query_stats_absent =
            !collection.bounded_nthash_query_execution_stats_available &&
            collection.bounded_nthash_query_execution_stats ==
                decltype(collection.bounded_nthash_query_execution_stats){};
        if (!scalar_query_stats_absent || !exact_query_stats_absent ||
            !bounded_query_stats_absent) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "prepared query sketch stats were recorded more than once");
        }
        if (completed_query_stats->exact_simd_available) {
            collection.record_exact_simd_query_execution_stats(completed_query_stats->exact_simd);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            collection.record_completed_minimizer_algorithm_identity(
                minimizer_algorithm::two_stack_nthash);
#endif
        }
        if (completed_query_stats->bounded_nthash_available) {
            collection.record_bounded_nthash_query_execution_stats(
                completed_query_stats->bounded_nthash);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            collection.record_completed_minimizer_algorithm_identity(
                minimizer_algorithm::bounded_nthash_simd);
#endif
        }
        if (completed_query_stats->scalar_available) {
            collection.record_scalar_query_minimizer_execution_stats(completed_query_stats->scalar);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            collection.record_completed_minimizer_algorithm_identity(minimizer_algorithm::scalar);
#endif
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        // Attribute the completed checked bridge, not merely the requested selector.
        if (completed_query_stats->checked_reference_simd_bridge_completed) {
            collection.record_completed_minimizer_algorithm_identity(
                minimizer_algorithm::simd_minimap2_reference);
        }
#endif
        consumed_shared_cached_query_stats = !per_row_query_mode;
    }
    return collection;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
