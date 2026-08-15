/**
 * @file compact_direct/reader_codec/02_grouped_reader_test.cpp
 * @brief Grouped reader widths, corruption checks, and fragment blocks.
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

TEST_CASE("fragment-native anchors are canonical and match ordinary lookup",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span = k_minimizer_preferred_segment_span_bp;
    // grouped-SoA directory segments use an int32 ordinal, so a mapped reader
    // fixture cannot encode the synthetic 2^63 boundary above. This position
    // is still well beyond 32 bits and leaves a large run of empty segments.
    constexpr std::uint64_t high_reference_position = (std::uint64_t{1} << 40u) + 19u;
    constexpr auto packed = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(10u, 0u)},
        {7u, packed(10u, 1u)},
        {7u, packed(segment_span + 3u, 0u)},
        {7u, packed(high_reference_position, 1u)},
        {9u, packed(21u, 0u)},
    };
    constexpr std::uint64_t target_length = high_reference_position + 100u;
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_count > 2u);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/20u, /*strand=*/0u},
        // The duplicate produces byte-identical equal canonical keys.
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/20u, /*strand=*/0u},
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/33u, /*strand=*/1u},
        minimap_sketch_data{/*kmer=*/9u, /*pos=*/44u, /*strand=*/0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;

    SECTION("accepted cross-segment positions are byte-identical") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 4u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected =
            ordinary_seed_anchors(pairs, target_length, query, query_length, 4, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 4, *scratch);
        REQUIRE(batch.query_count() == 1u);
        REQUIRE(batch.admitted_bytes() > 0u);
        REQUIRE(scratch->wave_outstanding());
        const auto actual =
            batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 4, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(std::ranges::any_of(actual,
                                    [](const auto& anchor) { return anchor.is_reverse == 0u; }));
        REQUIRE(std::ranges::any_of(actual,
                                    [](const auto& anchor) { return anchor.is_reverse == 1u; }));
        REQUIRE(std::ranges::any_of(actual, [](const auto& anchor) {
            return anchor.ref_position == high_reference_position;
        }));
        bool observed_equal_key_duplicate = false;
        for (std::size_t ordinal = 1u; ordinal < actual.size(); ++ordinal) {
            if (std::ranges::equal(std::as_bytes(actual.subspan(ordinal - 1u, 1u)),
                                   std::as_bytes(actual.subspan(ordinal, 1u)))) {
                observed_equal_key_duplicate = true;
                break;
            }
        }
        REQUIRE(observed_equal_key_duplicate);
        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.waves == 1u);
        REQUIRE(evidence.queries == queries.size());
        REQUIRE(evidence.query_occurrences == query.size());
        REQUIRE(evidence.unique_query_plans == 2u);
        REQUIRE(evidence.packed_occurrence_waves == 1u);
        REQUIRE(evidence.wide_occurrence_waves == 0u);
        REQUIRE(evidence.occurrence_fill_ns > 0u);
        REQUIRE(evidence.occurrence_radix_ns > 0u);
        REQUIRE(evidence.control_count_pass_ns > 0u);
        REQUIRE(evidence.orientation_layout_ns > 0u);
        REQUIRE(evidence.direct_emission_ns > 0u);
        REQUIRE(evidence.local_sort_ns > 0u);
        REQUIRE(evidence.tie_normalization_ns > 0u);
        REQUIRE(evidence.control_count_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes == evidence.pass3_position_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.local_sort_bytes > 0u);
        REQUIRE(evidence.tie_normalization_bytes > 0u);
        REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
        REQUIRE(batch.admitted_bytes() ==
                evidence.exact_final_bytes + queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(evidence.peak_exact_final_bytes == evidence.exact_final_bytes);
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == queries.size());
        REQUIRE(evidence.compositional_sortedness_receipts == queries.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE(evidence.peak_occurrence_capacity_bytes > 0u);
        REQUIRE(evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(evidence.peak_query_radix_capacity_bytes > 0u);
        REQUIRE(evidence.peak_occurrence_count_capacity_bytes > 0u);
        REQUIRE(evidence.peak_block_layout_capacity_bytes > 0u);
        REQUIRE(evidence.peak_block_stats_capacity_bytes > 0u);
        REQUIRE_FALSE(evidence.saturated);
        const query_occurrence_capacity_snapshot occurrence_snapshot =
            scratch->occurrence_capacity_snapshot();
        REQUIRE(occurrence_snapshot.route == query_occurrence_route::packed);
        REQUIRE(occurrence_snapshot.packed_size == query.size());
        REQUIRE(occurrence_snapshot.wide_size == 0u);
        REQUIRE(occurrence_snapshot.query_plan_size == 0u);
        REQUIRE(occurrence_snapshot.query_plan_capacity == 0u);
        REQUIRE(occurrence_snapshot.query_plan_reserve_request == 0u);
        REQUIRE(occurrence_snapshot.bucket_direct_matcher_active);
        REQUIRE(occurrence_snapshot.query_membership_filter_capacity_bytes == 16u * 1024u * 1024u);
        REQUIRE(occurrence_snapshot.occurrence_radix_invoked);
        REQUIRE(occurrence_snapshot.aggregate_occurrence_capacity_bytes ==
                evidence.peak_occurrence_capacity_bytes);
        scratch->complete_wave_after_join();
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("compact-direct P64 decodes postings once into canonical slices") {
        auto compact = make_fragment_compact_direct_test_index(
            generation->head, generation->manifest, pairs, 10u);
        REQUIRE(compact.view.header.posting_width_bytes == 8u);
        bioseq::parallelism::memory_admission_controller admission;
        selected_page_advice_recorder selected_page_recorder;
        auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            4u,
            own_fragment_compact_direct_test_index(compact),
            selected_page_recorder.token());
        REQUIRE(reader.has_value());
        REQUIRE(selected_page_recorder.record_count.load() == 0u);
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected =
            ordinary_seed_anchors(pairs, target_length, query, query_length, 4, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            4,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto actual =
            batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 4, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto& evidence = batch.stats();
        REQUIRE(evidence.compact_direct_selected_postings == pairs.size());
        REQUIRE(evidence.compact_direct_selection_shards == 2u);
        REQUIRE(evidence.compact_direct_shard_query_layouts == 2u);
        REQUIRE(evidence.compact_direct_layout_shards_completed == 2u);
        REQUIRE(evidence.compact_direct_emission_shards_completed == 2u);
        REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
        REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
        REQUIRE(evidence.compact_direct_emission_postings_visited == pairs.size());
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes ==
                pairs.size() * compact.view.header.posting_width_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(selected_page_recorder.overflow.load());
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        const std::uintptr_t postings_begin =
            reinterpret_cast<std::uintptr_t>(compact.view.image.data()) +
            compact.view.header.postings_offset;
        const std::uintptr_t postings_end = postings_begin + compact.view.header.postings_bytes;
        std::uint64_t advised_bytes = 0u;
        for (std::size_t ordinal = 0u; ordinal < 2u; ++ordinal) {
            const auto range = selected_page_recorder.records[ordinal];
            REQUIRE(range.begin >= postings_begin);
            REQUIRE(range.end <= postings_end);
            REQUIRE(range.begin < range.end);
            advised_bytes += range.end - range.begin;
        }
        REQUIRE(advised_bytes == pairs.size() * compact.view.header.posting_width_bytes);
        scratch->complete_wave_after_join();
    }

    SECTION("late cross-segment max-occ rejection drops only the heavy hash") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 3u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected =
            ordinary_seed_anchors(pairs, target_length, query, query_length, 3, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 3, *scratch);
        const auto actual =
            batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 3, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(actual.size() == 1u);
        REQUIRE(actual.front().ref_position == 21u);
        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.pass2_position_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes == evidence.pass3_position_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == 1u);
        REQUIRE(evidence.forbidden_buffers_are_zero());
        scratch->complete_wave_after_join();
    }

    SECTION("all-heavy rejection attributes validation without layout work") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 3u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const auto heavy_query = std::span<const minimap_sketch_data>{query}.first(3u);
        const std::array queries{
            fragment_seed_query{
                .minimizers = heavy_query,
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 3, *scratch);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            heavy_query, query_length, 3, kmer_size);
        REQUIRE(actual.empty());

        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.waves == 1u);
        REQUIRE(evidence.queries == queries.size());
        REQUIRE(evidence.control_count_bytes > 0u);
        REQUIRE(evidence.rejected_control_validation_ns > 0u);
        REQUIRE(evidence.rejected_control_validation_bytes > evidence.control_count_bytes);
        REQUIRE(evidence.orientation_layout_ns == 0u);
        REQUIRE(evidence.direct_emission_ns == 0u);
        REQUIRE(evidence.local_sort_ns == 0u);
        REQUIRE(evidence.tie_normalization_ns == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        REQUIRE(evidence.anchors_constructed == 0u);
        REQUIRE(evidence.exact_final_bytes == 0u);
        REQUIRE(evidence.peak_exact_final_bytes == 0u);
        REQUIRE(batch.admitted_bytes() == queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == queries.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(evidence.saturated);
        scratch->complete_wave_after_join();
    }
}

TEST_CASE("selective pass byte receipts are exact for accepted rejected and mixed work",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t control_count = 64u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(control_count + 1u);
    for (std::uint64_t hash = 1u; hash <= control_count; ++hash) {
        pairs.emplace_back(hash, (hash + 10u) << 1u);
        if (hash == 7u) {
            pairs.emplace_back(hash, (hash + 100u) << 1u);
        }
    }
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(256u, 65'534u);
    options.position_segment_span_bp = std::uint64_t{1} << 15u;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
    REQUIRE(generation->manifest.count_width == minimizer_scalar_width::u16);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);
    REQUIRE(generation->directory.entries().size() == 1u);
    REQUIRE(generation->directory.entries().front().shape_offset_width ==
            minimizer_scalar_width::u16);

    constexpr std::uint64_t hash_scan_bytes = control_count * 4u;
    constexpr std::uint64_t control_scan_bytes = control_count * (4u + 2u);
    constexpr std::uint64_t one_matched_count_bytes = 2u;
    constexpr std::uint64_t one_prefix_pair_bytes = 2u * 2u;
    constexpr std::uint64_t one_position_bytes = 2u;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array accepted_minimizers{
        minimap_sketch_data{control_count, 20u, 0u},
    };
    const std::array rejected_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array mixed_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{9u, 21u, 1u},
    };

    const auto run = [&generation, &sink](std::span<const minimap_sketch_data> minimizers) {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = minimizers,
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        const std::size_t anchor_count =
            batch.query_view(0u)
                .ordered_fragment_seed_anchors(minimizers, query_length, 1, kmer_size)
                .size();
        const prepared_fragment_anchor_batch_stats evidence = batch.stats();
        scratch->complete_wave_after_join();
        return std::pair{evidence, anchor_count};
    };

    SECTION("accepted") {
        const auto [evidence, anchor_count] = run(accepted_minimizers);
        REQUIRE(anchor_count == 1u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == 0u);
        REQUIRE(evidence.pass2_control_bytes == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_position_bytes == one_position_bytes);
        REQUIRE(evidence.pass3_control_bytes == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass3_position_bytes == one_position_bytes);
    }

    SECTION("rejected") {
        const auto [evidence, anchor_count] = run(rejected_minimizers);
        REQUIRE(anchor_count == 0u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes ==
                hash_scan_bytes + one_matched_count_bytes + one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_control_bytes == 0u);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_control_bytes == 0u);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
    }

    SECTION("mixed") {
        const auto [evidence, anchor_count] = run(mixed_minimizers);
        REQUIRE(anchor_count == 1u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_control_bytes == hash_scan_bytes + 2u * one_matched_count_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_position_bytes == one_position_bytes);
        REQUIRE(evidence.pass3_control_bytes == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass3_position_bytes == one_position_bytes);
    }
}

TEST_CASE("checked dynamic reader covers P and C width boundaries",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    const auto verify = [&queries,
                         &query_minimizers](std::uint64_t segment_span,
                                            std::uint64_t max_occ_cap,
                                            minimizer_scalar_width expected_position_width,
                                            minimizer_scalar_width expected_count_width) {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(segment_span, max_occ_cap);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.position_width == expected_position_width);
        REQUIRE(generation->manifest.count_width == expected_count_width);
        REQUIRE(generation->directory.entries().size() == 1u);
        REQUIRE(generation->directory.entries().front().shape_offset_width ==
                minimizer_scalar_width::u16);

        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        const auto anchors = batch.query_view(0u).ordered_fragment_seed_anchors(
            query_minimizers, query_length, 1, kmer_size);
        REQUIRE(anchors.size() == 1u);
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes =
            4u + static_cast<std::uint8_t>(expected_count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.pass2_control_bytes == control_scan_bytes);
        REQUIRE(evidence.pass3_control_bytes == control_scan_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == 4u);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == 4u);
        REQUIRE(evidence.pass2_position_bytes ==
                static_cast<std::uint8_t>(expected_position_width));
        REQUIRE(evidence.pass3_position_bytes ==
                static_cast<std::uint8_t>(expected_position_width));
        scratch->complete_wave_after_join();
    };

    SECTION("P32 lower boundary") {
        verify((std::uint64_t{1} << 15u) + 1u,
               65'534u,
               minimizer_scalar_width::u32,
               minimizer_scalar_width::u16);
    }
    SECTION("P64 lower boundary") {
        verify((std::uint64_t{1} << 31u) + 1u,
               65'534u,
               minimizer_scalar_width::u64,
               minimizer_scalar_width::u16);
    }
    SECTION("C32 lower boundary with P16") {
        verify(std::uint64_t{1} << 15u,
               65'535u,
               minimizer_scalar_width::u16,
               minimizer_scalar_width::u32);
    }
}

TEST_CASE("O32 leaf is exact for fixed and dynamic count profiles",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t position_count = 65'536u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(position_count);
    for (std::uint64_t encoded = 0u; encoded < position_count; ++encoded) {
        pairs.emplace_back(7u, encoded);
    }
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    const auto verify = [&pairs, &queries, &query_minimizers](std::uint64_t max_occ_cap,
                                                              minimizer_scalar_width count_width) {
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(std::uint64_t{1} << 15u, max_occ_cap);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.count_width == count_width);
        REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);
        REQUIRE(generation->directory.entries().size() == 1u);
        REQUIRE(generation->directory.entries().front().position_element_count == position_count);
        REQUIRE(generation->directory.entries().front().shape_offset_width ==
                minimizer_scalar_width::u32);

        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        REQUIRE(batch.query_view(0u)
                    .ordered_fragment_seed_anchors(query_minimizers, 100u, 1, 15)
                    .empty());
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes = 4u + static_cast<std::uint8_t>(count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == control_scan_bytes + 2u * 4u);
        REQUIRE(evidence.pass2_control_bytes == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_control_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        scratch->complete_wave_after_join();
    };

    SECTION("fixed H32 C16 P16 O32 leaf") {
        verify(65'534u, minimizer_scalar_width::u16);
    }
    SECTION("checked dynamic C32 O32 boundary") {
        verify(65'535u, minimizer_scalar_width::u32);
    }
}

TEST_CASE("O64 fast leaf and C64 dynamic rejection never walk positions",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(sizeof(std::size_t) >= sizeof(std::uint64_t));
    constexpr std::uint64_t position_count =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1u;
    constexpr std::uint64_t segment_span = std::uint64_t{1} << 15u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> base_pairs{
        {7u, 0u},
    };
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    const auto verify = [&base_pairs, &queries, &query_minimizers](
                            std::uint64_t max_occ_cap,
                            minimizer_scalar_width expected_count_width,
                            std::uint64_t encoded_local_count) {
        vector_replay_minimizer_producer producer{base_pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, max_occ_cap);
        options.position_segment_span_bp = segment_span;
        const auto encoded = encode_minimizer_generation(producer, sink, options);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(encoded->manifest.count_width == expected_count_width);
        REQUIRE(encoded->manifest.position_width == minimizer_scalar_width::u16);
        REQUIRE(encoded->directory.entries().size() == 1u);

        auto synthetic = *encoded;
        auto entry = synthetic.directory.entries().front();
        entry.position_element_count = position_count;
        entry.shape_offset_width = minimizer_scalar_width::u64;
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           synthetic.manifest.hash_width,
                                                           entry.shape_offset_width,
                                                           synthetic.manifest.count_width);
        REQUIRE(layout.has_value());
        entry.control_encoded_bytes = layout->encoded_bytes;
        entry.position_encoded_bytes = position_count * 2u;
        const std::uint64_t extent_bytes =
            minimizer_extent_payload_bytes(minimizer_generation_layout::grouped_soa_segment_local);
        entry.control_chunk_count = static_cast<std::uint32_t>(
            entry.control_encoded_bytes / extent_bytes +
            (entry.control_encoded_bytes % extent_bytes != 0u ? 1u : 0u));
        entry.position_chunk_count = static_cast<std::uint32_t>(
            entry.position_encoded_bytes / extent_bytes +
            (entry.position_encoded_bytes % extent_bytes != 0u ? 1u : 0u));
        const std::array entries{entry};
        synthetic.manifest.control_count = 1u;
        synthetic.manifest.position_count = position_count;
        synthetic.manifest.payload_bytes =
            entry.control_encoded_bytes + entry.position_encoded_bytes;
        const auto metadata = rebuild_generation_metadata(synthetic, entries);

        auto payload = make_sparse_mapped_test_payload(metadata.manifest.payload_bytes);
        REQUIRE(payload.has_value());
        auto mutable_payload = payload->bytes();
        test_write_le<std::uint32_t>(mutable_payload, 0u, 7u);
        test_write_le<std::uint64_t>(
            mutable_payload, static_cast<std::size_t>(layout->prefix_offset), 0u);
        test_write_le<std::uint64_t>(
            mutable_payload, static_cast<std::size_t>(layout->prefix_offset + 8u), position_count);
        if (expected_count_width == minimizer_scalar_width::u16) {
            test_write_le<std::uint16_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->count_offset),
                                         static_cast<std::uint16_t>(encoded_local_count));
        } else {
            REQUIRE(expected_count_width == minimizer_scalar_width::u64);
            test_write_le<std::uint64_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->count_offset),
                                         encoded_local_count);
        }

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload->owner;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            metadata.head,
            metadata.manifest,
            test_grouped_soa_expected_storage_contract(max_occ_cap),
            metadata.directory_bytes,
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{mutable_payload.data(), mutable_payload.size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        REQUIRE(batch.query_view(0u)
                    .ordered_fragment_seed_anchors(query_minimizers, 100u, 1, 15)
                    .empty());
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes =
            4u + static_cast<std::uint8_t>(expected_count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == control_scan_bytes + 2u * 8u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        scratch->complete_wave_after_join();
    };

    SECTION("fixed H32 C16 P16 O64 leaf") {
        verify(65'534u, minimizer_scalar_width::u16, 65'535u);
    }
    SECTION("checked dynamic C64 count above UINT32 rejects before narrowing") {
        verify((std::numeric_limits<std::uint32_t>::max)(),
               minimizer_scalar_width::u64,
               position_count);
    }
}

TEST_CASE("H64 reader open fails before admission or payload access",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {1u, 10u << 1u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(100u, 65'534u);
    options.position_segment_span_bp = std::uint64_t{1} << 15u;
    options.kmer_size = 17u;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u64);
    REQUIRE(generation->manifest.max_hash > (std::numeric_limits<std::uint32_t>::max)());

    auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    auto expected = test_grouped_soa_expected_storage_contract(65'534u);
    expected.kmer_size = 17u;
    bioseq::parallelism::memory_admission_controller admission;
    REQUIRE(admission.reserved_bytes() == 0u);
    std::shared_ptr<const void> payload_owner = payload;
    const auto opened = prepared_minimizer_fragment_reader::open(
        admission,
        generation->head,
        generation->manifest,
        expected,
        generation->directory.encoded_bytes(),
        1u,
        std::move(payload_owner),
        std::span<const std::byte>{payload->data(), payload->size()});
    require_wire_error(opened, minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("fixed and dynamic probes reject selected payload corruption",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    SECTION("fixed O16 prefix escapes the planned position extent") {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
            {7u, 20u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, 65'534u);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        const auto& entry = generation->directory.entries().front();
        REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           generation->manifest.hash_width,
                                                           entry.shape_offset_width,
                                                           generation->manifest.count_width);
        REQUIRE(layout.has_value());
        test_write_le<std::uint16_t>(std::span<std::byte>{payload->data(), payload->size()},
                                     static_cast<std::size_t>(layout->prefix_offset + 2u),
                                     3u);

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            2u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("fixed Pass1 authenticates unmatched H32 and C16 controls") {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
            {9u, 20u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, 65'534u);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        const auto& entry = generation->directory.entries().front();
        REQUIRE(entry.control_element_count == 2u);
        REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           generation->manifest.hash_width,
                                                           entry.shape_offset_width,
                                                           generation->manifest.count_width);
        REQUIRE(layout.has_value());
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        auto mutable_payload = std::span<std::byte>{payload->data(), payload->size()};

        SECTION("unmatched count is zero") {
            test_write_le<std::uint16_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->count_offset) +
                                             sizeof(std::uint16_t),
                                         0u);
        }
        SECTION("hash order is not strict") {
            test_write_le<std::uint32_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->hash_offset) +
                                             sizeof(std::uint32_t),
                                         7u);
        }

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            2u,
            std::move(payload_owner),
            std::span<const std::byte>{mutable_payload.data(), mutable_payload.size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("dynamic P64 position escapes the authenticated segment core") {
        constexpr std::uint64_t segment_span = (std::uint64_t{1} << 31u) + 1u;
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(segment_span, 65'534u);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u64);
        const auto& entry = generation->directory.entries().front();
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        test_write_le<std::uint64_t>(std::span<std::byte>{payload->data(), payload->size()},
                                     static_cast<std::size_t>(entry.control_encoded_bytes),
                                     segment_span << 1u);

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }
}

TEST_CASE("fragment blocks preserve segment runs beyond ordinal 128",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span = k_minimizer_preferred_segment_span_bp;
    constexpr auto packed = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(132u);
    for (std::uint64_t hash = 1u; hash <= 130u; ++hash) {
        pairs.emplace_back(hash, packed(hash + 1u, 0u));
    }
    // Hash 1 straddles the oversized segment-0 block and the block after the
    // empty segment-1 gap. max_occ therefore has to be reduced globally.
    pairs.emplace_back(1u, packed(2u * segment_span + 4u, 0u));
    pairs.emplace_back(500u, packed(2u * segment_span + 5u, 1u));

    // The planner's fixed-width limit shrinks physical segment geometry; it
    // does not split one segment into directory entries. Build the exact valid
    // wire image here so the reader really sees an equal-segment run crossing
    // its 128-entry block target.
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(2u * segment_span + 100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_span_bp == segment_span);
    const auto profile = generation->directory.profile();
    REQUIRE(profile.hash_width == minimizer_scalar_width::u32);
    REQUIRE(profile.position_width == minimizer_scalar_width::u32);
    REQUIRE(profile.count_width == minimizer_scalar_width::u16);
    constexpr minimizer_scalar_width offset_width = minimizer_scalar_width::u16;
    const auto control_layout =
        plan_grouped_soa_control_plane(1u, profile.hash_width, offset_width, profile.count_width);
    REQUIRE(control_layout.has_value());

    std::vector<grouped_soa_bucket_directory_entry> manual_entries;
    std::vector<std::byte> control_image;
    std::vector<std::byte> position_image;
    std::vector<std::uint64_t> control_row_checksums;
    std::vector<std::uint64_t> position_row_checksums;
    manual_entries.reserve(pairs.size());
    control_image.reserve(pairs.size() * static_cast<std::size_t>(control_layout->encoded_bytes));
    position_image.reserve(pairs.size() * sizeof(std::uint32_t));
    control_row_checksums.reserve(pairs.size());
    position_row_checksums.reserve(pairs.size());

    const auto append_manual_entry = [&generation,
                                      &control_layout,
                                      &manual_entries,
                                      &control_image,
                                      &position_image,
                                      &control_row_checksums,
                                      &position_row_checksums](
                                         std::int32_t position_segment,
                                         std::uint64_t hash,
                                         std::uint64_t global_packed_position) {
        const auto core = derive_minimizer_position_segment_bounds(
            generation->manifest, static_cast<std::uint32_t>(position_segment));
        REQUIRE(core.has_value());
        const std::uint64_t global_position = global_packed_position >> 1u;
        REQUIRE(global_position >= core->begin_bp);
        REQUIRE(global_position < core->end_bp);
        const std::uint64_t local_packed_position =
            ((global_position - core->begin_bp) << 1u) | (global_packed_position & 1u);
        REQUIRE(local_packed_position <= (std::numeric_limits<std::uint32_t>::max)());

        std::vector<std::byte> controls(static_cast<std::size_t>(control_layout->encoded_bytes));
        test_write_le<std::uint32_t>(controls,
                                     static_cast<std::size_t>(control_layout->hash_offset),
                                     static_cast<std::uint32_t>(hash));
        test_write_le<std::uint16_t>(
            controls, static_cast<std::size_t>(control_layout->prefix_offset), 0u);
        test_write_le<std::uint16_t>(controls,
                                     static_cast<std::size_t>(control_layout->prefix_offset) +
                                         sizeof(std::uint16_t),
                                     1u);
        test_write_le<std::uint16_t>(
            controls, static_cast<std::size_t>(control_layout->count_offset), 1u);
        std::vector<std::byte> positions(sizeof(std::uint32_t));
        test_write_le<std::uint32_t>(
            positions, 0u, static_cast<std::uint32_t>(local_packed_position));

        const auto partition = static_cast<std::uint32_t>(manual_entries.size());
        grouped_soa_bucket_directory_entry entry{
            .partition_id = partition,
            .position_segment = position_segment,
            .lo_hash = hash,
            .hi_hash = hash,
            .sub_key = static_cast<index_sub_key_t>(partition),
            .control_first_chunk = 0u,
            .control_chunk_count = 1u,
            .position_first_chunk = 0u,
            .position_chunk_count = 1u,
            .control_element_count = 1u,
            .position_element_count = 1u,
            .control_encoded_bytes = controls.size(),
            .position_encoded_bytes = positions.size(),
            .control_checksum = crc64_ecma182(controls),
            .position_checksum = crc64_ecma182(positions),
            .shape_offset_width = offset_width,
        };
        const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
        REQUIRE(position_sub_key.has_value());
        control_row_checksums.push_back(
            crc64_index_chunk_row(generation->manifest.generation_id,
                                  position_segment,
                                  entry.sub_key,
                                  0u,
                                  static_cast<std::uint32_t>(controls.size()),
                                  controls));
        position_row_checksums.push_back(
            crc64_index_chunk_row(generation->manifest.generation_id,
                                  position_segment,
                                  *position_sub_key,
                                  0u,
                                  static_cast<std::uint32_t>(positions.size()),
                                  positions));
        control_image.insert(control_image.end(), controls.begin(), controls.end());
        position_image.insert(position_image.end(), positions.begin(), positions.end());
        manual_entries.push_back(entry);
    };
    for (std::uint64_t hash = 1u; hash <= 130u; ++hash) {
        append_manual_entry(0, hash, packed(hash + 1u, 0u));
    }
    append_manual_entry(2, 1u, packed(2u * segment_span + 4u, 0u));
    append_manual_entry(2, 500u, packed(2u * segment_span + 5u, 1u));

    crc64_ecma182_accumulator payload_checksum;
    for (const std::uint64_t checksum : control_row_checksums) {
        payload_checksum.update_little_endian(checksum);
    }
    for (const std::uint64_t checksum : position_row_checksums) {
        payload_checksum.update_little_endian(checksum);
    }
    auto adjusted_generation = *generation;
    adjusted_generation.manifest.control_count = manual_entries.size();
    adjusted_generation.manifest.position_count = manual_entries.size();
    adjusted_generation.manifest.payload_bytes = control_image.size() + position_image.size();
    adjusted_generation.manifest.payload_checksum = payload_checksum.checksum();
    adjusted_generation.manifest.extent_count =
        static_cast<std::uint32_t>(manual_entries.size() * 2u);
    const std::uint64_t directory_bytes =
        manual_entries.size() * minimizer_directory_entry_encoded_bytes(profile);
    adjusted_generation.manifest.directory_chunk_count = static_cast<std::uint32_t>(
        directory_bytes / k_grouped_soa_extent_payload_bytes +
        (directory_bytes % k_grouped_soa_extent_payload_bytes != 0u ? 1u : 0u));
    const auto metadata = rebuild_generation_metadata(adjusted_generation, manual_entries);
    const auto directory = grouped_soa_directory_view::decode(metadata.directory_bytes, profile);
    REQUIRE(directory.has_value());
    REQUIRE(validate_minimizer_generation_metadata(metadata.head, metadata.manifest, *directory)
                .has_value());
    auto mapped_payload = std::make_shared<std::vector<std::byte>>();
    mapped_payload->reserve(control_image.size() + position_image.size());
    mapped_payload->insert(mapped_payload->end(), control_image.begin(), control_image.end());
    mapped_payload->insert(mapped_payload->end(), position_image.begin(), position_image.end());
    REQUIRE(mapped_payload->size() == metadata.manifest.payload_bytes);

    const auto entries = directory->entries();
    REQUIRE(entries.size() == pairs.size());
    REQUIRE(entries.size() > 128u);
    REQUIRE(entries[127].position_segment == 0);
    REQUIRE(entries[128].position_segment == 0);
    REQUIRE(entries[129].position_segment == 0);
    REQUIRE(entries[130].position_segment == 2);
    REQUIRE(std::ranges::count_if(
                entries, [](const auto& entry) { return entry.position_segment == 0; }) == 130);
    REQUIRE(std::ranges::count_if(
                entries, [](const auto& entry) { return entry.position_segment == 1; }) == 0);
    REQUIRE(std::ranges::count_if(
                entries, [](const auto& entry) { return entry.position_segment == 2; }) == 2);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{1u, 14u, 0u},
        minimap_sketch_data{127u, 15u, 0u},
        minimap_sketch_data{128u, 16u, 0u},
        minimap_sketch_data{129u, 17u, 0u},
        minimap_sketch_data{130u, 18u, 0u},
        minimap_sketch_data{500u, 19u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    auto compact =
        make_fragment_compact_direct_test_index(metadata.head, metadata.manifest, pairs, 10u);
    bioseq::parallelism::memory_admission_controller admission;
    std::shared_ptr<const void> payload_owner = mapped_payload;
    auto reader = prepared_minimizer_fragment_reader::open(
        admission,
        metadata.head,
        metadata.manifest,
        test_grouped_soa_expected_storage_contract(10u),
        metadata.directory_bytes,
        1u,
        std::move(payload_owner),
        std::span<const std::byte>{mapped_payload->data(), mapped_payload->size()},
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query},
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected =
        ordinary_seed_anchors(pairs, 2u * segment_span + 100u, query, query_length, 1, kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
    const auto actual =
        batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 1, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == query.size() - 1u);
    REQUIRE(std::ranges::none_of(actual, [segment_span](const auto& anchor) {
        return anchor.ref_position == 2u || anchor.ref_position == 2u * segment_span + 4u;
    }));
    REQUIRE(actual.back().ref_position == 2u * segment_span + 5u);
    REQUIRE(actual.back().is_reverse == 1u);
    scratch->complete_wave_after_join();

    auto direct_batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        1,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto direct = direct_batch.query_view(0u).ordered_fragment_seed_anchors(
        query, query_length, 1, kmer_size);
    require_anchor_bytes_equal(direct, expected);
    REQUIRE(direct_batch.stats().compact_direct_unique_lookups == query.size());
    REQUIRE(direct_batch.stats().compact_direct_selected_hashes == 5u);
    REQUIRE(direct_batch.stats().compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(direct_batch.stats().compact_direct_selected_postings == 5u);
    REQUIRE(direct_batch.stats().compact_direct_selection_shards == 5u);
    REQUIRE(direct_batch.stats().compact_direct_shard_query_layouts == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_shards_completed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_shards_completed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_posting_visit_passes == 1u);
    REQUIRE(direct_batch.stats().compact_direct_layout_postings_visited == 0u);
    REQUIRE(direct_batch.stats().compact_direct_emission_postings_visited == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_selection_callbacks == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_selection_callbacks == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_spans_exposed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_spans_exposed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_descriptors_examined == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_descriptors_examined == 5u);
    REQUIRE(direct_batch.stats().compact_direct_traversal_rank_checkpoints_examined == 0u);
    REQUIRE(direct_batch.stats().compact_direct_traversal_membership_words_examined == 0u);
    REQUIRE(direct_batch.stats().pass2_position_bytes == 0u);
    REQUIRE(direct_batch.stats().pass3_position_bytes ==
            5u * compact.view.header.posting_width_bytes);
    REQUIRE(direct_batch.stats().compact_direct_target_control_scan_bytes == 0u);
    REQUIRE(direct_batch.stats().compact_direct_fallbacks == 0u);
    scratch->complete_wave_after_join();
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
