/**
 * @file
 * @brief Checks edit counts and alignment memory plans for KSW2 and Block Aligner.
 */

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.alignment.ksw2_batched;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.pipeline.minimap_port;
import bioseq.prefilter.sneaky_snake;
import bioseq.chaining.core;
import bioseq.common.context;
import bioseq.storage.lazy_sequence;
import bioseq.parallelism.pipeline_ops;

namespace bioseq::simd {
using namespace bioseq::prefilter;

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_type = compact_sequence_range<dna4_u64>;

/**
 * Returns DNA4 alignment kernels for the test CPU target.
 * Throws std::logic_error if the generated table lacks the DNA4 entry.
 */
[[nodiscard]] auto fixed_alignment_kernels() -> const alignment_kernels<seq::dna4>& {
    const auto& root = ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity =
        root.is_complete() ? root.resolve_alphabet_identity<seq::dna4>() : nullptr;
    if (identity == nullptr ||
        root.resolve_alphabet_identity(identity->alphabet_code) != identity) {
        throw std::logic_error{"generated fixed-target test table lacks DNA4 identity"};
    }
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code && entry.typed_table != nullptr) {
            return *static_cast<const alignment_kernels<seq::dna4>*>(entry.typed_table);
        }
    }
    throw std::logic_error{"generated fixed-target test table lacks DNA4 alignment"};
}

/** Converts ASCII bases to DNA4 symbols for a test sequence. */
[[nodiscard]] auto dna4_from_ascii(std::string_view text) -> std::vector<seq::dna4> {
    std::vector<seq::dna4> sequence(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        sequence[i].assign_char(text[i]);
    }
    return sequence;
}

/**
 * Builds an anchor from inclusive query and reference seed-end coordinates.
 * The strand and span describe the seed used by the alignment fixture.
 */
[[nodiscard]] auto make_anchor(std::uint64_t query_position,
                               std::uint64_t ref_position,
                               bool reverse,
                               std::uint8_t span = 8) -> ::bioseq::chaining::anchor {
    return ::bioseq::chaining::anchor{
        .ref_position = ref_position,
        .query_position = query_position,
        .is_reverse = static_cast<std::uint32_t>(reverse),
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

/** Builds an anchor with the same seed-end coordinate in  query and reference. */
[[nodiscard]] auto make_anchor(std::uint64_t position, bool reverse, std::uint8_t span = 8)
    -> ::bioseq::chaining::anchor {
    return make_anchor(position, position, reverse, span);
}

/**
 * Builds DNA4 alignment settings with exact edit counting enabled.
 * Uses the test CPU target and throws if its DNA4 entry is missing
 */
[[nodiscard]] auto exact_alignment_params() -> minimap2_parameters {
    minimap2_parameters parameters{};
    const auto& binding = ::bioseq::simd_dispatch::highway_test_target_binding();
    parameters.highway_kernels = binding.kernels;
    parameters.highway_executed_target = binding.target;
    const auto* identity = binding.kernels == nullptr || !binding.kernels->is_complete()
                               ? nullptr
                               : binding.kernels->resolve_alphabet_identity<seq::dna4>();
    if (identity == nullptr ||
        binding.kernels->resolve_alphabet_identity(identity->alphabet_code) != identity) {
        throw std::logic_error{"generated fixed-target test binding lacks DNA4 identity"};
    }
    parameters.alphabet_code = identity->alphabet_code;
    parameters.match_score = 2;
    parameters.mismatch_pen = 4;
    parameters.gap_open1 = 20;
    parameters.gap_ext1 = 5;
    parameters.gap_open2 = 40;
    parameters.gap_ext2 = 2;
    parameters.bandwidth = 64;
    parameters.zdrop = 100;
    parameters.max_gap = 0;
    parameters.min_ksw_len = 1;
    parameters.block_min_size = 16;
    parameters.block_max_size = 64;
    parameters.do_cigar = true;
    parameters.compute_edit_distance = true;
    return parameters;
}

/** Sets one region's seed range and coordinates from the supplied anchors. */
[[nodiscard]] auto make_region(std::span<const ::bioseq::chaining::anchor> anchors,
                               sequence_size_t query_length) -> alignment_region {
    alignment_region region{};
    region.anchor_start = 0;
    region.seed_count = static_cast<std::int32_t>(anchors.size());
    set_region_coordinates(region, anchors, query_length);
    return region;
}

/** @brief Query and target lengths consumed by a decoded CIGAR. */
struct cigar_consumption {
    sequence_size_t query = 0;
    sequence_size_t target = 0;
};

/** Counts the query and target bases consumed by the CIGAR, failing on unsupported operations. */
[[nodiscard]] auto measure_cigar_consumption(std::span<const cigar_operation> cigar)
    -> cigar_consumption {
    cigar_consumption consumption{};
    for (const cigar_operation op : cigar) {
        switch (op.opcode) {
        case cigar_match_opcode:
        case cigar_equal_opcode:
        case cigar_mismatch_opcode:
            consumption.query += op.length;
            consumption.target += op.length;
            break;
        case cigar_insertion_opcode:
            consumption.query += op.length;
            break;
        case cigar_deletion_opcode:
        case cigar_reference_skip_opcode:
            consumption.target += op.length;
            break;
        default:
            FAIL("unexpected CIGAR opcode in edit-distance fixture");
        }
    }
    return consumption;
}

/** Results from ordinary KSW2, Block Aligner and batched KSW2. */
struct backend_regions {
    alignment_region ksw2;
    alignment_region block;
    alignment_region batched;
};

/** Chooses the seed positions and sequence differences used in a tesa. */
enum class fixture_kind {
    single_seed_collision,
    multi_seed_mismatch,
    off_diagonal_gap,
    out_of_band_gap,
    left_extension_orientation,
};

/**
 * Runs the selected fixture through KSW2, Block Aligner and batched KSW2.
 * Each run uses the same sequences, anchors and alignment settings.
 * @param reverse Use reverse-strand anchors.
 * @param kind Choose the seed positions and sequence differences.
 * @param max_gap Limit extension  beyond the seed region.
 * @param do_cigar Request a CIGAR explicitly. Exact, edit counting can require one too.
 */
[[nodiscard]] auto
run_fixture(bool reverse, fixture_kind kind, std::int32_t max_gap = 0, bool do_cigar = true)
    -> backend_regions {
    const std::size_t query_length = kind == fixture_kind::out_of_band_gap ? 256u : 128u;
    const std::size_t target_length = kind == fixture_kind::out_of_band_gap ? 256u : 128u;
    std::string query_text = kind == fixture_kind::left_extension_orientation
                                 ? (reverse ? "TTTTTTTTACGTACGT" : "ACGTACGTAAAAAAAA")
                                 : std::string(query_length, reverse ? 'T' : 'A');
    std::string target_text = kind == fixture_kind::left_extension_orientation
                                  ? "ACGTAAAAAAAAA"
                                  : std::string(target_length, 'A');
    std::vector<::bioseq::chaining::anchor> anchors;
    if (kind == fixture_kind::single_seed_collision) {
        anchors.push_back(make_anchor(31, reverse));
        // The seed covers [24, 32), but two bases differ. Edit counting must
        // compare the bases even when the anchor says their hashes match.
        target_text[26] = 'C';
        target_text[29] = 'C';
    } else if (kind == fixture_kind::multi_seed_mismatch) {
        anchors.push_back(make_anchor(31, reverse));
        anchors.push_back(make_anchor(63, reverse));
        target_text[40] = 'C';
        target_text[52] = 'C';
    } else if (kind == fixture_kind::off_diagonal_gap) {
        anchors.push_back(make_anchor(31, reverse));
        anchors.push_back(make_anchor(63, 66, reverse, 8));
    } else if (kind == fixture_kind::out_of_band_gap) {
        anchors.push_back(make_anchor(31, reverse));
        anchors.push_back(make_anchor(63, 200, reverse, 8));
    } else {
        anchors.push_back(make_anchor(15, 12, reverse, 8));
    }

    auto query_data = dna4_from_ascii(query_text);
    auto target_data = dna4_from_ascii(target_text);
    range_type query_range(query_data);
    range_type target_range(target_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(lazy_target);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);
    minimap2_parameters parameters = exact_alignment_params();
    parameters.max_gap = max_gap;
    parameters.do_cigar = do_cigar;
    if (kind == fixture_kind::left_extension_orientation) {
        parameters.gap_open1 = 1;
        parameters.gap_ext1 = 1;
        parameters.gap_open2 = 1;
        parameters.gap_ext2 = 1;
        parameters.end_bonus = 100;
    }
    const auto anchor_span =
        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()};

    backend_regions fixture_regions{
        .ksw2 = make_region(anchor_span, query_text.size()),
        .block = make_region(anchor_span, query_text.size()),
        .batched = make_region(anchor_span, query_text.size()),
    };

    align_regions<base_alignment_algorithm::ksw2>(
        std::span<alignment_region>{&fixture_regions.ksw2, 1},
        anchor_span,
        query_view,
        target_view,
        scoring,
        fixed_alignment_kernels(),
        parameters);
    align_regions<base_alignment_algorithm::block_aligner>(
        std::span<alignment_region>{&fixture_regions.block, 1},
        anchor_span,
        query_view,
        target_view,
        scoring,
        fixed_alignment_kernels(),
        parameters);
    fixed_alignment_kernels().align_ksw2_batched(
        std::span<alignment_region>{&fixture_regions.batched, 1},
        anchor_span,
        query_view,
        target_view,
        scoring,
        parameters,
        nullptr);
    return fixture_regions;
}

/** Checks the edit count, consumed sequence lengths and region boundaries. */
auto check_exact_region(const alignment_region& region,
                        sequence_size_t expected_edit_distance,
                        sequence_size_t expected_query_length,
                        sequence_size_t expected_target_length) -> void {
    CHECK(region.edit_distance == expected_edit_distance);
    const cigar_consumption consumed = measure_cigar_consumption(
        std::span<const cigar_operation>{region.cigar.data(), region.cigar.size()});
    CHECK(consumed.query == expected_query_length);
    CHECK(consumed.target == expected_target_length);
    CHECK(region.query_end - region.query_start == consumed.query);
    CHECK(region.ref_end - region.ref_start == consumed.target);
}

} // namespace

TEST_CASE("implicit seed CIGAR assembly is one neutral canonical contract",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    const auto assemble_cigar = [](std::size_t query_length,
                                   std::size_t target_length) -> std::string {
        std::vector<cigar_operation> cigar;
        append_implicit_seed_cigar(cigar, query_length, target_length);
        return format_cigar(cigar);
    };

    CHECK(assemble_cigar(5u, 5u) == "5M");
    CHECK(assemble_cigar(7u, 5u) == "5M2I");
    CHECK(assemble_cigar(5u, 7u) == "5M2D");
    CHECK(assemble_cigar(4u, 0u) == "4I");
    CHECK(assemble_cigar(0u, 4u) == "4D");
    CHECK(assemble_cigar(0u, 0u).empty());

    std::vector<cigar_operation> existing{
        cigar_operation{.length = 3u, .opcode = cigar_match_opcode}};
    append_implicit_seed_cigar(existing, 5u, 7u);
    CHECK(format_cigar(existing) == "8M2D");
}

TEST_CASE("Single-seed exact edit accounting compares implicit seed ranks",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context test_context;
    context::installed_context_scope installed_context(test_context);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(reverse, fixture_kind::single_seed_collision);
        check_exact_region(regions.ksw2, 2, 8, 8);
        check_exact_region(regions.block, 2, 8, 8);
        check_exact_region(regions.batched, 2, 8, 8);
        CHECK(format_cigar(regions.ksw2.cigar) == "8M");
        CHECK(format_cigar(regions.block.cigar) == "8M");
        CHECK(format_cigar(regions.batched.cigar) == "8M");
    }
}

TEST_CASE("Multi-seed exact edit accounting is unchanged by implicit seed handling",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context test_context;
    context::installed_context_scope installed_context(test_context);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(reverse, fixture_kind::multi_seed_mismatch);
        check_exact_region(regions.ksw2, 2, 40, 40);
        check_exact_region(regions.block, 2, 40, 40);
        check_exact_region(regions.batched, 2, 40, 40);
    }
}

TEST_CASE("Exact edit accounting forces traceback when CIGAR was not requested",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context test_context;
    context::installed_context_scope installed_context(test_context);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions =
            run_fixture(reverse, fixture_kind::single_seed_collision, 16, false);
        CHECK(regions.ksw2.edit_distance == 2);
        CHECK(regions.block.edit_distance == 2);
        CHECK(regions.batched.edit_distance == 2);
        CHECK(format_cigar(regions.ksw2.cigar) == "40M");
        CHECK(format_cigar(regions.block.cigar) == "40M");
        CHECK(format_cigar(regions.batched.cigar) == "40M");
    }
}

TEST_CASE("Left-extension exact counting uses source-order CIGAR windows",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context test_context;
    context::installed_context_scope installed_context(test_context);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions =
            run_fixture(reverse, fixture_kind::left_extension_orientation, 16);
        CHECK(regions.ksw2.edit_distance == 3);
        CHECK(regions.batched.edit_distance == 3);
        CHECK(regions.batched.query_start == regions.ksw2.query_start);
        CHECK(regions.batched.query_end == regions.ksw2.query_end);
        CHECK(regions.batched.ref_start == regions.ksw2.ref_start);
        CHECK(regions.batched.ref_end == regions.ksw2.ref_end);
        CHECK(format_cigar(regions.batched.cigar) == format_cigar(regions.ksw2.cigar));
        check_exact_region(regions.ksw2, 3, 16, 13);
        check_exact_region(regions.batched, 3, 16, 13);
    }
}

TEST_CASE("Exact gap fills disable z-drop with the supported sentinel",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context test_context;
    context::installed_context_scope installed_context(test_context);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(reverse, fixture_kind::off_diagonal_gap);
        check_exact_region(regions.ksw2, 3, 40, 43);
        check_exact_region(regions.block, 3, 40, 43);
        check_exact_region(regions.batched, 3, 40, 43);
    }
}

TEST_CASE("Exact mandatory gaps are unbanded before SneakySnake rejection",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context test_context;
    context::installed_context_scope installed_context(test_context);

    constexpr sequence_size_t query_gap_length = 40;
    constexpr sequence_size_t target_gap_length = 177;
    constexpr sequence_size_t length_difference = target_gap_length - query_gap_length;
    const minimap2_parameters params = exact_alignment_params();
    const std::int32_t normal_align_bandwidth =
        static_cast<std::int32_t>(params.bandwidth * 1.5 + 1.0);
    REQUIRE(length_difference > static_cast<sequence_size_t>(normal_align_bandwidth));

    const std::vector<std::uint8_t> query_ranks(query_gap_length, 0u);
    const std::vector<std::uint8_t> target_ranks(target_gap_length, 0u);
    const prefilter_result prefilter = sneaky_snake_scalar(query_ranks, target_ranks, 16u);
    REQUIRE(prefilter.lower_bound == length_difference);
    REQUIRE_FALSE(is_prefilter_accepted(prefilter));

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(reverse, fixture_kind::out_of_band_gap);
        check_exact_region(regions.ksw2, length_difference, query_gap_length, target_gap_length);
        check_exact_region(regions.block, length_difference, query_gap_length, target_gap_length);
        check_exact_region(regions.batched, length_difference, query_gap_length, target_gap_length);
        CHECK(regions.batched.query_start == regions.ksw2.query_start);
        CHECK(regions.batched.query_end == regions.ksw2.query_end);
        CHECK(regions.batched.ref_start == regions.ksw2.ref_start);
        CHECK(regions.batched.ref_end == regions.ksw2.ref_end);
        CHECK(regions.batched.edit_distance == regions.ksw2.edit_distance);
        CHECK(format_cigar(regions.batched.cigar) == format_cigar(regions.ksw2.cigar));
        CHECK(prefilter.lower_bound == regions.ksw2.edit_distance);
    }
}

TEST_CASE("Batched exact CIGAR and coordinates match unbatched KSW2 extensions",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context test_context;
    context::installed_context_scope installed_context(test_context);

    for (const bool reverse : {false, true}) {
        for (const fixture_kind kind : {fixture_kind::single_seed_collision,
                                        fixture_kind::multi_seed_mismatch,
                                        fixture_kind::off_diagonal_gap}) {
            CAPTURE(reverse, kind);
            const backend_regions regions = run_fixture(reverse, kind, 16);
            CHECK(regions.batched.query_start == regions.ksw2.query_start);
            CHECK(regions.batched.query_end == regions.ksw2.query_end);
            CHECK(regions.batched.ref_start == regions.ksw2.ref_start);
            CHECK(regions.batched.ref_end == regions.ksw2.ref_end);
            CHECK(regions.batched.dp_score == regions.ksw2.dp_score);
            CHECK(regions.batched.edit_distance == regions.ksw2.edit_distance);
            CHECK(regions.batched.match_length == regions.ksw2.match_length);
            CHECK(regions.batched.block_length == regions.ksw2.block_length);
            CHECK(format_cigar(regions.batched.cigar) == format_cigar(regions.ksw2.cigar));

            const cigar_consumption consumed =
                measure_cigar_consumption(std::span<const cigar_operation>{
                    regions.batched.cigar.data(), regions.batched.cigar.size()});
            CHECK(consumed.query == regions.batched.query_end - regions.batched.query_start);
            CHECK(consumed.target == regions.batched.ref_end - regions.batched.ref_start);
        }
    }
}


TEST_CASE("alignment storage plan preserves extension geometry and separates CIGAR reserve",
          "[component:bioseq][layer:unit][feature:alignment][feature:planner]") {
    minimap2_parameters params{};
    params.max_gap = 50;
    params.match_score = 2;
    params.gap_open1 = 4;
    params.gap_ext1 = 2;
    const std::array anchors{
        make_anchor(114u, 214u, false, 15u),
        make_anchor(199u, 299u, false, 15u),
    };
    alignment_region region{};
    region.anchor_start = 0;
    region.seed_count = 2;
    const auto plan = derive_alignment_region_storage_plan(
        region, anchors, 1000u, 2000u, params);
    REQUIRE(plan.has_work);
    CHECK(plan.anchor_begin == 0);
    CHECK(plan.seed_query_start == 100);
    CHECK(plan.seed_ref_start == 200);
    CHECK(plan.seed_query_end == 200);
    CHECK(plan.seed_ref_end == 300);
    CHECK(plan.outer_query_start == 50);
    CHECK(plan.outer_ref_start == 150);
    CHECK(plan.outer_query_end == 250);
    CHECK(plan.outer_ref_end == 350);
    CHECK(plan.outer_query_extent == 200u);
    CHECK(plan.outer_reference_extent == 200u);
    CHECK(plan.final_cigar_step_bound == 400u);
    CHECK(plan.final_cigar_capacity_bytes == 400u * sizeof(cigar_operation));
    CHECK(plan.initial_cigar_reserve == 264u);
    CHECK(plan.initial_cigar_reserve < plan.final_cigar_step_bound);
}

TEST_CASE("alignment storage plan handles empty jobs and checked ownership without allocation",
          "[component:bioseq][layer:unit][feature:alignment][feature:planner]") {
    minimap2_parameters params{};
    alignment_region empty{};
    const auto empty_plan = derive_alignment_region_storage_plan(
        empty, {}, 0u, 0u, params);
    CHECK_FALSE(empty_plan.has_work);
    CHECK(empty_plan.final_cigar_capacity_bytes == 0u);
    CHECK(empty_plan.initial_cigar_reserve == 0u);

    const std::array anchors{make_anchor(7u, false, 8u)};
    alignment_region region{};
    region.anchor_start = 0;
    region.seed_count = 1;
    const auto plan = derive_alignment_region_storage_plan(region, anchors, 8u, 8u, params);
    CHECK(plan.seed_query_start == 0);
    CHECK(plan.outer_query_extent == 8u);
    CHECK(plan.outer_reference_extent == 8u);
    CHECK(plan.final_cigar_step_bound == 16u);
    // The initial allocation can hold extra CIGAR operations. Account for that
    // capacity even when the alignment can emit fewer steps.
    CHECK(plan.initial_cigar_reserve == 32u);
    CHECK(plan.final_cigar_capacity_bytes == 32u * sizeof(cigar_operation));

    region.anchor_start = -1;
    CHECK_THROWS_AS(derive_alignment_region_storage_plan(
        region, anchors, 8u, 8u, params), std::invalid_argument);
    region.anchor_start = 0;
    region.seed_count = 2;
    CHECK_THROWS_AS(derive_alignment_region_storage_plan(
        region, anchors, 8u, 8u, params), std::invalid_argument);
    region.seed_count = 1;
    CHECK_THROWS_AS(derive_alignment_region_storage_plan(
        region, anchors, 7u, 8u, params), std::invalid_argument);
    params.gap_ext1 = 0;
    CHECK_THROWS_AS(derive_alignment_region_storage_plan(
        region, anchors, 8u, 8u, params), std::invalid_argument);
}

TEST_CASE("alignment storage plan rejects signed and CIGAR byte overflow before allocation",
          "[component:bioseq][layer:unit][feature:alignment][feature:planner]") {
    minimap2_parameters params{};
    const auto signed_limit =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const std::array anchors{
        make_anchor(0u, false, 1u),
        make_anchor(signed_limit - 1u, false, 1u),
    };
    alignment_region region{};
    region.anchor_start = 0;
    region.seed_count = 2;
    CHECK_THROWS_AS(derive_alignment_region_storage_plan(
        region, anchors, signed_limit + 1u, signed_limit, params), std::length_error);
    CHECK_THROWS_AS(derive_alignment_region_storage_plan(
        region, anchors, signed_limit, signed_limit, params), std::length_error);
}

TEST_CASE("Block storage plan matches existing constructor element requests",
          "[component:bioseq][layer:unit][feature:alignment][feature:planner]") {
    const auto eight = derive_block_aligner_storage_plan(100u, 200u, 256u, 8u, true);
    REQUIRE(eight.has_value());
    CHECK(eight->query_padded_bytes == 421u);
    CHECK(eight->reference_padded_bytes == 521u);
    CHECK(eight->border_elements == 256u);
    CHECK(eight->shift_elements == 64u);
    CHECK(eight->trace_capacity == 26048u);
    CHECK(eight->trace_word_elements == 26112u);
    CHECK(eight->direction_word_elements == 10u);
    CHECK(eight->coordinate_elements == 1208u);
    CHECK(eight->logical_alignment_bytes == 221518u);
    CHECK(eight->packed_cigar_step_bound == 300u);
    CHECK(eight->packed_cigar_bytes == 1200u);

    const auto sixteen = derive_block_aligner_storage_plan(100u, 200u, 256u, 16u, true);
    REQUIRE(sixteen.has_value());
    CHECK(sixteen->trace_capacity == 13024u);
    CHECK(sixteen->trace_word_elements == 13088u);
    CHECK(sixteen->logical_alignment_bytes == 117326u);

    const auto score_only = derive_block_aligner_storage_plan(100u, 200u, 256u, 8u, false);
    REQUIRE(score_only.has_value());
    CHECK(score_only->logical_alignment_bytes == 5294u);
    CHECK(score_only->trace_capacity == 0u);
    CHECK(score_only->direction_word_elements == 0u);
    CHECK(score_only->coordinate_elements == 0u);
    CHECK(score_only->packed_cigar_bytes == 0u);

    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    CHECK_FALSE(derive_block_aligner_storage_plan(maximum, 1u, 256u, 8u, true));
    CHECK_FALSE(derive_block_aligner_storage_plan(maximum / 2u, 1u, 256u, 8u, true));
    CHECK_FALSE(derive_block_aligner_storage_plan(100u, 200u, 256u, 0u, true));
    CHECK_FALSE(derive_block_aligner_storage_plan(100u, 200u, 7u, 8u, true));
}

TEST_CASE("KSW2 storage plan matches single and dual affine constructor requests",
          "[component:bioseq][layer:unit][feature:alignment][feature:planner]") {
    const auto single = derive_ksw2_storage_plan(100u, 200u, 16u, false, 31, true, 4u, true);
    REQUIRE(single.has_value());
    CHECK(single->target_vector_count == 13u);
    CHECK(single->query_vector_count == 7u);
    CHECK(single->state_array_count == 5u);
    CHECK(single->state_array_bytes == 208u);
    CHECK(single->state_bytes == 1376u);
    CHECK(single->exact_score_elements == 208u);
    CHECK(single->exact_score_bytes == 832u);
    CHECK(single->anti_diagonal_count == 299u);
    CHECK(single->band_vector_capacity == 3u);
    CHECK(single->backtrack_vector_slots == 898u);
    CHECK(single->backtrack_bytes == 14368u);
    CHECK(single->band_bound_elements == 598u);
    CHECK(single->packed_cigar_step_bound == 300u);
    CHECK(single->packed_cigar_capacity_bound == 300u);
    CHECK(single->packed_cigar_bytes == 1200u);
    CHECK(single->logical_alignment_bytes == 18968u);

    const auto dual = derive_ksw2_storage_plan(100u, 200u, 16u, true, 31, true, 4u, true);
    REQUIRE(dual.has_value());
    CHECK(dual->state_array_count == 7u);
    CHECK(dual->state_bytes == 1792u);
    CHECK(dual->logical_alignment_bytes == 19384u);

    const auto score_only = derive_ksw2_storage_plan(100u, 200u, 16u, false, -1, false, 4u, false);
    REQUIRE(score_only.has_value());
    CHECK(score_only->band_vector_capacity == 8u);
    CHECK(score_only->logical_alignment_bytes == 1376u);
    CHECK(score_only->exact_score_elements == 0u);
    CHECK(score_only->backtrack_bytes == 0u);
    CHECK(score_only->band_bound_elements == 0u);
    CHECK(score_only->packed_cigar_bytes == 0u);

    const auto short_trace =
        derive_ksw2_storage_plan(8u, 8u, 16u, false, -1, true, 4u, true);
    REQUIRE(short_trace.has_value());
    CHECK(short_trace->packed_cigar_step_bound == 16u);
    CHECK(short_trace->packed_cigar_capacity_bound == 64u);
    CHECK(short_trace->packed_cigar_bytes == 64u * sizeof(std::uint32_t));

    const auto signed_limit =
        static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)());
    CHECK_FALSE(derive_ksw2_storage_plan(0u, 1u, 16u, false, -1, true, 4u, true));
    CHECK_FALSE(derive_ksw2_storage_plan(100u, 200u, 0u, false, -1, true, 4u, true));
    CHECK_FALSE(derive_ksw2_storage_plan(signed_limit, 2u, 16u, false, -1, true, 4u, true));
    CHECK_FALSE(derive_ksw2_storage_plan(100u, 200u, 16u, false, -1, true, 0u, true));
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
TEST_CASE("alignment tail plan bounds every one-region-per-chain owner",
          "[component:bioseq][layer:unit][feature:alignment][feature:planner]") {
    const auto plan = derive_alignment_tail_storage_plan(
        7u, /*collect_block_stats=*/true, /*collect_scalar_stats=*/false);
    REQUIRE(plan.has_value());
    CHECK(plan->region_count == 7u);
    CHECK(plan->primary_index_capacity == 7u);
    CHECK(plan->coverage_interval_capacity == 7u);
    CHECK(plan->filter_marker_count == 7u);
    CHECK(plan->mapq_region_count == 7u);
    CHECK(plan->result_count == 7u);
    CHECK(plan->block_stats_count == 7u);
    CHECK(plan->scalar_stats_count == 0u);

    const auto empty = derive_alignment_tail_storage_plan(0u, true, true);
    REQUIRE(empty.has_value());
    CHECK(empty->region_count == 0u);
    CHECK(empty->block_stats_count == 0u);
    CHECK(empty->scalar_stats_count == 0u);

    constexpr std::size_t maximum_region_count =
        static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)());
    CHECK(derive_alignment_tail_storage_plan(maximum_region_count).has_value());
    CHECK_FALSE(derive_alignment_tail_storage_plan(maximum_region_count + 1u).has_value());
}

TEST_CASE("alignment storage plans cover concurrent selected-algorithm workers",
          "[component:bioseq][layer:unit][feature:alignment][feature:planner]") {
    minimap2_parameters params = exact_alignment_params();
    params.max_gap = 50;
    params.compute_edit_distance = false;
    params.block_min_size = 16u;
    params.block_max_size = 64u;
    const std::array anchors{
        make_anchor(114u, 214u, false, 15u),
        make_anchor(199u, 299u, false, 15u),
    };
    alignment_region region{};
    region.anchor_start = 0;
    region.seed_count = 2;
    const auto region_plan = derive_alignment_region_storage_plan(
        region, anchors, 1000u, 2000u, params);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);
    const auto geometry = fixed_alignment_kernels().prepare_block_global_gap_certificate(
        scoring,
        project_block_aligner_params(params, block_alignment_stage::gap_extension));
    REQUIRE(is_block_aligner_lane_shape_supported(geometry.simd_lane_count));
    REQUIRE(geometry.ksw2_byte_lane_count > 0u);
    CHECK(geometry.ksw2_full_score_bytes == sizeof(ksw2_full_score_type));

    constexpr std::int32_t align_bandwidth = 31;
    const auto selected_ksw =
        derive_selected_alignment_region_storage_plan<base_alignment_algorithm::ksw2>(
            region, anchors, region_plan, params, align_bandwidth, geometry);
    REQUIRE(selected_ksw.has_value());
    CHECK(selected_ksw->fill_count == 3u);

    const bool is_dual_affine = params.gap_open1 != params.gap_open2 ||
                                params.gap_ext1 != params.gap_ext2;
    const auto ksw_left = derive_ksw2_storage_plan(
        50u, 50u, geometry.ksw2_byte_lane_count, is_dual_affine,
        align_bandwidth, true, geometry.ksw2_full_score_bytes, true);
    const auto ksw_gap = derive_ksw2_storage_plan(
        100u, 100u, geometry.ksw2_byte_lane_count, is_dual_affine,
        align_bandwidth, false, geometry.ksw2_full_score_bytes, true);
    REQUIRE(ksw_left.has_value());
    REQUIRE(ksw_gap.has_value());
    const auto expected_ksw_peak = std::max(
        ksw_left->logical_alignment_bytes + ksw_left->packed_cigar_bytes,
        ksw_gap->logical_alignment_bytes + ksw_gap->packed_cigar_bytes);
    CHECK(selected_ksw->temporary_workspace_bytes == expected_ksw_peak);
    CHECK(selected_ksw->maximum_result_capacity_bytes == ksw_gap->packed_cigar_bytes);

    const auto selected_block =
        derive_selected_alignment_region_storage_plan<base_alignment_algorithm::block_aligner>(
            region, anchors, region_plan, params, align_bandwidth, geometry);
    REQUIRE(selected_block.has_value());
    CHECK(selected_block->fill_count == 3u);
    const auto block_gap = derive_block_aligner_storage_plan(
        100u,
        100u,
        std::max(params.block_max_size,
                 static_cast<std::size_t>(geometry.simd_lane_count)),
        geometry.simd_lane_count,
        true);
    REQUIRE(block_gap.has_value());
    CHECK(selected_block->maximum_kernel_workspace_bytes ==
          block_gap->logical_alignment_bytes);
    CHECK(selected_block->maximum_result_capacity_bytes == block_gap->packed_cigar_bytes);
    CHECK(selected_block->temporary_workspace_bytes ==
          block_gap->logical_alignment_bytes + block_gap->packed_cigar_bytes);

    constexpr std::array<std::uint64_t, 4> descending{900u, 600u, 200u, 0u};
    CHECK(maximum_alignment_concurrency_from_workspaces(descending, 4u, 1700u) == 4u);
    CHECK(maximum_alignment_concurrency_from_workspaces(descending, 4u, 1500u) == 2u);
    CHECK(maximum_alignment_concurrency_from_workspaces(descending, 4u, 1499u) == 1u);
    CHECK(maximum_alignment_concurrency_from_workspaces(descending, 4u, 899u) == 0u);
    CHECK(maximum_alignment_concurrency_from_workspaces(descending, 3u, 1700u) == 3u);
    CHECK(maximum_alignment_concurrency_from_workspaces(
              std::span<const std::uint64_t>{descending.data(), 2u}, 4u, 1500u) == 2u);
    constexpr std::array<std::uint64_t, 2> ascending{1u, 2u};
    CHECK_THROWS_AS(maximum_alignment_concurrency_from_workspaces(
                        ascending, 2u, 3u), std::invalid_argument);
    constexpr std::array<std::uint64_t, 2> overflowing{
        (std::numeric_limits<std::uint64_t>::max)(), 1u};
    CHECK_THROWS_AS(maximum_alignment_concurrency_from_workspaces(
                        overflowing,
                        2u,
                        (std::numeric_limits<std::uint64_t>::max)()),
                    std::length_error);

    std::vector<anchor> concurrency_anchors;
    concurrency_anchors.reserve(16u);
    for (std::uint64_t index = 0u; index < 16u; ++index) {
        concurrency_anchors.push_back(make_anchor(
            114u + index * 20u,
            214u + index * 20u,
            false,
            15u));
    }
    std::array<alignment_region, 4> concurrent_regions{};
    for (alignment_region& concurrent_region : concurrent_regions) {
        concurrent_region.anchor_start = 0;
        concurrent_region.seed_count = 16;
    }
    const auto concurrent_region_plan = derive_alignment_region_storage_plan(
        concurrent_regions.front(), concurrency_anchors, 1000u, 2000u, params);
    const auto concurrent_selected =
        derive_selected_alignment_region_storage_plan<base_alignment_algorithm::ksw2>(
            concurrent_regions.front(),
            concurrency_anchors,
            concurrent_region_plan,
            params,
            align_bandwidth,
            geometry);
    REQUIRE(concurrent_selected.has_value());
    REQUIRE(concurrent_selected->temporary_workspace_bytes > 0u);
    REQUIRE(concurrent_selected->temporary_workspace_bytes <=
            (std::numeric_limits<std::uint64_t>::max)() / 2u);
    std::array<std::uint64_t, 4> workspace_scratch{};
    CHECK(admitted_alignment_worker_concurrency<base_alignment_algorithm::ksw2>(
              concurrent_regions,
              concurrency_anchors,
              1000u,
              2000u,
              params,
              align_bandwidth,
              geometry,
              true,
              true,
              concurrent_regions.size(),
              concurrent_selected->temporary_workspace_bytes * 2u,
              workspace_scratch) == 2u);
    CHECK(admitted_alignment_worker_concurrency<base_alignment_algorithm::ksw2>(
              concurrent_regions,
              concurrency_anchors,
              1000u,
              2000u,
              params,
              align_bandwidth,
              geometry,
              true,
              true,
              concurrent_regions.size(),
              concurrent_selected->temporary_workspace_bytes - 1u,
              workspace_scratch) == 0u);
}
#endif

TEST_CASE("alignment workers use the existing region and seed thresholds",
          "[component:bioseq][layer:unit][feature:alignment]") {
    constexpr auto k_region_count =
        bioseq::parallelism::task_parallel_minimums::k_region_count;
    constexpr auto k_seed_count =
        bioseq::parallelism::task_parallel_minimums::k_seed_count;
    std::vector<alignment_region> regions(k_region_count);
    CHECK(alignment_worker_concurrency(regions, true, true, 2u) == 1u);
    regions.front().seed_count = static_cast<std::int32_t>(k_seed_count - 1u);
    CHECK(alignment_worker_concurrency(regions, true, true, 2u) == 1u);
    regions.back().seed_count = 1;
    CHECK(alignment_worker_concurrency(regions, true, true, 2u) == 2u);
    CHECK(alignment_worker_concurrency(regions, false, true, 2u) == 1u);
    CHECK(alignment_worker_concurrency(regions, true, false, 2u) == 1u);
    CHECK(alignment_worker_concurrency(regions, true, true, 1u) == 1u);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    CHECK(alignment_worker_concurrency(
        regions, true, true, regions.size() + 1u) == 2u);
#else
    CHECK(alignment_worker_concurrency(
        regions, true, true, regions.size() + 1u) == regions.size());
#endif
    CHECK(alignment_worker_concurrency(
        std::span<const alignment_region>{regions.data(), regions.size() - 1u},
        true, true, 2u) == 1u);
    regions.back().seed_count = -1;
    CHECK(alignment_worker_concurrency(regions, true, true, 2u) == 1u);
}

} // namespace bioseq::simd
