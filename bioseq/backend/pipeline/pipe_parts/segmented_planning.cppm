/**
 * @file pipeline/pipe_parts/segmented_planning.cppm
 * @brief Assigns segment minimizers to fragments and prepares query sketches.
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

    /// Minimizers assigned to one fragment  by a processed segment's core.
    template <class p_minimizer> struct segment_fragment_emission {
        /// Fragment number within the full sequence.
        std::size_t fragment_no = 0;
        /// Destination fragment length in bases.
        sequence_size_t fragment_len_bp = 0;
        /// Owned minimizers with positions relative to the fragment's start.
        std::vector<p_minimizer> minimizers{};
    };

    /**
     * Split a segment's minimizers between the fragments covered by its core.
     * Ignore minimizers in the overlapping context outside that core, then
     * convert, the accepted positions to offsets within each fragment.
     */
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

    /// Owns the query sketch used to select persisted-index buckets.
    /// GPU sketches keep both host results and device storage, together with
    /// their memory reservations, for as long as the sketch is needed.
    template <class p_minimizer> struct budgeted_sketch_vector {
        // Members are destroyed in  reverse order. Keep the host vector and
        // device owner after the pxrmit so their memory is released first.
        std::optional<bioseq::parallelism::memory_permit> permit;
        std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>> device;
        std::vector<p_minimizer> minimizers;
        scalar_minimizer_execution_stats scalar_execution_stats{};
        bool scalar_execution_stats_available = false;
        two_stack_execution_stats exact_simd_execution_stats{};
        bool exact_simd_execution_stats_available = false;
        bounded_nthash_execution_stats bounded_nthash_stats{};
        bool bounded_nthash_execution_stats_available = false;

        /// Create an empty query sketch.
        budgeted_sketch_vector() = default;
        budgeted_sketch_vector(const budgeted_sketch_vector&) = delete;
        budgeted_sketch_vector& operator=(const budgeted_sketch_vector&) = delete;

        /// Move the sketch storage and its memory reservations together.
        budgeted_sketch_vector(budgeted_sketch_vector&& other) noexcept {
            steal_from_(std::move(other));
        }

        /// Release the current sketch, then take ownership of the supplied sketch
        budgeted_sketch_vector& operator=(budgeted_sketch_vector&& other) noexcept {
            if (this != &other) {
                release_owned_memory_before_permit();
                steal_from_(std::move(other));
            }
            return *this;
        }

        /// Free sketch storage before releasing its memory reservations.
        void release_owned_memory_before_permit() noexcept {
            {
                std::vector<p_minimizer> empty;
                minimizers.swap(empty);
            }
            device.reset();
            permit.reset();
        }

        /// Borrow a read-only view of the host minimizers.
        [[nodiscard]] std::span<const p_minimizer> as_span() const noexcept {
            return minimizers;
        }

      private:
        /// Move a sketch and its statistics into  this empty owner.
        void steal_from_(budgeted_sketch_vector&& other) noexcept {
            // Move storage while its reservations remain held. Swapping the vector
            // leaves the source with no capacity before its permit is moved.
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

    /// Build the query sketch used to choose target-index buckets.
    /// Use the target's sketch backend so matching seeds use the same scheme.
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
        // Match the CPU sketch backend used to prepare target indexes.
        const minimizer_sketch_backend cpu_query_backend =
            pipe::target_index_sketch_backend_from_params(params);
        // Indexed exact-SIMD queries use chronological continuation with 32-bit
        // packed words to match the target index's seed selection. The access
        // planner borrows suitable storage or packs a  copy under a reservation.
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
