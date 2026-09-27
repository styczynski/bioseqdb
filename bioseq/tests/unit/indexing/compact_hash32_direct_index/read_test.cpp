/**
 * @file read_test.cpp
 * @brief Checks direct-index lookups, occurrence limits and reads of selected positions.
 */

#include <cstring>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.tests.unit.indexing.compact_hash32_direct_index_support;

namespace idx = bioseq::indexing;
using namespace bioseq::tests::unit::indexing::compact_hash32_direct_index_test_support;

TEST_CASE("direct hash32 index differentially resolves absent singleton duplicate and "
          "unsorted query occurrences",
          "[component:bioseq][layer:unit][feature:index]") {
    const std::vector<idx::direct_index_occurrence> input{
        {.hash = 0u, .target_bp = 3u, .target_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
         .target_bp = 11u,
         .target_strand_one = true},
        // Keep duplicate occurrences in the stored counts and position lists.
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
         .target_bp = 11u,
         .target_strand_one = true},
        {.hash = 42u, .target_bp = 90u, .target_strand_one = true},
        {.hash = 42u, .target_bp = 10u, .target_strand_one = false},
        {.hash = 42u, .target_bp = 20u, .target_strand_one = false},
    };
    const auto build_options = options(1'000u, 0u, 8u);
    const auto plan = idx::plan_direct_inverted_index(input, build_options);
    REQUIRE(plan.has_value());
    CHECK(plan->posting_width_bytes == 4u);
    CHECK(plan->unique_hash_count == 3u);
    CHECK(plan->occurrence_count == input.size());
    CHECK(plan->occupancy_bytes == plan->bucket_count / 8u);
    CHECK(plan->rank_bytes ==
          ((plan->bucket_count + idx::k_direct_inverted_index_rank_group_buckets - 1u) /
               idx::k_direct_inverted_index_rank_group_buckets +
           1u) *
              8u);
    CHECK(plan->controls_bytes ==
          plan->unique_hash_count * idx::k_direct_inverted_index_control_bytes);
    CHECK(plan->summaries_bytes == 0u);
    CHECK(plan->postings_bytes == input.size() * 4u);
    CHECK(plan->image_bytes == plan->postings_offset + plan->postings_bytes);
    CHECK(plan->estimated_build_scratch_bytes != 0u);

    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    const std::vector<idx::direct_index_query_occurrence> queries{
        {.hash = 42u, .query_strand_one = true},
        {.hash = 999u, .query_strand_one = false},
        {.hash = 0u, .query_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(), .query_strand_one = false},
        {.hash = 42u, .query_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(), .query_strand_one = true},
    };
    const auto query_plan = idx::query_direct_inverted_index(view, queries);
    REQUIRE(query_plan.has_value());
    const oracle_type oracle = make_oracle(input);
    require_matches_oracle(
        *query_plan, queries, oracle, (std::numeric_limits<std::uint64_t>::max)());
    // Repeated queries for one hash share the same target position list.
    CHECK(query_plan->selections.size() == oracle.size());
    CHECK(query_plan->stats.unique_selected_hash_count == oracle.size());
    CHECK(query_plan->stats.selected_posting_count == input.size());

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(query_plan->selections.size()),
    };
    const auto visited =
        idx::visit_direct_inverted_index_postings(view, *query_plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == input.size());
    CHECK(collected.callback_count == input.size());
    for (std::size_t selection = 0u; selection < query_plan->selections.size(); ++selection) {
        const auto hash = query_plan->selections[selection].hash;
        REQUIRE(oracle.contains(hash));
        CHECK(collected.by_selection[selection] == expected_postings(oracle.at(hash), 0u));
    }
}

TEST_CASE("direct hash32 summaries give exact both-strand layouts and visit heavy "
          "selected postings once",
          "[component:bioseq][layer:unit][feature:index]") {
    std::vector<idx::direct_index_occurrence> input;
    constexpr std::uint64_t heavy_count = 1'001u;
    input.reserve(heavy_count + 4u);
    for (std::uint64_t ordinal = 0u; ordinal < heavy_count; ++ordinal) {
        input.push_back(idx::direct_index_occurrence{
            .hash = 7u,
            .target_bp = (ordinal * 37u) % 9'900u,
            .target_strand_one = ordinal % 3u == 0u,
        });
    }
    for (std::uint64_t ordinal = 0u; ordinal < 4u; ++ordinal) {
        input.push_back(idx::direct_index_occurrence{
            .hash = 9u,
            .target_bp = 50u + ordinal * 100u,
            .target_strand_one = ordinal >= 2u,
        });
    }
    const auto build_options = options(10'000u, 100u, 8u);
    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    REQUIRE(view.has_run_summaries());
    CHECK(view.header.run_summary_count > view.header.unique_hash_count);
    const oracle_type oracle = make_oracle(input);

    const std::vector<idx::direct_index_query_occurrence> heavy_queries{
        {.hash = 7u, .query_strand_one = false},
        {.hash = 7u, .query_strand_one = true},
    };
    const auto accepted = idx::query_direct_inverted_index(view, heavy_queries, heavy_count);
    REQUIRE(accepted.has_value());
    require_matches_oracle(*accepted, heavy_queries, oracle, heavy_count);
    REQUIRE(accepted->selections.size() == 1u);
    CHECK(accepted->stats.selected_posting_count == heavy_count);
    std::set<std::uint64_t> heavy_fragments;
    for (const auto& occurrence : oracle.at(7u)) {
        heavy_fragments.insert(occurrence.target_bp / 100u);
    }
    CHECK(accepted->stats.run_summaries_examined == heavy_fragments.size());

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited =
        idx::visit_direct_inverted_index_postings(view, *accepted, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    // Two query orientations still read the selected target postings once.
    CHECK(visited->postings_visited == heavy_count);
    CHECK(collected.callback_count == heavy_count);
    CHECK(collected.by_selection.front() == expected_postings(oracle.at(7u), 100u));

    const auto rejected = idx::query_direct_inverted_index(view, heavy_queries, heavy_count - 1u);
    REQUIRE(rejected.has_value());
    require_matches_oracle(*rejected, heavy_queries, oracle, heavy_count - 1u);
    CHECK(rejected->selections.empty());
    CHECK(rejected->spans.empty());
    CHECK(rejected->stats.selected_posting_count == 0u);

    const std::array boundary_query{
        idx::direct_index_query_occurrence{.hash = 9u, .query_strand_one = false},
    };
    const auto boundary_accepted = idx::query_direct_inverted_index(view, boundary_query, 4u);
    const auto boundary_rejected = idx::query_direct_inverted_index(view, boundary_query, 3u);
    REQUIRE(boundary_accepted.has_value());
    REQUIRE(boundary_rejected.has_value());
    CHECK(boundary_accepted->results.front().positions_selected);
    CHECK_FALSE(boundary_rejected->results.front().positions_selected);
    CHECK(boundary_rejected->results.front().global_count == 4u);
}

TEST_CASE("direct hash32 point lookup examines a probe chain rather than every target "
          "control",
          "[component:bioseq][layer:unit][feature:index]") {
    std::vector<idx::direct_index_occurrence> input;
    for (std::uint32_t hash = 0u; hash < 128u; ++hash) {
        input.push_back(idx::direct_index_occurrence{
            .hash = hash,
            .target_bp = hash,
            .target_strand_one = (hash & 1u) != 0u,
        });
    }
    const auto build_options = options(1'000u);
    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    const std::array query{
        idx::direct_index_query_occurrence{
            // Hash zero maps to slot zero and is inserted first, giving a predictable short lookup.
            .hash = 0u,
            .query_strand_one = false},
    };
    const auto plan = idx::query_direct_inverted_index(view, query);
    REQUIRE(plan.has_value());
    REQUIRE(plan->results.front().present);
    CHECK(plan->stats.control_records_examined < view.header.unique_hash_count);
    CHECK(plan->stats.dictionary_slots_examined < view.header.bucket_count);
}

TEST_CASE("compact exact-domain hash32 monotonic batch derives dense ranks and omits "
          "globally rejected max-occ runs",
          "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 0u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(20u, false)},
                },
                compact_test_run{
                    .header = {.hash = 255u, .global_count = 3u, .target_strand_zero_count = 2u},
                    .encoded_positions =
                        {
                            encoded_position(1u, false),
                            encoded_position(9u, false),
                            encoded_position(5u, true),
                        },
                },
                compact_test_run{
                    .header = {.hash = 256u, .global_count = 2u, .target_strand_zero_count = 0u},
                    .encoded_positions =
                        {
                            encoded_position(10u, true),
                            encoded_position(20u, true),
                        },
                },
                // This hash occurs four times, above the stored limit of three.
                // Omit its membership bit, descriptor and positions.
                compact_test_run{
                    .header = {.hash = 300u, .global_count = 4u, .target_strand_zero_count = 2u},
                    .encoded_positions =
                        {
                            encoded_position(2u, false),
                            encoded_position(4u, false),
                            encoded_position(6u, true),
                            encoded_position(8u, true),
                        },
                },
                compact_test_run{
                    .header = {.hash = 511u, .global_count = 3u, .target_strand_zero_count = 1u},
                    .encoded_positions =
                        {
                            encoded_position(3u, false),
                            encoded_position(7u, true),
                            encoded_position(11u, true),
                        },
                },
            },
    };
    const auto build_options = compact_options(100u, 3u, 9u);
    const auto planned =
        idx::plan_compact_hash32_direct_index(compact_source_ref(source), build_options);
    REQUIRE(planned.has_value());
    CHECK(planned->source_hash_count == 5u);
    CHECK(planned->source_posting_count == 13u);
    CHECK(planned->accepted_hash_count == 4u);
    CHECK(planned->accepted_posting_count == 9u);
    CHECK(planned->layout_revision == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(planned->descriptor_width_bytes == idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(planned->inline_singleton_count == 1u);
    CHECK(planned->materialized_hash_count == 3u);
    CHECK(planned->materialized_posting_count == 8u);
    CHECK(planned->rejected_hash_count == 1u);
    CHECK(planned->rejected_posting_count == 4u);
    CHECK(planned->descriptors_bytes == 4u * 8u);
    CHECK(planned->postings_bytes == 8u * 4u);

    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.sorted_stream_crc64 == planned->sorted_stream_crc64);
    const std::vector<idx::compact_hash32_query_occurrence> queries{
        {.hash = 0u, .query_strand_one = false},
        {.hash = 1u, .query_strand_one = false},
        {.hash = 255u, .query_strand_one = false},
        {.hash = 255u, .query_strand_one = true},
        {.hash = 256u, .query_strand_one = false},
        {.hash = 300u, .query_strand_one = false},
        {.hash = 511u, .query_strand_one = true},
    };
    const auto query_plan = idx::query_compact_hash32_direct_index(view, queries, 3u);
    REQUIRE(query_plan.has_value());
    REQUIRE(query_plan->results.size() == queries.size());
    CHECK(query_plan->results[0].accepted);
    CHECK(query_plan->results[0].dense_ordinal == 0u);
    CHECK_FALSE(query_plan->results[1].accepted);
    CHECK(query_plan->results[2].accepted);
    CHECK(query_plan->results[2].dense_ordinal == 1u);
    CHECK(query_plan->results[2].forward_count == 2u);
    CHECK(query_plan->results[2].reverse_count == 1u);
    CHECK(query_plan->results[3].selection_ordinal == query_plan->results[2].selection_ordinal);
    CHECK(query_plan->results[3].forward_count == 1u);
    CHECK(query_plan->results[3].reverse_count == 2u);
    CHECK(query_plan->results[4].dense_ordinal == 2u);
    CHECK(query_plan->results[4].forward_count == 0u);
    CHECK(query_plan->results[4].reverse_count == 2u);
    CHECK_FALSE(query_plan->results[5].accepted);
    CHECK(query_plan->results[6].dense_ordinal == 3u);
    CHECK(query_plan->stats.rank_checkpoints_examined == 2u);
    CHECK(query_plan->stats.descriptors_examined == 4u);
    CHECK(query_plan->stats.unique_selected_hash_count == 4u);
    CHECK(query_plan->stats.selected_posting_count == 9u);
    CHECK(query_plan->stats.selected_inline_singleton_count == 1u);
    CHECK(query_plan->stats.selected_materialized_hash_count == 3u);
    CHECK(query_plan->stats.selected_materialized_posting_count == 8u);

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(query_plan->selections.size()),
    };
    const auto visited = idx::visit_compact_hash32_direct_index_postings(
        view, *query_plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == 9u);
    REQUIRE(collected.by_selection.size() == 4u);
    CHECK(collected.by_selection[0] == source.runs[0].encoded_positions);
    CHECK(collected.by_selection[1] == source.runs[1].encoded_positions);
    CHECK(collected.by_selection[2] == source.runs[2].encoded_positions);
    CHECK(collected.by_selection[3] == source.runs[4].encoded_positions);

    const std::array absent_query{
        idx::compact_hash32_query_occurrence{.hash = 1u, .query_strand_one = false},
    };
    const auto absent = idx::query_compact_hash32_direct_index(view, absent_query, 3u);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->results.front().accepted);
    CHECK(absent->stats.rank_checkpoints_examined == 1u);
    CHECK(absent->stats.membership_words_examined == 1u);
    CHECK(absent->stats.descriptors_examined == 0u);

    const std::array unsorted{
        idx::compact_hash32_query_occurrence{.hash = 256u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{.hash = 255u, .query_strand_one = false},
    };
    const auto bad_order = idx::query_compact_hash32_direct_index(view, unsorted, 3u);
    REQUIRE_FALSE(bad_order.has_value());
    CHECK(bad_order.error() == idx::direct_inverted_index_error::order);
}

TEST_CASE("compact hash32 unique lookup caches one four-word membership rank group "
          "across present absent and boundary hashes",
          "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 0u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(1u, false)},
                },
                compact_test_run{
                    .header = {.hash = 63u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
                compact_test_run{
                    .header = {.hash = 64u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(3u, false)},
                },
                compact_test_run{
                    .header = {.hash = 127u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(4u, false)},
                },
                compact_test_run{
                    .header = {.hash = 128u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(5u, false)},
                },
                compact_test_run{
                    .header = {.hash = 191u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(6u, false)},
                },
                compact_test_run{
                    .header = {.hash = 192u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(7u, false)},
                },
                compact_test_run{
                    .header = {.hash = 255u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(8u, false)},
                },
                compact_test_run{
                    .header = {.hash = 256u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(9u, false)},
                },
                compact_test_run{
                    .header = {.hash = 319u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(10u, false)},
                },
                compact_test_run{
                    .header = {.hash = 320u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(11u, false)},
                },
                compact_test_run{
                    .header = {.hash = 383u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(12u, false)},
                },
                compact_test_run{
                    .header = {.hash = 384u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(13u, false)},
                },
                compact_test_run{
                    .header = {.hash = 447u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(14u, false)},
                },
                compact_test_run{
                    .header = {.hash = 448u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(15u, false)},
                },
                compact_test_run{
                    .header = {.hash = 511u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(16u, false)},
                },
            },
    };
    const auto build_options = compact_options(100u, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    const std::array<std::uint32_t, 26u> hashes{
        0u,   1u,   63u,  64u,  65u,  127u, 128u, 129u, 191u, 192u, 193u, 254u, 255u,
        256u, 257u, 319u, 320u, 321u, 383u, 384u, 385u, 447u, 448u, 449u, 510u, 511u,
    };

    const std::span<const std::uint32_t> first_rank_group{hashes.data(), 13u};
    const auto same_group =
        idx::query_compact_hash32_direct_index_unique(view, first_rank_group, 1u);
    REQUIRE(same_group.has_value());
    CHECK(same_group->selections.size() == 8u);
    CHECK(same_group->stats.rank_checkpoints_examined == 1u);
    // All thirteen lookups share one cached group of four membership words.
    CHECK(same_group->stats.membership_words_examined == 4u);
    CHECK(same_group->stats.descriptors_examined == 8u);

    const auto cached = idx::query_compact_hash32_direct_index_unique(view, hashes, 1u);
    REQUIRE(cached.has_value());
    CHECK(cached->selections.size() == source.runs.size());
    CHECK(cached->stats.rank_checkpoints_examined == 2u);
    CHECK(cached->stats.membership_words_examined == 8u);
    CHECK(cached->stats.descriptors_examined == source.runs.size());
    REQUIRE(cached->selections.size() == 16u);
    CHECK(cached->selections[7u].query_ordinal == 12u);
    CHECK(cached->selections[7u].dense_ordinal == 7u);
    CHECK(cached->selections[8u].query_ordinal == 13u);
    CHECK(cached->selections[8u].dense_ordinal == 8u);

    struct projected_hash_record {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(projected_hash_record) == 16u);
    std::array<projected_hash_record, 26u> projected_hashes{};
    for (std::size_t ordinal = 0u; ordinal < hashes.size(); ++ordinal) {
        projected_hashes[ordinal] = projected_hash_record{
            .hash = hashes[ordinal],
            .occurrence_ref_begin = static_cast<std::uint32_t>(ordinal),
            .occurrence_ref_count = static_cast<std::uint32_t>(ordinal + 1u),
            .global_occurrence_count = static_cast<std::uint32_t>(ordinal + 2u),
        };
    }
    std::size_t projected_hash_reads = 0u;
    const auto projected = idx::query_compact_hash32_direct_index_unique_projected(
        view,
        projected_hashes.size(),
        [&projected_hashes, &projected_hash_reads](std::size_t ordinal) noexcept -> std::uint32_t {
            ++projected_hash_reads;
            return projected_hashes[ordinal].hash;
        },
        1u);
    REQUIRE(projected.has_value());
    CHECK(projected_hash_reads == projected_hashes.size());
    CHECK(projected->stats == cached->stats);
    REQUIRE(projected->selections.size() == cached->selections.size());
    for (std::size_t ordinal = 0u; ordinal < cached->selections.size(); ++ordinal) {
        CHECK(projected->selections[ordinal].query_ordinal ==
              cached->selections[ordinal].query_ordinal);
        CHECK(projected->selections[ordinal].dense_ordinal ==
              cached->selections[ordinal].dense_ordinal);
    }
    const auto projected_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(view, *projected);
    REQUIRE(projected_accessor.has_value());
    CHECK(projected_accessor->selection_count() == projected->selections.size());

    std::vector<idx::compact_hash32_query_occurrence> occurrence_queries;
    occurrence_queries.reserve(hashes.size());
    for (const std::uint32_t hash : hashes) {
        occurrence_queries.push_back(idx::compact_hash32_query_occurrence{
            .hash = hash,
            .query_strand_one = false,
        });
    }
    const auto reference = idx::query_compact_hash32_direct_index(view, occurrence_queries, 1u);
    REQUIRE(reference.has_value());
    REQUIRE(reference->results.size() == hashes.size());
    std::size_t selected_ordinal = 0u;
    for (std::size_t query_ordinal = 0u; query_ordinal < hashes.size(); ++query_ordinal) {
        if (!reference->results[query_ordinal].accepted)
            continue;
        REQUIRE(selected_ordinal < cached->selections.size());
        CHECK(cached->selections[selected_ordinal].query_ordinal == query_ordinal);
        CHECK(cached->selections[selected_ordinal].dense_ordinal ==
              reference->results[query_ordinal].dense_ordinal);
        ++selected_ordinal;
    }
    CHECK(selected_ordinal == cached->selections.size());
}

TEST_CASE("compact exact-domain hash32 point lookup decodes one descriptor among many",
          "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source;
    for (std::uint32_t hash = 0u; hash < 64u; ++hash) {
        source.runs.push_back(compact_test_run{
            .header = {.hash = hash, .global_count = 1u, .target_strand_zero_count = 1u},
            .encoded_positions = {encoded_position(hash, false)},
        });
    }
    const auto build_options = compact_options(100u, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.layout_revision == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(view.header.descriptor_width_bytes ==
          idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(view.header.inline_singleton_count == 64u);
    CHECK(view.header.materialized_hash_count == 0u);
    CHECK(view.header.materialized_posting_count == 0u);
    CHECK(view.header.postings_bytes == 0u);
    REQUIRE(view.header.accepted_hash_count == 64u);

    const std::array present_query{
        idx::compact_hash32_query_occurrence{.hash = 63u, .query_strand_one = false},
    };
    const auto present = idx::query_compact_hash32_direct_index(view, present_query, 1u);
    REQUIRE(present.has_value());
    REQUIRE(present->results.front().accepted);
    CHECK(present->results.front().dense_ordinal == 63u);
    CHECK(present->stats.descriptors_examined == 1u);
    CHECK(present->stats.unique_selected_hash_count == 1u);

    const std::array absent_query{
        idx::compact_hash32_query_occurrence{.hash = 64u, .query_strand_one = false},
    };
    const auto absent = idx::query_compact_hash32_direct_index(view, absent_query, 1u);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->results.front().accepted);
    CHECK(absent->stats.descriptors_examined == 0u);
    CHECK(absent->stats.unique_selected_hash_count == 0u);
}

TEST_CASE("compact exact-domain hash32 applies runtime max-occ below the persisted cap "
          "without retaining or reading rejected postings under exact U+c admission",
          "[component:bioseq][layer:unit][feature:index]") {
    STATIC_REQUIRE(sizeof(idx::compact_hash32_unique_selection) == 8u);
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 2u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
                compact_test_run{
                    .header = {.hash = 5u, .global_count = 3u, .target_strand_zero_count = 2u},
                    .encoded_positions =
                        {
                            encoded_position(5u, false),
                            encoded_position(7u, false),
                            encoded_position(9u, true),
                        },
                },
                compact_test_run{
                    .header = {.hash = 7u, .global_count = 4u, .target_strand_zero_count = 2u},
                    .encoded_positions =
                        {
                            encoded_position(10u, false),
                            encoded_position(12u, false),
                            encoded_position(14u, true),
                            encoded_position(16u, true),
                        },
                },
            },
    };
    const auto build_options = compact_options(100u, 4u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.layout_revision == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(view.header.descriptor_width_bytes ==
          idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(view.header.inline_singleton_count == 1u);
    CHECK(view.header.materialized_hash_count == 2u);
    CHECK(view.header.materialized_posting_count == 7u);

    const std::array queries{
        idx::compact_hash32_query_occurrence{.hash = 1u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{.hash = 2u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{.hash = 5u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{.hash = 7u, .query_strand_one = false},
    };
    const auto lower = idx::query_compact_hash32_direct_index(view, queries, 2u);
    REQUIRE(lower.has_value());
    REQUIRE(lower->results.size() == queries.size());
    CHECK_FALSE(lower->results[0].accepted);
    CHECK(lower->results[1].accepted);
    CHECK_FALSE(lower->results[2].accepted);
    CHECK_FALSE(lower->results[3].accepted);
    CHECK(lower->selections.size() == 1u);
    CHECK(lower->stats.runtime_rejected_hash_count == 2u);
    CHECK(lower->stats.runtime_rejected_posting_count == 7u);
    CHECK(lower->stats.selected_posting_count == 1u);
    CHECK(lower->stats.rejected_posting_reads == 0u);

    const std::array<std::uint32_t, 4u> unique_hashes{1u, 2u, 5u, 7u};
    const auto sparse = idx::query_compact_hash32_direct_index_unique(view, unique_hashes, 2u);
    REQUIRE(sparse.has_value());
    REQUIRE(sparse->selections.size() == 1u);
    CHECK(sparse->selections.front().dense_ordinal == 0u);
    CHECK(sparse->selections.front().query_ordinal == 1u);
    CHECK(sparse->retained_selection_capacity_bytes() ==
          sparse->selections.capacity() * sizeof(idx::compact_hash32_unique_selection));
    CHECK(sparse->selections.capacity() == 1u);
    CHECK(sparse->stats.selection_reserve_call_count == 1u);
    CHECK(sparse->stats.selection_reserve_requested_elements == 4u);
    CHECK(sparse->stats.selection_initial_capacity_elements == 4u);
    CHECK(sparse->stats.selection_fill_reallocation_count == 0u);
    CHECK(sparse->stats.selection_compaction_allocation_count == 1u);
    CHECK(sparse->stats.selection_storage_allocation_count == 2u);
    CHECK(sparse->stats.selection_peak_capacity_bytes ==
          5u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(sparse->stats.selection_retained_capacity_elements == 1u);
    CHECK(sparse->stats.selection_retained_capacity_bytes ==
          sizeof(idx::compact_hash32_unique_selection));
    CHECK(sparse->stats.runtime_rejected_hash_count == 2u);
    CHECK(sparse->stats.runtime_rejected_posting_count == 7u);
    CHECK(sparse->stats.selected_posting_count == 1u);
    CHECK(sparse->stats.selected_inline_singleton_count == 1u);
    CHECK(sparse->stats.selected_materialized_hash_count == 0u);
    CHECK(sparse->stats.selected_materialized_posting_count == 0u);
    CHECK(sparse->stats.rejected_posting_reads == 0u);

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited = idx::visit_compact_hash32_direct_index_postings(
        view, *sparse, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == 1u);
    CHECK(collected.callback_count == 1u);
    CHECK(collected.by_selection.front() == source.runs.front().encoded_positions);

    std::uint64_t sparse_selection_callbacks = 0u;
    const auto sparse_bulk_visited = idx::visit_compact_hash32_direct_index_selection_spans(
        view,
        *sparse,
        [&sparse_selection_callbacks](const idx::compact_hash32_selection_posting_spans& selected) {
            ++sparse_selection_callbacks;
            return selected.selection_ordinal == 0u && selected.dense_ordinal == 0u &&
                   selected.query_ordinal == 1u && selected.global_count == 1u &&
                   selected.strand_zero_count == 1u && selected.span_count == 1u;
        });
    REQUIRE(sparse_bulk_visited.has_value());
    CHECK(sparse_selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->spans_exposed == 1u);
    CHECK(sparse_bulk_visited->postings_exposed == 1u);
    CHECK(sparse_bulk_visited->inline_selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->materialized_selection_callbacks == 0u);
    CHECK(sparse_bulk_visited->inline_postings_exposed == 1u);
    CHECK(sparse_bulk_visited->materialized_postings_exposed == 0u);
    CHECK(sparse_bulk_visited->descriptors_examined == 1u);
    CHECK(sparse_bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(sparse_bulk_visited->membership_words_examined == 0u);

    const auto equal = idx::query_compact_hash32_direct_index_unique(view, unique_hashes, 4u);
    REQUIRE(equal.has_value());
    CHECK(equal->selections.size() == 3u);
    CHECK(equal->stats.runtime_rejected_hash_count == 0u);
    CHECK(equal->stats.runtime_rejected_posting_count == 0u);
    CHECK(equal->stats.selected_posting_count == 8u);
    CHECK(equal->stats.selected_inline_singleton_count == 1u);
    CHECK(equal->stats.selected_materialized_hash_count == 2u);
    CHECK(equal->stats.selected_materialized_posting_count == 7u);
    CHECK(equal->stats.rejected_posting_reads == 0u);
    CHECK(equal->selections.size() == 3u);
    CHECK(equal->selections.capacity() == 3u);
    CHECK(equal->stats.selection_reserve_requested_elements == 4u);
    CHECK(equal->stats.selection_initial_capacity_elements == 4u);
    CHECK(equal->stats.selection_fill_reallocation_count == 0u);
    CHECK(equal->stats.selection_compaction_allocation_count == 1u);
    CHECK(equal->stats.selection_storage_allocation_count == 2u);
    CHECK(equal->stats.selection_peak_capacity_bytes ==
          7u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(equal->stats.selection_retained_capacity_elements == 3u);
    CHECK(equal->stats.selection_retained_capacity_bytes ==
          3u * sizeof(idx::compact_hash32_unique_selection));
    REQUIRE(equal->selections.size() == 3u);
    CHECK(equal->selections[0].dense_ordinal == 0u);
    CHECK(equal->selections[0].query_ordinal == 1u);
    CHECK(equal->selections[1].dense_ordinal == 1u);
    CHECK(equal->selections[1].query_ordinal == 2u);
    CHECK(equal->selections[2].dense_ordinal == 2u);
    CHECK(equal->selections[2].query_ordinal == 3u);
    // The returned vector has room only for its results. Filling it required no growth.
    CHECK(equal->selections.size() + 1u > equal->selections.capacity());

    std::vector<std::tuple<std::uint32_t,
                           std::uint32_t,
                           std::uint32_t,
                           std::uint32_t,
                           std::uint32_t,
                           std::uint8_t>>
        callback_order;
    std::vector<std::vector<std::uint64_t>> bulk_postings(equal->selections.size());
    std::vector<std::vector<bool>> bulk_span_strands(equal->selections.size());
    bool all_bulk_positions_decoded = true;
    const auto bulk_visited = idx::visit_compact_hash32_direct_index_selection_spans(
        view,
        *equal,
        [&callback_order, &bulk_postings, &bulk_span_strands, &all_bulk_positions_decoded](
            const idx::compact_hash32_selection_posting_spans& selected) {
            callback_order.emplace_back(selected.selection_ordinal,
                                        selected.dense_ordinal,
                                        selected.query_ordinal,
                                        selected.global_count,
                                        selected.strand_zero_count,
                                        selected.span_count);
            if (selected.selection_ordinal >= bulk_postings.size()) {
                return false;
            }
            for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count;
                 ++span_ordinal) {
                const auto& span = selected.spans[span_ordinal];
                bulk_span_strands[selected.selection_ordinal].push_back(span.target_strand_one);
                if (span.encoded_bytes.size() != span.count * span.posting_width_bytes) {
                    return false;
                }
                for (std::uint64_t ordinal = 0u; ordinal < span.count; ++ordinal) {
                    const auto encoded = span.encoded_position_at(ordinal);
                    if (!encoded) {
                        all_bulk_positions_decoded = false;
                        return false;
                    }
                    bulk_postings[selected.selection_ordinal].push_back(*encoded);
                }
                CHECK_FALSE(span.encoded_position_at(span.count).has_value());
            }
            return true;
        });
    CHECK(all_bulk_positions_decoded);
    REQUIRE(bulk_visited.has_value());
    CHECK(bulk_visited->selection_callbacks == 3u);
    CHECK(bulk_visited->spans_exposed == 5u);
    CHECK(bulk_visited->postings_exposed == 8u);
    CHECK(bulk_visited->inline_selection_callbacks == 1u);
    CHECK(bulk_visited->materialized_selection_callbacks == 2u);
    CHECK(bulk_visited->inline_postings_exposed == 1u);
    CHECK(bulk_visited->materialized_postings_exposed == 7u);
    CHECK(bulk_visited->descriptors_examined == 3u);
    CHECK(bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(bulk_visited->membership_words_examined == 0u);
    CHECK(equal->stats.rank_checkpoints_examined != 0u);
    CHECK(equal->stats.membership_words_examined != 0u);
    CHECK((callback_order == decltype(callback_order){
                                 {0u, 0u, 1u, 1u, 1u, 1u},
                                 {1u, 1u, 2u, 3u, 2u, 2u},
                                 {2u, 2u, 3u, 4u, 2u, 2u},
                             }));
    REQUIRE(bulk_postings.size() == source.runs.size());
    REQUIRE(bulk_span_strands.size() == source.runs.size());
    for (std::size_t ordinal = 0u; ordinal < source.runs.size(); ++ordinal) {
        CHECK(bulk_postings[ordinal] == source.runs[ordinal].encoded_positions);
    }
    CHECK(bulk_span_strands[0] == std::vector<bool>{false});
    CHECK(bulk_span_strands[1] == (std::vector<bool>{false, true}));
    CHECK(bulk_span_strands[2] == (std::vector<bool>{false, true}));

    const auto range_accessor = idx::bind_compact_hash32_direct_index_selection_spans(view, *equal);
    REQUIRE(range_accessor.has_value());
    CHECK(range_accessor->selection_count() == equal->selections.size());
    std::vector<std::tuple<std::uint32_t,
                           std::uint32_t,
                           std::uint32_t,
                           std::uint32_t,
                           std::uint32_t,
                           std::uint8_t>>
        ranged_callback_order;
    std::vector<std::vector<std::uint64_t>> ranged_postings(equal->selections.size());
    const auto visit_range = [&range_accessor, &ranged_callback_order, &ranged_postings](
                                 std::uint64_t selection_begin, std::uint64_t selection_end) {
        return idx::visit_compact_hash32_direct_index_selection_span_range(
            *range_accessor,
            selection_begin,
            selection_end,
            [&ranged_callback_order,
             &ranged_postings](const idx::compact_hash32_selection_posting_spans& selected) {
                ranged_callback_order.emplace_back(selected.selection_ordinal,
                                                   selected.dense_ordinal,
                                                   selected.query_ordinal,
                                                   selected.global_count,
                                                   selected.strand_zero_count,
                                                   selected.span_count);
                if (selected.selection_ordinal >= ranged_postings.size()) {
                    return false;
                }
                for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const auto& span = selected.spans[span_ordinal];
                    const auto decoded = span.visit_encoded_positions(
                        [&ranged_postings, &selected](std::uint64_t encoded) {
                            ranged_postings[selected.selection_ordinal].push_back(encoded);
                            return true;
                        });
                    if (!decoded || *decoded != span.count)
                        return false;
                }
                return true;
            });
    };
    const auto first_shard = visit_range(0u, 2u);
    const auto second_shard = visit_range(2u, 3u);
    REQUIRE(first_shard.has_value());
    REQUIRE(second_shard.has_value());
    CHECK(ranged_callback_order == callback_order);
    CHECK(ranged_postings == bulk_postings);
    CHECK(first_shard->selection_callbacks + second_shard->selection_callbacks ==
          bulk_visited->selection_callbacks);
    CHECK(first_shard->spans_exposed + second_shard->spans_exposed == bulk_visited->spans_exposed);
    CHECK(first_shard->postings_exposed + second_shard->postings_exposed ==
          bulk_visited->postings_exposed);
    CHECK(first_shard->inline_selection_callbacks + second_shard->inline_selection_callbacks ==
          bulk_visited->inline_selection_callbacks);
    CHECK(first_shard->materialized_selection_callbacks +
              second_shard->materialized_selection_callbacks ==
          bulk_visited->materialized_selection_callbacks);
    CHECK(first_shard->inline_postings_exposed + second_shard->inline_postings_exposed ==
          bulk_visited->inline_postings_exposed);
    CHECK(first_shard->materialized_postings_exposed +
              second_shard->materialized_postings_exposed ==
          bulk_visited->materialized_postings_exposed);
    CHECK(first_shard->descriptors_examined + second_shard->descriptors_examined ==
          bulk_visited->descriptors_examined);

    // Reuse the validated accessor for another pass over the same selection ranges.
    // Its plan checksum was checked when the accessor was created.
    std::uint64_t second_pass_callbacks = 0u;
    for (const auto [selection_begin, selection_end] : std::array{
             std::pair{std::uint64_t{0u}, std::uint64_t{2u}},
             std::pair{std::uint64_t{2u}, std::uint64_t{3u}},
         }) {
        const auto revisited = idx::visit_compact_hash32_direct_index_selection_span_range(
            *range_accessor,
            selection_begin,
            selection_end,
            [&second_pass_callbacks](const idx::compact_hash32_selection_posting_spans&) {
                ++second_pass_callbacks;
                return true;
            });
        REQUIRE(revisited.has_value());
    }
    CHECK(second_pass_callbacks == equal->selections.size());

    for (const std::uint64_t empty_boundary : std::array<std::uint64_t, 3u>{0u, 1u, 3u}) {
        std::uint64_t empty_callbacks = 0u;
        const auto empty = idx::visit_compact_hash32_direct_index_selection_span_range(
            *range_accessor,
            empty_boundary,
            empty_boundary,
            [&empty_callbacks](const idx::compact_hash32_selection_posting_spans&) {
                ++empty_callbacks;
                return true;
            });
        REQUIRE(empty.has_value());
        CHECK(empty_callbacks == 0u);
        CHECK(empty->selection_callbacks == 0u);
        CHECK(empty->spans_exposed == 0u);
        CHECK(empty->postings_exposed == 0u);
        CHECK(empty->descriptors_examined == 0u);
    }
    for (const auto [selection_begin, selection_end] : std::array{
             std::pair{std::uint64_t{2u}, std::uint64_t{1u}},
             std::pair{std::uint64_t{0u}, std::uint64_t{4u}},
             std::pair{std::uint64_t{4u}, std::uint64_t{4u}},
         }) {
        std::uint64_t invalid_callbacks = 0u;
        const auto invalid = idx::visit_compact_hash32_direct_index_selection_span_range(
            *range_accessor,
            selection_begin,
            selection_end,
            [&invalid_callbacks](const idx::compact_hash32_selection_posting_spans&) {
                ++invalid_callbacks;
                return true;
            });
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error() == idx::direct_inverted_index_error::query_plan);
        CHECK(invalid_callbacks == 0u);
    }

    auto descriptor_mismatched_view = view;
    --descriptor_mismatched_view.header.descriptors_bytes;
    const auto descriptor_mismatched =
        idx::bind_compact_hash32_direct_index_selection_spans(descriptor_mismatched_view, *equal);
    REQUIRE_FALSE(descriptor_mismatched.has_value());
    CHECK(descriptor_mismatched.error() == idx::direct_inverted_index_error::query_plan);

    const auto require_unique_plan_rejected_without_callback =
        [&view](const idx::compact_hash32_unique_query_plan& forged) {
            const auto binding =
                idx::bind_compact_hash32_direct_index_selection_spans(view, forged);
            REQUIRE_FALSE(binding.has_value());
            CHECK(binding.error() == idx::direct_inverted_index_error::query_plan);
            std::uint64_t callback_count = 0u;
            const auto rejected = idx::visit_compact_hash32_direct_index_selection_spans(
                view,
                forged,
                [&callback_count](const idx::compact_hash32_selection_posting_spans&) {
                    ++callback_count;
                    return true;
                });
            REQUIRE_FALSE(rejected.has_value());
            CHECK(rejected.error() == idx::direct_inverted_index_error::query_plan);
            CHECK(callback_count == 0u);
        };

    auto forged_valid_dense = *equal;
    forged_valid_dense.selections.front().dense_ordinal = 1u;
    require_unique_plan_rejected_without_callback(forged_valid_dense);

    auto forged_out_of_range_dense = *equal;
    forged_out_of_range_dense.selections.front().dense_ordinal =
        static_cast<std::uint32_t>(view.header.accepted_hash_count);
    require_unique_plan_rejected_without_callback(forged_out_of_range_dense);

    idx::compact_hash32_unique_query_plan unsealed{};
    unsealed.image_header_crc64 = equal->image_header_crc64;
    unsealed.max_occ_cap = equal->max_occ_cap;
    unsealed.runtime_max_occ = equal->runtime_max_occ;
    unsealed.posting_width_bytes = equal->posting_width_bytes;
    unsealed.selections = equal->selections;
    unsealed.stats = equal->stats;
    require_unique_plan_rejected_without_callback(unsealed);

    const std::array<std::uint32_t, 3u> all_accepted_hashes{2u, 5u, 7u};
    const auto full = idx::query_compact_hash32_direct_index_unique(view, all_accepted_hashes, 4u);
    REQUIRE(full.has_value());
    CHECK(full->selections.size() == all_accepted_hashes.size());
    CHECK(full->selections[0].dense_ordinal == 0u);
    CHECK(full->selections[0].query_ordinal == 0u);
    CHECK(full->selections[1].dense_ordinal == 1u);
    CHECK(full->selections[1].query_ordinal == 1u);
    CHECK(full->selections[2].dense_ordinal == 2u);
    CHECK(full->selections[2].query_ordinal == 2u);
    CHECK(full->selections.capacity() == all_accepted_hashes.size());
    CHECK(full->stats.selection_reserve_requested_elements == 3u);
    CHECK(full->stats.selection_initial_capacity_elements == 3u);
    CHECK(full->stats.selection_fill_reallocation_count == 0u);
    CHECK(full->stats.selection_compaction_allocation_count == 0u);
    CHECK(full->stats.selection_storage_allocation_count == 1u);
    CHECK(full->stats.selection_peak_capacity_bytes ==
          3u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(full->stats.selection_retained_capacity_elements == 3u);
    CHECK(full->stats.selection_retained_capacity_bytes ==
          3u * sizeof(idx::compact_hash32_unique_selection));

    const std::array<std::uint32_t, 4u> all_absent_hashes{1u, 3u, 4u, 6u};
    const auto none = idx::query_compact_hash32_direct_index_unique(view, all_absent_hashes, 4u);
    REQUIRE(none.has_value());
    CHECK(none->selections.empty());
    CHECK(none->selections.capacity() == 0u);
    CHECK(none->stats.selection_reserve_requested_elements == 4u);
    CHECK(none->stats.selection_initial_capacity_elements == 4u);
    CHECK(none->stats.selection_fill_reallocation_count == 0u);
    CHECK(none->stats.selection_compaction_allocation_count == 0u);
    CHECK(none->stats.selection_storage_allocation_count == 1u);
    CHECK(none->stats.selection_peak_capacity_bytes ==
          4u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(none->stats.selection_retained_capacity_elements == 0u);
    CHECK(none->stats.selection_retained_capacity_bytes == 0u);
    std::uint64_t absent_selection_callbacks = 0u;
    const auto none_visited = idx::visit_compact_hash32_direct_index_selection_spans(
        view,
        *none,
        [&absent_selection_callbacks](const idx::compact_hash32_selection_posting_spans&) {
            ++absent_selection_callbacks;
            return true;
        });
    REQUIRE(none_visited.has_value());
    CHECK(absent_selection_callbacks == 0u);
    CHECK(none_visited->selection_callbacks == 0u);
    CHECK(none_visited->spans_exposed == 0u);
    CHECK(none_visited->postings_exposed == 0u);
    CHECK(none_visited->inline_selection_callbacks == 0u);
    CHECK(none_visited->materialized_selection_callbacks == 0u);
    CHECK(none_visited->inline_postings_exposed == 0u);
    CHECK(none_visited->materialized_postings_exposed == 0u);
    CHECK(none_visited->descriptors_examined == 0u);
    CHECK(none_visited->rank_checkpoints_examined == 0u);
    CHECK(none_visited->membership_words_examined == 0u);
    const auto none_range_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(view, *none);
    REQUIRE(none_range_accessor.has_value());
    CHECK(none_range_accessor->selection_count() == 0u);
    const auto none_range = idx::visit_compact_hash32_direct_index_selection_span_range(
        *none_range_accessor, 0u, 0u, [](const idx::compact_hash32_selection_posting_spans&) {
            return false;
        });
    REQUIRE(none_range.has_value());
    CHECK(none_range->selection_callbacks == 0u);
    CHECK(none_range->postings_exposed == 0u);

    const auto greater = idx::query_compact_hash32_direct_index_unique(view, unique_hashes, 5u);
    REQUIRE_FALSE(greater.has_value());
    CHECK(greater.error() == idx::direct_inverted_index_error::storage_contract_mismatch);
    const auto zero = idx::query_compact_hash32_direct_index_unique(view, unique_hashes, 0u);
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error() == idx::direct_inverted_index_error::storage_contract_mismatch);

    const std::array<std::uint32_t, 2u> duplicate_hashes{2u, 2u};
    const auto duplicate =
        idx::query_compact_hash32_direct_index_unique(view, duplicate_hashes, 2u);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == idx::direct_inverted_index_error::order);
    const auto duplicate_projected = idx::query_compact_hash32_direct_index_unique_projected(
        view,
        duplicate_hashes.size(),
        [&duplicate_hashes](std::size_t ordinal) noexcept -> std::uint32_t {
            return duplicate_hashes[ordinal];
        },
        2u);
    REQUIRE_FALSE(duplicate_projected.has_value());
    CHECK(duplicate_projected.error() == idx::direct_inverted_index_error::order);
}

TEST_CASE("compact exact-domain hash32 visitor rejects unbound misaligned and "
          "out-of-section spans",
          "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 10u, .global_count = 2u, .target_strand_zero_count = 2u},
                    .encoded_positions =
                        {
                            encoded_position(1u, false),
                            encoded_position(3u, false),
                        },
                },
            },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    const std::array query{
        idx::compact_hash32_query_occurrence{.hash = 10u, .query_strand_one = false},
    };
    const auto planned = idx::query_compact_hash32_direct_index(view, query, 2u);
    REQUIRE(planned.has_value());
    REQUIRE(planned->selections.size() == 1u);
    REQUIRE(planned->spans.size() == 1u);

    const auto require_plan_rejected = [&view](const idx::compact_hash32_query_plan& forged) {
        collected_postings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        const auto visited = idx::visit_compact_hash32_direct_index_postings(
            view, forged, &collected, &collect_posting);
        REQUIRE_FALSE(visited.has_value());
        CHECK(visited.error() == idx::direct_inverted_index_error::query_plan);
        CHECK(collected.callback_count == 0u);
    };

    auto misaligned = *planned;
    ++misaligned.spans.front().byte_offset;
    require_plan_rejected(misaligned);

    auto out_of_section = *planned;
    out_of_section.spans.front().byte_offset =
        view.header.postings_offset + view.header.postings_bytes;
    require_plan_rejected(out_of_section);

    auto unbound = *planned;
    ++unbound.selections.front().hash;
    require_plan_rejected(unbound);
}
