/**
 * @file compact_direct/anchor_planner/03_query_waves_test.cpp
 * @brief Checks reuse of query-batch scratch space and anchor counts.
 */

#include <hwy/highway.h>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>

import std;
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
import bioseq.common.hashing; 

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;
import bioseq.tests.unit.indexing.minimizer_hashmap.compact_direct_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

using namespace compact_direct_test_support;

TEST_CASE("query-wave scratch recycles without stale anchors",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(10u, 0u)},
        {7u, packed(20u, 1u)},
        {8u, packed(30u, 0u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 2u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    const std::vector<minimap_sketch_data> first_query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{7u, 25u, 1u},
    };
    const std::vector<minimap_sketch_data> empty_query{};
    const std::vector<minimap_sketch_data> third_query{
        minimap_sketch_data{8u, 30u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;

    {
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{first_query}, query_length, kmer_size},
            fragment_seed_query{
                std::span<const minimap_sketch_data>{empty_query}, query_length, kmer_size},
            fragment_seed_query{
                std::span<const minimap_sketch_data>{third_query}, query_length, kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        REQUIRE(batch.query_count() == queries.size());
        const auto first_expected =
            ordinary_seed_anchors(pairs, 100u, first_query, query_length, 2, kmer_size);
        const auto third_expected =
            ordinary_seed_anchors(pairs, 100u, third_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(batch.query_view(0u).ordered_fragment_seed_anchors(
                                       first_query, query_length, 2, kmer_size),
                                   first_expected);
        REQUIRE(batch.query_view(1u)
                    .ordered_fragment_seed_anchors(empty_query, query_length, 2, kmer_size)
                    .empty());
        require_anchor_bytes_equal(batch.query_view(2u).ordered_fragment_seed_anchors(
                                       third_query, query_length, 2, kmer_size),
                                   third_expected);
        scratch->complete_wave_after_join();
    }
    const std::uint64_t retained_after_first_wave = scratch->admitted_bytes();
    REQUIRE(retained_after_first_wave > 0u);

    const std::vector<minimap_sketch_data> last_query{
        minimap_sketch_data{8u, 14u, 0u},
    };
    {
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{last_query}, query_length, kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        const auto view = batch.query_view(0u);
        const auto actual =
            view.ordered_fragment_seed_anchors(last_query, query_length, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, last_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(actual.size() == 1u);

        const std::vector<minimap_sketch_data> copied_query = last_query;
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(copied_query, query_length, 2, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(last_query, query_length + 1u, 2, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(last_query, query_length, 1, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(last_query, query_length, 2, kmer_size + 1),
            persisted_minimizer_lookup_error);
        scratch->complete_wave_after_join();
    }
    REQUIRE(scratch->admitted_bytes() == retained_after_first_wave);

    {
        const std::array<fragment_seed_query, 0u> empty_wave{};
        auto batch = reader->prepare_fragment_anchor_batch(admission, empty_wave, 2, *scratch);
        REQUIRE(batch.query_count() == 0u);
        scratch->complete_wave_after_join();
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    constexpr bioseq::storage::sequence_size_t packed_query_limit =
        bioseq::storage::sequence_size_t{1} << 28u;
    const std::vector<minimap_sketch_data> boundary_query{
        minimap_sketch_data{8u, static_cast<std::uint32_t>(packed_query_limit - 1u), 0u},
    };
    {
        const std::array queries{
            fragment_seed_query{std::span<const minimap_sketch_data>{boundary_query},
                                packed_query_limit,
                                kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            boundary_query, packed_query_limit, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, boundary_query, packed_query_limit, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        scratch->complete_wave_after_join();
    }
    const std::array oversized_query{
        fragment_seed_query{std::span<const minimap_sketch_data>{boundary_query},
                            packed_query_limit + 1u,
                            kmer_size},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(admission, oversized_query, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());

    const std::array mismatched_kmer_query{
        fragment_seed_query{
            std::span<const minimap_sketch_data>{last_query}, query_length, kmer_size + 1},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(admission, mismatched_kmer_query, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());

    const std::vector<minimap_sketch_data> out_of_profile_hash_query{
        minimap_sketch_data{static_cast<std::uint64_t>(k_minimap_k15_max_hash) + 1u, 14u, 0u},
    };
    const std::array out_of_profile_hash_queries{
        fragment_seed_query{std::span<const minimap_sketch_data>{out_of_profile_hash_query},
                            query_length,
                            kmer_size},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(admission, out_of_profile_hash_queries, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
}

TEST_CASE("sixteen-query compact packed wave preserves exact receipts",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(100u, 10u);
    options.position_segment_span_bp = std::uint64_t{1u} << 15u;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
    REQUIRE(generation->manifest.count_width == minimizer_scalar_width::u16);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);
    REQUIRE(generation->directory.entries().size() == 1u);
    REQUIRE(generation->directory.entries().front().shape_offset_width ==
            minimizer_scalar_width::u16);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 2u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    const std::vector<minimap_sketch_data> minimizers{
        minimap_sketch_data{7u, 14u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    constexpr std::size_t query_count = 16u;
    const std::vector<fragment_seed_query> queries(query_count,
                                                   fragment_seed_query{
                                                       .minimizers = minimizers,
                                                       .query_length = query_length,
                                                       .kmer_size = kmer_size,
                                                   });
    const auto expected =
        ordinary_seed_anchors(pairs, 100u, minimizers, query_length, 2, kmer_size);
    REQUIRE(expected.size() == 2u);

    auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
    REQUIRE(batch.query_count() == query_count);
    for (std::size_t query_index = 0u; query_index < query_count; ++query_index) {
        require_anchor_bytes_equal(
            batch.query_view(query_index)
                .ordered_fragment_seed_anchors(minimizers, query_length, 2, kmer_size),
            expected);
    }

    constexpr std::uint64_t control_scan_bytes = sizeof(std::uint32_t) + sizeof(std::uint16_t);
    constexpr std::uint64_t accepted_prefix_bytes = 2u * sizeof(std::uint16_t);
    constexpr std::uint64_t position_bytes = 2u * sizeof(std::uint16_t);
    const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
    REQUIRE(evidence.waves == 1u);
    REQUIRE(evidence.queries == query_count);
    REQUIRE(evidence.query_occurrences == query_count);
    REQUIRE(evidence.unique_query_plans == 1u);
    REQUIRE(evidence.packed_occurrence_waves == 1u);
    REQUIRE(evidence.wide_occurrence_waves == 0u);
    REQUIRE(evidence.control_count_bytes == control_scan_bytes);
    REQUIRE(evidence.rejected_control_validation_bytes == 0u);
    REQUIRE(evidence.pass2_control_bytes == control_scan_bytes);
    REQUIRE(evidence.pass2_accepted_prefix_bytes == accepted_prefix_bytes);
    REQUIRE(evidence.pass2_position_bytes == position_bytes);
    REQUIRE(evidence.pass3_control_bytes == control_scan_bytes);
    REQUIRE(evidence.pass3_accepted_prefix_bytes == accepted_prefix_bytes);
    REQUIRE(evidence.pass3_position_bytes == position_bytes);
    REQUIRE(evidence.anchors_constructed == query_count * expected.size());
    REQUIRE(evidence.exact_final_bytes ==
            evidence.anchors_constructed * sizeof(bioseq::chaining::anchor));
    REQUIRE(evidence.peak_exact_final_bytes == evidence.exact_final_bytes);
    REQUIRE(batch.admitted_bytes() ==
            evidence.exact_final_bytes + query_count * sizeof(prepared_query_anchors));
    REQUIRE(evidence.global_sort_calls_invoked == 0u);
    REQUIRE(evidence.global_sort_calls_skipped == query_count);
    REQUIRE(evidence.compositional_sortedness_receipts == query_count);
    REQUIRE(evidence.forbidden_buffers_are_zero());
    REQUIRE_FALSE(evidence.saturated);

    const query_occurrence_capacity_snapshot occurrence_snapshot =
        scratch->occurrence_capacity_snapshot();
    REQUIRE(occurrence_snapshot.route == query_occurrence_route::packed);
    REQUIRE(occurrence_snapshot.packed_size == query_count);
    REQUIRE(occurrence_snapshot.wide_size == 0u);
    REQUIRE(occurrence_snapshot.query_plan_size == 0u);
    REQUIRE(occurrence_snapshot.query_plan_capacity == 0u);
    REQUIRE(occurrence_snapshot.bucket_direct_matcher_active);
    scratch->complete_wave_after_join();
    REQUIRE_FALSE(scratch->wave_outstanding());
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
