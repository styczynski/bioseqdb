#pragma once

#include <cstdint>
#include <string_view>

namespace bioseq::simd_dispatch {

/**
 * Target masks owned by the one Highway runtime-dispatch root.
 *
 * `runtime_supported` intentionally is not restricted to code generated into
 * this binary.  `runnable` is the intersection used by Highway dispatch, and
 * `planned` is its best (least-significant) target bit.
 */
struct HighwayDispatchInfo {
	std::int64_t generated_targets = 0;
	std::int64_t runtime_supported_targets = 0;
	std::int64_t runnable_targets = 0;
	std::int64_t planned_target = 0;
};

[[nodiscard]] HighwayDispatchInfo highway_dispatch_info() noexcept;

/** Returns an empty view unless `target` contains exactly one Highway bit. */
[[nodiscard]] std::string_view highway_target_name(
	std::int64_t target) noexcept;

} // namespace bioseq::simd_dispatch
