/**
 * @file minimizer_sequence_index/read_assembly/build.cppm
 * @brief Builds direct minimizer buckets using scalar, SIMD or Vulkan sketching.
 */
module;

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <libassert/assert.hpp>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_sequence:read_assembly_build;

import :read_assembly_contract;
import bioseq.indexing.minimizer_sequence.admitted_runs;
import bioseq.indexing.minimizer_sequence.bucket_codec;
import bioseq.indexing.minimizer_sequence.generation;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

export namespace bioseq::indexing {

/**
 * Build a saved minimizer index for one sequence with k=15 and w=10.
 * Collect global hash/position pairs into sorted runs, merge them into buckets,
 * then write the directory  after all buckets are stored. Memory pressure can
 * split collection ranges or defer them to the calling thread
 * kernels supplies the selected SIMD functions. scalar_build_stats_out optionally
 * receives collection counts. alphabet_code identifies the input alphabet.
 */
template <class p_storage>
template <sequence_alphabet p_alphabet>
inline void direct_minimizer_bucket_index<p_storage>::build(
    p_storage& storage,
    bioseq::storage::lazy_sequence_view<p_alphabet> seq,
    minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels,
    direct_minimizer_bucket_build_stats* scalar_build_stats_out,
    std::uint8_t alphabet_code) {
    constexpr std::size_t w = 10, k = 15;
    constexpr std::size_t lpu = 1u;
    constexpr std::size_t frag_bp = 32'768u;
    constexpr std::size_t window_span = w + k - 2u;
    static_assert(frag_bp < (std::size_t{1} << 28), "fragment core positions fit 28-bit field");
    using detail::k_encoded_header_bytes;
    using detail::k_encoded_bucket_bytes;
    using detail::encoded_bucket_record;
    constexpr std::size_t k_bucket_target_bytes = std::size_t{8} << 20; // 8 MiB

    if (backend == minimizer_sketch_backend::exact_simd &&
        (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
        throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_state,
                                             "minimizers_per_sequence build"};
    }
    if (backend == minimizer_sketch_backend::bounded_nthash_simd &&
        (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
        throw packed_bounded_nthash_index_runtime_error{
            packed_bounded_nthash_index_status::invalid_state, "minimizers_per_sequence build"};
    }

    if (scalar_build_stats_out != nullptr) {
        *scalar_build_stats_out = {};
    }
    const bool capture_scalar_build_stats = scalar_build_stats_out != nullptr;
    direct_minimizer_bucket_build_stats scalar_build_stats{};
    if (capture_scalar_build_stats) {
        scalar_build_stats.bind_backend(backend, 1u, 1u);
    }

    const sequence_size_t total_bp = seq.bp_size();
    const std::size_t nfrag = static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);

    // Collect minimizers as global hash/position pairs and spill sorted runs.
    const std::size_t run_buffer_bytes = storage.run_buffer_bytes();
    using pair_t = detail::sorted_pair_t;
    using detail::admitted_pair_run;
    /** Add the overlap needed to collect minimizers for one core range. */
    const auto make_core_range = [&seq](sequence_size_t core_lo, sequence_size_t core_hi) {
        auto range = make_generation_collection_range(seq, core_lo, core_hi, w, k);
        if (!range) {
            index_corruption_panic("minimizers_per_sequence build: invalid collection/core range");
        }
        return *range;
    };
    /** One worker's collected runs, deferred ranges, statistics and saved error. */
    struct worker_range_result {
        detail::admitted_vector_run<admitted_pair_run> runs;
        detail::admitted_vector_run<segment_unit_range> deferred_for_backend_spill;
        direct_minimizer_bucket_build_stats scalar_build_stats{};
        std::exception_ptr error;
        segment_unit_range error_range{};
    };

    /** Free untransferred worker buffers before releasing their memory permits. */
    auto release_untransferred_results_noexcept =
        [](std::vector<worker_range_result>& results) noexcept {
            for (worker_range_result& result : results) {
                for (admitted_pair_run& run : result.runs.pairs) {
                    run.release_capacity_before_permit();
                }
                result.runs.release_capacity_before_permit();
                result.deferred_for_backend_spill.release_capacity_before_permit();
            }
        };

    // Vulkan selects minimizers on the GPU and sorts the resulting  global
    // hash/position pairs on the host.
    /** Collect global hash/position pairs whose positions fall inside this core. */
    auto collect_core_range_pairs =
        [&seq, backend, kernels, alphabet_code](sequence_size_t core_lo,
                                                sequence_size_t core_hi,
                                                sketch_collector& collector,
                                                std::vector<pair_t>& out,
                                                direct_minimizer_bucket_build_stats* scalar_stats) {
            if (core_hi <= core_lo) {
                return;
            }
            auto range = make_generation_collection_range(seq, core_lo, core_hi, w, k);
            if (!range || !collect_generation_core_run(seq,
                                                       alphabet_code,
                                                       backend,
                                                       kernels,
                                                       *range,
                                                       w,
                                                       k,
                                                       nullptr,
                                                       collector,
                                                       out,
                                                       nullptr,
                                                       true,
                                                       scalar_stats)) {
                index_corruption_panic(
                    "minimizers_per_sequence build: collection/core geometry failed");
            }
        };

    /** Reserve scrotch memory, collect a deferred range, then sort and spill its pairs. */
    auto collect_sort_and_spill_backend_scratch =
        [&backend,
         &storage,
         &make_core_range,
         &collect_core_range_pairs,
         &capture_scalar_build_stats,
         &scalar_build_stats](auto& self, segment_unit_range range) -> void {
        const std::uint64_t bp = static_cast<std::uint64_t>(range.core_hi_bp - range.core_lo_bp);
        constexpr std::uint64_t maximum_bounded_records_per_candidate = w + 1u;
        const std::uint64_t maximum_output_pairs =
            backend == minimizer_sketch_backend::bounded_nthash_simd
                ? bioseq::common::saturating_mul(bp, maximum_bounded_records_per_candidate)
                : bp;
        const std::uint64_t pair_preallocation_bytes =
            detail::admitted_vector_preallocation_bytes<pair_t>(maximum_output_pairs);
        // The Vulkan helper reserves memory for its GPU work and returned pairs.
        // This caller reserves only its own buffers.
        const std::uint64_t backend_scratch_bytes = 0u;
        const std::uint64_t maximum_collection_candidates = bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(range.ce_u - range.cs_u), static_cast<std::uint64_t>(lpu));
        const std::uint64_t maximum_collector_sketches =
            backend == minimizer_sketch_backend::vulkan_segment
                ? 0u
                : (backend == minimizer_sketch_backend::bounded_nthash_simd
                       ? bioseq::common::saturating_mul(maximum_collection_candidates,
                                                        maximum_bounded_records_per_candidate)
                       : maximum_collection_candidates);
        const std::uint64_t collector_headroom_bytes =
            detail::admitted_vector_preallocation_bytes<minimap_sketch_data>(
                maximum_collector_sketches);
        const std::uint64_t admission_bytes = bioseq::common::saturating_add(
            pair_preallocation_bytes,
            bioseq::common::saturating_add(collector_headroom_bytes, backend_scratch_bytes));
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                              s.demote_resident_runs_until_available(bytes);
                          }) {
                storage.demote_resident_runs_until_available(admission_bytes);
            }
        }
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            admission_bytes);
        if (!permit) {
            if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                              s.demote_resident_runs_until_available(bytes);
                          }) {
                storage.demote_resident_runs_until_available(admission_bytes);
                permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                    admission_bytes);
            }
        }
        if (!permit) {
            constexpr sequence_size_t k_scalar_minimum_core_bp = 4096;
            constexpr sequence_size_t k_vulkan_minimum_core_bp = 256;
            const sequence_size_t min_core_bp = backend == minimizer_sketch_backend::vulkan_segment
                                                    ? k_vulkan_minimum_core_bp
                                                    : k_scalar_minimum_core_bp;
            if (range.core_hi_bp - range.core_lo_bp > min_core_bp) {
                const sequence_size_t mid =
                    range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                self(self, make_core_range(range.core_lo_bp, mid));
                self(self, make_core_range(mid, range.core_hi_bp));
                return;
            }
            // Stop when even the minimum core cannot fit the memory budget.
            throw std::bad_alloc{};
        }
        admitted_pair_run scratch;
        scratch.permit = std::move(*permit);
        if (!detail::reserve_admitted_vector_run_capacity(
                scratch,
                maximum_output_pairs,
                bioseq::common::saturating_add(collector_headroom_bytes, backend_scratch_bytes))) {
            index_corruption_panic(
                "minimizers_per_sequence build: scratch pair capacity exceeded admission");
        }
        {
            // Destroy the collector before shrinking or releasing scratch's shared permit.
            sketch_collector collector;
            if (maximum_collector_sketches != 0u) {
                if (maximum_collector_sketches >
                    static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: collector capacity overflow");
                }
                collector.reserve(static_cast<std::size_t>(maximum_collector_sketches));
            }
            const std::uint64_t exact_collector_bytes = bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(collector.extract_sketches_data().capacity()),
                static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
            const std::uint64_t live_before_collect = bioseq::common::saturating_add(
                scratch.admitted_bytes,
                bioseq::common::saturating_add(exact_collector_bytes, backend_scratch_bytes));
            if (exact_collector_bytes > collector_headroom_bytes ||
                live_before_collect > scratch.permit.bytes()) {
                index_corruption_panic(
                    "minimizers_per_sequence build: collector capacity exceeded admission");
            }
            scratch.permit.shrink_to(live_before_collect);
            collect_core_range_pairs(range.core_lo_bp,
                                     range.core_hi_bp,
                                     collector,
                                     scratch.pairs,
                                     capture_scalar_build_stats ? &scalar_build_stats : nullptr);
            const std::uint64_t live_after_collect = bioseq::common::saturating_add(
                detail::admitted_vector_capacity_bytes(scratch.pairs),
                bioseq::common::saturating_add(
                    bioseq::common::saturating_mul(
                        static_cast<std::uint64_t>(collector.extract_sketches_data().capacity()),
                        static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
                    backend_scratch_bytes));
            if (live_after_collect > scratch.permit.bytes()) {
                index_corruption_panic(
                    "minimizers_per_sequence build: collection grew beyond admission");
            }
        }
        // The collector has been freed. Keep  the reservation for the pair vector only.
        if (!detail::finish_admitted_vector_run_transient(scratch)) {
            index_corruption_panic(
                "minimizers_per_sequence build: scratch pair collection exceeded admission");
        }
        if (backend != minimizer_sketch_backend::vulkan_segment) {
            detail::sort_pair_run(scratch.pairs);
        }
        storage.spill_sorted_run(
            std::span<const pair_t>(scratch.pairs.data(), scratch.pairs.size()));
        scratch.release_capacity_before_permit();
    };

    /** Move worker runs into spill storage and collect ranges deferred by memory pressure. */
    auto transfer_result_runs = [&capture_scalar_build_stats,
                                 &scalar_build_stats,
                                 &storage,
                                 &collect_sort_and_spill_backend_scratch,
                                 &release_untransferred_results_noexcept](
                                    std::vector<worker_range_result>& results) {
        for (worker_range_result& result : results) {
            if (result.error) {
                try {
                    std::rethrow_exception(result.error);
                } catch (const exact_simd_index_runtime_error&) {
                    // Let the PostgreSQL error handler report the unsupported SIMD request
                    // using the original status.
                    throw;
                } catch (const packed_bounded_nthash_index_runtime_error&) {
                    // Let the caller handle the bounded-kernel failure with its original status.
                    throw;
                } catch (const std::exception& e) {
                    const std::string message =
                        "bioseq minimizers_per_sequence worker failed for core=[" +
                        std::to_string(static_cast<std::uint64_t>(result.error_range.core_lo_bp)) +
                        "," +
                        std::to_string(static_cast<std::uint64_t>(result.error_range.core_hi_bp)) +
                        "): " + e.what();
                    index_corruption_panic(message);
                } catch (...) {
                    const std::string message =
                        "bioseq minimizers_per_sequence worker failed for core=[" +
                        std::to_string(static_cast<std::uint64_t>(result.error_range.core_lo_bp)) +
                        "," +
                        std::to_string(static_cast<std::uint64_t>(result.error_range.core_hi_bp)) +
                        "): non-std exception";
                    index_corruption_panic(message);
                }
            }
            if (capture_scalar_build_stats) {
                scalar_build_stats.merge(result.scalar_build_stats);
            }
            for (admitted_pair_run& run : result.runs.pairs) {
                if (!run.pairs.empty()) {
                    storage.spill_sorted_run_owned_admitted(
                        std::move(run.pairs), run.admitted_bytes, std::move(run.permit));
                    run.admitted_bytes = 0;
                }
            }
            for (segment_unit_range range : result.deferred_for_backend_spill.pairs) {
                collect_sort_and_spill_backend_scratch(collect_sort_and_spill_backend_scratch,
                                                       range);
            }
        }
        release_untransferred_results_noexcept(results);
    };

    /** Collect a range, within its memory reservation, splitting or deferring it when space is short */
    auto collect_range_admitted =
        [&backend,
         &storage,
         &make_core_range,
         &collect_core_range_pairs,
         &capture_scalar_build_stats](
            auto& self, segment_unit_range range, worker_range_result& result) -> void {
        if (result.error) {
            return;
        }
        try {
            const std::uint64_t bp =
                static_cast<std::uint64_t>(range.core_hi_bp - range.core_lo_bp);
            constexpr std::uint64_t maximum_bounded_records_per_candidate = w + 1u;
            const std::uint64_t maximum_output_pairs =
                backend == minimizer_sketch_backend::bounded_nthash_simd
                    ? bioseq::common::saturating_mul(bp, maximum_bounded_records_per_candidate)
                    : bp;
            const std::uint64_t pair_preallocation_bytes =
                detail::admitted_vector_preallocation_bytes<pair_t>(maximum_output_pairs);
            const std::uint64_t backend_scratch_bytes = 0u;
            const std::uint64_t maximum_collection_candidates =
                bioseq::common::saturating_mul(static_cast<std::uint64_t>(range.ce_u - range.cs_u),
                                               static_cast<std::uint64_t>(lpu));
            const std::uint64_t maximum_collector_sketches =
                backend == minimizer_sketch_backend::vulkan_segment
                    ? 0u
                    : (backend == minimizer_sketch_backend::bounded_nthash_simd
                           ? bioseq::common::saturating_mul(maximum_collection_candidates,
                                                            maximum_bounded_records_per_candidate)
                           : maximum_collection_candidates);
            const std::uint64_t collector_headroom_bytes =
                detail::admitted_vector_preallocation_bytes<minimap_sketch_data>(
                    maximum_collector_sketches);
            const std::uint64_t admission_bytes = bioseq::common::saturating_add(
                pair_preallocation_bytes,
                bioseq::common::saturating_add(collector_headroom_bytes, backend_scratch_bytes));
            if (backend == minimizer_sketch_backend::vulkan_segment) {
                if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                                  s.demote_resident_runs_until_available(bytes);
                              }) {
                    storage.demote_resident_runs_until_available(admission_bytes);
                }
            }
            auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                admission_bytes);
            if (!permit) {
                constexpr sequence_size_t k_scalar_minimum_core_bp = 4096;
                constexpr sequence_size_t k_vulkan_minimum_core_bp = 256;
                const sequence_size_t min_core_bp =
                    backend == minimizer_sketch_backend::vulkan_segment ? k_vulkan_minimum_core_bp
                                                                        : k_scalar_minimum_core_bp;
                if (range.core_hi_bp - range.core_lo_bp <= min_core_bp) {
                    if (!detail::ensure_admitted_vector_run_capacity(
                            result.deferred_for_backend_spill,
                            result.deferred_for_backend_spill.pairs.size() + 1u)) {
                        throw std::bad_alloc{};
                    }
                    result.deferred_for_backend_spill.pairs.push_back(range);
                    return;
                }
                const sequence_size_t mid =
                    range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                self(self, make_core_range(range.core_lo_bp, mid), result);
                self(self, make_core_range(mid, range.core_hi_bp), result);
                return;
            }

            admitted_pair_run run{};
            run.permit = std::move(*permit);
            if (!detail::reserve_admitted_vector_run_capacity(
                    run,
                    maximum_output_pairs,
                    bioseq::common::saturating_add(collector_headroom_bytes,
                                                   backend_scratch_bytes))) {
                index_corruption_panic(
                    "minimizers_per_sequence build: worker pair capacity exceeded admission");
            }
            {
                sketch_collector collector;
                if (maximum_collector_sketches != 0u) {
                    if (maximum_collector_sketches >
                        static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: worker collector overflow");
                    }
                    collector.reserve(static_cast<std::size_t>(maximum_collector_sketches));
                }
                const std::uint64_t exact_collector_bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
                const std::uint64_t live_before_collect = bioseq::common::saturating_add(
                    run.admitted_bytes,
                    bioseq::common::saturating_add(exact_collector_bytes, backend_scratch_bytes));
                if (exact_collector_bytes > collector_headroom_bytes ||
                    live_before_collect > run.permit.bytes()) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: worker collector exceeded admission");
                }
                run.permit.shrink_to(live_before_collect);
                collect_core_range_pairs(range.core_lo_bp,
                                         range.core_hi_bp,
                                         collector,
                                         run.pairs,
                                         capture_scalar_build_stats ? &result.scalar_build_stats
                                                                    : nullptr);
                const std::uint64_t live_after_collect = bioseq::common::saturating_add(
                    detail::admitted_vector_capacity_bytes(run.pairs),
                    bioseq::common::saturating_add(
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(
                                collector.extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
                        backend_scratch_bytes));
                if (live_after_collect > run.permit.bytes()) {
                    index_corruption_panic("minimizers_per_sequence build: worker collection "
                                           "grew beyond admission");
                }
            }
            // The collector has been freed. Keep only the pair vector's reservation.
            if (!detail::finish_admitted_vector_run_transient(run)) {
                index_corruption_panic(
                    "minimizers_per_sequence build: worker pair collection exceeded admission");
            }
            if (backend != minimizer_sketch_backend::vulkan_segment) {
                detail::sort_pair_run(run.pairs);
            }
            detail::compact_admitted_pair_run_after_sort(run);
            if (!detail::ensure_admitted_vector_run_capacity(result.runs,
                                                             result.runs.pairs.size() + 1u)) {
                throw std::bad_alloc{};
            }
            result.runs.pairs.push_back(std::move(run));
        } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
            const std::string_view message{err.what()};
            const bool split_admission_decline =
                message.starts_with("Vulkan segment minimizer pair build returned invalid ");
            if (backend == minimizer_sketch_backend::vulkan_segment && split_admission_decline) {
                constexpr sequence_size_t k_minimum_gpu_core_bp = 256;
                if (range.core_hi_bp - range.core_lo_bp > k_minimum_gpu_core_bp) {
                    const sequence_size_t mid =
                        range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                    self(self, make_core_range(range.core_lo_bp, mid), result);
                    self(self, make_core_range(mid, range.core_hi_bp), result);
                    return;
                }
            }
            result.error = std::current_exception();
            result.error_range = range;
        } catch (const std::bad_alloc&) {
            if (backend == minimizer_sketch_backend::vulkan_segment) {
                constexpr sequence_size_t k_minimum_gpu_core_bp = 256;
                if (range.core_hi_bp - range.core_lo_bp > k_minimum_gpu_core_bp) {
                    const sequence_size_t mid =
                        range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                    self(self, make_core_range(range.core_lo_bp, mid), result);
                    self(self, make_core_range(mid, range.core_hi_bp), result);
                    return;
                }
            }
            result.error = std::current_exception();
            result.error_range = range;
        } catch (...) {
            result.error = std::current_exception();
            result.error_range = range;
        }
    };

    const std::uint64_t peak_bytes_per_fragment = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(frag_bp), static_cast<std::uint64_t>(sizeof(pair_t)));
    const std::size_t scalar_fragments_per_block = std::max<std::size_t>(
        1, run_buffer_bytes / std::max<std::uint64_t>(1, peak_bytes_per_fragment));
    // Limit GPU blocks so their local positions fit  the packed position fied.
    const std::size_t fragments_per_block =
        backend == minimizer_sketch_backend::vulkan_segment
            ? detail::gpu_fragments_per_block(scalar_fragments_per_block, frag_bp, 2u * window_span)
            : scalar_fragments_per_block;
    detail::admitted_vector_run<segment_unit_range> admitted_window_ranges;
    const std::size_t maximum_window_ranges =
        nfrag == 0u ? 0u : (nfrag + fragments_per_block - 1u) / fragments_per_block;
    if (!detail::ensure_admitted_vector_run_capacity(admitted_window_ranges,
                                                     maximum_window_ranges)) {
        index_corruption_panic("minimizers_per_sequence build: window metadata admission failed");
    }
    std::vector<segment_unit_range>& window_ranges = admitted_window_ranges.pairs;
    for (std::size_t frag_lo = 0; frag_lo < nfrag; frag_lo += fragments_per_block) {
        const std::size_t frag_hi = std::min(nfrag, frag_lo + fragments_per_block);
        const sequence_size_t core_lo = static_cast<sequence_size_t>(frag_lo) * frag_bp;
        const sequence_size_t core_hi =
            std::min(static_cast<sequence_size_t>(frag_hi) * frag_bp, total_bp);
        if (core_hi > core_lo) {
            window_ranges.push_back(make_core_range(core_lo, core_hi));
        }
    }

    /** Collect ranges on the calling thread, transferring each result to storage immediately. */
    auto collect_serial = [&window_ranges, &collect_range_admitted, &transfer_result_runs]() {
        detail::admitted_vector_run<worker_range_result> admitted_results;
        if (!detail::ensure_admitted_vector_run_capacity(admitted_results, 1u)) {
            throw std::bad_alloc{};
        }
        admitted_results.pairs.resize(1u);
        std::vector<worker_range_result>& results = admitted_results.pairs;
        for (segment_unit_range range : window_ranges) {
            results[0] = worker_range_result{};
            collect_range_admitted(collect_range_admitted, range, results[0]);
            transfer_result_runs(results);
        }
    };

    auto& pool = bioseq::parallelism::global_pool::instance();
    // Vulkan submits through one runtime queue. CPU ranges can run in parallel
    // when the sequence scanner supports sharing between workers.
    const bool may_parallel_collect =
        backend != minimizer_sketch_backend::vulkan_segment &&
        bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
        pool.maximum_concurrency() > 1 && bioseq::storage::is_worker_shareable(seq) &&
        nfrag >= bioseq::parallelism::task_parallel_minimums::k_fragment_count;

    if (!may_parallel_collect) {
        collect_serial();
    } else {
        detail::admitted_vector_run<worker_range_result> admitted_local_results;
        if (!detail::ensure_admitted_vector_run_capacity(admitted_local_results,
                                                         window_ranges.size())) {
            throw std::bad_alloc{};
        }
        admitted_local_results.pairs.resize(window_ranges.size());
        std::vector<worker_range_result>& local_results = admitted_local_results.pairs;
        try {
            bioseq::parallelism::parallel_for_pipeline(
                0,
                local_results.size(),
                1,
                bioseq::parallelism::task_parallel_minimums::k_fragment_count,
                bioseq::parallelism::task_parallel_region::fragment_build,
                [&collect_range_admitted, &local_results, &window_ranges](std::size_t lo,
                                                                          std::size_t hi) {
                    for (std::size_t rel = lo; rel < hi; ++rel) {
                        collect_range_admitted(
                            collect_range_admitted, window_ranges[rel], local_results[rel]);
                    }
                });
            transfer_result_runs(local_results);
        } catch (...) {
            release_untransferred_results_noexcept(local_results);
            throw;
        }
    }

    // Merge sorted runs into hash-range buckets and build their directory.
    detail::admitted_vector_run<direct_minimizer_bucket_directory_entry> admitted_dir;
    detail::admitted_vector_run<encoded_bucket_record> admitted_ob_buckets;
    detail::admitted_vector_run<std::uint64_t> admitted_ob_positions;
    std::vector<direct_minimizer_bucket_directory_entry>& dir = admitted_dir.pairs;
    std::vector<encoded_bucket_record>& ob_buckets = admitted_ob_buckets.pairs;
    std::vector<std::uint64_t>& ob_positions = admitted_ob_positions.pairs;
    std::uint64_t ob_lo = 0, ob_hi = 0;
    std::size_t ob_body_bytes = 0; // Bytes in the hash records and encoded positions, excluding the header.
    index_sub_key_t next_bucket_id = 0;
    std::uint64_t total_distinct = 0, total_positions = 0;

    /** Write the accumulated bucket and add its hash range to the directory. */
    auto flush_bucket = [&next_bucket_id,
                         &ob_body_bytes,
                         &storage,
                         &total_bp,
                         &ob_buckets,
                         &ob_positions,
                         &dir,
                         &ob_lo,
                         &ob_hi,
                         &admitted_dir]() {
        const index_sub_key_t sk = next_bucket_id++;
        if (sk >= k_temporary_run_sub_key_base)
            index_corruption_panic(
                "minimizers_per_sequence: bucket id collided with reserved range");
        const std::size_t framed = k_encoded_header_bytes + ob_body_bytes;
        std::array<std::size_t, 1> plan{framed};
        storage.resize_chunks(sk, std::span<const std::size_t>(plan));
        storage.template write_to_each<char>(sk,
                                             [&framed, &total_bp, &ob_buckets, &ob_positions](
                                                 std::size_t c, char* outbuf, std::size_t cap) {
                                                 DEBUG_ASSERT(c == 0 && cap >= framed,
                                                              "one bucket blob fits one chunk");
                                                 detail::serialize_direct_minimizer_bucket_image(
                                                     outbuf, total_bp, ob_buckets, ob_positions);
                                                 return c;
                                             });
        DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < ob_lo,
                     "directory not strictly ascending (flush)");
        if (!detail::ensure_admitted_vector_run_capacity(admitted_dir, dir.size() + 1u)) {
            index_corruption_panic("minimizers_per_sequence: directory admission failed");
        }
        dir.push_back({ob_lo, ob_hi, sk});
        ob_buckets.clear();
        ob_positions.clear();
        ob_body_bytes = 0;
        ob_lo = 0;
        ob_hi = 0;
    };

    storage.merge_runs([&total_distinct,
                        &total_positions,
                        &storage,
                        &next_bucket_id,
                        &flush_bucket,
                        &total_bp,
                        &admitted_dir,
                        &admitted_ob_positions,
                        &admitted_ob_buckets,
                        &dir,
                        &ob_positions,
                        &ob_buckets,
                        &ob_body_bytes,
                        &ob_lo,
                        &ob_hi](std::uint64_t kmer_key, auto&& positions) {
        const std::uint64_t pcount = positions.total_count();
        if (pcount > std::numeric_limits<std::uint32_t>::max())
            index_corruption_panic("merge_runs: kmer count > u32 (encoded_bucket.pos_count)");
        ++total_distinct;
        total_positions += pcount;
        const std::size_t kmer_framed =
            k_encoded_header_bytes + k_encoded_bucket_bytes * 1 + sizeof(std::uint64_t) * pcount;
        if (kmer_framed > k_bucket_target_bytes) {
            // Store this frequent hash across several chunks under one bucket  key.
            if (!ob_buckets.empty())
                flush_bucket();
            const index_sub_key_t sk = next_bucket_id++;
            if (sk >= k_temporary_run_sub_key_base)
                index_corruption_panic(
                    "minimizers_per_sequence: bucket id collided with reserved range");
            detail::admitted_vector_run<std::uint64_t> admitted_kpos;
            if (!detail::ensure_admitted_vector_run_capacity(admitted_kpos,
                                                             static_cast<std::size_t>(pcount))) {
                index_corruption_panic("minimizers_per_sequence: heavy positions admission failed");
            }
            std::vector<std::uint64_t>& kpos = admitted_kpos.pairs;
            positions.for_each_batch([&kpos](std::span<const std::uint64_t> b) {
                if (b.size() > (std::numeric_limits<std::size_t>::max)() - kpos.size()) {
                    index_corruption_panic("minimizers_per_sequence: heavy positions overflow");
                }
                kpos.insert(kpos.end(), b.begin(), b.end());
            });
            const std::size_t per =
                (k_bucket_target_bytes - k_encoded_header_bytes - k_encoded_bucket_bytes) /
                sizeof(std::uint64_t);
            const std::size_t plan_count = (static_cast<std::size_t>(pcount) + per - 1u) / per;
            detail::admitted_vector_run<std::size_t> admitted_plan;
            if (!detail::ensure_admitted_vector_run_capacity(admitted_plan, plan_count)) {
                index_corruption_panic("minimizers_per_sequence: heavy plan admission failed");
            }
            std::vector<std::size_t>& plan = admitted_plan.pairs;
            for (std::size_t off = 0; off < static_cast<std::size_t>(pcount); off += per) {
                plan.push_back(
                    k_encoded_header_bytes + k_encoded_bucket_bytes +
                    sizeof(std::uint64_t) *
                        std::min<std::size_t>(per, static_cast<std::size_t>(pcount) - off));
            }
            storage.resize_chunks(sk, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(
                sk,
                [&per, &pcount, &kmer_key, &total_bp, &kpos](
                    std::size_t c, char* outbuf, std::size_t) {
                    const std::size_t start = c * per;
                    const std::size_t cnt =
                        std::min<std::size_t>(per, static_cast<std::size_t>(pcount) - start);
                    encoded_bucket_record rec{kmer_key, 0u, static_cast<std::uint32_t>(cnt)};
                    detail::serialize_direct_minimizer_bucket_image(
                        outbuf,
                        total_bp,
                        std::span<const encoded_bucket_record>(&rec, 1),
                        std::span<const std::uint64_t>(kpos.data() + start, cnt));
                    return c;
                });
            DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < kmer_key,
                         "directory not strictly ascending (heavy-hitter)");
            if (!detail::ensure_admitted_vector_run_capacity(admitted_dir, dir.size() + 1u)) {
                index_corruption_panic("minimizers_per_sequence: directory admission failed");
            }
            dir.push_back({kmer_key, kmer_key, sk});
        } else {
            const std::size_t add = k_encoded_bucket_bytes + sizeof(std::uint64_t) * pcount;
            if (k_encoded_header_bytes + ob_body_bytes + add > k_bucket_target_bytes &&
                !ob_buckets.empty())
                flush_bucket();
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_ob_positions,
                    ob_positions.size() + static_cast<std::size_t>(pcount)) ||
                !detail::ensure_admitted_vector_run_capacity(admitted_ob_buckets,
                                                             ob_buckets.size() + 1u)) {
                index_corruption_panic("minimizers_per_sequence: bucket assembly admission failed");
            }
            const std::uint32_t off = static_cast<std::uint32_t>(ob_positions.size());
            positions.for_each_batch([&ob_positions](std::span<const std::uint64_t> b) {
                ob_positions.insert(ob_positions.end(), b.begin(), b.end());
            });
            ob_buckets.push_back(
                encoded_bucket_record{kmer_key, off, static_cast<std::uint32_t>(pcount)});
            ob_hi = kmer_key;
            if (ob_buckets.size() == 1)
                ob_lo = kmer_key;
            ob_body_bytes += add;
        }
    });
    if (!ob_buckets.empty())
        flush_bucket();

    // Write the directory after every bucket has been stored.
    {
        const std::size_t dir_bytes = sizeof(direct_minimizer_bucket_directory) +
                                      sizeof(direct_minimizer_bucket_directory_entry) * dir.size();
        std::array<std::size_t, 1> plan{dir_bytes};
        storage.resize_chunks(k_directory_sub_key, std::span<const std::size_t>(plan));
        storage.template write_to_each<char>(
            k_directory_sub_key,
            [&dir, &total_distinct, &total_positions, &backend](
                std::size_t c, char* outbuf, std::size_t) {
                direct_minimizer_bucket_directory h;
                h.magic = direct_minimizer_bucket_directory::k_direct_minimizer_bucket_magic;
                h.layout_revision =
                    direct_minimizer_bucket_directory::k_direct_minimizer_bucket_layout_revision;
                h.flags = backend == minimizer_sketch_backend::vulkan_segment &&
                                  !std::same_as<p_alphabet, ::seq::dna4>
                              ? direct_minimizer_bucket_directory::k_gpu_built
                              : 0u;
                h.num_buckets = dir.size();
                h.total_distinct_kmers = total_distinct;
                h.total_positions = total_positions;
                std::memcpy(outbuf, &h, sizeof(h));
                if (!dir.empty())
                    std::memcpy(outbuf + sizeof(h),
                                dir.data(),
                                sizeof(direct_minimizer_bucket_directory_entry) * dir.size());
                return c;
            });
    }

    // Save spill statistics when supported, then release the temporary runs.
    if constexpr (requires(p_storage& s) { s.snapshot_sorted_run_stats_before_release(); }) {
        storage.snapshot_sorted_run_stats_before_release();
    }
    storage.release_runs();
    if (capture_scalar_build_stats) {
        scalar_build_stats.record_completed_build();
        *scalar_build_stats_out = scalar_build_stats;
    }
}

/** Build the index without returning scalar collection statistics. */
template <class p_storage>
template <sequence_alphabet p_alphabet>
inline void direct_minimizer_bucket_index<p_storage>::build(
    p_storage& storage,
    bioseq::storage::lazy_sequence_view<p_alphabet> seq,
    minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels,
    const std::uint8_t alphabet_code) {
    build(storage, seq, backend, kernels, nullptr, alphabet_code);
}

// Use the scalar backend when none is specified.
/** Build with scalar sketching and the default alphabet argument. */
template <class p_storage>
template <sequence_alphabet p_alphabet>
inline void direct_minimizer_bucket_index<p_storage>::build(
    p_storage& storage, bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
    build(storage, seq, minimizer_sketch_backend::scalar, nullptr);
}

/** Check required SIMD functions, clear this entity's saved index, then build it again. */
template <class p_storage>
template <sequence_alphabet p_alphabet>
inline void direct_minimizer_bucket_index<p_storage>::rebuild(
    p_storage& storage,
    bioseq::storage::lazy_sequence_view<p_alphabet> seq,
    minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels,
    std::uint8_t alphabet_code) {
    if (backend == minimizer_sketch_backend::exact_simd &&
        (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
        throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_state,
                                             "minimizers_per_sequence rebuild"};
    }
    if (backend == minimizer_sketch_backend::bounded_nthash_simd &&
        (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
        throw packed_bounded_nthash_index_runtime_error{
            packed_bounded_nthash_index_status::invalid_state, "minimizers_per_sequence rebuild"};
    }
    storage.clear_entity();
    build(storage, seq, backend, kernels, nullptr, alphabet_code);
}

/** Clear and rebuild the index with scalar  sketching. */
template <class p_storage>
template <sequence_alphabet p_alphabet>
inline void direct_minimizer_bucket_index<p_storage>::rebuild(
    p_storage& storage, bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
    rebuild(storage, seq, minimizer_sketch_backend::scalar, nullptr);
}

} // namespace bioseq::indexing
