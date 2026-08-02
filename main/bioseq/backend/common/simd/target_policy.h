#pragma once

/*
 * This is the common include point for every classic translation unit in the
 * dynamically dispatched Highway closure.  It must be included before any
 * Highway header so every stage observes the same generated-target policy.
 *
 * Do not hard-code an architecture or raise the compiler baseline here.  The
 * Highway/compiler policy deliberately determines HWY_TARGETS, which keeps
 * cross-compiled binaries portable and makes build flags the single source of
 * truth.  The constants below expose that policy for compile/link checks and
 * runtime reporting without changing it.
 */
#if defined(HWY_COMPILE_ONLY_STATIC)
#error "Bioseq's dispatched Highway closure requires multiple generated targets"
#endif

#include <hwy/detect_targets.h>

#include <cstdint>

namespace bioseq::simd_dispatch {

inline constexpr std::int64_t kGeneratedHighwayTargets =
	static_cast<std::int64_t>(HWY_TARGETS);
inline constexpr std::int64_t kStaticHighwayTarget =
	static_cast<std::int64_t>(HWY_STATIC_TARGET);

static_assert(kGeneratedHighwayTargets != 0);
static_assert(
	(kGeneratedHighwayTargets & kStaticHighwayTarget) != 0,
	"Highway's generated target set must contain its static target");

} // namespace bioseq::simd_dispatch
