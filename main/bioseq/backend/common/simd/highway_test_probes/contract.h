#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Test-private ABI between ordinary Catch2 receipts and the four generated
// implementation units of the configured fixed-target primary. No Highway
// tag, vector, mask, iterator, or target namespace crosses this boundary.

struct BioseqHighwaySimdUtilsProbeReceipt final {
    std::int64_t target{};
    std::uint64_t passed_properties{};
    std::uint32_t cases_run{};
    std::uint32_t cases_passed{};
    std::uint32_t u8_lanes{};
    std::uint32_t u16_lanes{};
    std::uint32_t u32_lanes{};
    std::uint32_t u64_lanes{};
};

struct BioseqHighwaySequenceOpsProbeReceipt final {
    std::int64_t target{};
    std::uint64_t passed_properties{};
    std::uint32_t cases_run{};
    std::uint32_t cases_passed{};
};

struct BioseqHighwayHashersProbeReceipt final {
    std::int64_t target{};
    std::uint64_t passed_properties{};
    std::uint32_t cases_run{};
    std::uint32_t cases_passed{};
};

struct BioseqHighwayTwoStackProbeReceipt final {
    std::int64_t target{};
    std::uint64_t passed_properties{};
    std::uint32_t cases_run{};
    std::uint32_t cases_passed{};
    std::uint32_t lane_cases{};
    std::uint32_t mask_cases{};
    std::uint32_t swar_cases{};
    std::uint32_t decode_cases{};
    std::uint32_t selector_cases{};
};

inline constexpr std::size_t
    BIOSEQ_HIGHWAY_TWO_STACK_LEGACY_OUTPUT_CAPACITY = 64u;

struct BioseqHighwayTwoStackLegacyOutputPair final {
    std::uint64_t last_pos{};
    std::uint64_t kmer{};
};

struct BioseqHighwayTwoStackLegacyOutputReceipt final {
    std::int64_t target{};
    std::uint64_t pair_count{};
    std::uint32_t input_storage_unit_bytes{};
    std::uint32_t window{};
    std::uint32_t kmer{};
    BioseqHighwayTwoStackLegacyOutputPair
        pairs[BIOSEQ_HIGHWAY_TWO_STACK_LEGACY_OUTPUT_CAPACITY]{};
};

static_assert(std::is_standard_layout_v<BioseqHighwaySimdUtilsProbeReceipt>);
static_assert(std::is_standard_layout_v<BioseqHighwaySequenceOpsProbeReceipt>);
static_assert(std::is_standard_layout_v<BioseqHighwayHashersProbeReceipt>);
static_assert(std::is_standard_layout_v<BioseqHighwayTwoStackProbeReceipt>);
static_assert(std::is_standard_layout_v<
    BioseqHighwayTwoStackLegacyOutputPair>);
static_assert(std::is_standard_layout_v<
    BioseqHighwayTwoStackLegacyOutputReceipt>);
static_assert(std::is_trivially_copyable_v<
    BioseqHighwayTwoStackLegacyOutputPair>);
static_assert(std::is_trivially_copyable_v<
    BioseqHighwayTwoStackLegacyOutputReceipt>);

extern "C" bool bioseq_highway_probe_simd_utils(
    BioseqHighwaySimdUtilsProbeReceipt*) noexcept;
extern "C" bool bioseq_highway_probe_sequence_ops(
    BioseqHighwaySequenceOpsProbeReceipt*) noexcept;
extern "C" bool bioseq_highway_probe_hashers(
    BioseqHighwayHashersProbeReceipt*) noexcept;
extern "C" bool bioseq_highway_probe_two_stack(
    BioseqHighwayTwoStackProbeReceipt*) noexcept;
// Test-private access to the original dna4/u8/W10/K4 wrap-path fingerprint.
// The implementation rejects every other shape rather than widening the
// production two-stack route contract.
extern "C" bool bioseq_highway_probe_two_stack_legacy_output(
    const std::uint8_t* ranks,
    std::uint64_t rank_count,
    std::uint32_t input_storage_unit_bytes,
    std::uint32_t window,
    std::uint32_t kmer,
    BioseqHighwayTwoStackLegacyOutputReceipt*) noexcept;
