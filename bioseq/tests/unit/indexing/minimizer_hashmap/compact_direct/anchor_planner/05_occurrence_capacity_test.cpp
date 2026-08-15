/**
 * @file compact_direct/anchor_planner/05_occurrence_capacity_test.cpp
 * @brief Packed and wide occurrence capacity reuse.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <hwy/highway.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <tests/support/common.hpp>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.chaining.core;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

using namespace compact_direct_test_support;

TEST_CASE("packed and wide query occurrences recycle exact capacities",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {8u, 41u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    auto compact =
        make_fragment_compact_direct_test_index(generation->head, generation->manifest, pairs, 10u);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(
        admission, *generation, sink.rows, 2u, own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    constexpr std::size_t query_count = 65'536u;
    constexpr std::size_t present_query_ordinal = query_count - 1u;
    constexpr std::size_t singleton_query_ordinal = query_count - 2u;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    std::vector<minimap_sketch_data> skewed_query(65'536u, minimap_sketch_data{6u, 14u, 0u});
    skewed_query[0] = minimap_sketch_data{7u, 14u, 0u};
    skewed_query[1] = minimap_sketch_data{7u, 20u, 1u};
    const std::vector<minimap_sketch_data> present_hash{
        minimap_sketch_data{7u, 14u, 0u},
    };
    const std::vector<minimap_sketch_data> singleton_hash{
        minimap_sketch_data{8u, 30u, 1u},
    };
    std::vector<fragment_seed_query> queries(query_count,
                                             fragment_seed_query{
                                                 .minimizers = {},
                                                 .query_length = query_length,
                                                 .kmer_size = kmer_size,
                                             });
    queries[0].minimizers = skewed_query;
    queries[singleton_query_ordinal].minimizers = singleton_hash;
    queries[present_query_ordinal].minimizers = present_hash;

    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        const auto actual =
            batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto skewed_actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto skewed_expected =
            ordinary_seed_anchors(pairs, 100u, skewed_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(skewed_actual, skewed_expected);
        REQUIRE(skewed_actual.size() == 2u);
        const auto& evidence = batch.stats();
        REQUIRE(evidence.query_occurrences == 65'538u);
        REQUIRE(evidence.unique_query_plans == 3u);
        REQUIRE(evidence.packed_occurrence_waves == 1u);
        REQUIRE(evidence.wide_occurrence_waves == 0u);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 65'538u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                evidence.peak_occurrence_capacity_bytes);
        scratch->complete_wave_after_join();
    }
    const auto retained_packed = scratch->occurrence_capacity_snapshot();
    REQUIRE(retained_packed.packed_capacity >= 65'538u);

    STATIC_REQUIRE(k_compact_direct_parallel_packed_occurrence_radix_min_count == 65'536u);
    std::vector<minimap_sketch_data> split_boundary_query = skewed_query;
    split_boundary_query[2u] = minimap_sketch_data{(std::uint32_t{1u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[3u] = minimap_sketch_data{std::uint32_t{1u} << 24u, 14u, 0u};
    split_boundary_query[4u] = minimap_sketch_data{(std::uint32_t{2u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[5u] = minimap_sketch_data{std::uint32_t{2u} << 24u, 14u, 0u};
    split_boundary_query[6u] = minimap_sketch_data{(std::uint32_t{3u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[7u] = minimap_sketch_data{std::uint32_t{3u} << 24u, 14u, 0u};
    const auto run_compact_packed_boundary =
        [&pairs, &admission, &scratch, &generation, reader = &*reader](
            std::span<const minimap_sketch_data> minimizers, bool expected_split_selection) {
            const std::array boundary_queries{
                fragment_seed_query{
                    .minimizers = minimizers,
                    .query_length = query_length,
                    .kmer_size = kmer_size,
                },
            };
            const auto expected =
                ordinary_seed_anchors(pairs, 100u, minimizers, query_length, 2, kmer_size);
            auto batch = reader->prepare_fragment_anchor_batch(
                admission,
                boundary_queries,
                2,
                *scratch,
                /*baseline_query_plan_matcher_for_testing=*/false,
                /*verify_compositional_order_for_testing=*/true,
                minimizer_fragment_lookup_route::direct_compact_hash_lookup);
            const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
                minimizers, query_length, 2, kmer_size);
            require_anchor_bytes_equal(actual, expected);

            const std::array<std::uint32_t, 8u> exact_unique_hashes{
                6u,
                7u,
                (std::uint32_t{1u} << 24u) - 1u,
                std::uint32_t{1u} << 24u,
                (std::uint32_t{2u} << 24u) - 1u,
                std::uint32_t{2u} << 24u,
                (std::uint32_t{3u} << 24u) - 1u,
                std::uint32_t{3u} << 24u,
            };
            const compact_direct_query_set_stats expected_query_set_stats{
                .query_count = boundary_queries.size(),
                .query_occurrence_count = minimizers.size(),
                .unique_hash_count = exact_unique_hashes.size(),
                .group_key_set_checksum = compact_direct_query_set_checksum(
                    exact_unique_hashes,
                    static_cast<std::uint32_t>(kmer_size),
                    static_cast<std::uint32_t>(generation->manifest.window_size),
                    2u),
                .present = true,
            };
            REQUIRE(batch.compact_direct_query_stats() == expected_query_set_stats);
            REQUIRE(batch.stats().unique_query_plans == exact_unique_hashes.size());
            REQUIRE(batch.stats().packed_occurrence_waves == 1u);
            REQUIRE(batch.stats().wide_occurrence_waves == 0u);

            const auto snapshot = scratch->occurrence_capacity_snapshot();
            REQUIRE(snapshot.route == query_occurrence_route::packed);
            REQUIRE(snapshot.packed_size == minimizers.size());
            REQUIRE(snapshot.wide_size == 0u);
            REQUIRE(snapshot.query_plan_size == exact_unique_hashes.size());
            REQUIRE(snapshot.occurrence_radix_invoked);
            // This receipt records the exact split-route threshold and output
            // parity; it deliberately makes no claim about observed pool workers.
            REQUIRE(snapshot.parallel_occurrence_radix_split_selected == expected_split_selection);
            REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
            REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                    batch.stats().peak_occurrence_capacity_bytes);
            scratch->complete_wave_after_join();
        };
    const std::span<const minimap_sketch_data> skewed_span{split_boundary_query};
    run_compact_packed_boundary(
        skewed_span.first(k_compact_direct_parallel_packed_occurrence_radix_min_count - 1u), false);
    run_compact_packed_boundary(
        skewed_span.first(k_compact_direct_parallel_packed_occurrence_radix_min_count), true);

    skewed_query.push_back(minimap_sketch_data{6u, 14u, 0u});
    queries[0].minimizers = skewed_query;
    std::vector<bioseq::chaining::anchor> wide_production_query0;
    std::vector<bioseq::chaining::anchor> wide_production_present_query;
    prepared_fragment_anchor_batch_stats wide_production_evidence{};
    std::uint64_t wide_production_admitted_bytes = 0u;
    {
        auto batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  2,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/false,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto actual =
            batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto skewed_actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto skewed_expected =
            ordinary_seed_anchors(pairs, 100u, skewed_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(skewed_actual, skewed_expected);
        REQUIRE(skewed_actual.size() == 2u);
        const auto& evidence = batch.stats();
        wide_production_query0.assign(skewed_actual.begin(), skewed_actual.end());
        wide_production_present_query.assign(actual.begin(), actual.end());
        wide_production_evidence = evidence;
        wide_production_admitted_bytes = batch.admitted_bytes();
        REQUIRE(evidence.query_occurrences == 65'539u);
        REQUIRE(evidence.unique_query_plans == 3u);
        REQUIRE(evidence.packed_occurrence_waves == 0u);
        REQUIRE(evidence.wide_occurrence_waves == 1u);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.packed_size == 0u);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.packed_capacity == retained_packed.packed_capacity);
        REQUIRE(snapshot.wide_capacity >= 65'539u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                evidence.peak_occurrence_capacity_bytes);
        REQUIRE(admission.reserved_bytes() ==
                reader->admitted_bytes() + scratch->admitted_bytes() + batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
    {
        auto oracle_batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  2,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/true,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto oracle_query0 = oracle_batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto oracle_present_query =
            oracle_batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(oracle_query0, wide_production_query0);
        require_anchor_bytes_equal(oracle_present_query, wide_production_present_query);
        require_hot_loop_differential_evidence_equal(wide_production_evidence,
                                                     oracle_batch.stats());
        REQUIRE(oracle_batch.admitted_bytes() == wide_production_admitted_bytes);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.query_plan_size == 3u);
        REQUIRE(snapshot.query_plan_reserve_request == 3u);
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes() + scratch->admitted_bytes() +
                                                  oracle_batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
    {
        auto direct_batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto direct_query0 = direct_batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto direct_present_query =
            direct_batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        const auto direct_singleton_query =
            direct_batch.query_view(singleton_query_ordinal)
                .ordered_fragment_seed_anchors(singleton_hash, query_length, 2, kmer_size);
        const auto expected_singleton_query =
            ordinary_seed_anchors(pairs, 100u, singleton_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(direct_query0, wide_production_query0);
        require_anchor_bytes_equal(direct_present_query, wide_production_present_query);
        require_anchor_bytes_equal(direct_singleton_query, expected_singleton_query);
        REQUIRE(direct_singleton_query.size() == 1u);
        REQUIRE(direct_batch.stats().wide_occurrence_waves == 1u);
        REQUIRE(direct_batch.stats().packed_occurrence_waves == 0u);
        REQUIRE(direct_batch.stats().compact_direct_unique_lookups == 3u);
        REQUIRE(direct_batch.stats().compact_direct_selected_hashes == 2u);
        REQUIRE(direct_batch.stats().compact_direct_selected_postings == 2u);
        REQUIRE(direct_batch.stats().compact_direct_posting_visit_passes == 1u);
        REQUIRE(direct_batch.stats().compact_direct_emission_postings_visited == 2u);
        REQUIRE(direct_batch.stats().compact_direct_rejected_posting_reads == 0u);
        REQUIRE(direct_batch.stats().pass2_position_bytes == 0u);
        REQUIRE(direct_batch.stats().pass3_position_bytes ==
                2u * compact.view.header.posting_width_bytes);
        REQUIRE(direct_batch.stats().anchors_constructed == 4u);
        REQUIRE(direct_batch.stats().forbidden_buffers_are_zero());
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.query_plan_size == 3u);
        REQUIRE(snapshot.query_membership_filter_capacity_bytes == 0u);
        REQUIRE(snapshot.query_group_offset_capacity == 0u);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        scratch->complete_wave_after_join();
    }
    const auto retained_both = scratch->occurrence_capacity_snapshot();
    REQUIRE(retained_both.packed_capacity > 0u);
    REQUIRE(retained_both.wide_capacity > 0u);

    const std::array smaller_wave{
        fragment_seed_query{
            .minimizers = present_hash,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, smaller_wave, 2, *scratch);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 1u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.packed_capacity == retained_both.packed_capacity);
        REQUIRE(snapshot.wide_capacity == retained_both.wide_capacity);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE_FALSE(snapshot.occurrence_radix_invoked);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                8u * snapshot.packed_capacity + 12u * snapshot.wide_capacity);
        REQUIRE(batch.stats().query_occurrences == 1u);
        REQUIRE(batch.stats().unique_query_plans == 1u);
        REQUIRE(batch.stats().packed_occurrence_waves == 1u);
        REQUIRE(batch.stats().wide_occurrence_waves == 0u);
        scratch->complete_wave_after_join();
    }

    const std::array small_compact_minimizers{
        minimap_sketch_data{8u, 30u, 1u},
        minimap_sketch_data{7u, 14u, 0u},
        minimap_sketch_data{7u, 20u, 1u},
    };
    const std::array small_compact_wave{
        fragment_seed_query{
            .minimizers = small_compact_minimizers,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            small_compact_wave,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            small_compact_minimizers, query_length, 2, kmer_size);
        const auto expected = ordinary_seed_anchors(
            pairs, 100u, small_compact_minimizers, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const std::array<std::uint32_t, 2u> exact_unique_hashes{7u, 8u};
        const compact_direct_query_set_stats expected_query_set_stats{
            .query_count = small_compact_wave.size(),
            .query_occurrence_count = small_compact_minimizers.size(),
            .unique_hash_count = exact_unique_hashes.size(),
            .group_key_set_checksum = compact_direct_query_set_checksum(
                exact_unique_hashes,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(generation->manifest.window_size),
                2u),
            .present = true,
        };
        REQUIRE(batch.compact_direct_query_stats() == expected_query_set_stats);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        scratch->complete_wave_after_join();
    }

    constexpr std::size_t unique_count = 1'024u;
    constexpr bioseq::storage::sequence_size_t unique_query_length = 2'048u;
    std::vector<minimap_sketch_data> all_unique;
    all_unique.reserve(unique_count);
    for (std::size_t ordinal = 0u; ordinal < unique_count; ++ordinal) {
        all_unique.push_back(minimap_sketch_data{
            100u + ordinal,
            static_cast<std::uint32_t>(14u + ordinal),
            static_cast<std::uint32_t>(ordinal & 1u),
        });
    }
    const std::array all_unique_wave{
        fragment_seed_query{
            .minimizers = all_unique,
            .query_length = unique_query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, all_unique_wave, 2, *scratch);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            all_unique, unique_query_length, 2, kmer_size);
        REQUIRE(actual.empty());
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == unique_count);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE(batch.stats().query_occurrences == unique_count);
        REQUIRE(batch.stats().unique_query_plans == unique_count);
        scratch->complete_wave_after_join();
    }

    const std::array<fragment_seed_query, 0u> empty_wave{};
    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, empty_wave, 2, *scratch);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 0u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE_FALSE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(batch.stats().query_occurrences == 0u);
        REQUIRE(batch.stats().unique_query_plans == 0u);
        REQUIRE(batch.stats().packed_occurrence_waves == 1u);
        REQUIRE(batch.stats().wide_occurrence_waves == 0u);
        scratch->complete_wave_after_join();
    }

    scratch->release_retained_capacity();
    REQUIRE(scratch->admitted_bytes() == 0u);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
