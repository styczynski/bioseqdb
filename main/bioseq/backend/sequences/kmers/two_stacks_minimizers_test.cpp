#include "common_tests.h"

#include <cstdint>
#include <limits>
#include <string_view>

import bioseq.sequences.kmers.two_stacks;

namespace {

using bioseq::minimizers::MinimapSketchData;
using bioseq::simd::PackedPaperNthashExecutionProof;
using bioseq::simd::TwoStackContinuationStatus;
using bioseq::simd::exact_simd_minimizer_representation;
using bioseq::simd::packed_paper_nthash_scheme_id;
using bioseq::simd::two_stack_status_name;

struct unsupported_exact_representation {};

static_assert(exact_simd_minimizer_representation<MinimapSketchData>);
static_assert(
    !exact_simd_minimizer_representation<unsupported_exact_representation>);
static_assert(
    two_stack_status_name(TwoStackContinuationStatus::unsupported_vector_shape)
    == std::string_view{"unsupported_vector_shape"});

[[nodiscard]] constexpr PackedPaperNthashExecutionProof
valid_paper_proof(const std::uint64_t input_symbols) noexcept {
    PackedPaperNthashExecutionProof proof{};
    proof.scheme_id = packed_paper_nthash_scheme_id;
    proof.kmer_size = 15u;
    proof.window_size = 10u;
    proof.nthash_rotation = 7u;
    proof.source_storage_unit_bytes = sizeof(std::uint32_t);
    proof.input_symbols = input_symbols;
    proof.input_storage_units = input_symbols / 16u;
    proof.segments_combined = 1u;
    proof.logical_streams = 8u;
    proof.physical_vector_lanes = 4u;
    proof.vector_registers_per_step = 2u;
    proof.packed_decode_batches = 1u;
    proof.packed_decode_lane_visits = 8u;
    proof.hash_initialization_batches = 1u;
    proof.rolling_hash_batches = 1u;
    proof.hash_table_lookup_batches = 1u;
    proof.canonical_sum_batches = 1u;
    proof.window_update_batches = 1u;
    proof.emission_decision_batches = 1u;
    proof.emission_mask_store_batches = 1u;
    proof.minimizers_emitted = 2u;
    proof.scalar_collector_appends = 2u;
    proof.positions_are_segment_local = true;
    return proof;
}

TEST_CASE("packed paper proof aggregation is identity checked and saturating",
          "[two_stack][contract][packed_paper_nthash][proof]") {
    const auto first = valid_paper_proof(128u);
    const auto second = valid_paper_proof(256u);
    REQUIRE(first.accepted());
    REQUIRE(second.accepted());

    auto combined = first;
    combined.combine(second);
    REQUIRE(combined.segments_combined == 2u);
    REQUIRE(combined.input_symbols
        == first.input_symbols + second.input_symbols);
    REQUIRE(combined.accepted());

    auto unchanged = first;
    unchanged.combine(PackedPaperNthashExecutionProof{});
    REQUIRE(unchanged == first);

    PackedPaperNthashExecutionProof adopted{};
    adopted.combine(first);
    REQUIRE(adopted == first);

    auto incompatible = second;
    ++incompatible.kmer_size;
    auto mismatch = first;
    mismatch.combine(incompatible);
    REQUIRE(mismatch.aggregation_mismatch);
    REQUIRE_FALSE(mismatch.accepted());

    auto saturated = first;
    saturated.input_symbols =
        (std::numeric_limits<std::uint64_t>::max)();
    saturated.combine(second);
    REQUIRE(saturated.input_symbols
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(saturated.saturated);
    REQUIRE_FALSE(saturated.accepted());
}

} // namespace
