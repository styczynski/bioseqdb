#pragma once

#include <cstdint>
#include <type_traits>

namespace bioseq::simd {
struct ChainAnchorsSimdRequest;
} // namespace bioseq::simd

namespace bioseq::simd_dispatch {

using ChainAnchorsSimdTargetFunction = void (*)(
	bioseq::simd::ChainAnchorsSimdRequest* request);

/**
 * Direct entry points compiled in the same HWY_NAMESPACE as the selected
 * processor entry.  Adding a target leaf means adding a pointer here and
 * initializing it with a direct `&LeafTarget` reference in the target inl.
 * No member of this table may perform runtime target selection.
 */
struct HighwayKernelTable {
	ChainAnchorsSimdTargetFunction chain_anchors_simd = nullptr;
};

/**
 * Scalar-only context passed across the Highway target boundary.
 *
 * No Highway vector, tag, or target-namespaced C++ type may enter this ABI.
 */
struct HighwayTargetContext {
	std::int64_t target = 0;
	const char* target_name = nullptr;
	const HighwayKernelTable* kernels = nullptr;
};

using DispatchProcessorTargetCallback = void (*)(
	void* opaque,
	const HighwayTargetContext* selected_target);

/**
 * Neutral request used by the sole runtime dispatch in the scheduler.
 * `selected_target` is written before `callback` runs, so execution evidence
 * remains available even if the selected pipeline throws.
 */
struct DispatchProcessorTargetRequest {
	void* opaque = nullptr;
	DispatchProcessorTargetCallback callback = nullptr;
	HighwayTargetContext selected_target{};
};

static_assert(std::is_standard_layout_v<HighwayTargetContext>);
static_assert(std::is_trivially_copyable_v<HighwayTargetContext>);
static_assert(std::is_standard_layout_v<DispatchProcessorTargetRequest>);

/** The synchronously selected target, or null outside the processor call. */
[[nodiscard]] const HighwayTargetContext* active_highway_target() noexcept;

/**
 * Installs a selected target for direct leaf calls made by the synchronous
 * processor callback.  It restores the prior context on every exit path.
 */
class ActiveHighwayTargetScope final {
public:
	explicit ActiveHighwayTargetScope(
		const HighwayTargetContext* selected_target) noexcept;
	~ActiveHighwayTargetScope() noexcept;

	ActiveHighwayTargetScope(const ActiveHighwayTargetScope&) = delete;
	ActiveHighwayTargetScope& operator=(
		const ActiveHighwayTargetScope&) = delete;

private:
	const HighwayTargetContext* previous_target_ = nullptr;
};

} // namespace bioseq::simd_dispatch

namespace bioseq::simd {

/**
 * Enters the one Highway runtime-dispatch root and invokes the selected target
 * callback exactly once.  Downstream target code must use direct calls under
 * the selected HWY_TARGET; it must never call HWY_DYNAMIC_DISPATCH again.
 */
void dispatch_process_sequences_for_best_target(
	bioseq::simd_dispatch::DispatchProcessorTargetRequest* request);

} // namespace bioseq::simd
