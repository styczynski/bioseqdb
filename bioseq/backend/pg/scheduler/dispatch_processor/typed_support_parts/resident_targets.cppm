/**
 * @file typed_support_parts/resident_targets.cppm
 * @brief Defines transient indexed alignment and admitted resident-target materialization.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_targets;

import std;
export import bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_target_materialization;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.transient_storage;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.external_file_producer_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.storage.scanner;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

// Build a real transient minimizers_per_sequence index
// (GPU-backed when Vulkan is selected), assemble it for the query, and run the
// indexed aligner. Shared by BOTH the fast no_index recompute path and the
// deferred-persist recompute path, so neither serves a no_index x per_sequence
// cell as a minimizer-fragment-shaped segmented relabel. Streams from the lazy PostgreSQL
// target via the GPU sorted-pair seam (device emit plus host pair sort) with no
// whole-target resident materialization, so
// 2/4 Gbp targets do not OOM.
template <sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection build_and_align_transient_minimizer_sequence(
    bioseq::storage::lazy_sequence_view<p_alphabet> lazy_query_view,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    const minimap2_parameters& params,
    sequence_size_t total_bp_size,
    std::size_t target_letters_per_unit) {
    match_result_collection collection;
    // Every PostgreSQL terminal error below crosses a true
    // invoke_pg_as_cpp leaf. Keep this orchestration in ordinary C++ so minimizer-sequence
    // chunks, sorted runs, GPU cache, query stats, assembled indexes, and
    // segment results unwind normally on postgres_error.
    opportunistic_sequence_index_storage minimizer_sequence_store;
    minimizer_sequence_store.run_buffer_bytes_value =
        bioseq::settings::index_build::run_buffer_bytes > 0
            ? static_cast<std::size_t>(bioseq::settings::index_build::run_buffer_bytes)
            : (std::size_t{256} << 20);

    // direct_minimizer_bucket_index::build is pinned to w=10,k=15. The
    // transient read side must use the same minimizer parameters even if
    // apply_size_heuristic() reduced the caller's k/w for short reads.
    // Keep the caller's scoring/seed-count adjustments.
    minimap2_parameters minimizer_sequence_params = params;
    minimizer_sequence_params.kmer_size = 15;
    minimizer_sequence_params.window_size = 10;
    if (minimizer_sequence_params.highway_kernels == nullptr) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "transient minimizer-sequence build has no selected Highway minimizer table");
    }

    const auto backend = target_backend_from_params_for_recompute(minimizer_sequence_params);
    const bool scalar_sketch_selected = backend == bioseq::simd::minimizer_sketch_backend::scalar;
    const bool exact_simd_sketch_selected =
        backend == bioseq::simd::minimizer_sketch_backend::exact_simd;
    bioseq::indexing::direct_minimizer_bucket_build_stats scalar_target_build_stats{};
    bioseq::indexing::direct_minimizer_bucket_index<opportunistic_sequence_index_storage>::build(
        minimizer_sequence_store,
        target_view,
        backend,
        &minimizer_sequence_params.highway_kernels->minimizers,
        scalar_sketch_selected ? &scalar_target_build_stats : nullptr,
        minimizer_sequence_params.alphabet_code);

    auto query_sketch = sketch_query_for_index_selection<minimap_sketch_data>(
        lazy_query_view,
        minimizer_sequence_params,
        static_cast<std::size_t>(minimizer_sequence_params.window_size),
        static_cast<std::size_t>(minimizer_sequence_params.kmer_size));
    std::optional<bioseq::indexing::gpu_minimizer_index_cache> gpu_index_cache;
    if (pg_vulkan_seed_lookup_selected(minimizer_sequence_params)) {
        gpu_index_cache.emplace(pg_gpu_index_resident_limit_bytes());
    }
    auto* gpu_index_cache_ptr = gpu_index_cache ? &*gpu_index_cache : nullptr;
    const std::uint64_t transient_identity_base =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&minimizer_sequence_store));
    bioseq::indexing::minimizer_assembly_scratch scratch;
    auto run_minimizer_sequence_index = [&minimizer_sequence_store,
                                         lazy_query_view,
                                         target_view,
                                         &query_sketch,
                                         &minimizer_sequence_params,
                                         total_bp_size,
                                         &scratch,
                                         gpu_index_cache_ptr,
                                         transient_identity_base,
                                         backend](sequence_size_t clip_lo_bp,
                                                  sequence_size_t clip_hi_bp) {
        auto index =
            bioseq::indexing::direct_minimizer_bucket_index<opportunistic_sequence_index_storage>::
                assemble_for_query_segment(minimizer_sequence_store,
                                           query_sketch.as_span(),
                                           total_bp_size,
                                           minimizer_sequence_params.max_occ,
                                           clip_lo_bp,
                                           clip_hi_bp,
                                           scratch);
        const auto gpu_identity = pg_gpu_index_identity(transient_identity_base,
                                                        minimizers_per_sequence_index_name,
                                                        clip_lo_bp,
                                                        clip_hi_bp,
                                                        backend);
        return minimap_port_fragment_index_vector_pg_checked(lazy_query_view,
                                                             target_view,
                                                             query_sketch.minimizers,
                                                             index,
                                                             minimizer_sequence_params,
                                                             gpu_index_cache_ptr,
                                                             gpu_identity);
    };
    if (total_bp_size <= postgres_segmentation_threshold_bp) {
        collection = run_minimizer_sequence_index(0, total_bp_size);
    } else {
        const std::size_t letters_per_unit = target_letters_per_unit;
        match_result_collection merged_collection;
        std::vector<match_result>& merged = merged_collection.results;
        const sequence_size_t query_len = lazy_query_view.bp_size();
        for (const auto segment_range : pg_segment_unit_ranges(
                 total_bp_size, query_len, letters_per_unit, minimizer_sequence_params)) {
            const sequence_size_t clip_lo_bp =
                static_cast<sequence_size_t>(segment_range.cs_u) * letters_per_unit;
            const sequence_size_t clip_hi_bp = std::min(
                static_cast<sequence_size_t>(segment_range.ce_u) * letters_per_unit, total_bp_size);
            match_result_collection segment_collection =
                run_minimizer_sequence_index(clip_lo_bp, clip_hi_bp);
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
    if (exact_simd_sketch_selected) {
        if (!query_sketch.exact_simd_execution_stats_available ||
            !query_sketch.exact_simd_execution_stats.is_internally_consistent(
                /*require_nonempty_vector_work=*/false)) {
            bioseq_log<logging_level::error>(
                "Persisted-index two_stack_nthash query lacks actual execution stats");
            std::abort();
        }
        collection.record_exact_simd_query_execution_stats(query_sketch.exact_simd_execution_stats);
    } else if (scalar_sketch_selected) {
        if (!query_sketch.scalar_execution_stats_available ||
            !query_sketch.scalar_execution_stats.is_coherent() ||
            query_sketch.exact_simd_execution_stats_available ||
            query_sketch.bounded_nthash_execution_stats_available ||
            !scalar_target_build_stats.is_internally_consistent() ||
            backend != bioseq::simd::minimizer_sketch_backend::scalar) {
            bioseq_log<logging_level::error>(
                "Transient minimizer-sequence scalar build/query lacks actual execution stats");
            std::abort();
        }
        collection.record_scalar_query_minimizer_execution_stats(
            query_sketch.scalar_execution_stats);
        const scalar_minimizer_execution_stats target_stats{
            .calls = scalar_target_build_stats.scalar_collection_calls,
            .input_symbols = scalar_target_build_stats.input_symbols,
            .saturated = scalar_target_build_stats.saturated,
        };
        if (!target_stats.is_coherent()) {
            bioseq_log<logging_level::error>(
                "Transient minimizer-sequence scalar target stats conversion was rejected");
            std::abort();
        }
        collection.record_scalar_target_minimizer_execution_stats(target_stats);
    } else if (query_sketch.exact_simd_execution_stats_available) {
        bioseq_log<logging_level::error>(
            "Non-two_stack_nthash persisted-index query emitted two_stack_nthash stats");
        std::abort();
    }
    return collection;
}

template <class p_scanner_pg_context>
[[nodiscard]] bool
stage_recompute_minimizer_sequence_build_if_selected(const p_scanner_pg_context& ctx,
                                                     const bioseq::storage::sequence_locator& loc,
                                                     std::uint64_t sequence_rowno,
                                                     std::uint8_t alphabet_code,
                                                     std::uint8_t storage_unit_byte_width,
                                                     std::uint16_t last_fragment_padding,
                                                     const minimap2_parameters& params,
                                                     std::string_view reason) {
    if (!can_defer_recompute_minimizer_persist(ctx, params)) {
        if (ctx.recompute_async_persist_active &&
            target_backend_from_params_for_recompute(params) ==
                bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd) {
            bioseq_log<logging_level::debug>(
                "[bioseq] index: recompute async minimizer-sequence build skipped row={} "
                "target_bp={} reason={} because persistence requires a scheme-tagged READY "
                "generation; recompute continues without fallback",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(loc.logical_length),
                reason);
        } else if (ctx.recompute_async_persist_active &&
                   (params.kmer_size != 15 || params.window_size != 10)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] index: recompute async minimizer-sequence build skipped row={} "
                "target_bp={} reason={} because active params are k={} w={} but persisted "
                "minimizer indexes are pinned to k=15 w=10",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(loc.logical_length),
                reason,
                params.kmer_size,
                params.window_size);
        } else if (!ctx.recompute_async_persist_active || !ctx.index_async_persist_active ||
                   !OidIsValid(ctx.user_relid)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] index: recompute async minimizer-sequence build skipped row={} "
                "target_bp={} reason={} gates recompute_async={} index_async={} "
                "user_relid_valid={}",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(loc.logical_length),
                reason,
                ctx.recompute_async_persist_active,
                ctx.index_async_persist_active,
                OidIsValid(ctx.user_relid));
        }
        return false;
    }

    const auto choice = choose_deferred_minimizer_persist_index(ctx);
    if (!choice.has_value()) {
        bioseq_log<logging_level::debug>(
            "[bioseq] index: recompute async minimizer-sequence build skipped row={} target_bp={} "
            "reason={} because no minimizer index is enabled for UPDATE_ON_INSERT",
            static_cast<unsigned long long>(sequence_rowno),
            static_cast<unsigned long long>(loc.logical_length),
            reason);
        return false;
    }
    if (choice->kind != deferred_minimizer_index_kind::per_sequence) {
        bioseq_log<logging_level::debug>(
            "[bioseq] index: recompute async minimizer-sequence build skipped row={} index={} "
            "target_bp={} reason={} because deferred choice is not per-sequence",
            static_cast<unsigned long long>(sequence_rowno),
            choice->index_name,
            static_cast<unsigned long long>(loc.logical_length),
            reason);
        return false;
    }

    const auto persist_target =
        make_fragment_index_persist_target(ctx, loc, sequence_rowno, choice->index_name);
    const bool staged = bioseq::postgres::scheduler::stage_async_minimizer_sequence_build(
        persist_target,
        loc,
        alphabet_code,
        storage_unit_byte_width,
        last_fragment_padding,
        static_cast<std::int32_t>(
            bioseq::settings::index::sequence_minimizer_sort::algorithm.get_value()),
        static_cast<int>(bioseq::scheduler::settings::pipeline::mode.get_value()),
        bioseq::scheduler::settings::backend::max_concurrency,
        bioseq::settings::index::memory_cap_mb,
        static_cast<std::int32_t>( // Thread the selected backend into the async worker.
            target_backend_from_params_for_recompute(params)),
        params.highway_executed_target);
    bioseq_log<logging_level::debug>("[bioseq] index: recompute async minimizer-sequence build "
                                     "row={} index={} target_bp={} reason={} async_staged={}",
                                     static_cast<unsigned long long>(sequence_rowno),
                                     choice->index_name,
                                     static_cast<unsigned long long>(loc.logical_length),
                                     reason,
                                     staged);
    return staged;
}

template <sequence_alphabet p_alphabet, class p_scanner_pg_context>
[[nodiscard]] match_result_collection run_recompute_with_deferred_minimizer_persist(
    bioseq::storage::lazy_sequence_view<p_alphabet> lazy_query,
    bioseq::storage::lazy_sequence_view<p_alphabet> resident_target,
    const minimap2_parameters& params,
    const p_scanner_pg_context& ctx,
    const bioseq::storage::sequence_locator& loc,
    std::uint64_t sequence_rowno,
    std::uint8_t alphabet_code,
    std::uint8_t storage_unit_byte_width,
    std::uint16_t last_fragment_padding,
    const deferred_minimizer_persist_choice& choice) {
    if (choice.kind == deferred_minimizer_index_kind::per_sequence) {
        // Real minimizer-sequence no-index route: build a transient minimizers_per_sequence
        // index (GPU-backed when Vulkan is selected), assemble it for the query, and run
        // the indexed aligner. No minimizer-fragment relabelling. ctx is unused on this transient
        // path.
        (void)ctx;
        (void)alphabet_code;
        (void)storage_unit_byte_width;
        (void)last_fragment_padding;
        match_result_collection collection = build_and_align_transient_minimizer_sequence(
            lazy_query,
            resident_target,
            params,
            static_cast<sequence_size_t>(loc.logical_length),
            storage_unit_byte_width * std::numeric_limits<unsigned char>::digits /
                bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol);

        bioseq_log<logging_level::debug>(
            "[bioseq] index: recompute real-minimizer-sequence-noindex row={} index={} "
            "target_bp={} persisted=false backend={}",
            static_cast<unsigned long long>(sequence_rowno),
            choice.index_name,
            static_cast<unsigned long long>(loc.logical_length),
            bioseq::minimizers::minimizer_sketch_backend_name(
                target_backend_from_params_for_recompute(params)));
        return collection;
    }

    (void)ctx;
    (void)alphabet_code;
    (void)storage_unit_byte_width;
    (void)last_fragment_padding;
    match_result_collection collection =
        minimap_port_segmented_pg_checked(lazy_query, resident_target, params);
    bioseq_log<logging_level::debug>(
        "[bioseq] index: recompute external-file persist prohibited row={} index={} target_bp={} "
        "constructor_entries={} writer_entries={} async_staged=false",
        static_cast<unsigned long long>(sequence_rowno),
        choice.index_name,
        static_cast<unsigned long long>(loc.logical_length),
        static_cast<unsigned long long>(
            bioseq::storage::indexing::g_prohibited_external_file_producer_entry_counters
                .constructor_entries),
        static_cast<unsigned long long>(
            bioseq::storage::indexing::g_prohibited_external_file_producer_entry_counters
                .writer_entries));
    return collection;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
