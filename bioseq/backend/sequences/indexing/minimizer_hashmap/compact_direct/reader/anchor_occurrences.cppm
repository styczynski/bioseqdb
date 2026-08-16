/**
 * @file compact_direct/reader/anchor_occurrences.cppm
 * @brief Materialization and hash ordering of query minimizer occurrences.
 */

module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:compact_direct.reader_anchor_occurrences;

import :compact_direct.reader_contract;
import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
import :compact_direct.query_contracts;
import :compact_direct.planning_admission;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export namespace bioseq::indexing {

template <bool p_oracle, bool p_compact_direct>
[[gnu::always_inline]] inline void
prepared_minimizer_fragment_reader::prepare_query_occurrences_(
    std::span<const fragment_seed_query> queries,
    const std::expected<query_occurrence_packing_layout,
                        minimizer_generation_encoding_error>& occurrence_layout,
    std::size_t occurrence_count,
    query_wave_scratch& scratch,
    prepared_fragment_anchor_batch& batch) const {
    using bioseq::common::saturating_mul;
    constexpr bool use_bucket_direct_matcher = !p_oracle && !p_compact_direct;

    if (use_bucket_direct_matcher && occurrence_count != 0u) {
        const std::uint64_t query_radix_fill_started_ns = stage_now_ns_();
        const std::size_t radix_size = static_cast<std::size_t>(query_radix_bucket_count_) + 1u;
        const std::size_t old_query_radix_capacity = scratch.query_plan_radix_.capacity();
        scratch.ensure_vector_capacity_(scratch.query_plan_radix_, radix_size);
        batch.stats_.query_radix_allocations =
            scratch.query_plan_radix_.capacity() > old_query_radix_capacity ? 1u : 0u;
        batch.stats_.peak_query_radix_capacity_bytes =
            saturating_mul(scratch.query_plan_radix_.capacity(), sizeof(std::uint32_t));
        scratch.query_plan_radix_.assign(radix_size, 0u);
        for (const fragment_seed_query& query : queries) {
            for (const minimap_sketch_data& minimizer : query.minimizers) {
                const std::uint32_t bucket =
                    query_bucket_(static_cast<std::uint32_t>(minimizer.kmer()));
                ++scratch.query_plan_radix_[bucket + 1u];
            }
        }
        for (std::size_t bucket = 1u; bucket < radix_size; ++bucket) {
            if (scratch.query_plan_radix_[bucket] >
                (std::numeric_limits<std::uint32_t>::max)() -
                    scratch.query_plan_radix_[bucket - 1u]) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence bucket prefix overflow");
            }
            scratch.query_plan_radix_[bucket] += scratch.query_plan_radix_[bucket - 1u];
        }
        if (scratch.query_plan_radix_.back() != occurrence_count) {
            throw persisted_minimizer_lookup_error(
                "fragment-native occurrence bucket count disagrees");
        }
        batch.stats_.query_plan_radix_fill_ns = elapsed_stage_ns_(query_radix_fill_started_ns);
    }

    const std::uint64_t occurrence_fill_started_ns = stage_now_ns_();
    std::size_t occurrence_cursor = 0u;
    if (occurrence_layout->packed()) {
        const std::size_t old_occurrence_capacity = scratch.packed_occurrences_.capacity();
        scratch.ensure_vector_capacity_(scratch.packed_occurrences_, occurrence_count);
        batch.stats_.occurrence_allocations =
            scratch.packed_occurrences_.capacity() > old_occurrence_capacity ? 1u : 0u;
        scratch.packed_occurrences_.resize(occurrence_count);
        const auto write_packed_occurrence =
            [&scratch, occurrence_layout](std::size_t destination,
                                          std::size_t query_ordinal,
                                          std::size_t minimizer_ordinal,
                                          const minimap_sketch_data& minimizer) {
                const std::uint32_t encoded = encode_packed_query_occurrence_trusted(
                    *occurrence_layout,
                    static_cast<std::uint32_t>(query_ordinal),
                    static_cast<std::uint32_t>(minimizer_ordinal));
#ifndef NDEBUG
                const query_occurrence_reference decoded =
                    decode_packed_query_occurrence_trusted(*occurrence_layout, encoded);
                DEBUG_ASSERT(decoded.batch_query_ordinal == query_ordinal &&
                                 decoded.query_ordinal == minimizer_ordinal,
                             "packed query occurrence round-trip");
#endif
                scratch.packed_occurrences_[destination] = packed_query_occurrence{
                    .hash = static_cast<std::uint32_t>(minimizer.kmer()),
                    .query_and_ordinal = encoded,
                };
            };
        if constexpr (p_oracle || p_compact_direct) {
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                 ++query_ordinal) {
                const auto minimizers = queries[query_ordinal].minimizers;
                for (std::size_t minimizer_ordinal = 0u; minimizer_ordinal < minimizers.size();
                     ++minimizer_ordinal) {
                    write_packed_occurrence(occurrence_cursor++,
                                            query_ordinal,
                                            minimizer_ordinal,
                                            minimizers[minimizer_ordinal]);
                }
            }
        } else {
            for (std::size_t query_ordinal = queries.size(); query_ordinal-- > 0u;) {
                const auto minimizers = queries[query_ordinal].minimizers;
                for (std::size_t minimizer_ordinal = minimizers.size();
                     minimizer_ordinal-- > 0u;) {
                    const std::uint32_t hash =
                        static_cast<std::uint32_t>(minimizers[minimizer_ordinal].kmer());
                    const std::uint32_t bucket = query_bucket_(hash);
                    const std::size_t destination = --scratch.query_plan_radix_[bucket + 1u];
                    write_packed_occurrence(destination,
                                            query_ordinal,
                                            minimizer_ordinal,
                                            minimizers[minimizer_ordinal]);
                    ++occurrence_cursor;
                }
            }
        }
    } else {
        const std::size_t old_occurrence_capacity = scratch.wide_occurrences_.capacity();
        scratch.ensure_vector_capacity_(scratch.wide_occurrences_, occurrence_count);
        batch.stats_.occurrence_allocations =
            scratch.wide_occurrences_.capacity() > old_occurrence_capacity ? 1u : 0u;
        scratch.wide_occurrences_.resize(occurrence_count);
        const auto write_wide_occurrence = [&scratch](std::size_t destination,
                                                      std::size_t query_ordinal,
                                                      std::size_t minimizer_ordinal,
                                                      const minimap_sketch_data& minimizer) {
            scratch.wide_occurrences_[destination] = wide_query_occurrence{
                .hash = static_cast<std::uint32_t>(minimizer.kmer()),
                .batch_query_ordinal = static_cast<std::uint32_t>(query_ordinal),
                .query_ordinal = static_cast<std::uint32_t>(minimizer_ordinal),
            };
        };
        if constexpr (p_oracle || p_compact_direct) {
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                 ++query_ordinal) {
                const auto minimizers = queries[query_ordinal].minimizers;
                for (std::size_t minimizer_ordinal = 0u; minimizer_ordinal < minimizers.size();
                     ++minimizer_ordinal) {
                    write_wide_occurrence(occurrence_cursor++,
                                          query_ordinal,
                                          minimizer_ordinal,
                                          minimizers[minimizer_ordinal]);
                }
            }
        } else {
            for (std::size_t query_ordinal = queries.size(); query_ordinal-- > 0u;) {
                const auto minimizers = queries[query_ordinal].minimizers;
                for (std::size_t minimizer_ordinal = minimizers.size();
                     minimizer_ordinal-- > 0u;) {
                    const std::uint32_t hash =
                        static_cast<std::uint32_t>(minimizers[minimizer_ordinal].kmer());
                    const std::uint32_t bucket = query_bucket_(hash);
                    const std::size_t destination = --scratch.query_plan_radix_[bucket + 1u];
                    write_wide_occurrence(destination,
                                          query_ordinal,
                                          minimizer_ordinal,
                                          minimizers[minimizer_ordinal]);
                    ++occurrence_cursor;
                }
            }
        }
    }
    if (occurrence_cursor != occurrence_count) {
        throw persisted_minimizer_lookup_error(
            "fragment-native query occurrence count disagrees");
    }
    if (use_bucket_direct_matcher && occurrence_count != 0u) {
        for (std::size_t bucket = 0u; bucket < query_radix_bucket_count_; ++bucket) {
            scratch.query_plan_radix_[bucket] = scratch.query_plan_radix_[bucket + 1u];
        }
        scratch.query_plan_radix_[query_radix_bucket_count_] =
            static_cast<std::uint32_t>(occurrence_count);
    }
    batch.stats_.peak_occurrence_capacity_bytes = scratch.retained_occurrence_capacity_bytes_();
    batch.stats_.occurrence_fill_ns = elapsed_stage_ns_(occurrence_fill_started_ns);

    const std::uint64_t occurrence_radix_started_ns = stage_now_ns_();
    if constexpr (p_oracle || p_compact_direct) {
        if (occurrence_count >= 2u) {
            scratch.occurrence_radix_invoked_ = true;
            const auto key = [](const auto& occurrence) noexcept -> std::uint32_t {
                return occurrence.hash;
            };
            if constexpr (p_compact_direct) {
                if (occurrence_layout->packed() &&
                    occurrence_count >=
                        k_compact_direct_parallel_packed_occurrence_radix_min_count) {
                    const auto bucket_bounds =
                        partition_packed_occurrence_top_byte_(scratch.packed_occurrences_);
                    packed_query_occurrence* const occurrences =
                        scratch.packed_occurrences_.data();
                    scratch.parallel_occurrence_radix_split_selected_ = true;
                    parallel_entries_(
                        bucket_bounds.size() - 1u,
                        [occurrences, &bucket_bounds, key](std::size_t bucket_begin,
                                                           std::size_t bucket_end) noexcept {
                            for (std::size_t bucket = bucket_begin; bucket < bucket_end;
                                 ++bucket) {
                                const std::size_t begin = bucket_bounds[bucket];
                                const std::size_t end = bucket_bounds[bucket + 1u];
                                if (end - begin < 2u)
                                    continue;
                                bioseq::sorting::radix_sort_from_shift<16>(
                                    occurrences + begin, occurrences + end, key);
                            }
                        });
                } else if (occurrence_layout->packed()) {
                    bioseq::sorting::radix_sort(scratch.packed_occurrences_.data(),
                                                scratch.packed_occurrences_.data() +
                                                    scratch.packed_occurrences_.size(),
                                                key);
                } else {
                    bioseq::sorting::radix_sort(scratch.wide_occurrences_.data(),
                                                scratch.wide_occurrences_.data() +
                                                    scratch.wide_occurrences_.size(),
                                                key);
                }
            } else if (occurrence_layout->packed()) {
                bioseq::sorting::radix_sort(scratch.packed_occurrences_.data(),
                                            scratch.packed_occurrences_.data() +
                                                scratch.packed_occurrences_.size(),
                                            key);
            } else {
                bioseq::sorting::radix_sort(scratch.wide_occurrences_.data(),
                                            scratch.wide_occurrences_.data() +
                                                scratch.wide_occurrences_.size(),
                                            key);
            }
        }
    } else {
        if (occurrence_count >= 2u) {
            const auto sort_bucketed_occurrences = [&scratch](auto& occurrences) {
                const auto hash_less = [](const auto& left, const auto& right) noexcept {
                    return left.hash < right.hash;
                };
                for (std::size_t bucket = 0u; bucket < query_radix_bucket_count_; ++bucket) {
                    const std::size_t begin = scratch.query_plan_radix_[bucket];
                    const std::size_t end = scratch.query_plan_radix_[bucket + 1u];
                    if (end - begin < 2u)
                        continue;
                    scratch.occurrence_radix_invoked_ = true;
                    if (end - begin < 256u) {
                        std::sort(
                            occurrences.begin() + begin, occurrences.begin() + end, hash_less);
                    } else {
                        const auto key = [](const auto& occurrence) noexcept -> std::uint32_t {
                            return occurrence.hash;
                        };
                        bioseq::sorting::radix_sort(
                            occurrences.data() + begin, occurrences.data() + end, key);
                    }
                }
            };
            if (occurrence_layout->packed()) {
                sort_bucketed_occurrences(scratch.packed_occurrences_);
            } else {
                sort_bucketed_occurrences(scratch.wide_occurrences_);
            }
        }
    }
    batch.stats_.occurrence_radix_ns = elapsed_stage_ns_(occurrence_radix_started_ns);
}

} // namespace bioseq::indexing
