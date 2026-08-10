/**
 * @file bench/prefilter_algorithm_selection.cppm
 * @brief Selects fixed-threshold prefilter benchmark algorithms.
 */

module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

export module bioseq.bench.prefilter_algorithm_selection;

export namespace bioseq::bench::prefilter_algorithm_selection {

/**
 * @brief Resolves the fixed edit threshold encoded by an algorithm name.
 *
 * @param[in] algorithm Semantic benchmark algorithm name.
 * @return The fixed maximum edit count, or an empty value for another
 *         algorithm family.
 */
[[nodiscard]] constexpr std::optional<std::uint32_t>
fixed_max_edits_for_algorithm(std::string_view algorithm) noexcept
{
    if (algorithm == "sneakysnake_vulkan_e2"
        || algorithm == "gatekeeper_vulkan_e2") {
        return 2u;
    }
    if (algorithm == "sneakysnake_vulkan_e4"
        || algorithm == "gatekeeper_vulkan_e4") {
        return 4u;
    }
    return std::nullopt;
}

/**
 * @brief Reports whether an algorithm selects the GateKeeper backend.
 *
 * @param[in] algorithm Semantic benchmark algorithm name.
 * @return `true` when the algorithm selects a fixed-threshold GateKeeper row.
 */
[[nodiscard]] constexpr bool
is_gatekeeper_algorithm(std::string_view algorithm) noexcept
{
    return algorithm.starts_with("gatekeeper_")
        && fixed_max_edits_for_algorithm(algorithm).has_value();
}

/**
 * @brief Reports whether an algorithm belongs to the fixed-threshold family.
 *
 * @param[in] algorithm Semantic benchmark algorithm name.
 * @return `true` when the algorithm has a supported fixed edit threshold.
 */
[[nodiscard]] constexpr bool is_algorithm(
    std::string_view algorithm) noexcept
{
    return fixed_max_edits_for_algorithm(algorithm).has_value();
}

/** @brief Hardware validation state required before publishing a result row. */
struct hardware_validation_stats {
    bool input_valid = false;
    bool backend_available = false;
    bool validation_performed = false;
    bool validation_passed = false;
    std::size_t result_count = 0u;
    std::size_t expected_result_count = 0u;
};

/**
 * @brief Validates the hardware result required to publish measurements.
 *
 * @param[in] stats Observed hardware execution and result cardinality.
 * @return `true` only when validation ran, passed, and produced the expected
 *         result count.
 */
[[nodiscard]] constexpr bool has_required_hardware_validation(
    const hardware_validation_stats& stats) noexcept
{
    return stats.input_valid
        && stats.backend_available
        && stats.validation_performed
        && stats.validation_passed
        && stats.result_count == stats.expected_result_count;
}

}  // namespace bioseq::bench::prefilter_algorithm_selection
