#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Test-private ABI between ordinary Catch2 receipts and the four generated
// implementation units of the configured fixed-target primary. No Highway
// tag, vector, mask, iterator, or target namespace crosses this boundary.

struct bioseq_highway_simd_utils_probe_receipt final {
    std::int64_t target{};
    std::uint64_t passed_properties{};
    std::uint32_t cases_run{};
    std::uint32_t cases_passed{};
    std::uint32_t u8_lanes{};
    std::uint32_t u16_lanes{};
    std::uint32_t u32_lanes{};
    std::uint32_t u64_lanes{};
};

struct bioseq_highway_sequence_ops_probe_receipt final {
    std::int64_t target{};
    std::uint64_t passed_properties{};
    std::uint32_t cases_run{};
    std::uint32_t cases_passed{};
};

struct bioseq_highway_hashers_probe_receipt final {
    std::int64_t target{};
    std::uint64_t passed_properties{};
    std::uint32_t cases_run{};
    std::uint32_t cases_passed{};
};

struct bioseq_highway_two_stack_probe_receipt final {
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
    k_bioseq_highway_two_stack_reference_output_capacity = 64u;

struct bioseq_highway_two_stack_reference_output_pair final {
    std::uint64_t last_position{};
    std::uint64_t kmer{};
};

struct bioseq_highway_two_stack_reference_output_receipt final {
    std::int64_t target{};
    std::uint64_t pair_count{};
    std::uint32_t input_storage_unit_bytes{};
    std::uint32_t window{};
    std::uint32_t kmer{};
    bioseq_highway_two_stack_reference_output_pair
        pairs[k_bioseq_highway_two_stack_reference_output_capacity]{};
};

static_assert(std::is_standard_layout_v<
              bioseq_highway_simd_utils_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_sequence_ops_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_hashers_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_two_stack_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_two_stack_reference_output_pair>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_two_stack_reference_output_receipt>);
static_assert(std::is_trivially_copyable_v<
              bioseq_highway_two_stack_reference_output_pair>);
static_assert(std::is_trivially_copyable_v<
              bioseq_highway_two_stack_reference_output_receipt>);

extern "C" bool bioseq_highway_run_simd_utils_probe(
    bioseq_highway_simd_utils_probe_receipt*) noexcept;
extern "C" bool bioseq_highway_run_sequence_ops_probe(
    bioseq_highway_sequence_ops_probe_receipt*) noexcept;
extern "C" bool bioseq_highway_run_hashers_probe(
    bioseq_highway_hashers_probe_receipt*) noexcept;
extern "C" bool bioseq_highway_run_two_stack_probe(
    bioseq_highway_two_stack_probe_receipt*) noexcept;
// Test-private access to the original dna4/u8/W10/K4 wrap-path fingerprint.
// The implementation rejects every other shape rather than widening the
// production two-stack route contract.
extern "C" bool bioseq_highway_collect_two_stack_reference_output(
    const std::uint8_t* ranks,
    std::uint64_t rank_count,
    std::uint32_t input_storage_unit_bytes,
    std::uint32_t window,
    std::uint32_t kmer,
    bioseq_highway_two_stack_reference_output_receipt*) noexcept;
