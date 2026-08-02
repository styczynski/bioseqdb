/**
 * @file highway_dispatch_contract_impl.cppm
 * @brief Cold-path Highway target inventory for the neutral dispatch contract.
 */
module;

#include <hwy/targets.h>

#include <cstdint>
#include <string_view>
#include <vector>

module bioseq.highway.dispatch_contract;

namespace bioseq::simd_dispatch {

HighwayDispatchInfo highway_dispatch_info(
    const std::int64_t compiled_targets) noexcept
{
    const std::int64_t cpu_supported_targets = hwy::SupportedTargets();
    const std::int64_t runnable_targets =
        compiled_targets & cpu_supported_targets;
    std::int64_t planned_target = 0;
    for (const std::int64_t target :
         hwy::SupportedAndGeneratedTargets()) {
        if ((runnable_targets & target) != 0) {
            planned_target = target;
            break;
        }
    }

    return HighwayDispatchInfo{
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

}  // namespace bioseq::simd_dispatch
