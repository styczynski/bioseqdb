/**
 * @file minimizer_sequence_index/read_assembly/build.cppm
 * @brief Builds ordinary exact minimizer buckets using scalar or SIMD sketching.
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
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_sequence:read_assembly_build;

import bioseq.sequences.kmers.hashers.base;
import :read_assembly_contract;
import bioseq.indexing.minimizer_sequence.admitted_runs;
import bioseq.indexing.minimizer_sequence.bucket_codec;
import bioseq.indexing.minimizer_sequence.generation;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
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
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

export namespace bioseq::indexing {

/**
 * Build a saved minimizer index with the alphabet-derived ordinary exact profile.
 * Collect global hash/position pairs into sorted runs, merge them into buckets,
 * then write the directory  after all buckets are stored. Memory pressure can
 * split collection cores and transfer completed waves on the calling thread.
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
    alphabet_code = ordinary_minimizer_index_alphabet_code<p_alphabet>(alphabet_code);
    if (!ordinary_minimizer_index_backend_supported(backend)) {
        throw exact_simd_index_runtime_error{exact_simd_index_status::unsupported_representation,
            "ordinary minimizer index requires the exact scheme; use identity-bearing generation"};
    }
    constexpr std::size_t w = k_ordinary_minimizer_index_window_size;
        constexpr std::size_t k = ordinary_minimizer_index_kmer_size(
            bioseq::minimizers::minimizer_exact_rank_bits<p_alphabet>);
    constexpr std::size_t frag_bp = 32'768u;
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

    if (scalar_build_stats_out != nullptr) {
        *scalar_build_stats_out = {};
    }
    const bool capture_scalar_build_stats = scalar_build_stats_out != nullptr;
    direct_minimizer_bucket_build_stats scalar_build_stats{};
    if (capture_scalar_build_stats) {
        scalar_build_stats.bind_backend(backend, 1u, 1u);
    }

    const sequence_size_t total_bp = seq.bp_size();
    const std::size_t nfrag = static_cast<std::size_t>(total_bp / frag_bp + (total_bp % frag_bp != 0u));

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
    // Complete and transfer only one admitted wave at a time. Store metadata is
    // prepared before payload permits, including slots to spill every resident
    // run without allocating another file-run entry under a full budget.
    struct collection_admission_error final : std::bad_alloc {
        const char* stage;
        explicit collection_admission_error(const char* value) noexcept : stage(value) {}
        const char* what() const noexcept override { return stage; }
    };
    struct worker_range_result {
        admitted_pair_run run;
        segment_unit_range range{};
        direct_minimizer_bucket_build_stats scalar_build_stats{};
        std::exception_ptr error;
        std::uint64_t distinct_hashes = 0u;
    };

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto demote_for_budget_refusal = [&storage](
        const bioseq::parallelism::memory_admission_failure& failure,
        std::uint64_t bytes) {
        if (failure.reason !=
            bioseq::parallelism::memory_admission_failure_reason::budget_exhausted) {
            return false;
        }
        if constexpr (requires(p_storage& s, std::uint64_t needed) {
                          s.demote_resident_runs_until_available(needed);
                      }) {
            storage.demote_resident_runs_until_available(bytes);
        }
        return true;
    };

    /** Collect global hash/position pairs whose positions fall inside this core. */
    auto collect_core_range_pairs =
        [&seq, backend, kernels, alphabet_code](segment_unit_range range,
                                                std::vector<pair_t>& out,
                                                direct_minimizer_bucket_build_stats* scalar_stats) {
            // Ordinary backends emit pairs directly; the bounded-only collector stays empty.
            sketch_collector collector;
            const auto collected = collect_generation_core_run(seq,
                                             alphabet_code,
                                             backend,
                                             kernels,
                                             range,
                                             w,
                                             k,
                                             nullptr,
                                             collector,
                                             out,
                                             nullptr,
                                             true,
                                             scalar_stats);
            if (!collected) {
                if (collected.error() == minimizer_generation_encoding_error::admission) {
                    throw collection_admission_error{
                        "minimizers_per_sequence build: input materialization admission failed"};
                }
                index_corruption_panic(
                    "minimizers_per_sequence build: collection/core geometry failed");
            }
        };

    std::uint64_t collected_pair_count = 0u;
    std::uint64_t collected_distinct_upper = 0u;
    {
        auto& pool = bioseq::parallelism::global_pool::instance();
        const bool may_parallel_collect =
            bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
            pool.maximum_concurrency() > 1 && bioseq::storage::is_worker_shareable(seq) &&
            nfrag >= bioseq::parallelism::task_parallel_minimums::k_fragment_count;
        unsigned maximum_fanout = may_parallel_collect
            ? static_cast<unsigned>(std::max(1u, pool.maximum_concurrency())) : 1u;
        maximum_fanout = static_cast<unsigned>(std::min<std::uint64_t>(
            maximum_fanout, std::max<std::uint64_t>(1u, nfrag)));
        detail::admitted_vector_run<worker_range_result> admitted_results;
        if (total_bp != 0u) {
            for (;;) {
                const auto bytes =
                    detail::admitted_vector_preallocation_bytes<worker_range_result>(maximum_fanout);
                bioseq::parallelism::memory_admission_failure failure{};
                auto permit = admission.try_reserve(bytes, &failure);
                if (permit) {
                    admitted_results.permit = std::move(*permit);
                    if (!detail::reserve_admitted_vector_run_capacity(
                            admitted_results, maximum_fanout)) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: wave metadata exceeded admission");
                    }
                    break;
                }
                if (failure.reason !=
                        bioseq::parallelism::memory_admission_failure_reason::budget_exhausted ||
                    maximum_fanout == 1u) {
                    throw collection_admission_error{
                        "minimizers_per_sequence build: wave metadata admission failed"};
                }
                maximum_fanout = std::max(1u, maximum_fanout / 2u);
            }
        }

        const std::uint64_t bytes_per_fragment = bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(frag_bp), static_cast<std::uint64_t>(sizeof(pair_t)));
        const std::uint64_t fragments_per_block = std::max<std::uint64_t>(
            1u, run_buffer_bytes / std::max<std::uint64_t>(1u, bytes_per_fragment));
        sequence_size_t core_limit = std::min<std::uint64_t>(
            total_bp, bioseq::common::saturating_mul(fragments_per_block, frag_bp));
        constexpr sequence_size_t minimum_core_bp = 4096u;
        sequence_size_t next_core = 0u;
        while (next_core < total_bp) {
            const std::uint64_t remaining = total_bp - next_core;
            core_limit = std::min<sequence_size_t>(core_limit, remaining);
            const auto requested = static_cast<unsigned>(std::min<std::uint64_t>(
                maximum_fanout, remaining / core_limit + (remaining % core_limit != 0u)));
            unsigned prepared_fanout = requested;
            while (!storage.try_prepare_sorted_run_append(prepared_fanout)) {
                if (prepared_fanout == 1u) {
                    throw collection_admission_error{
                        "minimizers_per_sequence build: spill metadata admission failed"};
                }
                prepared_fanout = std::max(1u, prepared_fanout / 2u);
            }
            // First-window ties can repeat a core endpoint before deduplication.
            // Ordinary scalar/two-stack raw output is bounded by the complete
            // collection span, including the context on both sides of the core.
            constexpr std::uint64_t collection_halo_bp = w + k - 2u;
            const std::uint64_t maximum_collection_bp = std::min<std::uint64_t>(
                total_bp, bioseq::common::saturating_add(
                    core_limit, 2u * collection_halo_bp));
            const std::uint64_t per_task_bytes =
                detail::admitted_vector_preallocation_bytes<pair_t>(maximum_collection_bp);
            auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
                prepared_fanout, per_task_bytes);
            if (!wave.is_admitted) {
                // The wave API does not expose a refusal reason. A one-item
                // attempt identifies budget pressure without retrying PG/IO or
                // allocator exceptions as if they were admission refusals.
                bioseq::parallelism::memory_admission_failure failure{};
                auto single = admission.try_reserve(per_task_bytes, &failure);
                if (single) {
                    wave = {1u, std::move(*single), true};
                } else {
                    if (!demote_for_budget_refusal(failure, per_task_bytes)) {
                        throw collection_admission_error{
                            "minimizers_per_sequence build: collection memory pressure"};
                    }
                    wave = bioseq::parallelism::try_reserve_budgeted_wave(
                        prepared_fanout, per_task_bytes);
                    if (!wave.is_admitted) {
                        single = admission.try_reserve(per_task_bytes, &failure);
                        if (single) {
                            wave = {1u, std::move(*single), true};
                        } else if (failure.reason !=
                                   bioseq::parallelism::memory_admission_failure_reason::budget_exhausted) {
                            throw collection_admission_error{
                                "minimizers_per_sequence build: collection memory pressure"};
                        }
                    }
                    if (!wave.is_admitted) {
                        if (core_limit <= minimum_core_bp) {
                            throw collection_admission_error{
                                "minimizers_per_sequence build: minimum core admission failed"};
                        }
                        core_limit = std::max(minimum_core_bp, core_limit / 2u);
                        continue;
                    }
                }
            }

            auto& results = admitted_results.pairs;
            results.resize(wave.fanout);
            for (auto& result : results) {
                const sequence_size_t core_end = next_core +
                    std::min<sequence_size_t>(core_limit, total_bp - next_core);
                result.range = make_core_range(next_core, core_end);
                result.run.permit = wave.permit.split_off(per_task_bytes);
                if (!detail::reserve_admitted_vector_run_capacity(
                        result.run, result.range.ce_u - result.range.cs_u)) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: wave pair capacity exceeded admission");
                }
                next_core = core_end;
            }
            const auto collect_wave = [&results, &collect_core_range_pairs,
                                       capture_scalar_build_stats, backend](std::size_t lo,
                                                                           std::size_t hi) {
                for (std::size_t index = lo; index < hi; ++index) {
                    auto& result = results[index];
                    try {
                        collect_core_range_pairs(result.range, result.run.pairs,
                            capture_scalar_build_stats ? &result.scalar_build_stats : nullptr);
                        if (!detail::finish_admitted_vector_run_transient(result.run)) {
                            index_corruption_panic(
                                "minimizers_per_sequence build: wave collection exceeded admission");
                        }
                        if (backend != minimizer_sketch_backend::vulkan_segment) {
                            detail::sort_pair_run(result.run.pairs);
                        }
                        detail::compact_admitted_pair_run_after_sort(result.run);
                        // The sum of per-run distinct counts bounds global
                        // distinct hashes without retaining another key set.
                        for (std::size_t pair_index = 0u; pair_index < result.run.pairs.size(); ++pair_index) {
                            if (pair_index == 0u || result.run.pairs[pair_index - 1u].first !=
                                                       result.run.pairs[pair_index].first) {
                                ++result.distinct_hashes;
                            }
                        }
                    } catch (...) {
                        result.error = std::current_exception();
                    }
                }
            };
            if (may_parallel_collect) {
                // The whole input already passed the fragment-count threshold.
                // Two- and three-item admitted waves retain parallel execution.
                bioseq::parallelism::parallel_for_pipeline(
                    0u, results.size(), 1u, 1u,
                    bioseq::parallelism::task_parallel_region::fragment_build, collect_wave);
            } else {
                collect_wave(0u, results.size());
            }
            for (const auto& result : results) {
                if (result.error) {
                    std::rethrow_exception(result.error);
                }
            }
            for (auto& result : results) {
                if (capture_scalar_build_stats) {
                    scalar_build_stats.merge(result.scalar_build_stats);
                }
                if (result.run.pairs.size() >
                    (std::numeric_limits<std::uint64_t>::max)() - collected_pair_count) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: collected pair count overflow");
                }
                collected_pair_count += result.run.pairs.size();
                if (result.distinct_hashes >
                    (std::numeric_limits<std::uint64_t>::max)() - collected_distinct_upper) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: distinct hash bound overflow");
                }
                collected_distinct_upper += result.distinct_hashes;
                if (!result.run.pairs.empty()) {
                    storage.spill_sorted_run_owned_admitted(
                        std::move(result.run.pairs), result.run.admitted_bytes,
                        std::move(result.run.permit));
                    result.run.admitted_bytes = 0u;
                }
            }
            results.clear();
        }
    } // Release wave metadata before preparing merge/output owners.

    // Merge sorted runs into hash-range buckets and build their directory.
    detail::admitted_vector_run<direct_minimizer_bucket_directory_entry> admitted_dir;
    detail::admitted_vector_run<encoded_bucket_record> admitted_ob_buckets;
    detail::admitted_vector_run<std::uint64_t> admitted_ob_positions;
    // Admit the complete output preparation before allocating any array. Reduce
    // only the aggregation frame under budget pressure; a large single hash can
    // stream directly into a storage chunk. All demotions precede merge readers.
    const auto prepare_output = [](auto& owner, std::uint64_t count,
                                   bioseq::parallelism::memory_permit& combined_permit) {
        if (count == 0u) {
            return;
        }
        using value_type = typename std::remove_reference_t<decltype(owner.pairs)>::value_type;
        const auto bytes = detail::admitted_vector_preallocation_bytes<value_type>(count);
        owner.permit = combined_permit.split_off(bytes);
        if (!detail::reserve_admitted_vector_run_capacity(owner, count)) {
            index_corruption_panic(
                "minimizers_per_sequence build: output capacity exceeded admission");
        }
    };
    constexpr std::uint64_t body_per_position = k_encoded_bucket_bytes + sizeof(std::uint64_t);
    const auto body_bound = bioseq::common::saturating_add(
        bioseq::common::saturating_mul(collected_distinct_upper, k_encoded_bucket_bytes),
        bioseq::common::saturating_mul(collected_pair_count, sizeof(std::uint64_t)));
    if (body_bound == (std::numeric_limits<std::uint64_t>::max)()) {
        throw collection_admission_error{
            "minimizers_per_sequence build: output size bound overflow"};
    }
    std::uint64_t bucket_body_capacity = k_bucket_target_bytes - k_encoded_header_bytes;
    while (body_bound != 0u) {
        const auto body_frames = body_bound / bucket_body_capacity +
            (body_bound % bucket_body_capacity != 0u);
        // Adjacent normal entries together exceed one frame; a streamed or
        // heavy singleton exceeds one alone. This also bounds their boundaries.
        const auto directory_bound = std::min(collected_distinct_upper,
            bioseq::common::saturating_add(bioseq::common::saturating_mul(body_frames, 2u), 1u));
        const auto position_bound = std::min(
            collected_pair_count, bucket_body_capacity / sizeof(std::uint64_t));
        const auto record_bound = std::min(
            collected_distinct_upper, bucket_body_capacity / body_per_position);
        const auto preparation_bytes = bioseq::common::saturating_add(
            detail::admitted_vector_preallocation_bytes<direct_minimizer_bucket_directory_entry>(
                directory_bound),
            bioseq::common::saturating_add(
                detail::admitted_vector_preallocation_bytes<std::uint64_t>(position_bound),
                detail::admitted_vector_preallocation_bytes<encoded_bucket_record>(record_bound)));
        if (preparation_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw collection_admission_error{
                "minimizers_per_sequence build: output preparation size overflow"};
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = admission.try_reserve(preparation_bytes, &failure);
        if (!permit && demote_for_budget_refusal(failure, preparation_bytes)) {
            permit = admission.try_reserve(preparation_bytes, &failure);
        }
        if (permit) {
            prepare_output(admitted_dir, directory_bound, *permit);
            prepare_output(admitted_ob_positions, position_bound, *permit);
            prepare_output(admitted_ob_buckets, record_bound, *permit);
            break;
        }
        if (failure.reason !=
                bioseq::parallelism::memory_admission_failure_reason::budget_exhausted ||
            bucket_body_capacity == body_per_position) {
            throw collection_admission_error{
                "minimizers_per_sequence build: output preparation admission failed"};
        }
        bucket_body_capacity = std::max(body_per_position, bucket_body_capacity / 2u);
    }
    const std::size_t normal_bucket_target_bytes =
        k_encoded_header_bytes + static_cast<std::size_t>(bucket_body_capacity);
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
                        &ob_hi,
                        normal_bucket_target_bytes](std::uint64_t kmer_key, auto&& positions) {
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
            // A heavy hash owns its existing full position vector. Do not
            // retain idle conservative normal-bucket scratch beside it. Later
            // normal hashes use the existing actual-needed growth below.
            admitted_ob_positions.release_capacity_before_permit();
            admitted_ob_buckets.release_capacity_before_permit();
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
        } else if (kmer_framed > normal_bucket_target_bytes) {
            // One hash exceeds the admitted aggregation frame but still fits
            // the existing storage chunk limit. Stream it once into that chunk
            // while preserving normal scratch for subsequent smaller hashes.
            if (!ob_buckets.empty()) {
                flush_bucket();
            }
            const index_sub_key_t sk = next_bucket_id++;
            if (sk >= k_temporary_run_sub_key_base) {
                index_corruption_panic(
                    "minimizers_per_sequence: bucket id collided with reserved range");
            }
            const std::array<std::size_t, 1> plan{kmer_framed};
            storage.resize_chunks(sk, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(sk,
                [&positions, total_bp, kmer_key, pcount, kmer_framed](
                    std::size_t chunk_no, char* output, std::size_t capacity) {
                    if (chunk_no != 0u || capacity < kmer_framed) {
                        index_corruption_panic(
                            "minimizers_per_sequence: singleton chunk geometry mismatch");
                    }
                    const std::array<encoded_bucket_record, 1> records{
                        encoded_bucket_record{kmer_key, 0u, static_cast<std::uint32_t>(pcount)}};
                    char* position_destination = detail::serialize_direct_minimizer_bucket_prefix(
                        output, total_bp, records, pcount);
                    std::uint64_t written = 0u;
                    positions.for_each_batch([position_destination, pcount, &written](
                        std::span<const std::uint64_t> batch) {
                        if (batch.size() > pcount - written) {
                            index_corruption_panic(
                                "minimizers_per_sequence: singleton positions exceed count");
                        }
                        if (!batch.empty()) {
                            std::memcpy(position_destination + written * sizeof(std::uint64_t),
                                        batch.data(), batch.size_bytes());
                        }
                        written += batch.size();
                    });
                    if (written != pcount) {
                        index_corruption_panic(
                            "minimizers_per_sequence: singleton positions are incomplete");
                    }
                    return chunk_no;
                });
            DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < kmer_key,
                         "directory not strictly ascending (streamed singleton)");
            if (!detail::ensure_admitted_vector_run_capacity(admitted_dir, dir.size() + 1u)) {
                index_corruption_panic("minimizers_per_sequence: directory admission failed");
            }
            dir.push_back({kmer_key, kmer_key, sk});
        } else {
            const std::size_t add = k_encoded_bucket_bytes + sizeof(std::uint64_t) * pcount;
            if (k_encoded_header_bytes + ob_body_bytes + add > normal_bucket_target_bytes &&
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
            [&dir, &total_distinct, &total_positions](
                std::size_t c, char* outbuf, std::size_t) {
                direct_minimizer_bucket_directory h;
                h.magic = direct_minimizer_bucket_directory::k_direct_minimizer_bucket_magic;
                h.layout_revision =
                    direct_minimizer_bucket_directory::k_direct_minimizer_bucket_layout_revision;
                h.flags = 0u;
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
    alphabet_code = ordinary_minimizer_index_alphabet_code<p_alphabet>(alphabet_code);
    if (backend == minimizer_sketch_backend::exact_simd &&
        (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
        throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_state,
                                             "minimizers_per_sequence rebuild"};
    }
    if (!ordinary_minimizer_index_backend_supported(backend)) {
        throw exact_simd_index_runtime_error{exact_simd_index_status::unsupported_representation,
            "ordinary minimizer rebuild requires the exact scheme"};
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
