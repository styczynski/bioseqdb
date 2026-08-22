/**
 * @file pipeline/pipe_parts/execution_scalar_highway.cppm
 * @brief Runs scalar and Highway pipeline stages.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:execution_scalar_highway;

import std;
import :stage_graph;
import :minimizer_execution;
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
import bioseq.pipeline.stats;
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
import :execution_plan_context;

export {

    namespace bioseq::simd::pipe {

    // =============================================================================
    // Concrete stages
    // =============================================================================
    //
    // Each stage's `process*<p_aligner_parameters>(...)` body forwards to the existing kernel
    // function in the target-neutral pipeline or its selected kernel table
    // without renaming the kernel.  Wire-type between stages is `std::span<p_element>`
    // directly; arena ownership is provided by `arena_slab<p_element>` materialised
    // inside each stage before the downstream call.

    /**
     * @brief Minimizer extraction stage.
     *
     * Templated on `p_kernel` — the algorithm-specific call surface exposed by
     * `algorithm_traits<minimizer_algorithm::X>::kernel_type`.
     * The stage body has zero `if constexpr` over enum
     * values. Adding a new minimizer-extraction algorithm is a single trait
     * specialisation in the kernel's own module.
     *
     * The stage ignores its input batch and instead sketches the pre-captured
     * `query_seq_` pointer.
     */
    template <class p_minimizer, class p_kernel, sequence_alphabet p_alphabet>
    struct minimizer_stage
        : stage_base<minimizer_stage<p_minimizer, p_kernel, p_alphabet>, p_minimizer> {
        static constexpr std::string_view stage_name = "minimizer_stage";

        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
        std::int32_t runtime_w;
        std::int32_t runtime_k;
        // Runtime parameters let the Vulkan kernel inspect `vulkan_enabled`;
        // CPU kernels ignore them.
        ::bioseq::simd::minimap2_parameters params;
        match_result_collection* stats_dst = nullptr;
        match_result_collection* host_timing_dst = nullptr;

        // The kernel writes directly into the per-query arena slab via
        // `sketch_collector_arena_adapter`, avoiding an intermediate copy. The
        // specialised path calls `call_constexpr<p_window_size, p_kmer_size, p_minimizer>(...)` so
        // the (p_window_size, p_kmer_size) pair propagates as integer immediates through the
        // window-loop bounds and unrolls.
        /// Sketch the captured query and emit its admitted minimizer batch.
        template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_batch&& /*ignored*/, emit_token<p_sink>&& token) {
            const std::uint64_t host_stage_started =
                host_timing_dst == nullptr ? 0u : host_stage_now_ns();
            const std::size_t window_size = p_aligner_parameters::generic_fallback
                                                ? static_cast<std::size_t>(runtime_w)
                                                : p_aligner_parameters::window_len;
            const std::size_t kmer_size = p_aligner_parameters::generic_fallback
                                              ? static_cast<std::size_t>(runtime_k)
                                              : p_aligner_parameters::kmer_len;

            // Validate the zero-based position domain before any scalar, simd, or
            // Vulkan kernel can construct a masked minimizer_representation. A query shorter
            // than k is valid and naturally emits an empty minimizer batch.
            require_representable_minimizer_query_length<p_minimizer>(query_seq_.bp_size());

            // Only the Vulkan kernel exposes the device-minimizer
            // `call_result`. When it returns a result, copy the host minimizers into
            // the arena slab and forward the resident device span as stage-local
            // metadata so seed_hit_stage can skip a second query-minimizer upload.
            if constexpr (requires { p_kernel::may_emit_device_minimizers; }) {
                if constexpr (p_kernel::may_emit_device_minimizers) {
                    if (auto gpu_result = p_kernel::template call_result<p_minimizer>(
                            params, query_seq_, window_size, kmer_size)) {
                        if (stats_dst == nullptr ||
                            !gpu_result->dispatch_token.is_internally_consistent()) {
                            bioseq_log<logging_level::error>(
                                "Vulkan minimizer stage lacks an operation token or stats sink");
                            std::abort();
                        }
                        partial_vulkan_execution_stats stats{};
                        stats.query_sketch_dispatches =
                            gpu_result->dispatch_token.completed_roundtrips;
                        stats_dst->record_partial_vulkan_execution_stats(stats);
                        auto device_slab = make_arena_slab<p_minimizer>();
                        device_slab.reserve(gpu_result->host_minimizers.size());
                        if (!gpu_result->host_minimizers.empty()) {
                            std::memcpy(device_slab.data(),
                                        gpu_result->host_minimizers.data(),
                                        gpu_result->host_minimizers.size() * sizeof(p_minimizer));
                        }
                        device_slab.resize(gpu_result->host_minimizers.size());
                        minimizer_batch_view<p_minimizer> batch{
                            .host = device_slab.as_const_span(),
                            .device =
                                std::optional<
                                    bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>{
                                    gpu_result->device_span()},
                        };
                        // The reservation owns the vector capacity, so destroy
                        // that capacity before releasing its permit. `batch.device`
                        // independently retains the resident device permit.
                        std::vector<p_minimizer>{}.swap(gpu_result->host_minimizers);
                        gpu_result->host_permit.reset();
                        return std::move(token).emit_minimizer_batch(batch);
                    }
                }
            }

            auto sketches_slab = make_arena_slab<p_minimizer>();
            sketches_slab.reserve(detail::estimated_sketch_count(
                query_seq_.bp_size(), static_cast<std::int32_t>(window_size)));

            sketch_collector_arena_adapter<p_minimizer> collector{&sketches_slab};

            if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
                const auto dispatch_token = [this, &collector, window_size, kmer_size] {
                    if constexpr (!p_aligner_parameters::generic_fallback) {
                        return p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                                 p_aligner_parameters::kmer_len,
                                                                 p_minimizer>(
                            params, collector, query_seq_);
                    } else {
                        return p_kernel::template call<p_minimizer>(
                            params, collector, query_seq_, window_size, kmer_size);
                    }
                }();
                if (stats_dst == nullptr || !dispatch_token.is_internally_consistent()) {
                    bioseq_log<logging_level::error>(
                        "Vulkan minimizer stage returned no accepted execution token");
                    std::abort();
                }
                partial_vulkan_execution_stats stats{};
                stats.query_sketch_dispatches = dispatch_token.completed_roundtrips;
                stats_dst->record_partial_vulkan_execution_stats(stats);
            } else if constexpr (p_kernel::emits_scalar_minimizer_execution_stats) {
                scalar_minimizer_execution_stats stats{};
                if constexpr (!p_aligner_parameters::generic_fallback) {
                    stats = p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                              p_aligner_parameters::kmer_len,
                                                              p_minimizer>(
                        params, collector, query_seq_);
                } else {
                    stats = p_kernel::template call<p_minimizer>(
                        params, collector, query_seq_, window_size, kmer_size);
                }
                if (stats_dst == nullptr || !stats.is_coherent()) {
                    bioseq_log<logging_level::error>(
                        "scalar minimizer stage returned rejected stats");
                    std::abort();
                }
                stats_dst->record_scalar_query_minimizer_execution_stats(stats);
            } else if constexpr (p_kernel::emits_exact_simd_execution_stats) {
                const auto stats = [this, &collector, window_size, kmer_size] {
                    if constexpr (!p_aligner_parameters::generic_fallback) {
                        return p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                                 p_aligner_parameters::kmer_len,
                                                                 p_minimizer>(
                            params, collector, query_seq_);
                    } else {
                        return p_kernel::template call<p_minimizer>(
                            params, collector, query_seq_, window_size, kmer_size);
                    }
                }();
                if (stats_dst == nullptr || !stats.is_internally_consistent(
                                                /*require_nonempty_vector_work=*/false)) {
                    bioseq_log<logging_level::error>(
                        "exact_simd minimizer stage returned rejected stats");
                    std::abort();
                }
                stats_dst->record_exact_simd_query_execution_stats(stats);
            } else if constexpr (p_kernel::emits_bounded_nthash_execution_stats) {
                const auto stats = [this, &collector, window_size, kmer_size] {
                    if constexpr (!p_aligner_parameters::generic_fallback) {
                        return p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                                 p_aligner_parameters::kmer_len,
                                                                 p_minimizer>(
                            params, collector, query_seq_);
                    } else {
                        return p_kernel::template call<p_minimizer>(
                            params, collector, query_seq_, window_size, kmer_size);
                    }
                }();
                if (stats_dst == nullptr || !stats.is_internally_consistent(
                                                /*require_nonempty_vector_work=*/false)) {
                    bioseq_log<logging_level::error>(
                        "bounded_nthash_simd minimizer stage returned rejected stats");
                    std::abort();
                }
                stats_dst->record_bounded_nthash_query_execution_stats(stats);
            } else if constexpr (!p_aligner_parameters::generic_fallback) {
                p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                  p_aligner_parameters::kmer_len,
                                                  p_minimizer>(params, collector, query_seq_);
            } else {
                p_kernel::template call<p_minimizer>(
                    params, collector, query_seq_, window_size, kmer_size);
            }

            if (host_timing_dst != nullptr) {
                host_timing_dst->record_query_sketch_host_time(
                    elapsed_host_stage_ns(host_stage_started));
            }
            return std::move(token).emit_batch(
                std::span<const p_minimizer>{sketches_slab.data(), sketches_slab.size()});
        }

        /// Describe the selected query-sketch kernel and output estimate.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = std::string{p_kernel::algorithm_label()};
            p_kernel::populate_runtime_params({}, algo.parameters);
            algo.computed_properties = p_kernel::template static_properties<p_aligner_parameters>();
            algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
            desc.algorithm = std::move(algo);
            // Reserve the estimated sketch count for the arena slab. `query_seq_`
            // is borrowed; on
            // the describe path the proto is empty so bp_size() is zero — which
            // still yields the +64 safety margin from `estimated_sketch_count`.
            desc.estimated_batch_size = detail::estimated_sketch_count(
                query_seq_.bp_size(), runtime_w > 0 ? runtime_w : 10);
            return desc;
        }
    };

    /**
     * @brief Pass-through stage that decorates downstream with the target index.
     *
     * Converts the sink shape from `on_batch(span<p_minimizer>)` upstream to
     * `on_batch_with_index(span<p_minimizer>, p_index const&)` downstream.  Inlines
     * away at -O3 (zero runtime cost).
     */
    template <class p_index>
    struct index_stage
        : stage_base<index_stage<p_index>,
                     p_index /* pass-through; downstream sees the same sketch span */> {
        static constexpr std::string_view stage_name = "index_stage";

        const p_index* index_;
        bioseq::indexing::gpu_index_cache_identity identity_{};

        /// Pair the incoming minimizers with the borrowed target index.
        template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_batch&& sketches, emit_token<p_sink>&& token) {
            // Whole in-memory targets keep pointer identity (`{}`); segmented
            // recompute/assembly passes a nonzero
            // identity so a reused stack index object cannot alias another segment
            // in the GPU resident cache.
            if constexpr (is_minimizer_batch_view_v<p_batch>) {
                return std::move(token).emit_minimizer_batch_with_index(
                    std::forward<p_batch>(sketches), *index_, identity_);
            } else {
                using element_type =
                    std::remove_const_t<typename std::remove_cvref_t<p_batch>::value_type>;
                return std::move(token).emit_batch_with_index(
                    std::span<const element_type>{sketches.data(), sketches.size()},
                    *index_,
                    identity_);
            }
        }

        /// Describe the target-index decoration stage.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            desc.extra_description = "target index lookup hand-off";
            return desc;
        }
    };

    /**
     * @brief Anchor chaining stage.
     *
     * Templated on `p_kernel` — the algorithm-specific call surface exposed by
     * `algorithm_traits<chaining_algorithm::X>::kernel_type`.
     * The stage body has zero `if constexpr` over enum
     * values — adding a new chaining algorithm is a single trait specialisation
     * in the kernel's own module.
     *
     * The chosen kernel writes its `(chains, reordered_anchors)` output directly
     * into caller-provided arena_slab parameters. No intermediate vector is
     * materialised inside the stage.
     */
    template <class p_kernel> struct chain_stage : stage_base<chain_stage<p_kernel>, chain> {
        static constexpr std::string_view stage_name = "chain_stage";

        chaining_parameters cp;
        sequence_size_t query_len = 0;
        match_result_collection* stats_dst = nullptr;
        match_result_collection* host_timing_dst = nullptr;
        const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels = nullptr;

        /// Collect seed hits and execute the selected chaining kernel.
        template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_batch&& anchors_in, emit_token<p_sink>&& token) {
            const std::uint64_t host_stage_started =
                host_timing_dst == nullptr ? 0u : host_stage_now_ns();
            chaining_parameters active_cp = cp;
            active_cp.query_len = query_len;
            const std::span<const anchor> anchors_span{anchors_in.data(), anchors_in.size()};

            auto chains_slab = make_arena_slab<chain>();
            auto reordered_slab = make_arena_slab<anchor>();
            if (active_cp.coordinate_only) {
                chains_slab.reserve(1);
                // Reserve the full upstream bound before the Vulkan backend opens
                // its nested DP arena scope. A >65,536-anchor coordinate trace
                // must never grow this output slab inside that inner scope, whose
                // allocations are reclaimed before the stage emits downstream.
                reordered_slab.reserve(anchors_in.size());
            } else {
                chains_slab.reserve(
                    detail::estimated_chain_count(anchors_in.size(), active_cp.min_seed_count));
                reordered_slab.reserve(anchors_in.size());
            }

            if constexpr (p_kernel::emits_scalar_chaining_execution_stats) {
                if (stats_dst == nullptr) {
                    bioseq_log<logging_level::error>(
                        "scalar chaining stage has no execution-stats sink");
                    std::abort();
                }
                scalar_pipeline_execution_stats stats{};
                p_kernel::call(anchors_span, active_cp, chains_slab, reordered_slab, &stats);
                /* This receipt is written after the kernel returns, so a zero-hit
                 * negative is attested without claiming scalar score/backtrack
                 * work that the kernel correctly skipped. */
                stats.record_scalar_chain_stage_completion(
                    static_cast<std::uint64_t>(anchors_span.size()));
                stats_dst->record_scalar_pipeline_execution_stats(stats);
            } else if constexpr (p_kernel::emits_simd_chaining_execution_stats) {
                if (stats_dst == nullptr) {
                    bioseq_log<logging_level::error>(
                        "simd chaining stage has no execution-stats sink");
                    std::abort();
                }
                ::bioseq::chaining::simd_chaining_execution_stats stats{};
                p_kernel::call(
                    highway_kernels, anchors_span, active_cp, chains_slab, reordered_slab, &stats);
                if (!stats.is_internally_consistent(active_cp.simd_policy,
                                                    /*require_nonempty_vector_work=*/false)) {
                    const auto status = stats.has_consistent_policy_stats(active_cp.simd_policy)
                                            ? simd_chaining_status::invalid_parameters
                                            : simd_chaining_status::invalid_policy;
                    throw simd_chaining_runtime_error(
                        status,
                        "selected policy does not match operation-originated "
                        "simd chaining stats");
                }
                stats_dst->record_simd_chaining_execution_stats(stats);
            } else if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
                if (stats_dst == nullptr) {
                    bioseq_log<logging_level::error>(
                        "Vulkan chaining stage has no execution-stats sink");
                    std::abort();
                }
                if (p_kernel::call(anchors_span, active_cp, chains_slab, reordered_slab)) {
                    partial_vulkan_execution_stats stats{};
                    stats.chain_score_dispatches = 1;
                    stats_dst->record_partial_vulkan_execution_stats(stats);
                }
            } else {
                p_kernel::call(anchors_span, active_cp, chains_slab, reordered_slab);
            }

            if (host_timing_dst != nullptr) {
                host_timing_dst->record_chain_score_host_time(
                    elapsed_host_stage_ns(host_stage_started));
            }

            return std::move(token).emit_batch_pair(
                std::span<chain>{chains_slab.data(), chains_slab.size()},
                std::span<anchor>{reordered_slab.data(), reordered_slab.size()});
        }

        /// Describe the selected chaining kernel and parameters.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = std::string{p_kernel::algorithm_label()};
            algo.parameters.push_back({"min_chain_score", std::to_string(cp.min_chain_score)});
            algo.parameters.push_back({"min_seed_count", std::to_string(cp.min_seed_count)});
            algo.parameters.push_back({"max_chain_skip", std::to_string(cp.max_chain_skip)});
            algo.parameters.push_back({"max_chain_iter", std::to_string(cp.max_chain_iter)});
            algo.parameters.push_back({"bandwidth", std::to_string(cp.bandwidth)});
            algo.parameters.push_back({"max_gap", std::to_string(cp.max_gap)});
            // Let the selected kernel append its own runtime params (e.g. the
            // Vulkan backend's stage / vulkan_enabled markers) so the
            // EXPLAIN metadata is not dead code. No-op for the scalar/simd/range_minimum_query
            // kernels.
            p_kernel::populate_runtime_params(cp, algo.parameters);
            algo.computed_properties = p_kernel::template static_properties<p_aligner_parameters>();
            algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
            desc.algorithm = std::move(algo);
            // chain stage's two arena slabs are sized from the runtime
            // anchor count (which is unknown at describe time), so we leave
            // `estimated_batch_size` empty rather than fabricate a per-query
            // figure that doesn't reflect the actual reserve.  The
            // `[reserved: ...]` line in EXPLAIN is therefore omitted for
            // chain_stage; the runtime `chains_slab.reserve(...)` call still
            // sizes from `estimated_chain_count(anchors_in.size(), ...)`.
            return desc;
        }
    };

    /**
     * @brief Chain-to-region conversion stage.
     *
     * Wraps the `generate_regions` kernel, which writes its
     * `alignment_region`-per-chain output directly into a caller-provided
     * arena_slab via placement-new. `alignment_region` owns a `std::vector<cigar_operation>`,
     * so it is not trivially-destructible. The slab's destructor walks
     * `[0, len_)` and calls `~alignment_region()` on each live element before the
     * arena reclaims the storage.
     */
    struct region_stage : stage_base<region_stage, alignment_region> {
        static constexpr std::string_view stage_name = "region_stage";

        sequence_size_t query_len = 0;
        match_result_collection* host_timing_dst = nullptr;

        /// Convert chains into arena-owned alignment regions.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_chain_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink>
        process_pair(p_chain_batch&& chains, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
            if (host_timing_dst != nullptr) {
                host_timing_dst->begin_region_host_stage(host_stage_now_ns());
            }
            // `alignment_region` owns a `std::vector<cigar_operation>` so the slab uses the
            // placement-new pattern; no intermediate `std::vector` is needed.
            auto regions_slab = make_arena_slab<alignment_region>();
            generate_regions(std::span<chain>{chains.data(), chains.size()},
                             std::span<const anchor>{anchors.data(), anchors.size()},
                             query_len,
                             regions_slab);

            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions_slab.data(), regions_slab.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        /// Describe chain-to-region projection.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = "Region generation";
            algo.parameters.push_back({"query_len", std::to_string(query_len)});
            desc.algorithm = std::move(algo);
            // One region per chain (exact); the actual chain count is only
            // known at runtime so the describe-time figure is left empty.  The
            // runtime reserve in process_pair sizes the slab from chains.size().
            return desc;
        }
    };

    /**
     * @brief In-place primary/secondary classification stage.
     *
     * Mutates the regions span via `classify_primary_secondary` (which takes
     * std::vector<alignment_region>& and modifies parent / score fields).  The
     * vector-wrapper here copies in and out. The touched region count is bounded
     * by best_n + 16 so the cost is negligible.
     */
    struct primary_secondary_stage : stage_base<primary_secondary_stage, alignment_region> {
        static constexpr std::string_view stage_name = "primary_secondary_stage";

        float mask_level;
        std::int32_t mask_len;
        std::int32_t sub_diff_value;

        /// Classify primary and secondary regions in place.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            // The kernel mutates in place through a span without a vector round-trip.
            classify_primary_secondary(std::span<alignment_region>{regions.data(), regions.size()},
                                       mask_level,
                                       mask_len,
                                       sub_diff_value);
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        /// Describe primary/secondary classification parameters.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = "Classify primary/secondary";
            algo.parameters.push_back(
                {"mask_level", std::to_string(static_cast<double>(mask_level))});
            algo.parameters.push_back({"mask_len", std::to_string(mask_len)});
            algo.parameters.push_back({"sub_diff", std::to_string(sub_diff_value)});
            desc.algorithm = std::move(algo);
            return desc;
        }
    };

    /**
     * @brief In-place secondary-region filtering stage.
     */
    struct secondary_filter_stage : stage_base<secondary_filter_stage, alignment_region> {
        static constexpr std::string_view stage_name = "secondary_filter_stage";

        float pri_ratio;
        std::int32_t min_diff_value;
        std::int32_t best_n;

        /// Filter secondary regions and emit the surviving prefix.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            // The kernel filters in place via a span; the returned effective size
            // gives the downstream stage a shorter prefix.
            const std::size_t new_size = filter_secondary_regions(
                std::span<alignment_region>{regions.data(), regions.size()},
                pri_ratio,
                min_diff_value,
                best_n);
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), new_size},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        /// Describe secondary-region filtering parameters.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = "Filter secondary regions";
            algo.parameters.push_back(
                {"pri_ratio", std::to_string(static_cast<double>(pri_ratio))});
            algo.parameters.push_back({"min_diff", std::to_string(min_diff_value)});
            algo.parameters.push_back({"best_n", std::to_string(best_n)});
            desc.algorithm = std::move(algo);
            return desc;
        }
    };

    /**
     * @brief Conditional divergence-estimation stage.
     */
    template <class p_minimizer, class p_query_minimizers = std::vector<p_minimizer>>
    struct divergence_stage
        : stage_base<divergence_stage<p_minimizer, p_query_minimizers>, alignment_region> {
        static constexpr std::string_view stage_name = "divergence_stage";

        const p_query_minimizers* query_minimizers_;
        sequence_size_t query_len;
        sequence_size_t target_len;
        std::int32_t runtime_kmer_size;
        bool enabled;

        /// Estimate divergence when enabled and preserve the paired batch.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            if (enabled) {
                const std::int32_t kmer_size =
                    p_aligner_parameters::generic_fallback
                        ? runtime_kmer_size
                        : static_cast<std::int32_t>(p_aligner_parameters::kmer_len);
                estimate_divergence_from_seeds<p_minimizer>(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const p_minimizer>{query_minimizers_->data(),
                                                 query_minimizers_->size()},
                    std::span<const anchor>{anchors.data(), anchors.size()},
                    query_len,
                    kmer_size,
                    target_len);
            }
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        /// Describe the optional divergence-estimation stage.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (enabled) {
                stage_algorithm_description algo;
                algo.name = "Estimate divergence from seeds";
                desc.algorithm = std::move(algo);
            }
            return desc;
        }
    };

    /**
     * Authoritative exact edit-distance predicate gate.
     *
     * This runs after traceback has populated `alignment_region::edit_distance` and
     * before the final primary/secondary and MAPQ pass.  Consequently a candidate
     * that does not satisfy the SQL predicate cannot influence the classification
     * or mapping-quality metadata of a candidate that does.  The earlier
     * rank-prefilter stage remains only a conservative work-avoidance gate.
     */
    struct exact_edit_filter_stage : stage_base<exact_edit_filter_stage, alignment_region> {
        static constexpr std::string_view stage_name = "exact_edit_filter_stage";

        ::bioseq::simd::minimap2_parameters params;

        /// Retain only exact-edit survivors and reset their classification fields.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            if (!params.exact_edit_filter_enabled) {
                return std::move(token).emit_batch_pair(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const anchor>{anchors.data(), anchors.size()});
            }
            if (!params.compute_edit_distance) {
                throw std::logic_error("exact edit filter requires exact edit-distance accounting");
            }

            const std::int64_t exclusive_upper_bound =
                params.exact_edit_filter_exclusive_upper_bound;
            std::size_t output_size = 0;
            for (std::size_t index = 0; index < regions.size(); ++index) {
                alignment_region& region = regions.data()[index];
                if (!region.has_alignment) {
                    throw std::runtime_error(
                        "exact edit filter received a region without alignment traceback");
                }
                const bool keep =
                    exclusive_upper_bound > 0 &&
                    region.edit_distance < static_cast<std::uint64_t>(exclusive_upper_bound);
                if (!keep)
                    continue;

                if (output_size != index) {
                    regions.data()[output_size] = std::move(region);
                }
                alignment_region& survivor = regions.data()[output_size];
                survivor.region_id = static_cast<std::int32_t>(output_size);
                survivor.parent = -1;
                survivor.secondary_score = 0;
                survivor.secondary_count = 0;
                survivor.secondary_dp_max = 0;
                survivor.mapq = 0;
                ++output_size;
            }

            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), output_size},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        /// Describe the authoritative exact-edit predicate.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (params.exact_edit_filter_enabled) {
                stage_algorithm_description algorithm;
                algorithm.name = "Exact edit-distance predicate";
                algorithm.parameters.push_back(
                    {"exclusive_upper_bound",
                     std::to_string(params.exact_edit_filter_exclusive_upper_bound)});
                algorithm.computed_properties.push_back(
                    {"classification_domain", "exact predicate survivors"});
                desc.algorithm = std::move(algorithm);
            }
            return desc;
        }
    };

    /**
     * @brief Conditional base-level alignment stage.
     *
     * Algorithm dispatch occurs through `p_kernel = kernel_t<p_alignment_algorithm>`.
     * runtime parameters are populated by `p_kernel::populate_runtime_params(...)`.
     */
    template <class p_kernel, sequence_alphabet p_alphabet>
    struct align_stage : stage_base<align_stage<p_kernel, p_alphabet>, alignment_region> {
        static constexpr std::string_view stage_name = "align_stage";

        ::bioseq::simd::minimap2_parameters params;
        const scoring_matrix<p_alphabet>* sm;
        const ::bioseq::simd::alignment_kernels<p_alphabet>* alignment_kernels_;
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq_;
        bool enabled;
        match_result_collection* stats_dst_;
        match_result_collection* host_timing_dst_;

        /// Execute the selected base-alignment kernel and record its statistics.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            if (host_timing_dst_ != nullptr) {
                host_timing_dst_->transition_region_to_alignment_result_host_stage(
                    host_stage_now_ns(), enabled);
            }
            if (enabled) {
                if constexpr (p_kernel::emits_scalar_ksw2_execution_stats) {
                    if (stats_dst_ == nullptr) {
                        bioseq_log<logging_level::error>(
                            "scalar ksw2 stage has no execution-stats sink");
                        std::abort();
                    }
                    scalar_pipeline_execution_stats call_stats{};
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        /*forbidden_block_stats=*/nullptr,
                        &call_stats);
                    stats_dst_->record_scalar_pipeline_execution_stats(call_stats);
                } else if constexpr (p_kernel::emits_ksw2_batched_execution_stats) {
                    if (stats_dst_ == nullptr) {
                        bioseq_log<logging_level::error>(
                            "ksw2 batched stage has no execution-stats sink");
                        std::abort();
                    }
                    ksw2_batched_execution_stats call_stats{};
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        /*forbidden_block_stats=*/nullptr,
                        /*scalar_execution_stats=*/nullptr,
                        &call_stats);
                    if (!call_stats.is_coherent(
                            /*require_nonempty_work=*/false)) {
                        bioseq_log<logging_level::error>(
                            "ksw2 batched stage produced rejected execution stats");
                        std::abort();
                    }
                    stats_dst_->record_ksw2_batched_execution_stats(call_stats);
                } else if constexpr (p_kernel::emits_block_execution_stats) {
                    if (stats_dst_ == nullptr) {
                        bioseq_log<logging_level::error>(
                            "Block Aligner stage has no execution-stats sink");
                        std::abort();
                    }
                    block_aligner_execution_stats call_stats{};
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        &call_stats,
                        /*scalar_execution_stats=*/nullptr);
                    if (!call_stats.is_internally_consistent()) {
                        bioseq_log<logging_level::error>(
                            "Block Aligner stage produced a rejected execution stats");
                        std::abort();
                    }
                    stats_dst_->record_block_aligner_execution_stats(call_stats);
                } else {
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        /*forbidden_block_stats=*/nullptr,
                        /*scalar_execution_stats=*/nullptr);
                }
            }
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()});
        }

        /// Describe the enabled base-alignment kernel.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (enabled) {
                stage_algorithm_description algo;
                algo.name = std::string{p_kernel::algorithm_label()};
                p_kernel::populate_runtime_params(params, algo.parameters);
                algo.computed_properties =
                    p_kernel::template static_properties<p_aligner_parameters>();
                algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
                desc.algorithm = std::move(algo);
            }
            return desc;
        }
    };

    /**
     * @brief Conditional mapping-quality stage.
     *
     * Accepts a `(regions, anchors)` pair upstream but only uses the regions
     * span. It emits the regions span downstream as a single batch.
     */
    struct mapq_stage : stage_base<mapq_stage, alignment_region> {
        static constexpr std::string_view stage_name = "mapq_stage";

        ::bioseq::simd::minimap2_parameters params;
        bool enabled;

        /// Compute mapping quality when enabled and emit the region batch.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_region_batch&& regions, emit_token<p_sink>&& token) {
            if (enabled) {
                compute_mapping_quality(
                    std::span<alignment_region>{regions.data(), regions.size()}, params, 0);
            }
            return std::move(token).emit_batch(
                std::span<alignment_region>{regions.data(), regions.size()});
        }

        //: pair → single delegation.  The outer body has no
        // surviving `emit_token<p_sink>` after the inner call moves it, so a
        // second emit is structurally blocked.
        /// Drop the paired anchor batch and delegate to the single-batch path.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& /*unused*/,
                                           emit_token<p_sink>&& token) {
            return process<p_aligner_parameters>(std::forward<p_region_batch>(regions),
                                                 std::move(token));
        }

        /// Describe the optional mapping-quality stage.
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (enabled) {
                stage_algorithm_description algo;
                algo.name = "Compute mapping quality (mm_set_mapq2)";
                desc.algorithm = std::move(algo);
            }
            return desc;
        }
    };

    /**
     * @brief Terminal sink: push converted MatchResults into the collection.
     *
     * Not a `bare_stage` — appears as the tail of `make_pipeline<p_aligner_parameters>(...)`.
     */
    struct sink_stage {
        match_result_collection* dst_;
        sequence_size_t query_len = 0;
        sequence_size_t target_len = 0;
        match_result_collection* host_timing_dst_ = nullptr;

        /// Convert regions into final match results and close host timing.
        [[gnu::always_inline]] inline void on_batch(std::span<alignment_region> regions) {
            dst_->results.reserve(dst_->results.size() + regions.size());
            // `region_slot` is move-consumed into the converted match_result; the
            // slot is left in a valid-but-moved-from state and reclaimed by
            // whichever owner (arena slab or std::vector) holds the span.
            for (auto& region_slot : regions) {
                dst_->results.push_back(
                    region_to_match_result(std::move(region_slot), query_len, target_len));
            }
            if (host_timing_dst_ != nullptr &&
                host_timing_dst_->is_alignment_result_host_stage_active) {
                host_timing_dst_->finish_alignment_result_host_stage(host_stage_now_ns());
            }
        }

        /// Close host timing when upstream completes without a final batch.
        [[gnu::always_inline]] inline void on_completed() noexcept {
            if (host_timing_dst_ != nullptr &&
                host_timing_dst_->is_alignment_result_host_stage_active) {
                host_timing_dst_->finish_alignment_result_host_stage(host_stage_now_ns());
            }
        }
    };

    // =============================================================================
    // Out-of-line pipeline-description walker
    // =============================================================================

    /// Describe this stage and append the recursively described sink stages.
    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    [[nodiscard]] pipeline_description
    stage<p_aligner_parameters, p_self, p_sink>::describe() const {
        // Every concrete stage description uses the same parameter specialization
        // as the runtime path.
        pipeline_description d;
        d.stages.push_back(self_.template describe<p_aligner_parameters>());
        if constexpr (requires { sink_.describe(); }) {
            pipeline_description rest = sink_.describe();
            d.stages.insert(d.stages.end(),
                            std::make_move_iterator(rest.stages.begin()),
                            std::make_move_iterator(rest.stages.end()));
        }
        return d;
    }

    /**
     * @brief Injects the p_aligner_parameters computed properties as a leading entry in the
     * pipeline description so the EXPLAIN renderer sees w/k/p_bucket_width beside the first
     * stage.
     */
    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    [[nodiscard]] pipeline_description
    describe_with_params(const stage<p_aligner_parameters, p_self, p_sink>& pipeline) {
        auto d = pipeline.describe();
        if (!d.stages.empty()) {
            auto& first = d.stages.front();
            if (!first.algorithm.has_value()) {
                first.algorithm.emplace();
                first.algorithm->name = "pipeline configuration";
            }
            first.algorithm->computed_properties.push_back(
                {"cep.window_len", std::to_string(p_aligner_parameters::window_len)});
            first.algorithm->computed_properties.push_back(
                {"cep.kmer_len", std::to_string(p_aligner_parameters::kmer_len)});
            first.algorithm->computed_properties.push_back(
                {"cep.sequence_len_log2", std::to_string(p_aligner_parameters::sequence_len_log2)});
            first.algorithm->computed_properties.push_back(
                {"cep.specialised",
                 p_aligner_parameters::generic_fallback ? "fallback" : "specialised"});
        }
        return d;
    }

    } // namespace bioseq::simd::pipe

} // export
