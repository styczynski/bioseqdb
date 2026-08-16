/**
 * @file pipeline/pipe_parts/segmented_planning.cppm
 * @brief Plans segment geometry and admitted sketch storage.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:segmented_planning;

import std;
import :stage_graph;
import :minimizer_execution;
import :execution;
import :shape_dispatch;
import :entry_points;
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

export {

    namespace bioseq::simd {

    /// Fragment-local minimizers claimed by one processed segment core.
    template <class p_minimizer> struct segment_fragment_emission {
        /// Global fragment ordinal receiving this emission.
        std::size_t fragment_no = 0;
        /// Biological length of the destination fragment.
        sequence_size_t fragment_len_bp = 0;
        /// Fragment-local minimizers owned by this emission.
        std::vector<p_minimizer> minimizers{};
    };

    /** Collect fragment-local minimizers claimed by one segment core. */
    template <class p_minimizer>
    [[nodiscard]] std::vector<segment_fragment_emission<p_minimizer>>
    collect_fragment_emissions_for_segment(const ::bioseq::indexing::segment_unit_range& range,
                                           std::span<const p_minimizer> segment_minimizers,
                                           sequence_size_t total_bp,
                                           sequence_size_t frag_bp,
                                           std::size_t lpu) {
        const sequence_size_t emit_base_bp = static_cast<sequence_size_t>(range.cs_u) * lpu;
        const std::size_t frag_lo = static_cast<std::size_t>(range.core_lo_bp / frag_bp);
        const std::size_t frag_hi =
            static_cast<std::size_t>((range.core_hi_bp + frag_bp - 1) / frag_bp);

        std::vector<segment_fragment_emission<p_minimizer>> out;
        out.reserve(frag_hi > frag_lo ? frag_hi - frag_lo : 0);
        for (std::size_t fragment_no = frag_lo; fragment_no < frag_hi; ++fragment_no) {
            const sequence_size_t fragment_lo_bp =
                static_cast<sequence_size_t>(fragment_no) * frag_bp;
            const sequence_size_t fragment_hi_bp = std::min(fragment_lo_bp + frag_bp, total_bp);
            const sequence_size_t claim_lo = std::max(fragment_lo_bp, range.core_lo_bp);
            const sequence_size_t claim_hi = std::min(fragment_hi_bp, range.core_hi_bp);
            if (claim_lo >= claim_hi) {
                continue;
            }

            segment_fragment_emission<p_minimizer> emission{};
            emission.fragment_no = fragment_no;
            emission.fragment_len_bp = fragment_hi_bp - fragment_lo_bp;
            for (const p_minimizer& minimizer : segment_minimizers) {
                const sequence_size_t global_pos =
                    emit_base_bp + static_cast<sequence_size_t>(minimizer.last_pos());
                if (global_pos >= claim_lo && global_pos < claim_hi) {
                    emission.minimizers.emplace_back(
                        minimizer.kmer(),
                        static_cast<std::uint64_t>(global_pos - fragment_lo_bp),
                        minimizer.strand());
                }
            }
            out.push_back(std::move(emission));
        }
        return out;
    }

    // =============================================================================
    // Query sketch for persistent-index bucket selection
    // =============================================================================
    //
    // Holds the query minimizers produced for persistent-index bucket filtering,
    // keeping the optional GPU host-result permit and device span alive for the
    // caller's lifetime. The host vector feeds the assembly/filter helpers; the
    // Vulkan branch sketches the query on the GPU so the segmented
    // path's seed lookup consumes byte-identical minimizers.
    template <class p_minimizer> struct budgeted_sketch_vector {
        // Reverse destruction is the ownership contract: host capacity and the
        // device lifetime anchor disappear before either reservation can release.
        std::optional<bioseq::parallelism::memory_permit> permit;
        std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>> device;
        std::vector<p_minimizer> minimizers;
        scalar_minimizer_execution_stats scalar_execution_stats{};
        bool scalar_execution_stats_available = false;
        two_stack_execution_stats exact_simd_execution_stats{};
        bool exact_simd_execution_stats_available = false;
        bounded_nthash_execution_stats bounded_nthash_stats{};
        bool bounded_nthash_execution_stats_available = false;

        /// Create an empty query-sketch owner.
        budgeted_sketch_vector() = default;
        budgeted_sketch_vector(const budgeted_sketch_vector&) = delete;
        budgeted_sketch_vector& operator=(const budgeted_sketch_vector&) = delete;

        /// Transfer every allocation and its corresponding admission anchor.
        budgeted_sketch_vector(budgeted_sketch_vector&& other) noexcept {
            steal_from_(std::move(other));
        }

        /// Replace this query sketch after releasing its owned allocations.
        budgeted_sketch_vector& operator=(budgeted_sketch_vector&& other) noexcept {
            if (this != &other) {
                release_owned_memory_before_permit();
                steal_from_(std::move(other));
            }
            return *this;
        }

        /// Destroy host/device storage before releasing either permit.
        void release_owned_memory_before_permit() noexcept {
            {
                std::vector<p_minimizer> empty;
                minimizers.swap(empty);
            }
            device.reset();
            permit.reset();
        }

        /// Borrow the retained host minimizers.
        [[nodiscard]] std::span<const p_minimizer> as_span() const noexcept {
            return minimizers;
        }

      private:
        /// Move storage, permits, and execution evidence from one emptying owner.
        void steal_from_(budgeted_sketch_vector&& other) noexcept {
            // Transfer allocations while all source admission anchors remain live,
            // then transfer device and host permits. Swap proves the source has no
            // residual vector capacity after the permit move.
            minimizers.swap(other.minimizers);
            device = std::move(other.device);
            other.device.reset();
            permit = std::move(other.permit);
            other.permit.reset();
            scalar_execution_stats = other.scalar_execution_stats;
            scalar_execution_stats_available =
                std::exchange(other.scalar_execution_stats_available, false);
            exact_simd_execution_stats = other.exact_simd_execution_stats;
            exact_simd_execution_stats_available =
                std::exchange(other.exact_simd_execution_stats_available, false);
            bounded_nthash_stats = other.bounded_nthash_stats;
            bounded_nthash_execution_stats_available =
                std::exchange(other.bounded_nthash_execution_stats_available, false);
        }
    };

    /// Sketch one query through the backend used for index selection.
    template <class p_minimizer, sequence_alphabet p_alphabet>
    [[nodiscard]] budgeted_sketch_vector<p_minimizer>
    sketch_query_for_index_selection(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                                     const minimap2_parameters& params,
                                     std::size_t w,
                                     std::size_t k) {
        require_representable_minimizer_query_length<p_minimizer>(query_seq.bp_size());
        if (params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer) {
            if (!params.vulkan_enabled) {
                pipe::throw_vulkan_declined("Vulkan query minimizer sketch");
            }
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            if (auto gpu_sketch =
                    bioseq::gpu::vulkan::collect_minimizers_vulkan_result<p_minimizer>(
                        query_seq, w, k, &decline)) {
                budgeted_sketch_vector<p_minimizer> result;
                result.minimizers.swap(gpu_sketch->host_minimizers);
                result.device = gpu_sketch->device_span();
                result.permit = std::move(gpu_sketch->host_permit);
                return result;
            }
            pipe::throw_vulkan_sketch_declined("Vulkan query minimizer sketch", decline);
        }

        basic_sketch_collector<p_minimizer> query_collector;
        query_collector.reserve(query_seq.bp_size() / std::max<std::size_t>(w, 1) + 64);
        // Vulkan query sketching returned above. For the existing all-simd tuple,
        // use the same exact_simd continuation backend as target preparation;
        // every other non-GPU tuple retains its explicitly scalar query backend.
        const minimizer_sketch_backend cpu_query_backend =
            pipe::target_index_sketch_backend_from_params(params);
        // The SQL/bench dna4 query representation is packed in u64 units, while
        // the exact K15/W10/B8 continuation kernel consumes u32 words on NEON.
        // Reinterpret a whole resident query as u32 without copying so the same
        // persisted-index-compatible continuation scheme executes eight
        // chronological candidates per block.  This is intentionally *not* the
        // report-76 resident vector_stream selection route: its minimizer scheme
        // differs from the persisted target index and remains forbidden here.
        // The u32 view changes only the decoder lane width; scalar-oracle
        // differential tests freeze the emitted raw minimizer sequence. Native
        // little-endian word order is part of this zero-copy split; other targets
        // retain the existing generic-u64 continuation.
        const pipe::minimizer_range_execution_stats execution_stats =
            pipe::minimizer_dispatch_indexed_query_whole<p_minimizer>(
                query_collector, query_seq, w, k, cpu_query_backend, params.highway_kernels);
        if (!pipe::minimizer_range_stats_matches_backend(
                cpu_query_backend,
                execution_stats,
                /*require_nonempty_vector_work=*/false,
                pipe::exact_simd_range_route::require_continuation)) {
            throw std::runtime_error("indexed query minimizer sketch did not execute the required "
                                     "persisted-index-compatible route");
        }
        budgeted_sketch_vector<p_minimizer> result;
        auto minimizers = std::move(query_collector).extract_sketches_data();
        result.minimizers.swap(minimizers);
        result.scalar_execution_stats = execution_stats.scalar;
        result.scalar_execution_stats_available = execution_stats.scalar_available;
        result.exact_simd_execution_stats = execution_stats.exact_simd;
        result.exact_simd_execution_stats_available = execution_stats.exact_simd_available;
        result.bounded_nthash_stats = execution_stats.bounded_nthash;
        result.bounded_nthash_execution_stats_available = execution_stats.bounded_nthash_available;
        return result;
    }

    } // namespace bioseq::simd

} // export
