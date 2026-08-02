#if defined(BIOSEQ_PG_SCHEDULER_DISPATCH_PROCESSOR_TARGET_INL_H_) == \
	defined(HWY_TARGET_TOGGLE)
#ifdef BIOSEQ_PG_SCHEDULER_DISPATCH_PROCESSOR_TARGET_INL_H_
#undef BIOSEQ_PG_SCHEDULER_DISPATCH_PROCESSOR_TARGET_INL_H_
#else
#define BIOSEQ_PG_SCHEDULER_DISPATCH_PROCESSOR_TARGET_INL_H_
#endif

#include <hwy/highway.h>

#include "pg/scheduler/dispatch_processor_hwy.h"
#include "sequences/chaining/chaining_simd_target_decls.h"

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

namespace {

const bioseq::simd_dispatch::HighwayKernelTable
	kDispatchProcessorKernels{
		.chain_anchors_simd = &ChainAnchorsSimdTarget,
	};

} // namespace

/**
 * Target-generated entry selected by the dispatch table.
 *
 * This is intentionally a scalar/pointer-only ABI.  Target-compiled scheduler
 * kernels attach below this function and call one another directly in this
 * namespace, where HWY_TARGET already denotes the runtime-selected target.
 */
void ProcessSequencesTarget(
	bioseq::simd_dispatch::DispatchProcessorTargetRequest* request) {
	if (request == nullptr || request->callback == nullptr) {
		return;
	}

	request->selected_target.target = static_cast<std::int64_t>(HWY_TARGET);
	request->selected_target.target_name = hwy::TargetName(HWY_TARGET);
	request->selected_target.kernels = &kDispatchProcessorKernels;
	const bioseq::simd_dispatch::ActiveHighwayTargetScope target_scope(
		&request->selected_target);
	request->callback(request->opaque, &request->selected_target);
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

#endif // toggle-safe target include
