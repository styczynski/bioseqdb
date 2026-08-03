#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.alignment.ksw2_batched;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences.minimap_port;
import bioseq.sequences.prefilters.sneaky_snake;
import bioseq.sequences.chaining.utils;
import bioseq.common.context;
import bioseq.storage.lazy;

namespace bioseq::simd {
using namespace bioseq::sequences::prefilters;

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = compact_sequence_range<dna4_u64>;

[[nodiscard]] const AlignmentKernels<seq::dna4>&
fixed_alignment_kernels() {
    const auto& root =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == 0u && entry.typed_table != nullptr) {
            return *static_cast<const AlignmentKernels<seq::dna4>*>(
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

[[nodiscard]] ::bioseq::chaining::Anchor make_anchor(
        std::uint64_t query_position,
        std::uint64_t ref_position,
        bool reverse,
        std::uint8_t span = 8) {
    return ::bioseq::chaining::Anchor{
        .ref_position = ref_position,
        .query_position = query_position,
        .is_reverse = static_cast<std::uint32_t>(reverse),
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

[[nodiscard]] ::bioseq::chaining::Anchor make_anchor(
        std::uint64_t position,
        bool reverse,
        std::uint8_t span = 8) {
    return make_anchor(position, position, reverse, span);
}

[[nodiscard]] Minimap2Params exact_alignment_params() {
    Minimap2Params params;
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    params.highway_kernels = binding.kernels;
    params.highway_executed_target = binding.target;
    params.alphabet_code = 0u;
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

[[nodiscard]] AlignRegion make_region(
        std::span<const ::bioseq::chaining::Anchor> anchors,
        sequence_size_t query_length) {
    AlignRegion region;
    region.anchor_start = 0;
    region.seed_count = static_cast<std::int32_t>(anchors.size());
    set_region_coordinates(region, anchors, query_length);
    return region;
}

struct CigarConsumption {
    sequence_size_t query = 0;
    sequence_size_t target = 0;
};

[[nodiscard]] CigarConsumption cigar_consumption(
        std::span<const CigarOp> cigar) {
    CigarConsumption result;
    for (const CigarOp op : cigar) {
        switch (op.opcode) {
            case CIGAR_MATCH:
            case CIGAR_EQ:
            case CIGAR_X:
                result.query += op.length;
                result.target += op.length;
                break;
            case CIGAR_INS:
                result.query += op.length;
                break;
            case CIGAR_DEL:
            case CIGAR_N_SKIP:
                result.target += op.length;
                break;
            default:
                FAIL("unexpected CIGAR opcode in edit-distance fixture");
        }
    }
    return result;
}

struct BackendRegions {
    AlignRegion ksw2;
    AlignRegion block;
    AlignRegion batched;
};

enum class FixtureKind {
    SingleSeedCollision,
    MultiSeedMismatch,
    OffDiagonalGap,
    OutOfBandGap,
    LeftExtensionOrientation,
};

[[nodiscard]] BackendRegions run_fixture(
        bool reverse,
        FixtureKind kind,
        std::int32_t max_gap = 0,
        bool do_cigar = true) {
    const std::size_t query_length = kind == FixtureKind::OutOfBandGap
        ? 256u
        : 128u;
    const std::size_t target_length = kind == FixtureKind::OutOfBandGap
        ? 256u
        : 128u;
    std::string query_text = kind == FixtureKind::LeftExtensionOrientation
        ? (reverse ? "TTTTTTTTACGTACGT" : "ACGTACGTAAAAAAAA")
        : std::string(query_length, reverse ? 'T' : 'A');
    std::string target_text = kind == FixtureKind::LeftExtensionOrientation
        ? "ACGTAAAAAAAAA"
        : std::string(target_length, 'A');
    std::vector<::bioseq::chaining::Anchor> anchors;
    if (kind == FixtureKind::SingleSeedCollision) {
        anchors.push_back(make_anchor(31, reverse));
        // The anchor claims [24, 32) as a seed, but two underlying ranks
        // differ.  Hash equality must not be treated as exact equality.
        target_text[26] = 'C';
        target_text[29] = 'C';
    } else if (kind == FixtureKind::MultiSeedMismatch) {
        anchors.push_back(make_anchor(31, reverse));
        anchors.push_back(make_anchor(63, reverse));
        target_text[40] = 'C';
        target_text[52] = 'C';
    } else if (kind == FixtureKind::OffDiagonalGap) {
        anchors.push_back(make_anchor(31, reverse));
        anchors.push_back(make_anchor(63, 66, reverse, 8));
    } else if (kind == FixtureKind::OutOfBandGap) {
        anchors.push_back(make_anchor(31, reverse));
        anchors.push_back(make_anchor(63, 200, reverse, 8));
    } else {
        anchors.push_back(make_anchor(15, 12, reverse, 8));
    }

    auto query_data = dna4_from_ascii(query_text);
    auto target_data = dna4_from_ascii(target_text);
    range_t query_range(query_data);
    range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::as_lazy_sequence_view(lazy_target);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 4, 1);
    Minimap2Params params = exact_alignment_params();
    params.max_gap = max_gap;
    params.do_cigar = do_cigar;
    if (kind == FixtureKind::LeftExtensionOrientation) {
        params.gap_open1 = 1;
        params.gap_ext1 = 1;
        params.gap_open2 = 1;
        params.gap_ext2 = 1;
        params.end_bonus = 100;
    }
    const auto anchor_span = std::span<const ::bioseq::chaining::Anchor>{
        anchors.data(), anchors.size()};

    BackendRegions result{
        .ksw2 = make_region(anchor_span, query_text.size()),
        .block = make_region(anchor_span, query_text.size()),
        .batched = make_region(anchor_span, query_text.size()),
    };

    align_regions<BaseAlignmentAlgorithmType::KSW2>(
        std::span<AlignRegion>{&result.ksw2, 1},
        anchor_span, query_view, target_view, scoring,
        fixed_alignment_kernels(), params);
    align_regions<BaseAlignmentAlgorithmType::BLOCK_ALIGNER>(
        std::span<AlignRegion>{&result.block, 1},
        anchor_span, query_view, target_view, scoring,
        fixed_alignment_kernels(), params);
    fixed_alignment_kernels().ksw2_batched(
        std::span<AlignRegion>{&result.batched, 1},
        anchor_span, query_view, target_view, scoring, params, nullptr);
    return result;
}

void check_exact_region(
        const AlignRegion& region,
        sequence_size_t expected_edit_distance,
        sequence_size_t expected_query_length,
        sequence_size_t expected_target_length) {
    CHECK(region.edit_distance == expected_edit_distance);
    const CigarConsumption consumed = cigar_consumption(
        std::span<const CigarOp>{region.cigar.data(), region.cigar.size()});
    CHECK(consumed.query == expected_query_length);
    CHECK(consumed.target == expected_target_length);
    CHECK(region.query_end - region.query_start == consumed.query);
    CHECK(region.ref_end - region.ref_start == consumed.target);
}

}  // namespace

TEST_CASE("implicit seed CIGAR assembly is one neutral canonical contract",
          "[cigar][implicit-seed][contract]") {
    const auto assembled = [](std::size_t query_length,
                              std::size_t target_length) {
        std::vector<CigarOp> cigar;
        append_implicit_seed_cigar(cigar, query_length, target_length);
        return format_cigar(cigar);
    };

    CHECK(assembled(5u, 5u) == "5M");
    CHECK(assembled(7u, 5u) == "5M2I");
    CHECK(assembled(5u, 7u) == "5M2D");
    CHECK(assembled(4u, 0u) == "4I");
    CHECK(assembled(0u, 4u) == "4D");
    CHECK(assembled(0u, 0u).empty());

    std::vector<CigarOp> existing{
        CigarOp{.length = 3u, .opcode = CIGAR_MATCH}};
    append_implicit_seed_cigar(existing, 5u, 7u);
    CHECK(format_cigar(existing) == "8M2D");
}

TEST_CASE("Single-seed exact edit accounting compares implicit seed ranks",
          "[edit-distance][single-seed][ksw2][block-aligner][batched]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const BackendRegions regions = run_fixture(
            reverse, FixtureKind::SingleSeedCollision);
        check_exact_region(regions.ksw2, 2, 8, 8);
        check_exact_region(regions.block, 2, 8, 8);
        check_exact_region(regions.batched, 2, 8, 8);
        CHECK(format_cigar(regions.ksw2.cigar) == "8M");
        CHECK(format_cigar(regions.block.cigar) == "8M");
        CHECK(format_cigar(regions.batched.cigar) == "8M");
    }
}

TEST_CASE("Multi-seed exact edit accounting is unchanged by implicit seed handling",
          "[edit-distance][multi-seed][ksw2][block-aligner][batched]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const BackendRegions regions = run_fixture(
            reverse, FixtureKind::MultiSeedMismatch);
        check_exact_region(regions.ksw2, 2, 40, 40);
        check_exact_region(regions.block, 2, 40, 40);
        check_exact_region(regions.batched, 2, 40, 40);
    }
}

TEST_CASE("Exact edit accounting forces traceback when CIGAR was not requested",
          "[edit-distance][single-seed][forced-traceback]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const BackendRegions regions = run_fixture(
            reverse, FixtureKind::SingleSeedCollision, 16, false);
        CHECK(regions.ksw2.edit_distance == 2);
        CHECK(regions.block.edit_distance == 2);
        CHECK(regions.batched.edit_distance == 2);
        CHECK(format_cigar(regions.ksw2.cigar) == "40M");
        CHECK(format_cigar(regions.block.cigar) == "40M");
        CHECK(format_cigar(regions.batched.cigar) == "40M");
    }
}

TEST_CASE("Left-extension exact counting uses source-order CIGAR windows",
          "[edit-distance][left-extension][orientation][ksw2][batched]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const BackendRegions regions = run_fixture(
            reverse, FixtureKind::LeftExtensionOrientation, 16);
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
          "[edit-distance][zdrop][ksw2][block-aligner][batched]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const BackendRegions regions = run_fixture(
            reverse, FixtureKind::OffDiagonalGap);
        check_exact_region(regions.ksw2, 3, 40, 43);
        check_exact_region(regions.block, 3, 40, 43);
        check_exact_region(regions.batched, 3, 40, 43);
    }
}

TEST_CASE("Exact mandatory gaps are unbanded before SneakySnake rejection",
          "[edit-distance][bandwidth][sneaky-snake][ksw2][block-aligner][batched]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    constexpr sequence_size_t query_gap_length = 40;
    constexpr sequence_size_t target_gap_length = 177;
    constexpr sequence_size_t length_difference =
        target_gap_length - query_gap_length;
    const Minimap2Params params = exact_alignment_params();
    const std::int32_t normal_align_bandwidth = static_cast<std::int32_t>(
        params.bandwidth * 1.5 + 1.0);
    REQUIRE(length_difference
            > static_cast<sequence_size_t>(normal_align_bandwidth));

    const std::vector<std::uint8_t> query_ranks(query_gap_length, 0u);
    const std::vector<std::uint8_t> target_ranks(target_gap_length, 0u);
    const PrefilterResult prefilter = sneaky_snake_scalar(
        query_ranks, target_ranks, 16u);
    REQUIRE(prefilter.lower_bound == length_difference);
    REQUIRE_FALSE(prefilter_accepted(prefilter));

    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const BackendRegions regions = run_fixture(
            reverse, FixtureKind::OutOfBandGap);
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
          "[edit-distance][coordinates][ksw2][batched][parity]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    for (const bool reverse : {false, true}) {
        for (const FixtureKind kind : {
                 FixtureKind::SingleSeedCollision,
                 FixtureKind::MultiSeedMismatch,
                 FixtureKind::OffDiagonalGap}) {
            CAPTURE(reverse, kind);
            const BackendRegions regions = run_fixture(reverse, kind, 16);
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

            const CigarConsumption consumed = cigar_consumption(
                std::span<const CigarOp>{
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
