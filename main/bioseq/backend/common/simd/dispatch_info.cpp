#include "common/simd/target_policy.h"

#include <hwy/targets.h>

#include "common/simd/dispatch_info.h"

namespace bioseq::simd_dispatch {

HighwayDispatchInfo highway_dispatch_info() noexcept {
	const std::int64_t generated = kGeneratedHighwayTargets;
	const std::int64_t runtime_supported = hwy::SupportedTargets();
	const std::int64_t runnable = generated & runtime_supported;
	const std::int64_t planned = runnable & -runnable;
	return HighwayDispatchInfo{
		.generated_targets = generated,
		.runtime_supported_targets = runtime_supported,
		.runnable_targets = runnable,
		.planned_target = planned,
	};
}

std::string_view highway_target_name(const std::int64_t target) noexcept {
	if (target == 0 || (target & (target - 1)) != 0) {
		return {};
	}
	return hwy::TargetName(target);
}

} // namespace bioseq::simd_dispatch
