/**
 * @file compact_direct/reader_codec/04_matcher_test.cpp
 * @brief Bucket-direct and scalar matcher differential behavior.
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
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;
import bioseq.tests.unit.indexing.minimizer_hashmap.compact_direct_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

using namespace compact_direct_test_support;

TEST_CASE("bucket-direct Bloom matcher is byte-identical to its test oracle",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {511u, 40u},
        {512u, 60u},
        // Low-9 mask 63 is covered by the union of query masks 0 and 28.
        // This absent hash therefore exercises Bloom-positive exact fallback.
        {8'192u + 63u, 70u},
        {1'048'575u, 80u},
        {1'048'576u, 100u},
        {1'073'741'822u, 120u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(256u, 10u));
    REQUIRE(generation.has_value());

    const std::vector<minimap_sketch_data> minimizers{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{511u, 30u, 1u},
        minimap_sketch_data{512u, 40u, 0u},
        minimap_sketch_data{513u, 50u, 1u},
        minimap_sketch_data{8'192u + 0u, 52u, 0u},
        minimap_sketch_data{8'192u + 28u, 54u, 1u},
        minimap_sketch_data{1'048'575u, 60u, 0u},
        minimap_sketch_data{1'048'576u, 70u, 1u},
        minimap_sketch_data{1'073'741'822u, 80u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 128u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{
            .minimizers = minimizers,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 10u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    std::vector<bioseq::chaining::anchor> bucket_direct_anchors;
    prepared_fragment_anchor_batch_stats bucket_direct_evidence{};
    {
        auto batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  10,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/false,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto anchors = batch.query_view(0u).ordered_fragment_seed_anchors(
            minimizers, query_length, 10, kmer_size);
        bucket_direct_anchors.assign(anchors.begin(), anchors.end());
        bucket_direct_evidence = batch.stats();
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_group_offset_size == bucket_direct_evidence.unique_query_plans + 1u);
        REQUIRE(snapshot.query_membership_filter_capacity_bytes == 16u * 1024u * 1024u);
        REQUIRE(bucket_direct_evidence.peak_query_membership_filter_capacity_bytes ==
                snapshot.query_membership_filter_capacity_bytes);
        REQUIRE(bucket_direct_evidence.query_membership_filter_allocations == 1u);
        REQUIRE(bucket_direct_evidence.bucket_direct_matcher_view_receipts == 1u);
        REQUIRE(bucket_direct_evidence.query_plan_allocations == 0u);
        REQUIRE(bucket_direct_evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(bucket_direct_evidence.peak_occurrence_count_capacity_bytes ==
                bucket_direct_evidence.unique_query_plans * sizeof(std::atomic<std::uint32_t>));
        REQUIRE(bucket_direct_evidence.peak_query_group_offset_capacity_bytes >=
                snapshot.query_group_offset_size * sizeof(std::uint32_t));
        REQUIRE(bucket_direct_evidence.compositional_sortedness_receipts == queries.size());
        REQUIRE(bucket_direct_evidence.final_sortedness_validation_bytes ==
                bucket_direct_evidence.exact_final_bytes);
        REQUIRE(batch.admitted_bytes() == bucket_direct_evidence.exact_final_bytes +
                                              queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(admission.reserved_bytes() ==
                reader->admitted_bytes() + scratch->admitted_bytes() + batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }

    {
        auto oracle_batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  10,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/true,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto oracle_anchors = oracle_batch.query_view(0u).ordered_fragment_seed_anchors(
            minimizers, query_length, 10, kmer_size);
        require_anchor_bytes_equal(oracle_anchors, bucket_direct_anchors);
        const auto& oracle_evidence = oracle_batch.stats();
        REQUIRE(oracle_evidence.bucket_direct_matcher_view_receipts == 0u);
        REQUIRE(oracle_evidence.unique_query_plans == bucket_direct_evidence.unique_query_plans);
        REQUIRE(oracle_evidence.control_count_bytes == bucket_direct_evidence.control_count_bytes);
        REQUIRE(oracle_evidence.pass2_control_bytes == bucket_direct_evidence.pass2_control_bytes);
        REQUIRE(oracle_evidence.pass2_accepted_prefix_bytes ==
                bucket_direct_evidence.pass2_accepted_prefix_bytes);
        REQUIRE(oracle_evidence.pass2_position_bytes ==
                bucket_direct_evidence.pass2_position_bytes);
        REQUIRE(oracle_evidence.pass3_control_bytes == bucket_direct_evidence.pass3_control_bytes);
        REQUIRE(oracle_evidence.pass3_accepted_prefix_bytes ==
                bucket_direct_evidence.pass3_accepted_prefix_bytes);
        REQUIRE(oracle_evidence.pass3_position_bytes ==
                bucket_direct_evidence.pass3_position_bytes);
        REQUIRE(oracle_evidence.anchors_constructed == bucket_direct_evidence.anchors_constructed);
        REQUIRE(oracle_evidence.exact_final_bytes == bucket_direct_evidence.exact_final_bytes);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.query_plan_size == bucket_direct_evidence.unique_query_plans);
        REQUIRE(snapshot.query_plan_reserve_request == bucket_direct_evidence.unique_query_plans);
        REQUIRE(oracle_batch.admitted_bytes() ==
                oracle_evidence.exact_final_bytes +
                    queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes() + scratch->admitted_bytes() +
                                                  oracle_batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
}

TEST_CASE("scalar matcher differential covers counter mapping and dynamic leaves",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr auto packed_target = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed_target(10u, 0u)},
        {7u, packed_target(20u, 1u)},
        {7u, packed_target(30u, 0u)},
        {9u, packed_target(40u, 0u)},
        {9u, packed_target(50u, 1u)},
        {11u, packed_target(60u, 1u)},
    };
    const std::array query0{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{9u, 30u, 1u},
        minimap_sketch_data{13u, 40u, 0u},
    };
    const std::array query1{
        minimap_sketch_data{7u, 22u, 1u},
        minimap_sketch_data{9u, 31u, 0u},
        minimap_sketch_data{11u, 42u, 1u},
        minimap_sketch_data{11u, 50u, 0u},
    };
    const std::array heavy_query0{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array heavy_query1{
        minimap_sketch_data{7u, 22u, 1u},
        minimap_sketch_data{7u, 35u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 128u;
    constexpr std::int32_t kmer_size = 15;
    constexpr std::int32_t max_occ = 2;
    const std::array mixed_queries{
        fragment_seed_query{
            .minimizers = query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const std::array all_heavy_queries{
        fragment_seed_query{
            .minimizers = heavy_query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = heavy_query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    struct differential_run {
        std::vector<std::vector<bioseq::chaining::anchor>> anchors{};
        prepared_fragment_anchor_batch_stats evidence{};
        std::uint64_t admitted_bytes = 0u;
    };

    const auto verify_fixture = [&pairs, &mixed_queries, &all_heavy_queries](
                                    std::uint64_t target_length,
                                    std::uint64_t segment_span,
                                    minimizer_scalar_width position_width) {
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(target_length, 10u);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.count_width == minimizer_scalar_width::u16);
        REQUIRE(generation->manifest.position_width == position_width);
        REQUIRE(std::ranges::all_of(generation->directory.entries(),
                                    [](const grouped_soa_bucket_directory_entry& entry) noexcept {
                                        return entry.shape_offset_width ==
                                               minimizer_scalar_width::u16;
                                    }));

        bioseq::parallelism::memory_admission_controller admission;
        auto reader =
            open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, max_occ);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());

        const auto run = [&reader, &admission, &scratch](
                             std::span<const fragment_seed_query> queries, bool oracle) {
            differential_run result;
            auto batch = reader->prepare_fragment_anchor_batch(
                admission,
                queries,
                max_occ,
                *scratch,
                /*baseline_query_plan_matcher_for_testing=*/oracle,
                /*verify_compositional_order_for_testing=*/true);
            result.anchors.resize(queries.size());
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
                const fragment_seed_query& query = queries[query_ordinal];
                const auto anchors =
                    batch.query_view(query_ordinal)
                        .ordered_fragment_seed_anchors(
                            query.minimizers, query.query_length, max_occ, query.kmer_size);
                result.anchors[query_ordinal].assign(anchors.begin(), anchors.end());
            }
            result.evidence = batch.stats();
            result.admitted_bytes = batch.admitted_bytes();
            REQUIRE(admission.reserved_bytes() ==
                    reader->admitted_bytes() + scratch->admitted_bytes() + batch.admitted_bytes());
            scratch->complete_wave_after_join();
            REQUIRE_FALSE(scratch->wave_outstanding());
            return result;
        };

        const auto compare_runs = [](const differential_run& actual,
                                     const differential_run& oracle) {
            REQUIRE(actual.anchors.size() == oracle.anchors.size());
            for (std::size_t query_ordinal = 0u; query_ordinal < actual.anchors.size();
                 ++query_ordinal) {
                require_anchor_bytes_equal(actual.anchors[query_ordinal],
                                           oracle.anchors[query_ordinal]);
            }
            require_hot_loop_differential_evidence_equal(actual.evidence, oracle.evidence);
            REQUIRE(actual.admitted_bytes == oracle.admitted_bytes);
        };

        const differential_run mixed_actual = run(mixed_queries, false);
        const differential_run mixed_oracle = run(mixed_queries, true);
        compare_runs(mixed_actual, mixed_oracle);
        REQUIRE(mixed_actual.anchors[0u].size() == 2u);
        REQUIRE(mixed_actual.anchors[1u].size() == 4u);
        for (std::size_t query_ordinal = 0u; query_ordinal < mixed_queries.size();
             ++query_ordinal) {
            const fragment_seed_query& query = mixed_queries[query_ordinal];
            const auto ordinary = ordinary_seed_anchors(pairs,
                                                        target_length,
                                                        query.minimizers,
                                                        query.query_length,
                                                        max_occ,
                                                        query.kmer_size);
            require_anchor_bytes_equal(mixed_actual.anchors[query_ordinal], ordinary);
        }
        constexpr std::uint64_t control_count_bytes =
            3u * (sizeof(std::uint32_t) + sizeof(std::uint16_t));
        constexpr std::uint64_t hash_scan_bytes = 3u * sizeof(std::uint32_t);
        constexpr std::uint64_t accepted_prefix_bytes = 2u * 2u * sizeof(std::uint16_t);
        constexpr std::uint64_t rejected_prefix_bytes = 2u * sizeof(std::uint16_t);
        const bool dynamic = position_width != minimizer_scalar_width::u16;
        REQUIRE(mixed_actual.evidence.query_occurrences == 7u);
        REQUIRE(mixed_actual.evidence.unique_query_plans == 4u);
        REQUIRE(mixed_actual.evidence.control_count_bytes == control_count_bytes);
        REQUIRE(mixed_actual.evidence.rejected_control_validation_bytes == rejected_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass2_control_bytes == control_count_bytes);
        REQUIRE(mixed_actual.evidence.pass2_accepted_prefix_bytes == accepted_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass2_position_bytes ==
                3u * static_cast<std::uint8_t>(position_width));
        REQUIRE(mixed_actual.evidence.pass3_control_bytes ==
                (dynamic ? control_count_bytes : hash_scan_bytes + 2u * sizeof(std::uint16_t)));
        REQUIRE(mixed_actual.evidence.pass3_accepted_prefix_bytes == accepted_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass3_position_bytes ==
                mixed_actual.evidence.pass2_position_bytes);
        REQUIRE(mixed_actual.evidence.anchors_constructed == 6u);
        REQUIRE(mixed_actual.evidence.exact_final_bytes == 6u * sizeof(bioseq::chaining::anchor));
        REQUIRE(mixed_actual.admitted_bytes ==
                mixed_actual.evidence.exact_final_bytes +
                    mixed_queries.size() * sizeof(prepared_query_anchors));

        const differential_run rejected_actual = run(all_heavy_queries, false);
        const differential_run rejected_oracle = run(all_heavy_queries, true);
        compare_runs(rejected_actual, rejected_oracle);
        REQUIRE(rejected_actual.anchors[0u].empty());
        REQUIRE(rejected_actual.anchors[1u].empty());
        REQUIRE(rejected_actual.evidence.control_count_bytes == control_count_bytes);
        REQUIRE(rejected_actual.evidence.rejected_control_validation_bytes ==
                (dynamic ? control_count_bytes + rejected_prefix_bytes
                         : hash_scan_bytes + sizeof(std::uint16_t) + rejected_prefix_bytes));
        REQUIRE(rejected_actual.evidence.pass2_control_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass2_position_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_control_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_position_bytes == 0u);
        REQUIRE(rejected_actual.evidence.anchors_constructed == 0u);
        REQUIRE(rejected_actual.evidence.exact_final_bytes == 0u);
        REQUIRE(rejected_actual.admitted_bytes ==
                all_heavy_queries.size() * sizeof(prepared_query_anchors));
    };

    SECTION("fixed H32 C16 P16 leaves") {
        verify_fixture(256u, std::uint64_t{1u} << 15u, minimizer_scalar_width::u16);
    }
    SECTION("checked dynamic H32 C16 P32 leaves") {
        verify_fixture((std::uint64_t{1u} << 15u) + 1u,
                       (std::uint64_t{1u} << 15u) + 1u,
                       minimizer_scalar_width::u32);
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
