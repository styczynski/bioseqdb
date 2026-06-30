module;

#if !defined(__GNUC__) || defined(__clang__)

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if HWY_TARGET == HWY_RVV || HWY_TARGET == HWY_SVE || HWY_TARGET == HWY_SVE2
#  error "bioseq_simd.vector_stream: vectors are sizeless on RVV/SVE; this module is only enabled for fixed-vector targets (NEON, SSE/AVX, AVX-512, SCALAR)."
#endif

#include "common/simd/utils_impl.hpp"

#endif

export module bioseq.simd.vector_stream;

#if !defined(__GNUC__) || defined(__clang__)

import bioseq.common.mlogging;

export {

#include "common/simd/vector_stream_impl.hpp"

}  // export

#endif
