#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bioseq::bench::prefilters::small_seed {

/// Resolve the fixed edit threshold encoded by a small-seed benchmark key.
/// Keeping this selector shared between production and its config regression
/// tests prevents the benchmark label from silently drifting away from E.
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

[[nodiscard]] constexpr bool
is_gatekeeper_algorithm(std::string_view algorithm) noexcept
{
    return algorithm.starts_with("gatekeeper_")
        && fixed_max_edits_for_algorithm(algorithm).has_value();
}

[[nodiscard]] constexpr bool is_algorithm(
    std::string_view algorithm) noexcept
{
    return fixed_max_edits_for_algorithm(algorithm).has_value();
}

/** Hardware execution evidence required before publishing a small-seed row. */
struct hardware_validation_evidence {
    bool input_valid = false;
    bool backend_available = false;
    bool validation_performed = false;
    bool validation_passed = false;
    std::size_t result_count = 0u;
    std::size_t expected_result_count = 0u;
};

/// Fail-closed evidence predicate used before a small-seed row may emit
/// performance measurements into its blocking comparison rules.
[[nodiscard]] constexpr bool has_required_hardware_validation(
    const hardware_validation_evidence& evidence) noexcept
{
    return evidence.input_valid
        && evidence.backend_available
        && evidence.validation_performed
        && evidence.validation_passed
        && evidence.result_count == evidence.expected_result_count;
}

}  // namespace bioseq::bench::prefilters::small_seed
