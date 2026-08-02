/*
 * The scheduler's sole Highway runtime-dispatch root.
 *
 * foreach_target.h re-includes this file for each generated target.  Keep all
 * target-dependent definitions in dispatch_processor_target_inl.h and keep
 * the export/table plus ordinary wrappers under HWY_ONCE.
 */
#include "common/simd/target_policy.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "pg/scheduler/dispatch_processor_hwy.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "pg/scheduler/dispatch_processor_hwy.h"
#include "pg/scheduler/dispatch_processor_target_inl.h"

#if HWY_ONCE

namespace bioseq::simd {

HWY_EXPORT(ProcessSequencesTarget);

void dispatch_process_sequences_for_best_target(
	bioseq::simd_dispatch::DispatchProcessorTargetRequest* request) {
	HWY_DYNAMIC_DISPATCH(ProcessSequencesTarget)(request);
}

} // namespace bioseq::simd

namespace bioseq::simd_dispatch {

namespace {

thread_local const HighwayTargetContext* g_active_highway_target = nullptr;

} // namespace

const HighwayTargetContext* active_highway_target() noexcept {
	return g_active_highway_target;
}

ActiveHighwayTargetScope::ActiveHighwayTargetScope(
		const HighwayTargetContext* selected_target) noexcept
	: previous_target_(g_active_highway_target) {
	g_active_highway_target = selected_target;
}

ActiveHighwayTargetScope::~ActiveHighwayTargetScope() noexcept {
	g_active_highway_target = previous_target_;
}

} // namespace bioseq::simd_dispatch

#endif // HWY_ONCE
