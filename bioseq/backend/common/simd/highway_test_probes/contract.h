#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

/**
 * @file
 * @brief Test-private ABI for generated fixed-target Highway probes.
 *
 * No Highway tag, vector, mask, iterator, or target namespace crosses this
 * boundary.
 */

/** @brief Receipt for target-local SIMD utility checks. */
struct bioseq_highway_simd_utils_probe_receipt final {
    std::int64_t target{};                ///< Compiled Highway target identifier.
    std::uint64_t passed_properties{};    ///< Bit set for each passing category.
    std::uint32_t cases_run{};            ///< Number of typed checks attempted.
    std::uint32_t cases_passed{};         ///< Number of typed checks passed.
    std::uint32_t u8_lanes{};             ///< Active unsigned-byte lanes.
    std::uint32_t u16_lanes{};            ///< Active unsigned-16-bit lanes.
    std::uint32_t u32_lanes{};            ///< Active unsigned-32-bit lanes.
    std::uint32_t u64_lanes{};            ///< Active unsigned-64-bit lanes.
};

/** @brief Receipt for target-local packed-sequence iteration checks. */
struct bioseq_highway_sequence_ops_probe_receipt final {
    std::int64_t target{};              ///< Compiled Highway target identifier.
    std::uint64_t passed_properties{};  ///< Bit set for each passing fixture.
    std::uint32_t cases_run{};          ///< Number of context checks attempted.
    std::uint32_t cases_passed{};       ///< Number of context checks passed.
};

/** @brief Receipt for target-local scalar and SIMD hash parity checks. */
struct bioseq_highway_hashers_probe_receipt final {
    std::int64_t target{};              ///< Compiled Highway target identifier.
    std::uint64_t passed_properties{};  ///< Bit set for each passing hasher case.
    std::uint32_t cases_run{};          ///< Number of hasher cases attempted.
    std::uint32_t cases_passed{};       ///< Number of hasher cases passed.
};

/** @brief Receipt for target-local two-stack minimizer checks. */
struct bioseq_highway_two_stack_probe_receipt final {
    std::int64_t target{};              ///< Compiled Highway target identifier.
    std::uint64_t passed_properties{};  ///< Bit set for each passing category.
    std::uint32_t cases_run{};          ///< Number of checks attempted.
    std::uint32_t cases_passed{};       ///< Number of checks passed.
    std::uint32_t lane_cases{};         ///< Lane-index checks attempted.
    std::uint32_t mask_cases{};         ///< Mask round-trip checks attempted.
    std::uint32_t swar_cases{};         ///< Packed-rank checks attempted.
    std::uint32_t decode_cases{};       ///< Decode and transpose checks attempted.
    std::uint32_t selector_cases{};     ///< Kernel-selection checks attempted.
};

/** @brief Maximum number of reference minimizer pairs returned by a probe. */
inline constexpr std::size_t
    k_bioseq_highway_two_stack_reference_output_capacity = 64U;

/** @brief One position and k-mer pair emitted by the reference probe. */
struct bioseq_highway_two_stack_reference_output_pair final {
    std::uint64_t last_position{};  ///< Final logical position of the minimizer.
    std::uint64_t kmer{};           ///< Canonical packed k-mer value.
};

/** @brief Fixed-capacity output receipt for the supported reference shape. */
struct bioseq_highway_two_stack_reference_output_receipt final {
    std::int64_t target{};              ///< Compiled Highway target identifier.
    std::uint64_t pair_count{};         ///< Number of populated pairs.
    std::uint32_t input_storage_unit_bytes{};  ///< Packed storage-unit width.
    std::uint32_t window{};             ///< Minimizer window used by the probe.
    std::uint32_t kmer{};               ///< K-mer length used by the probe.
    bioseq_highway_two_stack_reference_output_pair
        pairs[k_bioseq_highway_two_stack_reference_output_capacity]{};  ///< ABI array of pairs.
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

/**
 * @brief Runs SIMD utility checks for the configured fixed target.
 * @param[out] receipt Receives target identity, counters, and lane widths.
 * @return `true` when every utility check passes.
 */
extern "C" auto bioseq_highway_run_simd_utils_probe(
    bioseq_highway_simd_utils_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Runs packed-sequence iteration checks for the configured fixed target.
 * @param[out] receipt Receives target identity and result counters.
 * @return `true` when every iteration check passes.
 */
extern "C" auto bioseq_highway_run_sequence_ops_probe(
    bioseq_highway_sequence_ops_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Runs scalar and SIMD hash parity checks for the configured fixed target.
 * @param[out] receipt Receives target identity and result counters.
 * @return `true` when every hash check passes.
 */
extern "C" auto bioseq_highway_run_hashers_probe(
    bioseq_highway_hashers_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Runs two-stack minimizer checks for the configured fixed target.
 * @param[out] receipt Receives target identity and per-category counters.
 * @return `true` when every minimizer check passes.
 */
extern "C" auto bioseq_highway_run_two_stack_probe(
    bioseq_highway_two_stack_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Collects reference outputs for the supported DNA4/u8/W10/K4 shape.
 *
 * @param[in] ranks Contiguous DNA4 rank values.
 * @param[in] rank_count Number of input ranks.
 * @param[in] input_storage_unit_bytes Storage-unit width, required to be one.
 * @param[in] window Minimizer window, required to be ten.
 * @param[in] kmer K-mer length, required to be four.
 * @param[out] receipt Receives bounded position and k-mer pairs.
 * @return `true` when the shape is supported and collection succeeds.
 */
extern "C" auto bioseq_highway_collect_two_stack_reference_output(
    const std::uint8_t* ranks,
    std::uint64_t rank_count,
    std::uint32_t input_storage_unit_bytes,
    std::uint32_t window,
    std::uint32_t kmer,
    bioseq_highway_two_stack_reference_output_receipt* receipt) noexcept
    -> bool;
