/**
 * @file
 * @brief Highway target inventory implementation for the neutral contract.
 */
module;

#include <hwy/targets.h>

#include <cstdint>
#include <string_view>
#include <vector>

module bioseq.highway.dispatch_contract;

namespace bioseq::simd_dispatch {

highway_dispatch_plan make_highway_dispatch_plan(
    const std::int64_t compiled_targets) noexcept
{
    const std::int64_t cpu_supported_targets = hwy::SupportedTargets();
    const std::int64_t runnable_targets =
        compiled_targets & cpu_supported_targets;
    const std::uint64_t runnable_bits =
        static_cast<std::uint64_t>(runnable_targets);
    // Highway assigns higher priority to lower target bits. Isolating the
    // least-significant runnable bit selects that target without allocating.
    const std::int64_t planned_target = static_cast<std::int64_t>(
        runnable_bits & (~runnable_bits + std::uint64_t{1}));

    return highway_dispatch_plan{
        .compiled_targets = compiled_targets,
        .cpu_supported_targets = cpu_supported_targets,
        .runnable_targets = runnable_targets,
        .planned_target = planned_target,
    };
}

std::vector<std::string_view> highway_target_names(const std::int64_t mask) {
    std::vector<std::string_view> names;
    std::uint64_t remaining = static_cast<std::uint64_t>(mask);

    while (remaining != 0) {
        const std::uint64_t target = remaining & (~remaining + 1u);
        names.emplace_back(
            hwy::TargetName(static_cast<std::int64_t>(target)));
        remaining &= remaining - 1u;
    }

    return names;
}

} // namespace bioseq::simd_dispatch
