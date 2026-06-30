module;

#if !defined(__GNUC__) || defined(__clang__)
#include <hwy/highway.h>
#include <hwy/print-inl.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#endif

export module bioseq.simd.utils;

#if !defined(__GNUC__) || defined(__clang__)
import bioseq.common.mlogging;

export {
#include "common/simd/utils_impl.hpp"
}
#endif
