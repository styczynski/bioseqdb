/**
 * @file pipeline/pipe_parts/segmented_execution.cppm
 * @brief Executes admitted segments and returns merged pipeline results.
 */
module;

#include <boost/unordered/unordered_flat_set.hpp>

export module bioseq.pipeline.pipe:segmented_execution;

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
import :segmented_planning;
import :segmented_finalization;

export {

    namespace bioseq::simd {

    /**
     * Execute target indexing and alignment in admission-bounded overlapping
     * segments while retaining global biological coordinates and result order.
     *
     * `max_occ` filtering remains segment-local, so exact genome-wide filtering
     * for highly repetitive targets requires a global occurrence pre-count.
     *
     * @tparam p_index_dispatch Transient or generated fragment-index route.
     * @tparam p_alphabet Logical alphabet shared by query and target.
     * @tparam p_fragment_index_storage Optional persistent fragment sink.
     */
    template <class p_index_dispatch,
              sequence_alphabet p_alphabet,
              class p_fragment_index_storage = std::nullptr_t>
    [[nodiscard]] match_result_collection
    minimap_port_segmented_impl(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                                bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                                const minimap2_parameters& params = minimap2_parameters{},
                                p_fragment_index_storage* fragment_index_out = nullptr) {
        using p_minimizer = minimap_sketch_data;
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("minimap_port_segmented");
        auto _scope = bioseq::instrumentation::region(_desc);

        const std::size_t w = static_cast<std::size_t>(params.window_size);
        const std::size_t k = static_cast<std::size_t>(params.kmer_size);
        const sequence_size_t query_len = query_seq.bp_size();
        const sequence_size_t target_len = target_seq.bp_size();
        if (query_len == 0 || target_len == 0) {
            return match_result_collection{};
        }

        // Resolve the target-index sketch backend once so every segment preserves
        // the selected route without an implicit scalar fallback.
        const minimizer_sketch_backend target_sketch_backend =
            pipe::target_index_sketch_backend_from_params(params);

        std::optional<bioseq::indexing::gpu_minimizer_index_cache> segmented_gpu_index_cache;
        if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
            bioseq::gpu::vulkan::runtime& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
            std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
                                      ? gpu_rt.features().device_local_bytes / 2u
                                      : (std::uint64_t{512} << 20);
            if (limit < (std::uint64_t{512} << 20))
                limit = std::uint64_t{512} << 20;
            if (limit > (std::uint64_t{4} << 30))
                limit = std::uint64_t{4} << 30;
            segmented_gpu_index_cache.emplace(limit);
        }
        bioseq::indexing::gpu_minimizer_index_cache* segmented_gpu_index_cache_ptr =
            segmented_gpu_index_cache ? &*segmented_gpu_index_cache : nullptr;
        auto segment_gpu_identity = [](std::size_t segment_index) noexcept {
            return bioseq::indexing::gpu_index_cache_identity{
                .assembly_identity =
                    0x9e3779b97f4a7c15ULL ^ (static_cast<std::uint64_t>(segment_index) + 1u),
            };
        };

        // (1) Sketch the query ONCE as minimap_sketch_data (query <= 2^28 bp; its
        //     positions fit the 28-bit field). Same kmer hashing as the target, so
        //     `collect_seed_hits` matches identically. — the helper
        //     sketches on the GPU when vulkan_minimap_integer is selected (byte-identical
        //     minimizers) and otherwise uses the scalar CPU sketch.
        auto query_sketch = sketch_query_for_index_selection<p_minimizer>(query_seq, params, w, k);
        const pipe::minimizer_batch_view<p_minimizer> query_minimizers{
            .host = std::span<const p_minimizer>{query_sketch.minimizers.data(),
                                                 query_sketch.minimizers.size()},
            .device = query_sketch.device,
        };
        const bool exact_simd_segmented_route =
            target_sketch_backend == minimizer_sketch_backend::exact_simd;
        const bool bounded_nthash_segmented_route =
            target_sketch_backend == minimizer_sketch_backend::bounded_nthash_simd;
        const bool scalar_segmented_route =
            target_sketch_backend == minimizer_sketch_backend::scalar;
        auto record_target_range_stats =
            [exact_simd_segmented_route, bounded_nthash_segmented_route, scalar_segmented_route](
                match_result_collection& collection,
                const pipe::minimizer_range_execution_stats& range_stats) {
                if (exact_simd_segmented_route) {
                    if (!range_stats.exact_simd_available ||
                        !range_stats.exact_simd.is_internally_consistent(
                            /*require_nonempty_vector_work=*/false) ||
                        range_stats.bounded_nthash_available || range_stats.scalar_available) {
                        bioseq_log<logging_level::error>(
                            "exact_simd segmented target range lacks actual stats");
                        std::abort();
                    }
                    collection.record_exact_simd_target_execution_stats(range_stats.exact_simd);
                } else if (bounded_nthash_segmented_route) {
                    if (!range_stats.bounded_nthash_available ||
                        !range_stats.bounded_nthash.is_internally_consistent(
                            /*require_nonempty_vector_work=*/false) ||
                        range_stats.exact_simd_available || range_stats.scalar_available) {
                        bioseq_log<logging_level::error>(
                            "bounded_nthash_simd segmented target range lacks actual stats");
                        std::abort();
                    }
                    collection.record_bounded_nthash_target_execution_stats(
                        range_stats.bounded_nthash);
                } else if (scalar_segmented_route) {
                    if (!range_stats.scalar_available ||
                        !range_stats.scalar.is_coherent(
                            /*require_nonempty_work=*/false) ||
                        range_stats.exact_simd_available || range_stats.bounded_nthash_available) {
                        bioseq_log<logging_level::error>(
                            "scalar segmented target range lacks actual stats");
                        std::abort();
                    }
                    collection.record_scalar_target_minimizer_execution_stats(range_stats.scalar);
                } else if (range_stats.exact_simd_available ||
                           range_stats.bounded_nthash_available || range_stats.scalar_available) {
                    bioseq_log<logging_level::error>(
                        "scalar/Vulkan segmented target range emitted CPU simd sketch stats");
                    std::abort();
                }
            };
        auto finalize_segmented_stats = [&query_sketch,
                                         exact_simd_segmented_route,
                                         bounded_nthash_segmented_route,
                                         scalar_segmented_route](
                                            match_result_collection collection) {
            if (exact_simd_segmented_route) {
                if (!query_sketch.exact_simd_execution_stats_available ||
                    !query_sketch.exact_simd_execution_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false) ||
                    query_sketch.bounded_nthash_execution_stats_available ||
                    query_sketch.scalar_execution_stats_available ||
                    !collection.exact_simd_target_execution_stats_available ||
                    !collection.exact_simd_target_execution_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false) ||
                    collection.bounded_nthash_target_execution_stats_available) {
                    bioseq_log<logging_level::error>(
                        "exact_simd segmented query/target stats coverage is incomplete");
                    std::abort();
                }
                collection.record_exact_simd_query_execution_stats(
                    query_sketch.exact_simd_execution_stats);
            } else if (bounded_nthash_segmented_route) {
                if (!query_sketch.bounded_nthash_execution_stats_available ||
                    !query_sketch.bounded_nthash_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false) ||
                    query_sketch.exact_simd_execution_stats_available ||
                    query_sketch.scalar_execution_stats_available ||
                    !collection.bounded_nthash_target_execution_stats_available ||
                    !collection.bounded_nthash_target_execution_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false) ||
                    collection.exact_simd_target_execution_stats_available) {
                    bioseq_log<logging_level::error>(
                        "bounded_nthash_simd segmented query/target stats coverage is incomplete");
                    std::abort();
                }
                collection.record_bounded_nthash_query_execution_stats(
                    query_sketch.bounded_nthash_stats);
            } else if (scalar_segmented_route) {
                if (!query_sketch.scalar_execution_stats_available ||
                    !query_sketch.scalar_execution_stats.is_coherent() ||
                    query_sketch.exact_simd_execution_stats_available ||
                    query_sketch.bounded_nthash_execution_stats_available ||
                    !collection.scalar_pipeline_execution_stats_available ||
                    !collection.scalar_pipeline_stats.target_minimizers.is_coherent() ||
                    collection.exact_simd_target_execution_stats_available ||
                    collection.bounded_nthash_target_execution_stats_available) {
                    bioseq_log<logging_level::error>(
                        "scalar segmented query/target stats coverage is incomplete");
                    std::abort();
                }
                collection.record_scalar_query_minimizer_execution_stats(
                    query_sketch.scalar_execution_stats);
            } else if (query_sketch.exact_simd_execution_stats_available ||
                       query_sketch.bounded_nthash_execution_stats_available ||
                       query_sketch.scalar_execution_stats_available ||
                       collection.exact_simd_target_execution_stats_available ||
                       collection.bounded_nthash_target_execution_stats_available ||
                       (collection.scalar_pipeline_execution_stats_available &&
                        collection.scalar_pipeline_stats.target_minimizers.calls != 0)) {
                bioseq_log<logging_level::error>(
                    "scalar/Vulkan segmented route emitted CPU simd sketch stats");
                std::abort();
            }
            return collection;
        };
        boost::unordered_flat_set<std::uint64_t> query_kmer_filter;
        query_kmer_filter.reserve(query_minimizers.size());
        for (const p_minimizer& minimizer : query_minimizers) {
            query_kmer_filter.insert(minimizer.kmer());
        }
        struct query_kmer_gate {
            std::vector<std::uint32_t> words;
            std::uint64_t mask = 0u;
            bool exact = false;

            void build(const boost::unordered_flat_set<std::uint64_t>& filter,
                       std::size_t /*kmer_size*/) {
                const unsigned gate_bits = 26u;
                exact = false;
                if (filter.empty()) {
                    words.clear();
                    mask = 0u;
                    return;
                }
                const std::uint64_t bit_count = std::uint64_t{1} << gate_bits;
                mask = bit_count - 1u;
                words.assign(static_cast<std::size_t>(bit_count / 32u), 0u);
                for (const std::uint64_t kmer : filter) {
                    const std::uint64_t bit = kmer & mask;
                    words[static_cast<std::size_t>(bit >> 5u)] |= std::uint32_t{1} << (bit & 31u);
                }
            }

            [[nodiscard, gnu::always_inline]] bool
            maybe_contains(std::uint64_t kmer) const noexcept {
                if (words.empty())
                    return true;
                const std::uint64_t bit = kmer & mask;
                return (words[static_cast<std::size_t>(bit >> 5u)] &
                        (std::uint32_t{1} << (bit & 31u))) != 0u;
            }

            [[nodiscard, gnu::always_inline]] bool
            accepts_exactly(std::uint64_t /*kmer*/) const noexcept {
                return false;
            }

            [[nodiscard]] bioseq::gpu::vulkan::vulkan_sketch_query_gate
            vulkan_gate() const noexcept {
                return bioseq::gpu::vulkan::vulkan_sketch_query_gate{
                    std::span<const std::uint32_t>{words.data(), words.size()},
                    static_cast<std::uint32_t>(mask)};
            }
        };
        query_kmer_gate query_gate;
        if (target_sketch_backend == minimizer_sketch_backend::vulkan_segment) {
            query_gate.build(query_kmer_filter, k);
        }
        using filtered_minimizer_pair = std::pair<std::uint64_t, std::uint64_t>;
        auto build_query_filtered_segment_index =
            [&query_kmer_filter, &query_gate, target_len](
                std::span<const p_minimizer> minimizers,
                minimizer_hashmap_fragment_index& index,
                std::vector<filtered_minimizer_pair>& filtered_pairs,
                bool already_query_filtered = false) -> std::size_t {
            filtered_pairs.clear();
            const std::size_t filter_size = query_kmer_filter.size();
            const std::size_t reserve_base =
                filter_size > (std::numeric_limits<std::size_t>::max)() - 64u ? filter_size
                                                                              : filter_size + 64u;
            const std::size_t reserve_hint = std::min(minimizers.size(), reserve_base);
            if (filtered_pairs.capacity() < reserve_hint) {
                filtered_pairs.reserve(reserve_hint);
            }
            for (const p_minimizer& minimizer : minimizers) {
                const std::uint64_t kmer = minimizer.kmer();
                if (already_query_filtered || query_gate.accepts_exactly(kmer) ||
                    (query_gate.maybe_contains(kmer) &&
                     query_kmer_filter.find(kmer) != query_kmer_filter.end())) {
                    filtered_pairs.emplace_back(kmer, minimizer.pos_strand_encoded());
                }
            }
            index.build_from_pairs(
                std::span<filtered_minimizer_pair>{filtered_pairs.data(), filtered_pairs.size()},
                target_len);
            return filtered_pairs.size();
        };
        using query_kmer_filter_type = decltype(query_kmer_filter);
        struct query_filtered_pair_collector {
            using minimizer_type = p_minimizer;

            const query_kmer_filter_type* filter = nullptr;
            const query_kmer_gate* gate = nullptr;
            std::uint64_t position_offset = 0u;
            std::vector<filtered_minimizer_pair> pairs;

            void reserve(std::size_t expected_count) {
                const std::size_t filtered_hint =
                    filter == nullptr ? expected_count
                                      : std::min<std::size_t>(expected_count, filter->size() + 64u);
                pairs.reserve(filtered_hint);
            }
            void clear() {
                pairs.clear();
            }
            [[gnu::always_inline]] void collect(p_minimizer&& sketch) {
                collect(static_cast<const p_minimizer&>(sketch));
            }
            [[gnu::always_inline]] void collect(const p_minimizer& sketch) {
                const std::uint64_t kmer = sketch.kmer();
                if (filter == nullptr || ((gate == nullptr || gate->maybe_contains(kmer)) &&
                                          filter->find(kmer) != filter->end())) {
                    const std::uint64_t encoded = ((position_offset + sketch.last_pos()) << 1u) |
                                                  static_cast<std::uint64_t>(sketch.strand());
                    pairs.emplace_back(kmer, encoded);
                }
            }
            [[nodiscard]] bool append_raw_storage(const typename p_minimizer::storage_type* raw,
                                                  std::size_t count) {
                if (count > pairs.max_size() - pairs.size()) {
                    return false;
                }
                for (std::size_t index = 0; index < count; ++index) {
                    collect(p_minimizer::from_raw_storage(raw[index]));
                }
                return true;
            }
        };

        // (2) Segment the target. Overlap >= longest target match span so a match
        //     starting in a segment core is fully indexed within that segment.
        constexpr std::size_t lpu = 1u;
        const sequence_size_t overlap_bp = pipe::segment_overlap_bp(query_len);

        auto run_serial_segmented = [target_len,
                                     query_len,
                                     lpu,
                                     w,
                                     k,
                                     &target_seq,
                                     &query_seq,
                                     &query_minimizers,
                                     &query_kmer_filter,
                                     &query_gate,
                                     &params,
                                     fragment_index_out,
                                     target_sketch_backend,
                                     segmented_gpu_index_cache_ptr,
                                     &segment_gpu_identity,
                                     &build_query_filtered_segment_index,
                                     &record_target_range_stats,
                                     &finalize_segmented_stats]() -> match_result_collection {
            match_result_collection merged_collection;
            std::vector<match_result>& merged = merged_collection.results;
            basic_sketch_collector<p_minimizer> seg_collector;
            minimizer_hashmap_fragment_index seg_index;
            std::vector<filtered_minimizer_pair> filtered_pairs;
            const auto ranges = pipe::segment_unit_ranges_for_backend(
                target_len, query_len, lpu, target_sketch_backend);
            constexpr sequence_size_t frag_bp = 32768u;

            bioseq_log_lazy<logging_level::debug>([target_len, query_len, &ranges] {
                bioseq_log_progress<logging_level::debug>(
                    "[minimap_port_segmented] serial target_len={} query_len={} segments={} "
                    "| footprint={}MB avail={}MB",
                    target_len,
                    query_len,
                    ranges.size(),
                    bioseq::common::memory::process_footprint_bytes() >> 20,
                    bioseq::common::memory::system_available_bytes() >> 20);
            });

            if constexpr (!std::is_same_v<p_fragment_index_storage, std::nullptr_t>) {
                if (fragment_index_out != nullptr) {
                    fragment_persist_accumulator<p_fragment_index_storage, p_minimizer> acc{
                        *fragment_index_out, target_len, frag_bp};
                    for (std::size_t segment_index = 0; segment_index < ranges.size();
                         ++segment_index) {
                        const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                        const auto [segment_lo_bp, segment_hi_bp] =
                            pipe::validated_segment_bp_bounds(range, lpu, target_len);
                        seg_collector.clear();
                        seg_collector.reserve(
                            (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                        const auto range_stats = pipe::minimizer_dispatch_range<p_minimizer>(
                            seg_collector,
                            target_seq,
                            segment_lo_bp,
                            segment_hi_bp,
                            w,
                            k,
                            target_sketch_backend,
                            pipe::exact_simd_range_route::require_continuation,
                            params.highway_kernels);
                        record_target_range_stats(merged_collection, range_stats);
                        const std::vector<p_minimizer>& seg_minimizers =
                            seg_collector.extract_sketches_data();

                        auto emissions = collect_fragment_emissions_for_segment(
                            range,
                            std::span<const p_minimizer>(seg_minimizers.data(),
                                                         seg_minimizers.size()),
                            target_len,
                            frag_bp,
                            lpu);
                        acc.ingest(std::span<const segment_fragment_emission<p_minimizer>>(
                            emissions.data(), emissions.size()));
                        acc.flush_ready(range.core_hi_bp);

                        build_query_filtered_segment_index(
                            std::span<const p_minimizer>{seg_minimizers.data(),
                                                         seg_minimizers.size()},
                            seg_index,
                            filtered_pairs);
                        seg_index.add_position_offset(segment_lo_bp);

                        match_result_collection seg_res =
                            p_index_dispatch::template call<p_alphabet>(
                                query_seq,
                                target_seq,
                                query_minimizers,
                                seg_index,
                                params,
                                segmented_gpu_index_cache_ptr,
                                segment_gpu_identity(segment_index));
                        merged_collection.combine_execution_stats(seg_res);
                        for (auto& result : seg_res.results) {
                            if (result.target_start >= range.core_lo_bp &&
                                result.target_start < range.core_hi_bp) {
                                merged.push_back(std::move(result));
                            }
                        }
                    }
                    acc.finish();
                    return finalize_segmented_stats(std::move(merged_collection));
                }
            }

            query_filtered_pair_collector filtered_pair_collector{&query_kmer_filter, &query_gate};
            for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
                const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                const auto [segment_lo_bp, segment_hi_bp] =
                    pipe::validated_segment_bp_bounds(range, lpu, target_len);
                bioseq::indexing::gpu_index_cache_identity gpu_identity =
                    segment_gpu_identity(segment_index);
                std::size_t seg_result_count = 0;
                if (target_sketch_backend == minimizer_sketch_backend::vulkan_segment) {
                    filtered_pair_collector.position_offset = segment_lo_bp;
                    filtered_pair_collector.clear();
                    filtered_pair_collector.reserve(
                        (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                    bioseq::gpu::vulkan::collect_minimizers_vulkan_segment_or_throw<p_minimizer>(
                        filtered_pair_collector,
                        target_seq,
                        segment_lo_bp,
                        segment_hi_bp,
                        w,
                        k,
                        query_gate.vulkan_gate());
                    // Safe with the approximate device gate because the collector
                    // still exact-confirms each emitted kmer against query_kmer_filter.
                    seg_index.build_from_pairs(
                        std::span<filtered_minimizer_pair>{filtered_pair_collector.pairs.data(),
                                                           filtered_pair_collector.pairs.size()},
                        target_len);

                    match_result_collection seg_res =
                        p_index_dispatch::template call<p_alphabet>(query_seq,
                                                                    target_seq,
                                                                    query_minimizers,
                                                                    seg_index,
                                                                    params,
                                                                    segmented_gpu_index_cache_ptr,
                                                                    gpu_identity);
                    merged_collection.combine_execution_stats(seg_res);
                    seg_result_count = seg_res.results.size();
                    for (auto& result : seg_res.results) {
                        if (result.target_start >= range.core_lo_bp &&
                            result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                } else {
                    seg_collector.clear();
                    seg_collector.reserve(
                        (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                    const auto range_stats = pipe::minimizer_dispatch_range<p_minimizer>(
                        seg_collector,
                        target_seq,
                        segment_lo_bp,
                        segment_hi_bp,
                        w,
                        k,
                        target_sketch_backend,
                        pipe::exact_simd_range_route::require_continuation,
                        params.highway_kernels);
                    record_target_range_stats(merged_collection, range_stats);
                    const std::vector<p_minimizer>& seg_minimizers =
                        seg_collector.extract_sketches_data();
                    build_query_filtered_segment_index(
                        std::span<const p_minimizer>{seg_minimizers.data(), seg_minimizers.size()},
                        seg_index,
                        filtered_pairs);
                    seg_index.add_position_offset(segment_lo_bp);

                    match_result_collection seg_res =
                        p_index_dispatch::template call<p_alphabet>(query_seq,
                                                                    target_seq,
                                                                    query_minimizers,
                                                                    seg_index,
                                                                    params,
                                                                    segmented_gpu_index_cache_ptr,
                                                                    gpu_identity);
                    merged_collection.combine_execution_stats(seg_res);
                    seg_result_count = seg_res.results.size();
                    for (auto& result : seg_res.results) {
                        if (result.target_start >= range.core_lo_bp &&
                            result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                }
                bioseq_log_lazy<logging_level::debug>([&range, &merged, seg_result_count] {
                    bioseq_log_progress<logging_level::debug>(
                        "[minimap_port_segmented] segment core [{},{}) done, kept {} of {} "
                        "(merged {}) | footprint={}MB avail={}MB",
                        range.core_lo_bp,
                        range.core_hi_bp,
                        merged.size(),
                        seg_result_count,
                        merged.size(),
                        bioseq::common::memory::process_footprint_bytes() >> 20,
                        bioseq::common::memory::system_available_bytes() >> 20);
                });
            }
            return finalize_segmented_stats(std::move(merged_collection));
        };

        auto run_vulkan_segment_coordinate_batch = [target_len,
                                                    query_len,
                                                    lpu,
                                                    target_sketch_backend,
                                                    &query_minimizers,
                                                    &query_gate,
                                                    &target_seq,
                                                    w,
                                                    k,
                                                    &segment_gpu_identity,
                                                    &params]() -> match_result_collection {
            auto& runtime = bioseq::gpu::vulkan::runtime::instance();
            if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
                pipe::throw_vulkan_declined("vulkan_segment resident coordinate batch");
            }
            const auto ranges = pipe::segment_unit_ranges_for_backend(
                target_len, query_len, lpu, target_sketch_backend);
            std::span<const p_minimizer> sketch_span{query_minimizers.data(),
                                                     query_minimizers.size()};
            const bioseq::gpu::vulkan::buffer* device_words =
                (query_minimizers.device && query_minimizers.device->words() != nullptr &&
                 query_minimizers.device->count() == sketch_span.size())
                    ? query_minimizers.device->words()
                    : nullptr;

            std::vector<std::shared_ptr<bioseq::indexing::gpu_resident_minimizer_index>> residents;
            std::vector<bioseq::gpu::vulkan::gpu_seed_coordinate_job> jobs;
            std::vector<std::size_t> job_segment_indices;
            residents.reserve(ranges.size());
            jobs.reserve(ranges.size());
            job_segment_indices.reserve(ranges.size());

            const auto frozen_query_gate = pipe::freeze_gpu_query_gate(query_gate.vulkan_gate());
            const auto frozen_gate_identity = frozen_query_gate.canonical_identity();

            for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
                const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                const auto [segment_lo_bp, segment_hi_bp] =
                    pipe::validated_segment_bp_bounds(range, lpu, target_len);
                bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                auto built = bioseq::gpu::vulkan::build_gpu_segment_index_on_device<p_minimizer>(
                    target_seq, segment_lo_bp, segment_hi_bp, w, k, frozen_query_gate, &decline);
                if (!built) {
                    pipe::throw_vulkan_sketch_declined("vulkan_segment GPU resident index build",
                                                       decline);
                }
                if (!built->valid || built->position_count == 0u || built->occupied_slots == 0u) {
                    continue;
                }

                auto identity = segment_gpu_identity(segment_index);
                bioseq::indexing::gpu_minimizer_index_cache_key key{
                    .source_id = 0u,
                    .caller_identity = identity.assembly_identity,
                    .target_len = built->target_len,
                    .table_size = built->occupied_slots,
                    .position_count = built->position_count,
                };
                pipe::bind_verified_gpu_segment_gate_identity(
                    key, frozen_gate_identity, built->gate_identity);
                auto adopted =
                    bioseq::indexing::adopt_gpu_built_index(runtime, std::move(*built), key);
                if (!adopted) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                        "vulkan_segment GPU resident index adoption rejected ({})",
                        bioseq::indexing::gpu_built_index_adoption_error_name(adopted.error())));
                }
                auto resident = std::move(*adopted);
                resident->target_position_offset = segment_lo_bp;
                residents.push_back(resident);
                jobs.push_back(bioseq::gpu::vulkan::gpu_seed_coordinate_job{
                    .slots = &resident->slots,
                    .positions = &resident->positions,
                    .target_len = resident->target_len,
                    .slot_mask = resident->slot_mask,
                    .position_count = resident->position_count,
                    .bucket_count = resident->bucket_count,
                    .target_position_offset = resident->target_position_offset,
                });
                job_segment_indices.push_back(segment_index);
            }
            if (jobs.empty()) {
                return match_result_collection{};
            }

            auto cp = params.make_chaining_parameters();
            std::int32_t max_dist_x = cp.max_gap;
            std::int32_t max_dist_y = cp.max_gap;
            if (max_dist_x < cp.bandwidth) {
                max_dist_x = cp.bandwidth;
            }
            if (max_dist_y < cp.bandwidth) {
                max_dist_y = cp.bandwidth;
            }
            const auto chain_key =
                bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
            bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
                .host_minimizer_bytes = device_words != nullptr ? std::span<const std::byte>{}
                                                                : std::as_bytes(sketch_span),
                .device_minimizer_words = device_words,
                .minimizer_count = static_cast<std::uint32_t>(sketch_span.size()),
                .minimizer_word_kind = static_cast<std::uint32_t>(
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_word_kind),
                .minimizer_words32_per_record = bioseq::gpu::vulkan::minimizer_wire_traits<
                    p_minimizer>::k_words32_per_minimizer,
                .hash_shift = bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_hash_shift,
                .pos_bits =
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_position_bits,
            };
            const auto coords = runtime.run_seed_to_coordinate_results_roundtrip(
                query_inputs,
                std::span<const bioseq::gpu::vulkan::gpu_seed_coordinate_job>{jobs.data(),
                                                                              jobs.size()},
                query_len,
                static_cast<std::uint32_t>(params.kmer_size),
                params.max_occ,
                bioseq::gpu::vulkan::gpu_seed_coordinate_algorithm::vulkan_tiled_dp,
                chain_key,
                max_dist_x,
                max_dist_y,
                cp.bandwidth,
                std::min<std::int32_t>(cp.max_chain_iter, 3000),
                cp.gap_penalty,
                cp.skip_penalty,
                cp.max_chain_skip,
                cp.min_chain_score,
                cp.min_seed_count,
                16u);

            std::vector<match_result> merged;
            merged.reserve(coords.size());
            for (std::size_t job_index = 0; job_index < coords.size(); ++job_index) {
                const auto& coord = coords[job_index];
                if (coord.is_valid == 0u) {
                    continue;
                }
                const ::bioseq::indexing::segment_unit_range& range =
                    ranges[job_segment_indices[job_index]];
                if (coord.target_start >= range.core_lo_bp &&
                    coord.target_start < range.core_hi_bp) {
                    merged.push_back(
                        pipe::gpu_coordinate_to_match_result(coord, query_len, target_len));
                }
            }
            return match_result_collection{std::move(merged)};
        };

        if (target_sketch_backend == minimizer_sketch_backend::vulkan_segment &&
            pipe::is_gpu_coordinate_only_execution_active(params) &&
            fragment_index_out == nullptr) {
            return finalize_segmented_stats(run_vulkan_segment_coordinate_batch());
        }

        auto& pool = bioseq::parallelism::global_pool::instance();
        if (pipe::vulkan_accelerator_requested(params) ||
            !bioseq::parallelism::has_parallel_tasks(pool.get_mode()) ||
            pool.maximum_concurrency() <= 1 ||
            target_sketch_backend == minimizer_sketch_backend::vulkan_segment ||
            !bioseq::storage::is_worker_shareable(query_seq) ||
            !bioseq::storage::is_worker_shareable(target_seq)) {
            return run_serial_segmented();
        }

        struct segment_wave_admission {
            unsigned fanout = 1;
            unsigned geometry_divisor = 1;
            sequence_size_t segment_bp = 0;
            std::size_t estimated_segments = 0;
            std::size_t estimated_waves = 0;
            std::uint64_t peak_per_segment = 0;
            bioseq::parallelism::memory_permit permit;
            bool is_admitted = false;
        };

        constexpr std::uint64_t context_reserve = std::uint64_t{8} << 20;
        auto collected_bp_upper_bound = [target_len,
                                         overlap_bp](sequence_size_t core_bp) -> std::uint64_t {
            const std::uint64_t core = static_cast<std::uint64_t>(core_bp);
            const std::uint64_t overlap = static_cast<std::uint64_t>(overlap_bp);
            const std::uint64_t expanded = bioseq::common::saturating_mul(2, overlap);
            const std::uint64_t collected =
                core > (std::numeric_limits<std::uint64_t>::max)() - expanded
                    ? (std::numeric_limits<std::uint64_t>::max)()
                    : core + expanded;
            return std::min<std::uint64_t>(collected, static_cast<std::uint64_t>(target_len));
        };

        auto peak_for_segment_core_bp = [&collected_bp_upper_bound](sequence_size_t core_bp) {
            return bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
                       collected_bp_upper_bound(core_bp)) +
                   context_reserve;
        };

        struct segment_wave_candidate {
            unsigned fanout = 1;
            unsigned geometry_divisor = 1;
            sequence_size_t segment_bp = 0;
            std::size_t estimated_segments = 0;
            std::size_t estimated_waves = 0;
            std::uint64_t peak_per_segment = 0;
            std::uint64_t reserve_bytes = 0;
            std::uint64_t estimated_work_bp = 0;
        };

        auto try_admit_segment_geometry = [&pool,
                                           target_len,
                                           &collected_bp_upper_bound,
                                           &peak_for_segment_core_bp]() -> segment_wave_admission {
            auto& admission = bioseq::parallelism::memory_admission_controller::instance();
            const unsigned max_fanout = pool.maximum_concurrency();
            if (max_fanout <= 1) {
                return segment_wave_admission{};
            }

            const std::uint64_t allocatable = admission.allocatable_after_reservations();
            std::vector<segment_wave_candidate> candidates;
            candidates.reserve(static_cast<std::size_t>(max_fanout));

            for (unsigned fanout = max_fanout; fanout > 1; --fanout) {
                const unsigned geometry_divisor = fanout;
                const sequence_size_t bp = pipe::segment_bp_from_budget_divided(geometry_divisor);
                const std::uint64_t collected_bp = collected_bp_upper_bound(bp);
                const std::size_t estimated_segments = static_cast<std::size_t>(
                    (target_len + bp - 1) / std::max<sequence_size_t>(bp, 1));
                const std::uint64_t peak = peak_for_segment_core_bp(bp);

                const std::uint64_t reserve_bytes =
                    bioseq::common::saturating_mul(static_cast<std::uint64_t>(fanout), peak);
                if (reserve_bytes > allocatable) {
                    continue;
                }

                const std::size_t estimated_waves = (estimated_segments + fanout - 1) / fanout;
                const std::uint64_t estimated_work = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(estimated_waves), collected_bp);
                candidates.push_back(segment_wave_candidate{fanout,
                                                            geometry_divisor,
                                                            bp,
                                                            estimated_segments,
                                                            estimated_waves,
                                                            peak,
                                                            reserve_bytes,
                                                            estimated_work});
            }

            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const segment_wave_candidate& lhs, const segment_wave_candidate& rhs) {
                          if (lhs.estimated_work_bp != rhs.estimated_work_bp) {
                              return lhs.estimated_work_bp < rhs.estimated_work_bp;
                          }
                          if (lhs.estimated_segments != rhs.estimated_segments) {
                              return lhs.estimated_segments < rhs.estimated_segments;
                          }
                          if (lhs.fanout != rhs.fanout) {
                              return lhs.fanout > rhs.fanout;
                          }
                          return lhs.geometry_divisor < rhs.geometry_divisor;
                      });

            for (const segment_wave_candidate& candidate : candidates) {
                if (auto permit = admission.try_reserve(candidate.reserve_bytes)) {
                    return segment_wave_admission{candidate.fanout,
                                                  candidate.geometry_divisor,
                                                  candidate.segment_bp,
                                                  candidate.estimated_segments,
                                                  candidate.estimated_waves,
                                                  candidate.peak_per_segment,
                                                  std::move(*permit),
                                                  true};
                }
            }

            return segment_wave_admission{};
        };

        auto first_wave = try_admit_segment_geometry();
        if (!first_wave.is_admitted || first_wave.fanout <= 1) {
            return run_serial_segmented();
        }

        bioseq_log_lazy<logging_level::debug>([&first_wave] {
            bioseq_log_progress<logging_level::debug>(
                "[minimap_port_segmented] admitted parallel geometry fanout={} "
                "divisor={} segment_bp={} estimated_segments={} "
                "estimated_waves={} peak_per_segment={}MB reserve={}MB "
                "| footprint={}MB avail={}MB",
                first_wave.fanout,
                first_wave.geometry_divisor,
                first_wave.segment_bp,
                first_wave.estimated_segments,
                first_wave.estimated_waves,
                first_wave.peak_per_segment >> 20,
                first_wave.permit.bytes() >> 20,
                bioseq::common::memory::process_footprint_bytes() >> 20,
                bioseq::common::memory::system_available_bytes() >> 20);
        });

        const unsigned geometry_divisor = first_wave.geometry_divisor;
        auto ranges = pipe::segment_unit_ranges(target_len, query_len, lpu, geometry_divisor);
        std::vector<match_result_collection> seg_out(ranges.size());
        bioseq::parallelism::task_parallel_scope scope(
            bioseq::parallelism::task_parallel_region::segment_recompute);
        if (!scope.is_acquired()) {
            return run_serial_segmented();
        }

        if constexpr (!std::is_same_v<p_fragment_index_storage, std::nullptr_t>) {
            if (fragment_index_out != nullptr) {
                constexpr sequence_size_t frag_bp = 32768u;
                fragment_persist_accumulator<p_fragment_index_storage, p_minimizer> acc{
                    *fragment_index_out, target_len, frag_bp};
                match_result_collection merged_collection;
                std::vector<match_result>& merged = merged_collection.results;

                auto process_segment_serial = [&ranges,
                                               &target_seq,
                                               &query_seq,
                                               &query_minimizers,
                                               &params,
                                               &acc,
                                               &merged_collection,
                                               &merged,
                                               target_len,
                                               frag_bp,
                                               w,
                                               k,
                                               lpu,
                                               target_sketch_backend,
                                               segmented_gpu_index_cache_ptr,
                                               &segment_gpu_identity,
                                               &record_target_range_stats](
                                                  std::size_t segment_index) {
                    basic_sketch_collector<p_minimizer> collector;
                    minimizer_hashmap_fragment_index index;
                    const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                    const auto [segment_lo_bp, segment_hi_bp] =
                        pipe::validated_segment_bp_bounds(range, lpu, target_len);
                    collector.reserve(
                        (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                    const auto range_stats = pipe::minimizer_dispatch_range<p_minimizer>(
                        collector,
                        target_seq,
                        segment_lo_bp,
                        segment_hi_bp,
                        w,
                        k,
                        target_sketch_backend,
                        pipe::exact_simd_range_route::require_continuation,
                        params.highway_kernels);
                    record_target_range_stats(merged_collection, range_stats);
                    const std::vector<p_minimizer>& minimizers = collector.extract_sketches_data();
                    auto emissions = collect_fragment_emissions_for_segment(
                        range,
                        std::span<const p_minimizer>(minimizers.data(), minimizers.size()),
                        target_len,
                        frag_bp,
                        lpu);
                    acc.ingest(std::span<const segment_fragment_emission<p_minimizer>>(
                        emissions.data(), emissions.size()));
                    acc.flush_ready(range.core_hi_bp);

                    index.build(minimizers, target_len);
                    index.add_position_offset(segment_lo_bp);
                    match_result_collection seg_res = p_index_dispatch::template call<p_alphabet>(
                        query_seq,
                        target_seq,
                        query_minimizers,
                        index,
                        params,
                        segmented_gpu_index_cache_ptr,
                        segment_gpu_identity(segment_index));
                    merged_collection.combine_execution_stats(seg_res);
                    for (auto& result : seg_res.results) {
                        if (result.target_start >= range.core_lo_bp &&
                            result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                };

                for (std::size_t wave_lo = 0; wave_lo < ranges.size();) {
                    const std::size_t wave_count =
                        std::min<std::size_t>(first_wave.fanout, ranges.size() - wave_lo);
                    bioseq::parallelism::memory_permit wave_permit;
                    if (wave_lo == 0) {
                        wave_permit = std::move(first_wave.permit);
                    } else if (auto permit =
                                   bioseq::parallelism::memory_admission_controller::instance()
                                       .try_reserve(bioseq::common::saturating_mul(
                                           static_cast<std::uint64_t>(wave_count),
                                           first_wave.peak_per_segment))) {
                        wave_permit = std::move(*permit);
                    }
                    if (!wave_permit) {
                        for (std::size_t segment_index = wave_lo; segment_index < ranges.size();
                             ++segment_index) {
                            process_segment_serial(segment_index);
                        }
                        wave_lo = ranges.size();
                        break;
                    }

                    const std::size_t current_wave_lo = wave_lo;
                    const std::size_t wave_hi = wave_lo + wave_count;
                    std::vector<match_result_collection> wave_seg_out(wave_count);
                    std::vector<std::vector<segment_fragment_emission<p_minimizer>>> wave_frag_out(
                        wave_count);
                    pool.parallel_for_with_context_tracked(
                        wave_lo,
                        wave_hi,
                        1,
                        bioseq::parallelism::range_body{
                            [&ranges,
                             &wave_seg_out,
                             &wave_frag_out,
                             &target_seq,
                             &query_seq,
                             &query_minimizers,
                             &params,
                             current_wave_lo,
                             target_len,
                             frag_bp,
                             w,
                             k,
                             lpu,
                             target_sketch_backend,
                             segmented_gpu_index_cache_ptr,
                             &segment_gpu_identity,
                             &record_target_range_stats](std::size_t lo, std::size_t hi) {
                                basic_sketch_collector<p_minimizer> collector;
                                minimizer_hashmap_fragment_index index;
                                for (std::size_t segment_index = lo; segment_index < hi;
                                     ++segment_index) {
                                    const ::bioseq::indexing::segment_unit_range& range =
                                        ranges[segment_index];
                                    const auto [segment_lo_bp, segment_hi_bp] =
                                        pipe::validated_segment_bp_bounds(range, lpu, target_len);
                                    const std::size_t rel = segment_index - current_wave_lo;
                                    collector.clear();
                                    collector.reserve((range.ce_u - range.cs_u) * lpu /
                                                          std::max<std::size_t>(w, 1) +
                                                      64);
                                    const auto range_stats =
                                        pipe::minimizer_dispatch_range<p_minimizer>(
                                            collector,
                                            target_seq,
                                            segment_lo_bp,
                                            segment_hi_bp,
                                            w,
                                            k,
                                            target_sketch_backend,
                                            pipe::exact_simd_range_route::require_continuation,
                                            params.highway_kernels);
                                    const std::vector<p_minimizer>& minimizers =
                                        collector.extract_sketches_data();
                                    wave_frag_out[rel] = collect_fragment_emissions_for_segment(
                                        range,
                                        std::span<const p_minimizer>(minimizers.data(),
                                                                     minimizers.size()),
                                        target_len,
                                        frag_bp,
                                        lpu);
                                    index.build(minimizers, target_len);
                                    index.add_position_offset(segment_lo_bp);
                                    wave_seg_out[rel] = p_index_dispatch::template call<p_alphabet>(
                                        query_seq,
                                        target_seq,
                                        query_minimizers,
                                        index,
                                        params,
                                        segmented_gpu_index_cache_ptr,
                                        segment_gpu_identity(segment_index));
                                    record_target_range_stats(wave_seg_out[rel], range_stats);
                                }
                            }},
                        bioseq::parallelism::current_tracked_job_observer());

                    for (std::size_t rel = 0; rel < wave_count; ++rel) {
                        const std::size_t segment_index = current_wave_lo + rel;
                        const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                        acc.ingest(std::span<const segment_fragment_emission<p_minimizer>>(
                            wave_frag_out[rel].data(), wave_frag_out[rel].size()));
                        acc.flush_ready(range.core_hi_bp);
                        merged_collection.combine_execution_stats(wave_seg_out[rel]);
                        for (auto& result : wave_seg_out[rel].results) {
                            if (result.target_start >= range.core_lo_bp &&
                                result.target_start < range.core_hi_bp) {
                                merged.push_back(std::move(result));
                            }
                        }
                    }
                    wave_lo = wave_hi;
                }

                acc.finish();
                return finalize_segmented_stats(std::move(merged_collection));
            }
        }

        std::vector<std::size_t> segment_minimizer_counts(ranges.size());
        std::vector<std::size_t> segment_filtered_counts(ranges.size());

        auto process_segment_serial = [&ranges,
                                       &seg_out,
                                       &target_seq,
                                       &query_seq,
                                       &query_minimizers,
                                       &segment_minimizer_counts,
                                       &segment_filtered_counts,
                                       &build_query_filtered_segment_index,
                                       &params,
                                       target_len,
                                       w,
                                       k,
                                       lpu,
                                       target_sketch_backend,
                                       segmented_gpu_index_cache_ptr,
                                       &segment_gpu_identity,
                                       &record_target_range_stats](std::size_t segment_index) {
            basic_sketch_collector<p_minimizer> collector;
            minimizer_hashmap_fragment_index index;
            std::vector<filtered_minimizer_pair> filtered_pairs;
            const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
            const auto [segment_lo_bp, segment_hi_bp] =
                pipe::validated_segment_bp_bounds(range, lpu, target_len);
            collector.reserve((range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
            const auto range_stats = pipe::minimizer_dispatch_range<p_minimizer>(
                collector,
                target_seq,
                segment_lo_bp,
                segment_hi_bp,
                w,
                k,
                target_sketch_backend,
                pipe::exact_simd_range_route::require_continuation,
                params.highway_kernels);
            const std::vector<p_minimizer>& minimizers = collector.extract_sketches_data();
            segment_minimizer_counts[segment_index] = minimizers.size();
            segment_filtered_counts[segment_index] = build_query_filtered_segment_index(
                std::span<const p_minimizer>{minimizers.data(), minimizers.size()},
                index,
                filtered_pairs);
            index.add_position_offset(segment_lo_bp);
            seg_out[segment_index] =
                p_index_dispatch::template call<p_alphabet>(query_seq,
                                                            target_seq,
                                                            query_minimizers,
                                                            index,
                                                            params,
                                                            segmented_gpu_index_cache_ptr,
                                                            segment_gpu_identity(segment_index));
            record_target_range_stats(seg_out[segment_index], range_stats);
            bioseq_log_lazy<logging_level::debug>([&range, &seg_out, segment_index] {
                bioseq_log_progress<logging_level::debug>(
                    "[minimap_port_segmented] adaptive serial segment core [{},{}) "
                    "produced {} results | footprint={}MB avail={}MB",
                    range.core_lo_bp,
                    range.core_hi_bp,
                    seg_out[segment_index].results.size(),
                    bioseq::common::memory::process_footprint_bytes() >> 20,
                    bioseq::common::memory::system_available_bytes() >> 20);
            });
        };

        for (std::size_t wave_lo = 0; wave_lo < ranges.size();) {
            const std::size_t requested_wave_count =
                std::min<std::size_t>(first_wave.fanout, ranges.size() - wave_lo);
            std::size_t wave_count = requested_wave_count;
            bioseq::parallelism::memory_permit wave_permit;
            if (wave_lo == 0) {
                wave_permit = std::move(first_wave.permit);
                wave_permit.shrink_to(bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(wave_count), first_wave.peak_per_segment));
            } else {
                for (std::size_t candidate = requested_wave_count; candidate > 1; --candidate) {
                    if (auto permit = bioseq::parallelism::memory_admission_controller::instance()
                                          .try_reserve(bioseq::common::saturating_mul(
                                              static_cast<std::uint64_t>(candidate),
                                              first_wave.peak_per_segment))) {
                        wave_count = candidate;
                        wave_permit = std::move(*permit);
                        break;
                    }
                }
            }
            if (!wave_permit) {
                bioseq_log_lazy<logging_level::debug>([wave_lo, &ranges] {
                    bioseq_log_progress<logging_level::debug>(
                        "[minimap_port_segmented] wave admission failed at segment {} "
                        "of {}; processing one segment serially | footprint={}MB "
                        "avail={}MB",
                        wave_lo,
                        ranges.size(),
                        bioseq::common::memory::process_footprint_bytes() >> 20,
                        bioseq::common::memory::system_available_bytes() >> 20);
                });
                process_segment_serial(wave_lo);
                ++wave_lo;
                continue;
            }
            if (wave_count != requested_wave_count) {
                bioseq_log_lazy<logging_level::debug>(
                    [requested_wave_count, wave_count, wave_lo, &ranges] {
                        bioseq_log_progress<logging_level::debug>(
                            "[minimap_port_segmented] wave fanout reduced from {} to {} "
                            "at segment {} of {} | footprint={}MB avail={}MB",
                            requested_wave_count,
                            wave_count,
                            wave_lo,
                            ranges.size(),
                            bioseq::common::memory::process_footprint_bytes() >> 20,
                            bioseq::common::memory::system_available_bytes() >> 20);
                    });
            }

            const std::size_t wave_hi = wave_lo + wave_count;
            pool.parallel_for_with_context_tracked(
                wave_lo,
                wave_hi,
                1,
                bioseq::parallelism::range_body{[&ranges,
                                                 &seg_out,
                                                 &target_seq,
                                                 &query_seq,
                                                 &query_minimizers,
                                                 &segment_minimizer_counts,
                                                 &segment_filtered_counts,
                                                 &build_query_filtered_segment_index,
                                                 &params,
                                                 target_len,
                                                 w,
                                                 k,
                                                 lpu,
                                                 target_sketch_backend,
                                                 segmented_gpu_index_cache_ptr,
                                                 &segment_gpu_identity,
                                                 &record_target_range_stats](std::size_t lo,
                                                                             std::size_t hi) {
                    basic_sketch_collector<p_minimizer> collector;
                    minimizer_hashmap_fragment_index index;
                    std::vector<filtered_minimizer_pair> filtered_pairs;
                    for (std::size_t segment_index = lo; segment_index < hi; ++segment_index) {
                        const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                        const auto [segment_lo_bp, segment_hi_bp] =
                            pipe::validated_segment_bp_bounds(range, lpu, target_len);
                        collector.clear();
                        collector.reserve(
                            (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                        const auto range_stats = pipe::minimizer_dispatch_range<p_minimizer>(
                            collector,
                            target_seq,
                            segment_lo_bp,
                            segment_hi_bp,
                            w,
                            k,
                            target_sketch_backend,
                            pipe::exact_simd_range_route::require_continuation,
                            params.highway_kernels);
                        const std::vector<p_minimizer>& minimizers =
                            collector.extract_sketches_data();
                        segment_minimizer_counts[segment_index] = minimizers.size();
                        segment_filtered_counts[segment_index] = build_query_filtered_segment_index(
                            std::span<const p_minimizer>{minimizers.data(), minimizers.size()},
                            index,
                            filtered_pairs);
                        index.add_position_offset(segment_lo_bp);
                        seg_out[segment_index] = p_index_dispatch::template call<p_alphabet>(
                            query_seq,
                            target_seq,
                            query_minimizers,
                            index,
                            params,
                            segmented_gpu_index_cache_ptr,
                            segment_gpu_identity(segment_index));
                        record_target_range_stats(seg_out[segment_index], range_stats);
                    }
                }},
                bioseq::parallelism::current_tracked_job_observer());
            wave_lo = wave_hi;
        }

        std::size_t total_segment_minimizers = 0;
        std::size_t total_filtered_minimizers = 0;
        for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
            total_segment_minimizers += segment_minimizer_counts[segment_index];
            total_filtered_minimizers += segment_filtered_counts[segment_index];
        }
        bioseq_log_lazy<logging_level::debug>(
            [total_filtered_minimizers, total_segment_minimizers, &ranges] {
                bioseq_log_progress<logging_level::debug>(
                    "[minimap_port_segmented] query-filtered transient index kept {} "
                    "of {} segment minimizers across {} segments | footprint={}MB "
                    "avail={}MB",
                    total_filtered_minimizers,
                    total_segment_minimizers,
                    ranges.size(),
                    bioseq::common::memory::process_footprint_bytes() >> 20,
                    bioseq::common::memory::system_available_bytes() >> 20);
            });

        match_result_collection merged_collection;
        std::vector<match_result>& merged = merged_collection.results;
        for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
            const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
            merged_collection.combine_execution_stats(seg_out[segment_index]);
            for (auto& result : seg_out[segment_index].results) {
                if (result.target_start >= range.core_lo_bp &&
                    result.target_start < range.core_hi_bp) {
                    merged.push_back(std::move(result));
                }
            }
        }
        return finalize_segmented_stats(std::move(merged_collection));
    }

    } // namespace bioseq::simd

} // export
