/**
 * @file contract.cppm
 * @brief C-callable test functions and results shared across Highway CPU targets.
 *
 * Results use plain fields so tests can inspect a target's behavior without
 * importing its Highway vectors, masks or tags.
 */
export module bioseq.highway.test_probe_contract;

import std;

/** @brief Results of SIMD utility tests for one compiled CPU target. */
export struct bioseq_highway_simd_utils_probe_receipt final {
    std::int64_t target{};                ///< Compiled Highway target identifier.
    std::uint64_t passed_properties{};    ///< Bit set for each passing category.
    std::uint32_t cases_run{};            ///< Number of test cases counted across categories.
    std::uint32_t cases_passed{};         ///< cases_run if every category passed, otherwise zero.
    std::uint32_t u8_lanes{};             ///< Active unsigned-byte lanes.
    std::uint32_t u16_lanes{};            ///< Active unsigned-16-bit lanes.
    std::uint32_t u32_lanes{};            ///< Active unsigned-32-bit lanes.
    std::uint32_t u64_lanes{};            ///< Active unsigned-64-bit lanes.
};

/** @brief Results of packed-sequence iteration tests for one CPU target. */
export struct bioseq_highway_sequence_ops_probe_receipt final {
    std::int64_t target{};              ///< Compiled Highway target identifier.
    std::uint64_t passed_properties{};  ///< Bit set for each passing fixture.
    std::uint32_t cases_run{};          ///< Number of context checks attempted.
    std::uint32_t cases_passed{};       ///< Number of context checks passed.
};

/** @brief Results comparing scalar and SIMD hashes for one CPU target. */
export struct bioseq_highway_hashers_probe_receipt final {
    std::int64_t target{};              ///< Compiled Highway target identifier.
    std::uint64_t passed_properties{};  ///< Bit set for each passing hasher case.
    std::uint32_t cases_run{};          ///< Number of hasher cases attempted.
    std::uint32_t cases_passed{};       ///< Number of hasher cases passed.
};

/** @brief Results of two-stack minimizer tests for one CPU target. */
export struct bioseq_highway_two_stack_probe_receipt final {
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
export inline constexpr std::size_t
    k_bioseq_highway_two_stack_reference_output_capacity = 64U;

/** @brief One position and k-mer pair emitted by the reference probe. */
export struct bioseq_highway_two_stack_reference_output_pair final {
    std::uint64_t last_position{};  ///< Final logical position of the minimizer.
    std::uint64_t kmer{};           ///< Canonical packed k-mer value.
};

/** @brief Up to 64 reference minimizers and the parameters used to collect them. */
export struct bioseq_highway_two_stack_reference_output_receipt final {
    std::int64_t target{};              ///< Compiled Highway target identifier.
    std::uint64_t pair_count{};         ///< Number of populated pairs.
    std::uint32_t input_storage_unit_bytes{};  ///< Bytes per packed storage unit.
    std::uint32_t window{};             ///< Minimizer window used by the probe.
    std::uint32_t kmer{};               ///< K-mer length used by the probe.
    bioseq_highway_two_stack_reference_output_pair
        pairs[k_bioseq_highway_two_stack_reference_output_capacity]{};  ///< Sorted unique position and k-mer pairs.
};

static_assert(sizeof(std::int64_t) == 8U);
static_assert(sizeof(std::uint64_t) == 8U);
static_assert(sizeof(std::uint32_t) == 4U);
static_assert(std::is_standard_layout_v<
              bioseq_highway_simd_utils_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_sequence_ops_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_hashers_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_two_stack_probe_receipt>);
static_assert(
    std::is_trivially_copyable_v<bioseq_highway_two_stack_probe_receipt>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_two_stack_reference_output_pair>);
static_assert(std::is_standard_layout_v<
              bioseq_highway_two_stack_reference_output_receipt>);
static_assert(std::is_trivially_copyable_v<
              bioseq_highway_two_stack_reference_output_pair>);
static_assert(std::is_trivially_copyable_v<
              bioseq_highway_two_stack_reference_output_receipt>);
static_assert(sizeof(bioseq_highway_simd_utils_probe_receipt) == 40U);
static_assert(sizeof(bioseq_highway_sequence_ops_probe_receipt) == 24U);
static_assert(sizeof(bioseq_highway_hashers_probe_receipt) == 24U);
static_assert(alignof(bioseq_highway_two_stack_probe_receipt) == 8U);
static_assert(sizeof(bioseq_highway_two_stack_probe_receipt) == 48U);
static_assert(sizeof(bioseq_highway_two_stack_reference_output_pair) == 16U);
static_assert(
    sizeof(bioseq_highway_two_stack_reference_output_receipt) == 1056U);

/**
 * @brief Runs SIMD utility checks for the configured fixed target.
 * @param[out] receipt Receives target identity, counters, and lane widths.
 * @return `true` when every utility check passes.
 */
export extern "C" auto bioseq_highway_run_simd_utils_probe(
    bioseq_highway_simd_utils_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Runs packed-sequence iteration checks for the configured fixed target.
 * @param[out] receipt Receives target identity and result counters.
 * @return `true` when every iteration check passes.
 */
export extern "C" auto bioseq_highway_run_sequence_ops_probe(
    bioseq_highway_sequence_ops_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Checks that scalar and SIMD hashing produce the same position/hash sets.
 * @param[out] receipt Receives the CPU target and test counts.
 * @return True when all eight hasher and lane-width cases pass.
 */
export extern "C" auto bioseq_highway_run_hashers_probe(
    bioseq_highway_hashers_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Runs two-stack minimizer checks for the configured fixed target.
 * @param[out] receipt Receives target identity and per-category counters.
 * @return `true` when every minimizer check passes.
 */
export extern "C" auto bioseq_highway_run_two_stack_probe(
    bioseq_highway_two_stack_probe_receipt* receipt) noexcept -> bool;

/**
 * @brief Collects sorted unique reference minimizers for the DNA4 test fixture.
 * @param[in] ranks Fifty DNA4 ranks, each in the range 0..3.
 * @param[in] rank_count Must be 50.
 * @param[in] input_storage_unit_bytes Must be one byte.
 * @param[in] window Must be ten k-mers.
 * @param[in] kmer Must be four bases.
 * @param[out] receipt Receives at most 64 position and k-mer pairs.
 * @return True if the parameters and ranks are valid and the output fits.
 */
export extern "C" auto bioseq_highway_collect_two_stack_reference_output(
    const std::uint8_t* ranks,
    std::uint64_t rank_count,
    std::uint32_t input_storage_unit_bytes,
    std::uint32_t window,
    std::uint32_t kmer,
    bioseq_highway_two_stack_reference_output_receipt* receipt) noexcept
    -> bool;
