#include <tests/support/common.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

namespace bioseq::simd {
using namespace bioseq::prefilter;

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = compact_sequence_range<dna4_u64>;

[[nodiscard]] const alignment_kernels<seq::dna4>&
fixed_alignment_kernels() {
    const auto& root =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity = root.is_complete()
        ? root.resolve_alphabet_identity<seq::dna4>() : nullptr;
    if (identity == nullptr
        || root.resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated fixed-target test table lacks DNA4 identity"};
    }
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code
            && entry.typed_table != nullptr) {
            return *static_cast<const alignment_kernels<seq::dna4>*>(
                entry.typed_table);
        }
    }
    throw std::logic_error{
        "generated fixed-target test table lacks DNA4 alignment"};
}

[[nodiscard]] std::vector<seq::dna4> dna4_from_ascii(
        std::string_view text) {
    std::vector<seq::dna4> result(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        result[i].assign_char(text[i]);
    }
    return result;
}

[[nodiscard]] ::bioseq::chaining::anchor make_anchor(
        std::uint64_t query_position,
        std::uint64_t ref_position,
        bool reverse,
        std::uint8_t span = 8) {
    return ::bioseq::chaining::anchor{
        .ref_position = ref_position,
        .query_position = query_position,
        .is_reverse = static_cast<std::uint32_t>(reverse),
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

[[nodiscard]] ::bioseq::chaining::anchor make_anchor(
        std::uint64_t position,
        bool reverse,
        std::uint8_t span = 8) {
    return make_anchor(position, position, reverse, span);
}

[[nodiscard]] minimap2_parameters exact_alignment_params() {
    minimap2_parameters params;
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    params.highway_kernels = binding.kernels;
    params.highway_executed_target = binding.target;
    const auto* identity = binding.kernels == nullptr
            || !binding.kernels->is_complete()
        ? nullptr : binding.kernels->resolve_alphabet_identity<seq::dna4>();
    if (identity == nullptr
        || binding.kernels->resolve_alphabet_identity(
               identity->alphabet_code) != identity) {
        throw std::logic_error{
            "generated fixed-target test binding lacks DNA4 identity"};
    }
    params.alphabet_code = identity->alphabet_code;
    params.match_score = 2;
    params.mismatch_pen = 4;
    params.gap_open1 = 20;
    params.gap_ext1 = 5;
    params.gap_open2 = 40;
    params.gap_ext2 = 2;
    params.bandwidth = 64;
    params.zdrop = 100;
    params.max_gap = 0;
    params.min_ksw_len = 1;
    params.block_min_size = 16;
    params.block_max_size = 64;
    params.do_cigar = true;
    params.compute_edit_distance = true;
    return params;
}

[[nodiscard]] alignment_region make_region(
        std::span<const ::bioseq::chaining::anchor> anchors,
        sequence_size_t query_length) {
    alignment_region region;
    region.anchor_start = 0;
    region.seed_count = static_cast<std::int32_t>(anchors.size());
    set_region_coordinates(region, anchors, query_length);
    return region;
}

struct cigar_consumption {
    sequence_size_t query = 0;
    sequence_size_t target = 0;
};

[[nodiscard]] cigar_consumption measure_cigar_consumption(
        std::span<const cigar_operation> cigar) {
    cigar_consumption result;
    for (const cigar_operation op : cigar) {
        switch (op.opcode) {
            case cigar_match_opcode:
            case cigar_equal_opcode:
            case cigar_mismatch_opcode:
                result.query += op.length;
                result.target += op.length;
                break;
            case cigar_insertion_opcode:
                result.query += op.length;
                break;
            case cigar_deletion_opcode:
            case cigar_reference_skip_opcode:
                result.target += op.length;
                break;
            default:
                FAIL("unexpected CIGAR opcode in edit-distance fixture");
        }
    }
    return result;
}

struct backend_regions {
    alignment_region ksw2;
    alignment_region block;
    alignment_region batched;
};

enum class fixture_kind {
    single_seed_collision,
    multi_seed_mismatch,
    off_diagonal_gap,
    out_of_band_gap,
    left_extension_orientation,
};

[[nodiscard]] backend_regions run_fixture(
        bool reverse,
        fixture_kind kind,
        std::int32_t max_gap = 0,
        bool do_cigar = true) {
    const std::size_t query_length = kind == fixture_kind::out_of_band_gap
        ? 256u
        : 128u;
    const std::size_t target_length = kind == fixture_kind::out_of_band_gap
        ? 256u
        : 128u;
    std::string query_text = kind == fixture_kind::left_extension_orientation
        ? (reverse ? "TTTTTTTTACGTACGT" : "ACGTACGTAAAAAAAA")
        : std::string(query_length, reverse ? 'T' : 'A');
    std::string target_text = kind == fixture_kind::left_extension_orientation
        ? "ACGTAAAAAAAAA"
        : std::string(target_length, 'A');
    std::vector<::bioseq::chaining::anchor> anchors;
    if (kind == fixture_kind::single_seed_collision) {
        anchors.push_back(make_anchor(31, reverse));
        // The anchor claims [24, 32) as a seed, but two underlying ranks
        // differ.  Hash equality must not be treated as exact equality.
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
    range_t query_range(query_data);
    range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(lazy_target);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);
    minimap2_parameters params = exact_alignment_params();
    params.max_gap = max_gap;
    params.do_cigar = do_cigar;
    if (kind == fixture_kind::left_extension_orientation) {
        params.gap_open1 = 1;
        params.gap_ext1 = 1;
        params.gap_open2 = 1;
        params.gap_ext2 = 1;
        params.end_bonus = 100;
    }
    const auto anchor_span = std::span<const ::bioseq::chaining::anchor>{
        anchors.data(), anchors.size()};

    backend_regions result{
        .ksw2 = make_region(anchor_span, query_text.size()),
        .block = make_region(anchor_span, query_text.size()),
        .batched = make_region(anchor_span, query_text.size()),
    };

    align_regions<base_alignment_algorithm::ksw2>(
        std::span<alignment_region>{&result.ksw2, 1},
        anchor_span, query_view, target_view, scoring,
        fixed_alignment_kernels(), params);
    align_regions<base_alignment_algorithm::block_aligner>(
        std::span<alignment_region>{&result.block, 1},
        anchor_span, query_view, target_view, scoring,
        fixed_alignment_kernels(), params);
    fixed_alignment_kernels().align_ksw2_batched(
        std::span<alignment_region>{&result.batched, 1},
        anchor_span, query_view, target_view, scoring, params, nullptr);
    return result;
}

void check_exact_region(
        const alignment_region& region,
        sequence_size_t expected_edit_distance,
        sequence_size_t expected_query_length,
        sequence_size_t expected_target_length) {
    CHECK(region.edit_distance == expected_edit_distance);
    const cigar_consumption consumed = measure_cigar_consumption(
        std::span<const cigar_operation>{
            region.cigar.data(), region.cigar.size()});
    CHECK(consumed.query == expected_query_length);
    CHECK(consumed.target == expected_target_length);
    CHECK(region.query_end - region.query_start == consumed.query);
    CHECK(region.ref_end - region.ref_start == consumed.target);
}

}  // namespace

TEST_CASE("implicit seed CIGAR assembly is one neutral canonical contract",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    const auto assembled = [](std::size_t query_length,
                              std::size_t target_length) {
        std::vector<cigar_operation> cigar;
        append_implicit_seed_cigar(cigar, query_length, target_length);
        return format_cigar(cigar);
    };

    CHECK(assembled(5u, 5u) == "5M");
    CHECK(assembled(7u, 5u) == "5M2I");
    CHECK(assembled(5u, 7u) == "5M2D");
    CHECK(assembled(4u, 0u) == "4I");
    CHECK(assembled(0u, 4u) == "4D");
    CHECK(assembled(0u, 0u).empty());

    std::vector<cigar_operation> existing{
        cigar_operation{.length = 3u, .opcode = cigar_match_opcode}};
    append_implicit_seed_cigar(existing, 5u, 7u);
    CHECK(format_cigar(existing) == "8M2D");
}

TEST_CASE("Single-seed exact edit accounting compares implicit seed ranks",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(
            reverse, fixture_kind::single_seed_collision);
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
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(
            reverse, fixture_kind::multi_seed_mismatch);
        check_exact_region(regions.ksw2, 2, 40, 40);
        check_exact_region(regions.block, 2, 40, 40);
        check_exact_region(regions.batched, 2, 40, 40);
    }
}

TEST_CASE("Exact edit accounting forces traceback when CIGAR was not requested",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(
            reverse, fixture_kind::single_seed_collision, 16, false);
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
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(
            reverse, fixture_kind::left_extension_orientation, 16);
        CHECK(regions.ksw2.edit_distance == 3);
        CHECK(regions.batched.edit_distance == 3);
        CHECK(regions.batched.query_start == regions.ksw2.query_start);
        CHECK(regions.batched.query_end == regions.ksw2.query_end);
        CHECK(regions.batched.ref_start == regions.ksw2.ref_start);
        CHECK(regions.batched.ref_end == regions.ksw2.ref_end);
        CHECK(format_cigar(regions.batched.cigar)
              == format_cigar(regions.ksw2.cigar));
        check_exact_region(regions.ksw2, 3, 16, 13);
        check_exact_region(regions.batched, 3, 16, 13);
    }
}

TEST_CASE("Exact gap fills disable z-drop with the supported sentinel",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(
            reverse, fixture_kind::off_diagonal_gap);
        check_exact_region(regions.ksw2, 3, 40, 43);
        check_exact_region(regions.block, 3, 40, 43);
        check_exact_region(regions.batched, 3, 40, 43);
    }
}

TEST_CASE("Exact mandatory gaps are unbanded before SneakySnake rejection",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    constexpr sequence_size_t query_gap_length = 40;
    constexpr sequence_size_t target_gap_length = 177;
    constexpr sequence_size_t length_difference =
        target_gap_length - query_gap_length;
    const minimap2_parameters params = exact_alignment_params();
    const std::int32_t normal_align_bandwidth = static_cast<std::int32_t>(
        params.bandwidth * 1.5 + 1.0);
    REQUIRE(length_difference
            > static_cast<sequence_size_t>(normal_align_bandwidth));

    const std::vector<std::uint8_t> query_ranks(query_gap_length, 0u);
    const std::vector<std::uint8_t> target_ranks(target_gap_length, 0u);
    const prefilter_result prefilter = sneaky_snake_scalar(
        query_ranks, target_ranks, 16u);
    REQUIRE(prefilter.lower_bound == length_difference);
    REQUIRE_FALSE(is_prefilter_accepted(prefilter));

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const backend_regions regions = run_fixture(
            reverse, fixture_kind::out_of_band_gap);
        check_exact_region(
            regions.ksw2,
            length_difference,
            query_gap_length,
            target_gap_length);
        check_exact_region(
            regions.block,
            length_difference,
            query_gap_length,
            target_gap_length);
        check_exact_region(
            regions.batched,
            length_difference,
            query_gap_length,
            target_gap_length);
        CHECK(regions.batched.query_start == regions.ksw2.query_start);
        CHECK(regions.batched.query_end == regions.ksw2.query_end);
        CHECK(regions.batched.ref_start == regions.ksw2.ref_start);
        CHECK(regions.batched.ref_end == regions.ksw2.ref_end);
        CHECK(regions.batched.edit_distance == regions.ksw2.edit_distance);
        CHECK(format_cigar(regions.batched.cigar)
              == format_cigar(regions.ksw2.cigar));
        CHECK(prefilter.lower_bound == regions.ksw2.edit_distance);
    }
}

TEST_CASE("Batched exact CIGAR and coordinates match unbatched KSW2 extensions",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        for (const fixture_kind kind : {
                 fixture_kind::single_seed_collision,
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
            CHECK(format_cigar(regions.batched.cigar)
                  == format_cigar(regions.ksw2.cigar));

            const cigar_consumption consumed = measure_cigar_consumption(
                std::span<const cigar_operation>{
                    regions.batched.cigar.data(),
                    regions.batched.cigar.size()});
            CHECK(consumed.query
                  == regions.batched.query_end - regions.batched.query_start);
            CHECK(consumed.target
                  == regions.batched.ref_end - regions.batched.ref_start);
        }
    }
}

}  // namespace bioseq::simd
