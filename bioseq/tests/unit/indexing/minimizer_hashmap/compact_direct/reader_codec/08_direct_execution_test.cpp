/**
 * @file compact_direct/reader_codec/08_direct_execution_test.cpp
 * @brief Checks direct index lookup, anchor output, and work split across hash groups.
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

TEST_CASE("compact direct fragment route is exact and scan free",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t build_cap = 4u;
    constexpr std::int32_t runtime_max_occ = 2;
    constexpr std::uint64_t target_length = 400u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(20u, 0u)},
        {7u, packed(150u, 1u)},
        {8u, packed(30u, 0u)},
        {8u, packed(31u, 0u)},
        {8u, packed(32u, 1u)},
        {9u, packed(40u, 0u)},
        {9u, packed(41u, 0u)},
        {9u, packed(42u, 0u)},
        {9u, packed(43u, 1u)},
        {9u, packed(44u, 1u)},
        {10u, packed(250u, 0u)},
        {10u, packed(251u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, build_cap));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, build_cap);

    const std::vector<minimap_sketch_data> query0{
        minimap_sketch_data{7u, 14u, 0u},
        minimap_sketch_data{7u, 20u, 1u},
        minimap_sketch_data{8u, 30u, 0u},
        minimap_sketch_data{9u, 40u, 1u},
        minimap_sketch_data{11u, 50u, 0u},
    };
    const std::vector<minimap_sketch_data> query1{
        minimap_sketch_data{10u, 14u, 0u},
        minimap_sketch_data{7u, 25u, 1u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
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

    {
        bioseq::parallelism::memory_admission_controller no_direct_admission;
        auto no_direct_reader = open_grouped_soa_fragment_query_reader(
            no_direct_admission, *generation, sink.rows, runtime_max_occ);
        REQUIRE(no_direct_reader.has_value());
        auto no_direct_scratch = query_wave_scratch::create(no_direct_admission);
        REQUIRE(no_direct_scratch.has_value());
        REQUIRE_THROWS_AS(no_direct_reader->prepare_fragment_anchor_batch(
                              no_direct_admission,
                              queries,
                              runtime_max_occ,
                              *no_direct_scratch,
                              /*baseline_query_plan_matcher_for_testing=*/false,
                              /*verify_compositional_order_for_testing=*/false,
                              minimizer_fragment_lookup_route::direct_compact_hash_lookup),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(no_direct_scratch->wave_outstanding());
    }

    {
        auto subset_view = compact.view;
        REQUIRE(subset_view.header.geometry.hash_domain_bits > 9u);
        --subset_view.header.geometry.hash_domain_bits;
        subset_view.header.geometry.max_hash = static_cast<std::uint32_t>(
            (std::uint64_t{1u} << subset_view.header.geometry.hash_domain_bits) - 1u);
        bioseq::parallelism::memory_admission_controller subset_admission;
        const auto subset_reader = open_grouped_soa_fragment_query_reader(
            subset_admission,
            *generation,
            sink.rows,
            runtime_max_occ,
            own_fragment_compact_direct_test_index(compact, subset_view));
        REQUIRE_FALSE(subset_reader.has_value());
        REQUIRE(subset_reader.error() ==
                minimizer_generation_encoding_error::storage_contract_mismatch);

        auto superset_view = compact.view;
        REQUIRE(superset_view.header.geometry.hash_domain_bits < 32u);
        ++superset_view.header.geometry.hash_domain_bits;
        superset_view.header.geometry.max_hash =
            superset_view.header.geometry.hash_domain_bits == 32u
                ? (std::numeric_limits<std::uint32_t>::max)()
                : static_cast<std::uint32_t>(
                      (std::uint64_t{1u} << superset_view.header.geometry.hash_domain_bits) - 1u);
        bioseq::parallelism::memory_admission_controller superset_admission;
        const auto superset_reader = open_grouped_soa_fragment_query_reader(
            superset_admission,
            *generation,
            sink.rows,
            runtime_max_occ,
            own_fragment_compact_direct_test_index(compact, superset_view));
        REQUIRE_FALSE(superset_reader.has_value());
        REQUIRE(superset_reader.error() ==
                minimizer_generation_encoding_error::storage_contract_mismatch);

        auto unrelated = std::make_shared<std::vector<std::byte>>(1u);
        std::shared_ptr<const std::byte> unrelated_alias{unrelated, unrelated->data()};
        REQUIRE_FALSE(
            owned_compact_hash32_direct_index_view::bind(std::move(unrelated_alias), compact.view));
    }

    {
        auto direct_only_compact = make_fragment_compact_direct_test_index(
            generation->head, generation->manifest, pairs, build_cap);
        selected_page_advice_recorder selected_page_recorder;
        auto wrong_segment_manifest = generation->manifest;
        ++wrong_segment_manifest.position_segment_count;
        bioseq::parallelism::memory_admission_controller wrong_segment_admission;
        const auto wrong_segment_reader = prepared_minimizer_fragment_reader::open_compact_direct(
            wrong_segment_admission,
            generation->head,
            wrong_segment_manifest,
            test_grouped_soa_expected_storage_contract(build_cap),
            runtime_max_occ,
            own_fragment_compact_direct_test_index(direct_only_compact));
        REQUIRE_FALSE(wrong_segment_reader.has_value());
        REQUIRE(wrong_segment_reader.error() == minimizer_generation_encoding_error::segment_count);
        REQUIRE(wrong_segment_admission.reserved_bytes() == 0u);

        auto wrong_identity_view = direct_only_compact.view;
        ++wrong_identity_view.header.identity.generation_id;
        bioseq::parallelism::memory_admission_controller wrong_identity_admission;
        const auto wrong_identity_reader = prepared_minimizer_fragment_reader::open_compact_direct(
            wrong_identity_admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(build_cap),
            runtime_max_occ,
            own_fragment_compact_direct_test_index(direct_only_compact, wrong_identity_view));
        REQUIRE_FALSE(wrong_identity_reader.has_value());
        REQUIRE(wrong_identity_reader.error() ==
                minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(wrong_identity_admission.reserved_bytes() == 0u);

        // Keep an inaccessible grouped payload allocation beside the direct reader.
        // The direct-only API receives only the compact index.
        auto forbidden_grouped_soa_payload =
            make_sparse_mapped_test_payload(generation->manifest.payload_bytes);
        REQUIRE(forbidden_grouped_soa_payload.has_value());
        REQUIRE(mprotect(forbidden_grouped_soa_payload->owner.get(),
                         forbidden_grouped_soa_payload->size,
                         PROT_NONE) == 0);

        const std::uint8_t direct_only_posting_width =
            direct_only_compact.view.header.posting_width_bytes;
        bioseq::parallelism::memory_admission_controller direct_admission;
        // A one-byte budget forces direct open to keep no fragment scan plan.
        direct_admission.set_index_memory_cap_bytes(1u);
        auto direct_reader = prepared_minimizer_fragment_reader::open_compact_direct(
            direct_admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(build_cap),
            runtime_max_occ,
            own_fragment_compact_direct_test_index(direct_only_compact),
            selected_page_recorder.token());
        REQUIRE(direct_reader.has_value());
        REQUIRE(selected_page_recorder.record_count.load() == 0u);
        REQUIRE(direct_reader->ready());
        REQUIRE(direct_reader->compact_direct_ready());
        REQUIRE(direct_reader->compact_direct_only());
        REQUIRE(direct_reader->mapped_grouped_soa_payload_bytes() == 0u);
        REQUIRE(direct_reader->grouped_soa_directory_entry_count() == 0u);
        REQUIRE(direct_reader->fragment_block_count() ==
                generation->manifest.position_segment_count);
        REQUIRE(direct_reader->largest_fragment_block_entry_count() == 0u);
        REQUIRE(direct_reader->oversized_segment_block_count() == 0u);
        const auto direct_memory = direct_reader->memory_stats();
        REQUIRE(direct_memory.directory_capacity_bytes == 0u);
        REQUIRE(direct_memory.global_position_capacity_bytes == 0u);
        REQUIRE(direct_memory.control_payload_capacity_bytes == 0u);
        REQUIRE(direct_memory.ordinal_route_capacity_bytes == 0u);
        REQUIRE(direct_memory.owned_capacity_bytes == 0u);
        REQUIRE(direct_memory.retained_layout_equation_holds());
        REQUIRE(direct_reader->admitted_bytes() == 0u);
        REQUIRE(direct_admission.reserved_bytes() == 0u);
        direct_admission.set_index_memory_cap_bytes(0u);

        // Drop the external compact index owner to check that the reader keeps
        // its data alive. The protected grouped payload stays allocated.
        const std::uintptr_t direct_image_begin =
            reinterpret_cast<std::uintptr_t>(direct_only_compact.view.image.data());
        const std::uint64_t direct_postings_offset =
            direct_only_compact.view.header.postings_offset;
        direct_only_compact.image.reset();
        direct_only_compact.view = {};
        auto direct_scratch = query_wave_scratch::create(direct_admission);
        REQUIRE(direct_scratch.has_value());
        REQUIRE_THROWS_AS(direct_reader->prepare_fragment_anchor_batch(
                              direct_admission,
                              queries,
                              runtime_max_occ,
                              *direct_scratch,
                              /*baseline_query_plan_matcher_for_testing=*/false,
                              /*verify_compositional_order_for_testing=*/false,
                              minimizer_fragment_lookup_route::grouped_soa_control_scans),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(direct_scratch->wave_outstanding());

        std::array<std::vector<bioseq::chaining::anchor>, 2u> direct_expected;
        direct_expected[0u] = ordinary_seed_anchors(
            pairs, target_length, query0, query_length, runtime_max_occ, kmer_size);
        direct_expected[1u] = ordinary_seed_anchors(
            pairs, target_length, query1, query_length, runtime_max_occ, kmer_size);
        auto direct_batch = direct_reader->prepare_fragment_anchor_batch(
            direct_admission,
            queries,
            runtime_max_occ,
            *direct_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const std::array<std::uint32_t, 5u> exact_unique_hashes{7u, 8u, 9u, 10u, 11u};
        const compact_direct_query_set_stats expected_query_set_stats{
            .query_count = queries.size(),
            .query_occurrence_count = query0.size() + query1.size(),
            .unique_hash_count = exact_unique_hashes.size(),
            .group_key_set_checksum =
                compact_direct_query_set_checksum(exact_unique_hashes,
                                                  static_cast<std::uint32_t>(kmer_size),
                                                  10u,
                                                  static_cast<std::uint32_t>(runtime_max_occ)),
            .present = true,
        };
        REQUIRE(direct_batch.compact_direct_query_stats() == expected_query_set_stats);
        auto moved_direct_batch = std::move(direct_batch);
        REQUIRE_FALSE(direct_batch.compact_direct_query_stats().present);
        REQUIRE(moved_direct_batch.compact_direct_query_stats() == expected_query_set_stats);
        require_anchor_bytes_equal(moved_direct_batch.query_view(0u).ordered_fragment_seed_anchors(
                                       query0, query_length, runtime_max_occ, kmer_size),
                                   direct_expected[0u]);
        require_anchor_bytes_equal(moved_direct_batch.query_view(1u).ordered_fragment_seed_anchors(
                                       query1, query_length, runtime_max_occ, kmer_size),
                                   direct_expected[1u]);
        const auto& direct_evidence = moved_direct_batch.stats();
        REQUIRE(direct_evidence.compact_direct_waves == 1u);
        REQUIRE(direct_evidence.compact_direct_selection_shards == 2u);
        REQUIRE(direct_evidence.compact_direct_shard_query_layouts == 4u);
        REQUIRE(direct_evidence.compact_direct_layout_shards_completed == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_shards_completed == 2u);
        REQUIRE(direct_evidence.compact_direct_posting_visit_passes == 1u);
        REQUIRE(direct_evidence.compact_direct_layout_selection_callbacks == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_selection_callbacks == 2u);
        REQUIRE(direct_evidence.compact_direct_layout_spans_exposed == 4u);
        REQUIRE(direct_evidence.compact_direct_emission_spans_exposed == 4u);
        REQUIRE(direct_evidence.compact_direct_layout_descriptors_examined == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_descriptors_examined == 2u);
        REQUIRE(direct_evidence.compact_direct_traversal_rank_checkpoints_examined == 0u);
        REQUIRE(direct_evidence.compact_direct_traversal_membership_words_examined == 0u);
        REQUIRE(direct_evidence.compact_direct_rejected_posting_reads == 0u);
        REQUIRE(direct_evidence.compact_direct_target_control_scan_bytes == 0u);
        REQUIRE(direct_evidence.compact_direct_target_control_scan_passes == 0u);
        REQUIRE(direct_evidence.compact_direct_fallbacks == 0u);
        REQUIRE(direct_evidence.control_count_pass_ns == 0u);
        REQUIRE(direct_evidence.control_count_bytes == 0u);
        REQUIRE(direct_evidence.pass2_control_bytes == 0u);
        REQUIRE(direct_evidence.pass3_control_bytes == 0u);
        REQUIRE(direct_evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(direct_evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(direct_evidence.pass2_position_bytes == 0u);
        REQUIRE(direct_evidence.pass3_position_bytes == 4u * direct_only_posting_width);
        REQUIRE(direct_evidence.anchors_constructed == 8u);
        REQUIRE(direct_evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(selected_page_recorder.overflow.load());
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        std::array<compact_direct_selected_page_range, 2u> advised_ranges{
            selected_page_recorder.records[0u],
            selected_page_recorder.records[1u],
        };
        std::ranges::sort(advised_ranges, {}, &compact_direct_selected_page_range::begin);
        REQUIRE(advised_ranges[0u] == compact_direct_selected_page_range{
                                          .begin = direct_image_begin + direct_postings_offset,
                                          .end = direct_image_begin + direct_postings_offset + 8u,
                                      });
        REQUIRE(advised_ranges[1u] ==
                compact_direct_selected_page_range{
                    .begin = direct_image_begin + direct_postings_offset + 20u,
                    .end = direct_image_begin + direct_postings_offset + 28u,
                });
        const auto direct_memory_after_query = direct_reader->memory_stats();
        REQUIRE(direct_memory_after_query.directory_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.ordinal_route_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.owned_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.retained_layout_equation_holds());
        REQUIRE(direct_reader->admitted_bytes() == 0u);
        direct_scratch->complete_wave_after_join();

        const std::array<fragment_seed_query, 0u> empty_queries{};
        auto empty_direct_batch = direct_reader->prepare_fragment_anchor_batch(
            direct_admission,
            empty_queries,
            runtime_max_occ,
            *direct_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/false,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const compact_direct_query_set_stats expected_empty_stats{
            .query_count = 0u,
            .query_occurrence_count = 0u,
            .unique_hash_count = 0u,
            .group_key_set_checksum = compact_direct_query_set_checksum_prefix(
                static_cast<std::uint32_t>(kmer_size),
                10u,
                static_cast<std::uint32_t>(runtime_max_occ)),
            .present = true,
        };
        REQUIRE(empty_direct_batch.compact_direct_query_stats() == expected_empty_stats);
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        direct_scratch->complete_wave_after_join();
    }

    {
        bioseq::parallelism::memory_admission_controller zero_cap_admission;
        auto zero_cap_reader =
            open_grouped_soa_fragment_query_reader(zero_cap_admission,
                                                   *generation,
                                                   sink.rows,
                                                   0u,
                                                   own_fragment_compact_direct_test_index(compact));
        REQUIRE(zero_cap_reader.has_value());
        auto zero_cap_scratch = query_wave_scratch::create(zero_cap_admission);
        REQUIRE(zero_cap_scratch.has_value());
        auto zero_cap_batch = zero_cap_reader->prepare_fragment_anchor_batch(
            zero_cap_admission,
            queries,
            0,
            *zero_cap_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        REQUIRE_FALSE(zero_cap_batch.compact_direct_query_stats().present);
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            REQUIRE(zero_cap_batch.query_view(query_ordinal)
                        .ordered_fragment_seed_anchors(queries[query_ordinal].minimizers,
                                                       queries[query_ordinal].query_length,
                                                       0,
                                                       queries[query_ordinal].kmer_size)
                        .empty());
        }
        const auto& zero_evidence = zero_cap_batch.stats();
        REQUIRE(zero_evidence.compact_direct_waves == 1u);
        REQUIRE(zero_evidence.compact_direct_unique_query_groups == 0u);
        REQUIRE(zero_evidence.compact_direct_unique_lookups == 0u);
        REQUIRE(zero_evidence.compact_direct_selected_hashes == 0u);
        REQUIRE(zero_evidence.compact_direct_selected_postings == 0u);
        REQUIRE(zero_evidence.compact_direct_selection_shards == 0u);
        REQUIRE(zero_evidence.compact_direct_shard_query_layouts == 0u);
        REQUIRE(zero_evidence.compact_direct_layout_shards_completed == 0u);
        REQUIRE(zero_evidence.compact_direct_emission_shards_completed == 0u);
        REQUIRE(zero_evidence.compact_direct_posting_visit_passes == 0u);
        REQUIRE(zero_evidence.compact_direct_plan_bytes == 0u);
        REQUIRE(zero_evidence.query_plan_allocations == 0u);
        REQUIRE(zero_evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(zero_evidence.compact_direct_lookup_ns == 0u);
        REQUIRE(zero_evidence.control_count_pass_ns == 0u);
        REQUIRE(zero_evidence.control_count_bytes == 0u);
        REQUIRE(zero_evidence.pass2_position_bytes == 0u);
        REQUIRE(zero_evidence.pass3_position_bytes == 0u);
        REQUIRE(zero_evidence.anchors_constructed == 0u);
        REQUIRE(zero_evidence.exact_final_bytes == 0u);
        const auto zero_snapshot = zero_cap_scratch->occurrence_capacity_snapshot();
        REQUIRE(zero_snapshot.packed_size == 0u);
        REQUIRE(zero_snapshot.wide_size == 0u);
        REQUIRE(zero_snapshot.query_plan_size == 0u);
        zero_cap_scratch->complete_wave_after_join();
    }

    const std::uint8_t direct_posting_width = compact.view.header.posting_width_bytes;
    auto grouped_soa_payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    auto protected_payload = make_sparse_mapped_test_payload(grouped_soa_payload->size());
    REQUIRE(protected_payload.has_value());
    std::copy(grouped_soa_payload->begin(),
              grouped_soa_payload->end(),
              protected_payload->bytes().begin());
    const auto protected_payload_bytes = std::span<const std::byte>{protected_payload->bytes()};
    void* const protected_payload_address = protected_payload->owner.get();
    const std::size_t protected_payload_size = protected_payload->size;

    bioseq::parallelism::memory_admission_controller admission;
    std::shared_ptr<const void> protected_payload_owner = protected_payload->owner;
    auto reader = prepared_minimizer_fragment_reader::open(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(build_cap),
        generation->directory.encoded_bytes(),
        runtime_max_occ,
        std::move(protected_payload_owner),
        protected_payload_bytes,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    REQUIRE(reader->compact_direct_ready());
    // Make the grouped payload unreadable and drop the external owners.
    // Direct lookup must use the compact image kept alive by the reader.
    REQUIRE(mprotect(protected_payload_address, protected_payload_size, PROT_NONE) == 0);
    protected_payload->owner.reset();
    grouped_soa_payload.reset();
    compact.image.reset();
    compact.view = {};
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    std::array<std::vector<bioseq::chaining::anchor>, 2u> expected;
    expected[0u] = ordinary_seed_anchors(
        pairs, target_length, query0, query_length, runtime_max_occ, kmer_size);
    expected[1u] = ordinary_seed_anchors(
        pairs, target_length, query1, query_length, runtime_max_occ, kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    require_anchor_bytes_equal(batch.query_view(0u).ordered_fragment_seed_anchors(
                                   query0, query_length, runtime_max_occ, kmer_size),
                               expected[0u]);
    require_anchor_bytes_equal(batch.query_view(1u).ordered_fragment_seed_anchors(
                                   query1, query_length, runtime_max_occ, kmer_size),
                               expected[1u]);
    REQUIRE(expected[0u].size() == 4u);
    REQUIRE(expected[1u].size() == 4u);
    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_waves == 1u);
    REQUIRE(evidence.compact_direct_unique_query_groups == 5u);
    REQUIRE(evidence.compact_direct_unique_lookups == 5u);
    REQUIRE(evidence.compact_direct_selected_hashes == 2u);
    REQUIRE(evidence.compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(evidence.compact_direct_runtime_rejected_postings == 3u);
    REQUIRE(evidence.compact_direct_selected_postings == 4u);
    REQUIRE(evidence.compact_direct_selection_shards == 2u);
    REQUIRE(evidence.compact_direct_shard_query_layouts == 4u);
    REQUIRE(evidence.compact_direct_layout_shards_completed == 2u);
    REQUIRE(evidence.compact_direct_emission_shards_completed == 2u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == 4u);
    REQUIRE(evidence.compact_direct_layout_selection_callbacks == 2u);
    REQUIRE(evidence.compact_direct_emission_selection_callbacks == 2u);
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 4u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 4u);
    REQUIRE(evidence.compact_direct_layout_descriptors_examined == 2u);
    REQUIRE(evidence.compact_direct_emission_descriptors_examined == 2u);
    REQUIRE(evidence.compact_direct_traversal_rank_checkpoints_examined == 0u);
    REQUIRE(evidence.compact_direct_traversal_membership_words_examined == 0u);
    REQUIRE(evidence.compact_direct_rejected_posting_reads == 0u);
    REQUIRE(evidence.compact_direct_target_control_scan_bytes == 0u);
    REQUIRE(evidence.compact_direct_target_control_scan_passes == 0u);
    REQUIRE(evidence.compact_direct_fallbacks == 0u);
    REQUIRE(evidence.compact_direct_plan_bytes == 2u * sizeof(compact_hash32_unique_selection));
    REQUIRE(evidence.compact_direct_plan_reserve_calls == 1u);
    REQUIRE(evidence.compact_direct_plan_reserve_requested_elements == 5u);
    REQUIRE(evidence.compact_direct_plan_initial_capacity_elements == 5u);
    REQUIRE(evidence.compact_direct_plan_fill_reallocations == 0u);
    REQUIRE(evidence.compact_direct_plan_compaction_allocations == 1u);
    REQUIRE(evidence.compact_direct_plan_storage_allocations == 2u);
    REQUIRE(evidence.compact_direct_plan_retained_capacity_elements == 2u);
    REQUIRE(evidence.compact_direct_plan_retained_capacity_bytes ==
            evidence.compact_direct_plan_bytes);
    REQUIRE(evidence.peak_compact_direct_plan_capacity_bytes ==
            7u * sizeof(compact_hash32_unique_selection));
    REQUIRE(evidence.compact_direct_lookup_ns > 0u);
    REQUIRE(evidence.control_count_pass_ns == 0u);
    REQUIRE(evidence.control_count_bytes == 0u);
    REQUIRE(evidence.pass2_control_bytes == 0u);
    REQUIRE(evidence.pass3_control_bytes == 0u);
    REQUIRE(evidence.pass2_accepted_prefix_bytes == 0u);
    REQUIRE(evidence.pass3_accepted_prefix_bytes == 0u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == 4u * direct_posting_width);
    REQUIRE(evidence.anchors_constructed == 8u);
    REQUIRE(evidence.exact_final_bytes == 8u * sizeof(bioseq::chaining::anchor));
    REQUIRE(evidence.forbidden_buffers_are_zero());
    const auto snapshot = scratch->occurrence_capacity_snapshot();
    REQUIRE(snapshot.route == query_occurrence_route::packed);
    REQUIRE(snapshot.query_plan_size == 5u);
    REQUIRE(evidence.peak_query_plan_capacity_bytes ==
            static_cast<std::uint64_t>(snapshot.query_plan_capacity) * 16u);
    REQUIRE(snapshot.query_membership_filter_capacity_bytes == 0u);
    REQUIRE(snapshot.query_group_offset_capacity == 0u);
    REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
    scratch->complete_wave_after_join();

    const auto retained_before_plan_boundary = admission.reserved_bytes();
    const auto capacities_before_plan_boundary = scratch->occurrence_capacity_snapshot();
    constexpr std::uint64_t admitted_unique_capacity = 4u;
    const std::uint64_t admitted_plan_peak =
        2u * admitted_unique_capacity * sizeof(compact_hash32_unique_selection);
    const std::uint64_t next_plan_metadata = queries.size() * sizeof(prepared_query_anchors);
    admission.set_index_memory_cap_bytes(retained_before_plan_boundary + next_plan_metadata +
                                         admitted_plan_peak);
    // Five unique hashes exceed the budget for four. Rejection must leave
    // the scratch buffers and their memory reservations unchanged.
    REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(
                          admission,
                          queries,
                          runtime_max_occ,
                          *scratch,
                          /*baseline_query_plan_matcher_for_testing=*/false,
                          /*verify_compositional_order_for_testing=*/false,
                          minimizer_fragment_lookup_route::direct_compact_hash_lookup),
                      persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
    REQUIRE(admission.reserved_bytes() == retained_before_plan_boundary);
    const auto capacities_after_plan_boundary = scratch->occurrence_capacity_snapshot();
    REQUIRE(capacities_after_plan_boundary.route == capacities_before_plan_boundary.route);
    REQUIRE(capacities_after_plan_boundary.packed_size ==
            capacities_before_plan_boundary.packed_size);
    REQUIRE(capacities_after_plan_boundary.packed_capacity ==
            capacities_before_plan_boundary.packed_capacity);
    REQUIRE(capacities_after_plan_boundary.wide_size == capacities_before_plan_boundary.wide_size);
    REQUIRE(capacities_after_plan_boundary.wide_capacity ==
            capacities_before_plan_boundary.wide_capacity);
    REQUIRE(capacities_after_plan_boundary.query_plan_size ==
            capacities_before_plan_boundary.query_plan_size);
    REQUIRE(capacities_after_plan_boundary.query_plan_capacity ==
            capacities_before_plan_boundary.query_plan_capacity);
    REQUIRE(capacities_after_plan_boundary.query_plan_reserve_request ==
            capacities_before_plan_boundary.query_plan_reserve_request);
    REQUIRE(capacities_after_plan_boundary.query_group_offset_size ==
            capacities_before_plan_boundary.query_group_offset_size);
    REQUIRE(capacities_after_plan_boundary.query_group_offset_capacity ==
            capacities_before_plan_boundary.query_group_offset_capacity);
    REQUIRE(capacities_after_plan_boundary.aggregate_occurrence_capacity_bytes ==
            capacities_before_plan_boundary.aggregate_occurrence_capacity_bytes);
    REQUIRE(capacities_after_plan_boundary.query_membership_filter_capacity_bytes ==
            capacities_before_plan_boundary.query_membership_filter_capacity_bytes);
    REQUIRE(capacities_after_plan_boundary.occurrence_radix_invoked ==
            capacities_before_plan_boundary.occurrence_radix_invoked);
    REQUIRE(capacities_after_plan_boundary.bucket_direct_matcher_active ==
            capacities_before_plan_boundary.bucket_direct_matcher_active);

    admission.set_index_memory_cap_bytes((std::numeric_limits<std::uint64_t>::max)());
    auto reused_batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/false,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    REQUIRE(reused_batch.stats().occurrence_allocations == 0u);
    REQUIRE(reused_batch.stats().query_plan_allocations == 0u);
    REQUIRE(reused_batch.stats().compact_direct_plan_fill_reallocations == 0u);
    REQUIRE(reused_batch.stats().compact_direct_plan_storage_allocations == 2u);
    const auto reused_snapshot = scratch->occurrence_capacity_snapshot();
    REQUIRE(reused_batch.stats().peak_query_plan_capacity_bytes ==
            static_cast<std::uint64_t>(reused_snapshot.query_plan_capacity) * 16u);
    scratch->complete_wave_after_join();

    REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(
                          admission,
                          queries,
                          runtime_max_occ,
                          *scratch,
                          /*baseline_query_plan_matcher_for_testing=*/true,
                          /*verify_compositional_order_for_testing=*/false,
                          minimizer_fragment_lookup_route::direct_compact_hash_lookup),
                      persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
    scratch->release_retained_capacity();
    REQUIRE(scratch->admitted_bytes() == 0u);
}

TEST_CASE("compact-direct single-query occurrence emission covers P32 spans",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t max_occ = 4u;
    constexpr std::uint64_t target_length = 400u;
    constexpr bioseq::storage::sequence_size_t query_length = 200u;
    constexpr std::int32_t kmer_size = 15;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        // One inline 32-bit position on each strand.
        {7u, packed(10u, 0u)},
        {8u, packed(20u, 1u)},
        // One query minimizer matches four stored positions across both strands.
        {9u, packed(30u, 0u)},
        {9u, packed(31u, 0u)},
        {9u, packed(32u, 1u)},
        {9u, packed(33u, 1u)},
        // Repeated query minimizers exercise the general emission loop.
        {10u, packed(40u, 0u)},
        {10u, packed(41u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, max_occ));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, max_occ);
    REQUIRE(compact.view.header.posting_width_bytes == 4u);
    REQUIRE(compact.view.header.inline_singleton_count == 2u);
    REQUIRE(compact.view.header.materialized_posting_count == 6u);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{8u, 30u, 1u},
        minimap_sketch_data{9u, 40u, 0u},
        minimap_sketch_data{10u, 50u, 0u},
        minimap_sketch_data{10u, 70u, 1u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = query,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected =
        ordinary_seed_anchors(pairs, target_length, query, query_length, max_occ, kmer_size);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(max_occ),
        max_occ,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual =
        batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, max_occ, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == 10u);

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_selected_hashes == 4u);
    REQUIRE(evidence.compact_direct_selected_postings == 8u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == 8u);
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 6u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 6u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == 8u * 4u);
    REQUIRE(evidence.anchors_constructed == actual.size());
    REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    scratch->complete_wave_after_join();
}

TEST_CASE("compact-direct single-query occurrence emission covers P64 spans",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t max_occ = 4u;
    constexpr std::uint64_t high_bp = (std::uint64_t{1u} << 31u) + 10u;
    constexpr std::uint64_t target_length = high_bp + 1'000u;
    constexpr bioseq::storage::sequence_size_t query_length = 200u;
    constexpr std::int32_t kmer_size = 15;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        // Even one 64-bit position needs a separate posting list.
        {7u, packed(high_bp, 0u)},
        // One query minimizer matches four 64-bit positions across both strands.
        {8u, packed(100u, 0u)},
        {8u, packed(101u, 0u)},
        {8u, packed(high_bp + 1u, 1u)},
        {8u, packed(high_bp + 2u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, max_occ));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, max_occ);
    REQUIRE(compact.view.header.posting_width_bytes == 8u);
    REQUIRE(compact.view.header.inline_singleton_count == 0u);
    REQUIRE(compact.view.header.materialized_posting_count == pairs.size());

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{8u, 40u, 1u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = query,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected =
        ordinary_seed_anchors(pairs, target_length, query, query_length, max_occ, kmer_size);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(max_occ),
        max_occ,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual =
        batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, max_occ, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == pairs.size());

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_selected_hashes == 2u);
    REQUIRE(evidence.compact_direct_selected_postings == pairs.size());
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == pairs.size());
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 3u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 3u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == pairs.size() * 8u);
    REQUIRE(evidence.anchors_constructed == actual.size());
    REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    scratch->complete_wave_after_join();
}

TEST_CASE("compact-direct selection shards preserve P32 differential parity",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t build_cap = 4u;
    constexpr std::int32_t runtime_max_occ = 2;
    constexpr std::uint64_t target_length = 10'000u;
    constexpr std::uint64_t selected_hash_count = 40u;
    constexpr std::uint64_t heavy_hash = 500u;
    constexpr bioseq::storage::sequence_size_t query_length = 300u;
    constexpr std::int32_t kmer_size = 15;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(selected_hash_count + 3u);
    for (std::uint64_t hash = 1u; hash <= selected_hash_count; ++hash) {
        pairs.emplace_back(hash, packed(100u + 3u * hash, hash & 1u));
    }
    pairs.emplace_back(heavy_hash, packed(1'000u, 0u));
    pairs.emplace_back(heavy_hash, packed(1'001u, 0u));
    pairs.emplace_back(heavy_hash, packed(1'002u, 1u));

    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, build_cap));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, build_cap);
    REQUIRE(compact.view.header.posting_width_bytes == 4u);

    std::vector<minimap_sketch_data> query0;
    std::vector<minimap_sketch_data> query1;
    query0.reserve(selected_hash_count + 2u);
    query1.reserve(selected_hash_count + 1u);
    for (std::uint64_t hash = 1u; hash <= selected_hash_count; ++hash) {
        query0.push_back(minimap_sketch_data{
            hash,
            20u + hash,
            static_cast<std::uint8_t>((hash + 1u) & 1u),
        });
    }
    // Repeat an accepted query minimizer to check duplicate anchors across
    // work groups. Each group writes through its own output cursor.
    query0.push_back(query0[6u]);
    query0.push_back(minimap_sketch_data{heavy_hash, 90u, 0u});
    for (std::uint64_t hash = selected_hash_count; hash != 0u; --hash) {
        query1.push_back(minimap_sketch_data{
            hash,
            140u + hash,
            static_cast<std::uint8_t>(hash & 1u),
        });
    }
    query1.push_back(minimap_sketch_data{heavy_hash, 200u, 1u});
    const std::array queries{
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

    bioseq::parallelism::memory_admission_controller admission;
    selected_page_advice_recorder selected_page_recorder;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(build_cap),
        runtime_max_occ,
        own_fragment_compact_direct_test_index(compact),
        selected_page_recorder.token());
    REQUIRE(reader.has_value());
    REQUIRE(selected_page_recorder.record_count.load() == 0u);
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    const auto expected0 = ordinary_seed_anchors(
        pairs, target_length, query0, query_length, runtime_max_occ, kmer_size);
    const auto expected1 = ordinary_seed_anchors(
        pairs, target_length, query1, query_length, runtime_max_occ, kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual0 = batch.query_view(0u).ordered_fragment_seed_anchors(
        query0, query_length, runtime_max_occ, kmer_size);
    const auto actual1 = batch.query_view(1u).ordered_fragment_seed_anchors(
        query1, query_length, runtime_max_occ, kmer_size);
    require_anchor_bytes_equal(actual0, expected0);
    require_anchor_bytes_equal(actual1, expected1);
    REQUIRE(actual0.size() == selected_hash_count + 1u);
    REQUIRE(actual1.size() == selected_hash_count);

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_unique_query_groups == selected_hash_count + 1u);
    REQUIRE(evidence.compact_direct_selected_hashes == selected_hash_count);
    REQUIRE(evidence.compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(evidence.compact_direct_runtime_rejected_postings == 3u);
    REQUIRE(evidence.compact_direct_selected_postings == selected_hash_count);
    REQUIRE(evidence.compact_direct_selection_shards == 32u);
    REQUIRE(evidence.compact_direct_shard_query_layouts == 64u);
    REQUIRE(evidence.compact_direct_layout_shards_completed == 32u);
    REQUIRE(evidence.compact_direct_emission_shards_completed == 32u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == selected_hash_count);
    REQUIRE(evidence.compact_direct_layout_selection_callbacks == selected_hash_count);
    REQUIRE(evidence.compact_direct_emission_selection_callbacks == selected_hash_count);
    REQUIRE(evidence.compact_direct_layout_descriptors_examined == selected_hash_count);
    REQUIRE(evidence.compact_direct_emission_descriptors_examined == selected_hash_count);
    REQUIRE(evidence.compact_direct_traversal_rank_checkpoints_examined == 0u);
    REQUIRE(evidence.compact_direct_traversal_membership_words_examined == 0u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes ==
            selected_hash_count * compact.view.header.posting_width_bytes);
    REQUIRE(evidence.anchors_constructed == actual0.size() + actual1.size());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    REQUIRE_FALSE(selected_page_recorder.overflow.load());
    REQUIRE(selected_page_recorder.record_count.load() == 0u);
    scratch->complete_wave_after_join();
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
