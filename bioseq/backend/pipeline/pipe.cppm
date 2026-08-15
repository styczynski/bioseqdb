/**
 * @file pipeline/pipe.cppm
 * @brief Compile-time dispatched sequence-alignment pipeline.
 *
 * This module implements the compile-time dispatched stage graph used by the
 * scheduler-facing mapping entry points. It sits above kernel functions in
 * the target-neutral `bioseq::simd` namespace and provides:
 *
 *   - Compile-time parameter packs
 * (`static_aligner_parameters<p_window_size,p_kmer_size,p_bucket_width>` /
 *     `fallback_aligner_parameters<p_bucket_width>`).
 *   - stage skeletons (`bare_stage<p_self>`, `stage_base<p_self,p_output>`,
 *     `stage<p_aligner_parameters,p_self,p_sink>`).
 *   - `make_pipeline<p_aligner_parameters>` variadic composer.
 *   - Concrete stages — every pipeline stage exposed as a stand-alone struct
 *     whose `process<p_aligner_parameters>` body forwards to the existing kernel function
 *     (collect_seed_hits, chain_anchors_*, generate_regions, etc.).
 *   - Top-level `dispatch_pipeline(...)` cascade that fixes (p_aligner_parameters,
 * p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point) at compile
 * time before driving the pipeline.
 *   - `pipeline_description` metadata surface for the Postgres EXPLAIN
 *     renderer.
 *
 * The pipeline calls into the existing kernels directly. Stages adapt
 * span-based I/O to the kernel's `std::vector`
 * signatures by materialising the kernel's output into arena-backed slabs
 * after the kernel returns.  The wire type between stages is `std::span<p_element>`
 * (with mutating stages taking non-const elements and read-only stages
 * taking const).
 */
module;

#include <bioseq/pg_build_profile.h>

// The chain primitive (`pipe::detail::dispatch_chain`) relies
// on tree-inlining of [[gnu::always_inline]] recursive instantiations.  Both
// gcc and clang honour the attribute at -O1+, but downgrade silently at -O0.
// Build configs that compile this module without optimisation produce
// stack-spilled adaptor frames that defeat the point of the cascade; refuse
// to compile in that mode.
#if !defined(__OPTIMIZE__) && !defined(BIOSEQ_PACKAGE_O0_PROFILE)
#error "pipeline chain primitive requires -O1 or higher. " \
        "Use -Og or -O1 -g for debug builds of pipeline/pipe.cppm; only the explicit " \
        "distribution package O0 profile may disable this guard."
#endif

#ifndef BIOSEQ_PIPELINE_FORCE_INLINE
#define BIOSEQ_PIPELINE_FORCE_INLINE 1
#endif

#if defined(BIOSEQ_PACKAGE_O0_PROFILE) && BIOSEQ_PIPELINE_FORCE_INLINE
#error "BIOSEQ_PACKAGE_O0_PROFILE requires BIOSEQ_PIPELINE_FORCE_INLINE=0"
#endif

#if BIOSEQ_PIPELINE_FORCE_INLINE
#define BIOSEQ_PIPELINE_INLINE_ATTR [[gnu::always_inline]]
#define BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR [[gnu::always_inline, gnu::flatten]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR [[nodiscard, gnu::always_inline]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR                                              \
    [[nodiscard, gnu::always_inline, gnu::flatten]]
#else
#define BIOSEQ_PIPELINE_INLINE_ATTR
#define BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR [[nodiscard]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR [[nodiscard]]
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <boost/unordered/unordered_flat_set.hpp>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.pipeline.pipe;

import bioseq.common.logging;
import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.ksw2_batched;
import bioseq.alignment.block_aligner;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.chaining.vulkan;
import bioseq.pipeline.minimap_port;
import bioseq.prefilter.types;
import bioseq.prefilter.sneaky_snake;
import bioseq.prefilter.sneaky_snake.vulkan;
import bioseq.prefilter.gatekeeper;
import bioseq.prefilter.gatekeeper.vulkan;
import bioseq.common.context;
import bioseq.instrumentation;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.seeding.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

// This target-independent execution value deliberately precedes Highway
// target toggling: these types describe
// a whole statement and must have one ABI regardless of the compiled simd
// target. algorithm_type-local statistics are mapped into this model at dispatch
// boundaries; no Highway namespace owns an alias of it.
export import bioseq.pipeline.stats;

export import :stage_graph;
export import :minimizer_execution;
export import :execution;
export import :shape_dispatch;
export import :entry_points;
export import :segmented;

export {

    namespace bioseq::simd::pipe {

    namespace detail {

    template <pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index,
              std::size_t p_bucket_width,
              std::size_t p_kmer_size,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm>
    struct pipeline_run_leaf {
        static_assert(std::is_same_v<decltype(p_bucket_width), std::size_t>,
                      "pipeline_run_leaf: p_bucket_width must be std::size_t (comes from "
                      "seq_len_log2_step).");
        static_assert(
            std::is_same_v<decltype(p_kmer_size), std::size_t>,
            "pipeline_run_leaf: p_kmer_size must be std::size_t (comes from kmer_size_step).");
        static_assert(std::is_same_v<decltype(p_alignment_algorithm),
                                     ::bioseq::simd::base_alignment_algorithm>,
                      "pipeline_run_leaf: p_alignment_algorithm must be base_alignment_algorithm "
                      "(from baa_step).");
        static_assert(
            std::is_same_v<decltype(p_chaining_algorithm), ::bioseq::simd::chaining_algorithm>,
            "pipeline_run_leaf: p_chaining_algorithm must be chaining_algorithm (from ca_step).");
        static_assert(
            std::is_same_v<decltype(p_minimizer_algorithm), ::bioseq::simd::minimizer_algorithm>,
            "pipeline_run_leaf: p_minimizer_algorithm must be minimizer_algorithm (from ma_step).");

        template <class p_query_minimizers>
        BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR static match_result_collection
        call(const ::bioseq::simd::minimap2_parameters& params,
             bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
             const p_query_minimizers& query_minimizers,
             const p_index& target_index,
             bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
             bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
             bool allow_gpu_target_index_build,
             transient_target_index_release_authorization transient_target_index_owner = {}) {
            using specialized_parameters =
                static_aligner_parameters<10u, p_kmer_size, p_bucket_width>;
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS)
            using fallback_parameters = fallback_aligner_parameters<63>;
#else
            using fallback_parameters = fallback_aligner_parameters<p_bucket_width>;
#endif

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION)
            // Both parameter specializations are available; the runtime condition
            // selects the matching specialization.
            if (params.window_size == 10 && params.kmer_size == static_cast<int32_t>(p_kmer_size)) {
                return pipeline_run_impl<specialized_parameters,
                                         p_alignment_algorithm,
                                         p_chaining_algorithm,
                                         p_minimizer_algorithm,
                                         p_entry_point,
                                         p_alphabet,
                                         p_minimizer,
                                         p_index,
                                         p_query_minimizers>(query_seq,
                                                             target_seq,
                                                             query_minimizers,
                                                             target_index,
                                                             params,
                                                             gpu_index_cache,
                                                             gpu_index_identity,
                                                             allow_gpu_target_index_build,
                                                             transient_target_index_owner);
            }
#endif
            return pipeline_run_impl<fallback_parameters,
                                     p_alignment_algorithm,
                                     p_chaining_algorithm,
                                     p_minimizer_algorithm,
                                     p_entry_point,
                                     p_alphabet,
                                     p_minimizer,
                                     p_index,
                                     p_query_minimizers>(query_seq,
                                                         target_seq,
                                                         query_minimizers,
                                                         target_index,
                                                         params,
                                                         gpu_index_cache,
                                                         gpu_index_identity,
                                                         allow_gpu_target_index_build,
                                                         transient_target_index_owner);
        }
    };

    template <pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index>
    struct pipeline_run_traits {
        template <auto p_kmer_size,
                  auto p_bucket_width,
                  auto p_alignment_algorithm,
                  auto p_chaining_algorithm,
                  auto p_minimizer_algorithm>
        using leaf = pipeline_run_leaf<p_entry_point,
                                       p_alphabet,
                                       p_minimizer,
                                       p_index,
                                       p_bucket_width,
                                       p_kmer_size,
                                       p_alignment_algorithm,
                                       p_chaining_algorithm,
                                       p_minimizer_algorithm>;
    };

    // Scheduler-owned raw entries have already selected the representation from
    // the same `(kmer_size, max(query_bp, target_bp))` values consumed by the
    // pipeline cascade.  Preserve those two pinned values while dispatching the
    // remaining algorithm axes, instead of rebuilding every impossible sibling
    // representation below each already-selected outer leaf.
    template <pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index,
              std::size_t p_bucket_width,
              std::size_t p_kmer_size>
    struct pipeline_run_pinned_shape_traits {
        template <auto p_alignment_algorithm, auto p_chaining_algorithm, auto p_minimizer_algorithm>
        using leaf = pipeline_run_leaf<p_entry_point,
                                       p_alphabet,
                                       p_minimizer,
                                       p_index,
                                       p_bucket_width,
                                       p_kmer_size,
                                       p_alignment_algorithm,
                                       p_chaining_algorithm,
                                       p_minimizer_algorithm>;
    };

    /// Common leaf body shared by specialized and fallback parameter branches in
    /// `describe_leaf::call`. Declared before
    /// `describe_leaf` so its unqualified call inside `describe_leaf::call`
    /// resolves at the point of template definition.
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm,
              pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet>
    [[nodiscard]] pipeline_description
    describe_pipeline_leaf(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                           bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                           const ::bioseq::simd::minimap2_parameters& params,
                           sequence_size_t query_len,
                           sequence_size_t target_len,
                           const query_sketch_route query_sketch_route) {
        using p_minimizer = typename p_aligner_parameters::minimizer_repr_t;
        const std::vector<p_minimizer> empty_minimizers{};
        minimizer_hashmap_fragment_index empty_index{};
        match_result_collection ignored_out{};
        const auto& sm = thread_local_scoring_matrix<p_alphabet>(params);
        /* Description construction inspects selected stage traits and frozen
         * parameters. It never executes a kernel. Keep the runtime Highway table
         * out of this cold metadata path so plain EXPLAIN can describe an unbound
         * GUC snapshot before BatchProcessor performs the one allowed dispatch. */
        const ::bioseq::simd::alignment_kernels<p_alphabet> description_alignment_kernels{};

        auto pipeline =
            make_minimap_pipeline<p_aligner_parameters,
                                  p_alignment_algorithm,
                                  p_chaining_algorithm,
                                  p_minimizer_algorithm,
                                  p_entry_point,
                                  p_alphabet,
                                  p_minimizer,
                                  minimizer_hashmap_fragment_index>(query_seq,
                                                                    target_seq,
                                                                    empty_minimizers,
                                                                    empty_index,
                                                                    sm,
                                                                    description_alignment_kernels,
                                                                    ignored_out,
                                                                    params,
                                                                    query_len,
                                                                    target_len);
        auto description = describe_with_params<p_aligner_parameters>(pipeline);
        if (complete_all_gpu_tuple_selected(params)) {
            /* The scheduler admits this exact tuple only to the dedicated
             * resident GPU route and returns before constructing the generic
             * CPU/simd pipeline. In particular, bounded CIGAR is produced by the
             * Vulkan extension, not by the selected block_aligner trait. Keep the
             * generic stage skeleton for EXPLAIN, but do not advertise any of its
             * bypassed Highway entry points as planned work. */
            for (auto& stage : description.stages) {
                if (stage.algorithm.has_value()) {
                    stage.algorithm->highway_kernels.reset();
                }
            }
        }
        auto query_sketch = describe_query_sketch_stage(params, query_sketch_route);
        if (!query_sketch.has_value()) {
            throw std::invalid_argument{
                "query-sketch description received an unsupported algorithm or "
                "alphabet code"};
        }
        description.stages.insert(description.stages.begin(), std::move(*query_sketch));
        return description;
    }

    // Describe-side leaf — p_entry_point/p_minimizer_algorithm remain pinned so the structural
    // stage skeleton does not multiply describe instantiations.  The exact query-minimizer Highway
    // function is resolved neutrally from the frozen runtime enum and attached to the plan-only
    // QuerySketchStage above.
    template <sequence_alphabet p_alphabet,
              std::size_t p_bucket_width,
              std::size_t p_kmer_size,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm>
    struct describe_leaf {
        static_assert(
            std::is_same_v<decltype(p_bucket_width), std::size_t>,
            "describe_leaf: p_bucket_width must be std::size_t (from seq_len_log2_step).");
        static_assert(std::is_same_v<decltype(p_kmer_size), std::size_t>,
                      "describe_leaf: p_kmer_size must be std::size_t (from kmer_size_step).");
        static_assert(std::is_same_v<decltype(p_alignment_algorithm),
                                     ::bioseq::simd::base_alignment_algorithm>,
                      "describe_leaf: p_alignment_algorithm must be base_alignment_algorithm (from "
                      "baa_step).");
        static_assert(
            std::is_same_v<decltype(p_chaining_algorithm), ::bioseq::simd::chaining_algorithm>,
            "describe_leaf: p_chaining_algorithm must be chaining_algorithm (from ca_step).");

        BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR
        static pipeline_description call(const ::bioseq::simd::minimap2_parameters& params,
                                         bioseq::storage::lazy_sequence_view<p_alphabet> q,
                                         bioseq::storage::lazy_sequence_view<p_alphabet> t,
                                         const int32_t& query_len,
                                         const int32_t& target_len,
                                         const query_sketch_route& query_sketch_route) {
            using specialized_parameters =
                static_aligner_parameters<10u, p_kmer_size, p_bucket_width>;
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS)
            using fallback_parameters = fallback_aligner_parameters<63>;
#else
            using fallback_parameters = fallback_aligner_parameters<p_bucket_width>;
#endif

            constexpr auto describe_entry_point = pipeline_entry_point::with_prebuilt_minimizers;
            constexpr auto describe_minimizer_algorithm =
                algorithm_enum_values<::bioseq::simd::minimizer_algorithm>::default_value;
            static_assert(
                describe_minimizer_algorithm == ::bioseq::simd::minimizer_algorithm::scalar,
                "describe_leaf relies on p_minimizer_algorithm default = scalar.  Update both call "
                "sites if the p_minimizer_algorithm default ever changes.");

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION)
            if (params.window_size == 10 && params.kmer_size == static_cast<int32_t>(p_kmer_size)) {
                return describe_pipeline_leaf<specialized_parameters,
                                              p_alignment_algorithm,
                                              p_chaining_algorithm,
                                              describe_minimizer_algorithm,
                                              describe_entry_point,
                                              p_alphabet>(
                    q, t, params, query_len, target_len, query_sketch_route);
            }
#endif
            return describe_pipeline_leaf<fallback_parameters,
                                          p_alignment_algorithm,
                                          p_chaining_algorithm,
                                          describe_minimizer_algorithm,
                                          describe_entry_point,
                                          p_alphabet>(
                q, t, params, query_len, target_len, query_sketch_route);
        }
    };

    template <sequence_alphabet p_alphabet> struct describe_traits {
        // The chain pins p_kmer_size before p_bucket_width.
        template <auto p_kmer_size,
                  auto p_bucket_width,
                  auto p_alignment_algorithm,
                  auto p_chaining_algorithm>
        using leaf = describe_leaf<p_alphabet,
                                   p_bucket_width,
                                   p_kmer_size,
                                   p_alignment_algorithm,
                                   p_chaining_algorithm>;
    };

    } // namespace detail

    // =============================================================================
    // / — top-level runtime cascade entry
    // =============================================================================
    //
    // The runtime cascade is one `dispatch_chain<...>::run<>(...)` over the
    // bucket-width, k-mer-size, alignment, chaining, and minimizer dimensions.

    template <pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index,
              class p_query_minimizers = std::vector<p_minimizer>>
    BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR match_result_collection dispatch_pipeline(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        const p_query_minimizers& query_minimizers,
        const p_index& target_index,
        const ::bioseq::simd::minimap2_parameters& params,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false,
        transient_target_index_release_authorization transient_target_index_owner = {}) {
        using traits_type =
            detail::pipeline_run_traits<p_entry_point, p_alphabet, p_minimizer, p_index>;
        // Pin p_kmer_size before p_bucket_width so only relevant breakpoints run.
        return detail::dispatch_chain<
            match_result_collection,
            traits_type::template leaf,
            detail::kmer_size_step,
            detail::seq_len_log2_step_per_k,
            detail::baa_step,
            detail::ca_step,
            detail::guarded_step<detail::ma_step,
                                 /*active=*/(p_entry_point !=
                                             pipeline_entry_point::with_prebuilt_minimizers)>>::
            template run<>(params,
                           query_seq,
                           target_seq,
                           query_minimizers,
                           target_index,
                           gpu_index_cache,
                           gpu_index_identity,
                           allow_gpu_target_index_build,
                           transient_target_index_owner);
    }

    /**
     * Scheduler-private counterpart of `dispatch_pipeline` for a raw
     * representation leaf whose k-mer and sequence-length bucket were fixed by
     * the outer raw cascade.  The leaf body remains the sole owner of the
     * full-specialization specialised/fallback branch. Only the redundant second
     * k-mer/bucket cascade is removed.
     */
    template <std::size_t p_bucket_width,
              std::size_t p_kmer_size,
              pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index,
              class p_query_minimizers = std::vector<p_minimizer>>
    BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR match_result_collection
    dispatch_pipeline_pinned_shape(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        const p_query_minimizers& query_minimizers,
        const p_index& target_index,
        const ::bioseq::simd::minimap2_parameters& params,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false,
        transient_target_index_release_authorization transient_target_index_owner = {}) {
        static_assert(p_kmer_size > 0u);
        static_assert(p_bucket_width >= p_kmer_size);
        using traits_type = detail::pipeline_run_pinned_shape_traits<p_entry_point,
                                                                     p_alphabet,
                                                                     p_minimizer,
                                                                     p_index,
                                                                     p_bucket_width,
                                                                     p_kmer_size>;
        return detail::dispatch_chain<
            match_result_collection,
            traits_type::template leaf,
            detail::baa_step,
            detail::ca_step,
            detail::guarded_step<detail::ma_step,
                                 /*active=*/(p_entry_point !=
                                             pipeline_entry_point::with_prebuilt_minimizers)>>::
            template run<>(params,
                           query_seq,
                           target_seq,
                           query_minimizers,
                           target_index,
                           gpu_index_cache,
                           gpu_index_identity,
                           allow_gpu_target_index_build,
                           transient_target_index_owner);
    }

    // =============================================================================
    // — describe_pipeline_for (EXPLAIN path)
    // =============================================================================
    //
    // One chain instantiation over four steps (p_bucket_width, p_kmer_size, p_alignment_algorithm,
    // p_chaining_algorithm). p_entry_point and p_minimizer_algorithm are pinned constants inside
    // `describe_leaf`; algorithm-specific query-sketch metadata is added through the neutral
    // enum-to-kernel mapping above.

    template <sequence_alphabet p_alphabet>
    BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR pipeline_description describe_pipeline_for(
        const ::bioseq::simd::minimap2_parameters& params,
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_proto,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq_proto,
        const query_sketch_route query_sketch_route = query_sketch_route::recompute_capable) {
        const sequence_size_t query_len = query_seq_proto.bp_size();
        const sequence_size_t target_len = target_seq_proto.bp_size();

        using traits_type = detail::describe_traits<p_alphabet>;
        // Pin p_kmer_size before the dependent p_bucket_width step.
        return detail::dispatch_chain<pipeline_description,
                                      traits_type::template leaf,
                                      detail::kmer_size_step,
                                      detail::seq_len_log2_step_per_k,
                                      detail::baa_step,
                                      detail::ca_step>::template run<>(params,
                                                                       query_seq_proto,
                                                                       target_seq_proto,
                                                                       query_len,
                                                                       target_len,
                                                                       query_sketch_route);
    }

    /**
     * Cold typed EXPLAIN entry. The owning metadata registry selects `p_alphabet`
     * and supplies its runtime code together, so this layer neither redispatches
     * nor maintains a second type/code map.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] inline pipeline_description describe_pipeline_for(
        const ::bioseq::simd::minimap2_parameters& params,
        const std::uint8_t alphabet_code,
        const query_sketch_route query_sketch_route = query_sketch_route::recompute_capable) {
        if (params.highway_kernels != nullptr) {
            const auto* identity =
                params.highway_kernels->is_complete()
                    ? params.highway_kernels->template resolve_alphabet_identity<p_alphabet>()
                    : nullptr;
            if (identity == nullptr || identity->alphabet_code != alphabet_code ||
                params.highway_kernels->resolve_alphabet_identity(alphabet_code) != identity) {
                throw std::invalid_argument{
                    "pipeline description received mismatched alphabet identity"};
            }
        }
        using sequence_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
        compact_sequence_range<sequence_type> empty_query_proto{};
        compact_sequence_range<sequence_type> empty_target_proto{};
        auto empty_query_owner = bioseq::storage::make_resident_lazy_sequence(empty_query_proto);
        auto empty_target_owner = bioseq::storage::make_resident_lazy_sequence(empty_target_proto);
        auto typed_params = params;
        typed_params.alphabet_code = alphabet_code;
        return describe_pipeline_for(typed_params,
                                     bioseq::storage::make_lazy_sequence_view(empty_query_owner),
                                     bioseq::storage::make_lazy_sequence_view(empty_target_owner),
                                     query_sketch_route);
    }

    } // namespace bioseq::simd::pipe

    namespace bioseq::simd {

    /**
     * @brief Run the minimap2 pipeline against a pre-built target index using a
     * caller-supplied query minimiser vector.
     *
     * Single-query convenience wrapper that hands off to
     * `pipe::dispatch_pipeline<p_entry_point::with_prebuilt_minimizers>(...)`.  All algorithm-
     * choice and bucket dispatch happens inside the pipe cascade — no
     * `(p_alignment_algorithm, p_chaining_algorithm)` switch at this layer.
     */
    template <class p_minimizer,
              sequence_alphabet p_alphabet,
              ::bioseq::indexing::minimizer_seed_index p_index>
    [[nodiscard]] match_result_collection
    minimap2_port_with_index(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                             const std::vector<p_minimizer>& query_minimizers,
                             const p_index& prebuilt_target_index,
                             const minimap2_parameters& params = minimap2_parameters{},
                             bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
                             bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
                             bool allow_gpu_target_index_build = false) {
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("minimap2_port_with_index");
        auto _scope = bioseq::instrumentation::region(_desc);
        return ::bioseq::simd::pipe::dispatch_pipeline<
            ::bioseq::simd::pipe::pipeline_entry_point::with_prebuilt_minimizers,
            p_alphabet,
            p_minimizer,
            p_index>(query_seq,
                     target_seq,
                     query_minimizers,
                     prebuilt_target_index,
                     params,
                     gpu_index_cache,
                     gpu_index_identity,
                     allow_gpu_target_index_build);
    }

    template <class p_minimizer,
              sequence_alphabet p_alphabet,
              ::bioseq::indexing::minimizer_seed_index p_index>
    [[nodiscard]] match_result_collection minimap2_port_with_index(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        const ::bioseq::simd::pipe::minimizer_batch_view<p_minimizer>& query_minimizers,
        const p_index& prebuilt_target_index,
        const minimap2_parameters& params = minimap2_parameters{},
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false) {
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("minimap2_port_with_index");
        auto _scope = bioseq::instrumentation::region(_desc);
        return ::bioseq::simd::pipe::dispatch_pipeline<
            ::bioseq::simd::pipe::pipeline_entry_point::with_prebuilt_minimizers,
            p_alphabet,
            p_minimizer,
            p_index>(query_seq,
                     target_seq,
                     query_minimizers,
                     prebuilt_target_index,
                     params,
                     gpu_index_cache,
                     gpu_index_identity,
                     allow_gpu_target_index_build);
    }

    namespace pipe_entry_detail {

    struct dynamic_shape_pipeline_dispatch {
        template <::bioseq::simd::pipe::pipeline_entry_point p_entry_point,
                  sequence_alphabet p_alphabet,
                  class p_minimizer,
                  class p_index,
                  class p_query_minimizers>
        BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR static match_result_collection
        call(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
             const p_query_minimizers& query_minimizers,
             const p_index& target_index,
             const minimap2_parameters& params,
             bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
             bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
             bool allow_gpu_target_index_build,
             ::bioseq::simd::pipe::transient_target_index_release_authorization
                 transient_target_index_owner) {
            return ::bioseq::simd::pipe::dispatch_pipeline<p_entry_point,
                                                           p_alphabet,
                                                           p_minimizer,
                                                           p_index,
                                                           p_query_minimizers>(
                query_seq,
                target_seq,
                query_minimizers,
                target_index,
                params,
                gpu_index_cache,
                gpu_index_identity,
                allow_gpu_target_index_build,
                transient_target_index_owner);
        }
    };

    template <std::size_t p_bucket_width, std::size_t p_kmer_size>
    struct pinned_shape_pipeline_dispatch {
        template <::bioseq::simd::pipe::pipeline_entry_point p_entry_point,
                  sequence_alphabet p_alphabet,
                  class p_minimizer,
                  class p_index,
                  class p_query_minimizers>
        BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR static match_result_collection
        call(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
             const p_query_minimizers& query_minimizers,
             const p_index& target_index,
             const minimap2_parameters& params,
             bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
             bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
             bool allow_gpu_target_index_build,
             ::bioseq::simd::pipe::transient_target_index_release_authorization
                 transient_target_index_owner) {
            static_assert(
                std::same_as<
                    std::remove_cvref_t<p_minimizer>,
                    ::bioseq::minimizers::minimizer_representation<p_bucket_width, p_kmer_size>>);
            return ::bioseq::simd::pipe::dispatch_pipeline_pinned_shape<p_bucket_width,
                                                                        p_kmer_size,
                                                                        p_entry_point,
                                                                        p_alphabet,
                                                                        p_minimizer,
                                                                        p_index,
                                                                        p_query_minimizers>(
                query_seq,
                target_seq,
                query_minimizers,
                target_index,
                params,
                gpu_index_cache,
                gpu_index_identity,
                allow_gpu_target_index_build,
                transient_target_index_owner);
        }
    };

    } // namespace pipe_entry_detail

    template <class p_minimizer, sequence_alphabet p_alphabet>
    [[nodiscard]] match_result_collection
    minimap2_port(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                  bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                  const std::vector<p_minimizer>& query_minimizers,
                  std::vector<p_minimizer> target_minimizers,
                  const minimap2_parameters& params = minimap2_parameters{}) {
        return pipe_entry_detail::minimap2_port_impl<
            pipe_entry_detail::dynamic_shape_pipeline_dispatch,
            p_minimizer,
            p_alphabet>(
            query_seq, target_seq, query_minimizers, std::move(target_minimizers), params);
    }

    namespace pipe_entry_detail {

    template <class p_minimizer, sequence_alphabet p_alphabet>
    [[nodiscard]] match_result_collection minimap2_port_with_fixed_vulkan_target_handoff(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        const std::vector<p_minimizer>& query_minimizers,
        fixed_vulkan_target_host_owner<p_minimizer> target_host,
        const minimap2_parameters& params) {
        return minimap2_port_with_fixed_vulkan_target_handoff_impl<dynamic_shape_pipeline_dispatch,
                                                                   p_minimizer,
                                                                   p_alphabet>(
            query_seq, target_seq, query_minimizers, std::move(target_host), params);
    }

    template <class p_minimizer,
              class p_collector,
              class p_range,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm>
    struct sketch_leaf {
        static_assert(
            std::is_same_v<decltype(p_minimizer_algorithm), ::bioseq::simd::minimizer_algorithm>,
            "sketch_leaf: p_minimizer_algorithm must be minimizer_algorithm (from ma_step).");

        BIOSEQ_PIPELINE_INLINE_ATTR
        static minimizer_sketch_execution_stats
        call(const ::bioseq::simd::minimap2_parameters& params,
             p_collector& collector,
             p_range& range,
             const std::size_t& window_len,
             const std::size_t& kmer_len) {
            using p_kernel =
                typename ::bioseq::simd::pipe::algorithm_traits<p_minimizer_algorithm>::kernel_type;
            if constexpr (p_kernel::emits_scalar_minimizer_execution_stats) {
                return {
                    .scalar = p_kernel::template call<p_minimizer>(
                        params, collector, range, window_len, kmer_len),
                    .scalar_available = true,
                };
            } else if constexpr (p_kernel::emits_exact_simd_execution_stats) {
                return {
                    .exact_simd = p_kernel::template call<p_minimizer>(
                        params, collector, range, window_len, kmer_len),
                    .exact_simd_available = true,
                };
            } else if constexpr (p_kernel::emits_bounded_nthash_execution_stats) {
                return {
                    .bounded_nthash = p_kernel::template call<p_minimizer>(
                        params, collector, range, window_len, kmer_len),
                    .bounded_nthash_available = true,
                };
            } else if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
                const auto token = p_kernel::template call<p_minimizer>(
                    params, collector, range, window_len, kmer_len);
                if (!token.is_internally_consistent()) {
                    ::bioseq::simd::pipe::throw_vulkan_declined(
                        "Vulkan minimizer sketch execution token");
                }
                minimizer_sketch_execution_stats stats{};
                stats.partial_vulkan.sketch_dispatches = token.completed_roundtrips;
                return stats;
            } else {
                p_kernel::template call<p_minimizer>(
                    params, collector, range, window_len, kmer_len);
                return {};
            }
        }
    };

    template <class p_minimizer, class p_collector, class p_range> struct sketch_traits {
        template <auto p_minimizer_algorithm>
        using leaf = sketch_leaf<p_minimizer, p_collector, p_range, p_minimizer_algorithm>;
    };

    /// One-step chain over the p_minimizer_algorithm dimension. The chain pins
    /// p_minimizer_algorithm at compile time and the
    /// leaf dispatches via the trait's kernel.
    template <class p_minimizer, class p_collector, class p_range>
    BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR minimizer_sketch_execution_stats
    sketch_one(p_collector& collector,
               p_range& range,
               const std::size_t& window_len,
               const std::size_t& kmer_len,
               const ::bioseq::simd::minimap2_parameters& params) {
        using traits_type = sketch_traits<p_minimizer, p_collector, p_range>;
        return ::bioseq::simd::pipe::detail::dispatch_chain<minimizer_sketch_execution_stats,
                                                            traits_type::template leaf,
                                                            ::bioseq::simd::pipe::detail::ma_step>::
            template run<>(params, collector, range, window_len, kmer_len);
    }

    /// Packages the per-pass instrumentation scope, collector reservation, and
    /// sketch dispatch into one inline call.
    template <class p_minimizer, class p_range>
    BIOSEQ_PIPELINE_INLINE_ATTR minimizer_sketch_execution_stats
    sketch_into_collector(basic_sketch_collector<p_minimizer>& collector,
                          p_range& range,
                          const std::size_t& window_len,
                          const std::size_t& kmer_len,
                          const ::bioseq::simd::minimap2_parameters& params) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("mm_sketch");
        auto _scope = bioseq::instrumentation::region(_desc);
        collector.reserve(range.bp_size() / std::max<std::size_t>(window_len, 1) + 64);
        return sketch_one<p_minimizer>(collector, range, window_len, kmer_len, params);
    }

    /**
     * @brief Per-bucket helper for the from-sequence `minimap_port` entry.
     *
     * Owns one `Arena::chain` scope around the target minimiser sketching + index
     * build. Sketching is dispatched via `sketch_into_collector<p_minimizer>` and
     * a single-step `dispatch_chain` in `sketch_one` resolves the requested
     * minimizer algorithm. Once the
     * target index is built, the function delegates to `minimap2_port_with_index`
     * so the per-query minimizer_stage inside `pipe::dispatch_pipeline` handles
     * query sketching.
     */
    template <class p_pipeline_dispatch, class p_minimizer, sequence_alphabet p_alphabet>
    [[nodiscard]] match_result_collection
    minimap_port_with_repr_impl(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                                bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                                const minimap2_parameters& params) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("minimap_port");
        auto _scope = bioseq::instrumentation::region(_desc);

        std::optional<context::context> owned_context;
        std::optional<context::installed_context_scope> context_install;
        if (context::current_or_null() == nullptr) {
            owned_context.emplace();
            context_install.emplace(*owned_context);
        }

        const std::size_t window_len = static_cast<std::size_t>(params.window_size);
        const std::size_t kmer_len = static_cast<std::size_t>(params.kmer_size);

        // — skip the host target sketch for the Stage C GPU-index leaf;
        // the device index build sketches the target itself. If that selected GPU
        // path cannot run, the pipeline throws instead of rebuilding on CPU.
        // The fixed Vulkan host vector carries either footprint authority or its
        // exact retained permit. Its dedicated handoff owns the later CPU-index
        // permit locally and destroys the nested index before releasing it.
        std::vector<p_minimizer> target_minimizers;
        std::optional<fixed_vulkan_target_sketch<p_minimizer>> fixed_target_sketch;
        minimizer_sketch_execution_stats target_sketch_stats{};
        const bool is_gpu_target_index_build =
            ::bioseq::simd::pipe::is_gpu_target_index_build_active(params);
        using target_alphabet_type = p_alphabet;
        const bool fixed_target_zero_copy_route =
            fixed_vulkan_target_host_adoption_route_selected<p_minimizer>(
                is_gpu_target_index_build,
                params,
                static_cast<std::uint32_t>(seqan3::alphabet_size<target_alphabet_type>),
                static_cast<std::uint64_t>(target_seq.bp_size()));
        if (!is_gpu_target_index_build) {
            if (fixed_target_zero_copy_route) {
                fixed_target_sketch.emplace(sketch_vulkan_into_adopted_host_vector<p_minimizer>(
                    target_seq, window_len, kmer_len, params));
                target_sketch_stats = fixed_target_sketch->stats;
            } else {
                basic_sketch_collector<p_minimizer> target_collector;
                target_sketch_stats = sketch_into_collector<p_minimizer>(
                    target_collector, target_seq, window_len, kmer_len, params);
                target_minimizers = std::move(target_collector).extract_sketches_data();
            }
        }

        const std::uint64_t query_sketch_started_ns = pipe::host_stage_now_ns();
        // Query results are intentionally left on the collector-compatible route.
        // Small/repeated allocations can reuse already-resident allocator pages,
        // so a footprint-delta release is not reliable authority for them.
        basic_sketch_collector<p_minimizer> query_collector;
        const minimizer_sketch_execution_stats query_sketch_stats =
            sketch_into_collector<p_minimizer>(
                query_collector, query_seq, window_len, kmer_len, params);
        const auto query_minimizers = std::move(query_collector).extract_sketches_data();
        const std::uint64_t query_sketch_host_wall_ns =
            pipe::elapsed_host_stage_ns(query_sketch_started_ns);
        const bool query_route_valid = minimizer_sketch_stats_matches_selected_route(
            params.minimizer_algorithm, query_sketch_stats);
        const bool target_route_valid =
            is_gpu_target_index_build || minimizer_sketch_stats_matches_selected_route(
                                             params.minimizer_algorithm, target_sketch_stats);
        const bool exact_simd_pair_route_compatible =
            is_gpu_target_index_build ||
            params.minimizer_algorithm != minimizer_algorithm::two_stack_nthash ||
            (query_sketch_stats.exact_simd_available && target_sketch_stats.exact_simd_available &&
             pipe::exact_simd_stats_share_selection_route(query_sketch_stats.exact_simd,
                                                          target_sketch_stats.exact_simd));
        if (!query_route_valid || !target_route_valid || !exact_simd_pair_route_compatible) {
            bioseq_log<logging_level::error>(
                "Selected minimizer route returned mismatched execution stats or "
                "incompatible exact_simd selection schemes");
            std::abort();
        }

        auto owned_target_minimizers = std::move(target_minimizers);
        std::vector<p_minimizer>{}.swap(target_minimizers);
        match_result_collection result =
            fixed_target_zero_copy_route
                ? minimap2_port_with_fixed_vulkan_target_handoff_impl<p_pipeline_dispatch,
                                                                      p_minimizer,
                                                                      p_alphabet>(
                      query_seq,
                      target_seq,
                      query_minimizers,
                      std::move(fixed_target_sketch->host),
                      params)
                : minimap2_port_impl<p_pipeline_dispatch, p_minimizer, p_alphabet>(
                      query_seq,
                      target_seq,
                      query_minimizers,
                      std::move(owned_target_minimizers),
                      params);
        result.record_query_sketch_host_time(query_sketch_host_wall_ns);
        record_minimizer_sketch_execution_stats(
            result, query_sketch_stats, minimizer_sketch_role::query);
        if (!is_gpu_target_index_build) {
            record_minimizer_sketch_execution_stats(
                result, target_sketch_stats, minimizer_sketch_role::target);
        }
        return result;
    }

    template <class p_minimizer, sequence_alphabet p_alphabet>
    [[nodiscard]] match_result_collection
    minimap_port_with_repr(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                           bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                           const minimap2_parameters& params) {
        return minimap_port_with_repr_impl<dynamic_shape_pipeline_dispatch,
                                           p_minimizer,
                                           p_alphabet>(query_seq, target_seq, params);
    }

    // =============================================================================
    // — from-sequence cascade leaf and traits
    // =============================================================================
    //
    // `minimap_port` uses one `dispatch_chain<...>::run<>(...)` over
    // two steps (p_bucket_width, p_kmer_size).  The leaf below pins `(p_bucket_width, p_kmer_size)`
    // into the `minimizer_representation<p_bucket_width, p_kmer_size>` storage type and delegates
    // to `minimap_port_with_repr<p_minimizer>`.

    template <sequence_alphabet p_alphabet, std::size_t p_bucket_width, std::size_t p_kmer_size>
    struct minimap_port_repr_leaf {
        static_assert(
            std::is_same_v<decltype(p_bucket_width), std::size_t>,
            "minimap_port_repr_leaf: p_bucket_width must be std::size_t (from seq_len_log2_step).");
        static_assert(
            std::is_same_v<decltype(p_kmer_size), std::size_t>,
            "minimap_port_repr_leaf: p_kmer_size must be std::size_t (from kmer_size_step).");

        BIOSEQ_PIPELINE_INLINE_ATTR
        static match_result_collection call(const minimap2_parameters& params,
                                            bioseq::storage::lazy_sequence_view<p_alphabet> q,
                                            bioseq::storage::lazy_sequence_view<p_alphabet> t) {
            using p_minimizer =
                ::bioseq::minimizers::minimizer_representation<p_bucket_width, p_kmer_size>;
            return minimap_port_with_repr<p_minimizer, p_alphabet>(q, t, params);
        }
    };

    template <sequence_alphabet p_alphabet> struct minimap_port_traits {
        template <auto p_kmer_size, auto p_bucket_width>
        using leaf = minimap_port_repr_leaf<p_alphabet, p_bucket_width, p_kmer_size>;
    };

    template <sequence_alphabet p_alphabet, std::size_t p_bucket_width, std::size_t p_kmer_size>
    struct minimap_port_owned_repr_leaf {
        static_assert(std::is_same_v<decltype(p_bucket_width), std::size_t>);
        static_assert(std::is_same_v<decltype(p_kmer_size), std::size_t>);

        BIOSEQ_PIPELINE_INLINE_ATTR
        static match_result_collection
        call(const minimap2_parameters& params,
             bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq) {
            using p_minimizer =
                ::bioseq::minimizers::minimizer_representation<p_bucket_width, p_kmer_size>;
            using pipeline_dispatch = pinned_shape_pipeline_dispatch<p_bucket_width, p_kmer_size>;
            return minimap_port_with_repr_impl<pipeline_dispatch, p_minimizer, p_alphabet>(
                query_seq, target_seq, params);
        }
    };

    template <sequence_alphabet p_alphabet> struct minimap_port_owned_traits {
        template <auto p_kmer_size, auto p_bucket_width>
        using leaf = minimap_port_owned_repr_leaf<p_alphabet, p_bucket_width, p_kmer_size>;
    };

    template <sequence_alphabet p_alphabet>
    BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR match_result_collection
    minimap_port_owned_pinned_shape(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                                    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                                    const minimap2_parameters& params) {
        using traits_type = minimap_port_owned_traits<p_alphabet>;
        return ::bioseq::simd::pipe::detail::dispatch_chain<
            match_result_collection,
            traits_type::template leaf,
            ::bioseq::simd::pipe::detail::kmer_size_step,
            ::bioseq::simd::pipe::detail::seq_len_log2_step_per_k>::template run<>(params,
                                                                                   query_seq,
                                                                                   target_seq);
    }

    } // namespace pipe_entry_detail

    /**
     * @brief Top-level minimap2 entry for raw query and target sequences.
     *
     * One `dispatch_chain<...>::run<>(...)` line over `seq_len_log2_step` +
     * `kmer_size_step`. Each pinned
     * `(p_bucket_width, p_kmer_size)` selects a `minimizer_representation<p_bucket_width,
     * p_kmer_size>` and forwards to `pipe_entry_detail::minimap_port_with_repr<p_minimizer>` which
     * sketches both target and query minimisers, builds the target index, and delegates to
     * `minimap2_port_with_index` (which in turn calls
     * `pipe::dispatch_pipeline<p_entry_point::with_prebuilt_minimizers>`).
     */
    template <sequence_alphabet p_alphabet>
    BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR match_result_collection
    minimap_port(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                 bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                 const minimap2_parameters& params = minimap2_parameters{}) {
        using traits_type = pipe_entry_detail::minimap_port_traits<p_alphabet>;
        // Pin p_kmer_size before the dependent p_bucket_width step.
        return ::bioseq::simd::pipe::detail::dispatch_chain<
            match_result_collection,
            traits_type::template leaf,
            ::bioseq::simd::pipe::detail::kmer_size_step,
            ::bioseq::simd::pipe::detail::seq_len_log2_step_per_k>::template run<>(params,
                                                                                   query_seq,
                                                                                   target_seq);
    }

    namespace pipe_entry_detail {

    /// Dispatch segmented work through the dynamic fragment-index entry.
    struct segmented_dynamic_index_dispatch {
        /// Execute one segment against its transient fragment index.
        template <sequence_alphabet p_alphabet>
        BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR static match_result_collection
        call(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
             const pipe::minimizer_batch_view<minimap_sketch_data>& query_minimizers,
             const minimizer_hashmap_fragment_index& target_index,
             const minimap2_parameters& params,
             bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
             bioseq::indexing::gpu_index_cache_identity gpu_index_identity) {
            return minimap2_port_with_index(query_seq,
                                            target_seq,
                                            query_minimizers,
                                            target_index,
                                            params,
                                            gpu_index_cache,
                                            gpu_index_identity);
        }
    };

    /// Dispatch segmented work through an alphabet-owned fragment-index entry.
    struct segmented_owned_index_dispatch {
        /// Execute one segment through its generated alphabet owner.
        template <sequence_alphabet p_alphabet>
        BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR static match_result_collection
        call(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
             const pipe::minimizer_batch_view<minimap_sketch_data>& query_minimizers,
             const minimizer_hashmap_fragment_index& target_index,
             const minimap2_parameters& params,
             bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
             bioseq::indexing::gpu_index_cache_identity gpu_index_identity) {
            return minimap_port_fragment_index_batch_owned(query_seq,
                                                           target_seq,
                                                           query_minimizers,
                                                           target_index,
                                                           params,
                                                           gpu_index_cache,
                                                           gpu_index_identity);
        }
    };

    } // namespace pipe_entry_detail

    /// Execute the dynamic segmented route for one alphabet.
    template <sequence_alphabet p_alphabet, class p_fragment_index_storage = std::nullptr_t>
    [[nodiscard]] match_result_collection
    minimap_port_segmented(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                           bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                           const minimap2_parameters& params = minimap2_parameters{},
                           p_fragment_index_storage* fragment_index_out = nullptr) {
        return minimap_port_segmented_impl<pipe_entry_detail::segmented_dynamic_index_dispatch,
                                           p_alphabet,
                                           p_fragment_index_storage>(
            query_seq, target_seq, params, fragment_index_out);
    }

    namespace pipe_entry_detail {

    /// Execute the generated-owner segmented route for one alphabet.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] match_result_collection minimap_port_segmented_owned_dispatch(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        const minimap2_parameters& params) {
        return minimap_port_segmented_impl<segmented_owned_index_dispatch,
                                           p_alphabet,
                                           std::nullptr_t>(query_seq, target_seq, params, nullptr);
    }

    } // namespace pipe_entry_detail

    // The three-argument scheduler route always uses a null fragment-index output.
    // Own that concrete post-erasure specialization out of line while retaining
    // the four-argument template for callers that request index materialization.
    [[nodiscard, gnu::noinline]] match_result_collection
    minimap_port_segmented_owned(bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
                                 bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
                                 const minimap2_parameters& params = minimap2_parameters{});

    [[nodiscard, gnu::noinline]] match_result_collection
    minimap_port_segmented_owned(bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
                                 bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
                                 const minimap2_parameters& params = minimap2_parameters{});

    } // namespace bioseq::simd

} // export
