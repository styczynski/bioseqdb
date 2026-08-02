#include <hwy/highway.h>

#include "sequences/chaining/chaining_simd_abi.h"

// Per-target declaration guard. foreach_target.h alternates
// HWY_TARGET_TOGGLE, so every generated namespace receives the declaration.
#if defined(BIOSEQ_CHAINING_SIMD_TARGET_DECLS_H_) == \
    defined(HWY_TARGET_TOGGLE)
#ifdef BIOSEQ_CHAINING_SIMD_TARGET_DECLS_H_
#undef BIOSEQ_CHAINING_SIMD_TARGET_DECLS_H_
#else
#define BIOSEQ_CHAINING_SIMD_TARGET_DECLS_H_
#endif

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

void ChainAnchorsSimdTarget(ChainAnchorsSimdRequest* request);

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#endif  // per-target include guard
