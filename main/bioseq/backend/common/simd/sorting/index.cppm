/**
 * @file index.cppm
 * @brief SIMD sorting wrappers exposed as a C++26 module.
 */
module;

#if !defined(__GNUC__) || defined(__clang__)

#include <hwy/highway.h>

#include "common.h"

#ifndef VQSORT_ONLY_STATIC
#define VQSORT_ONLY_STATIC 1
#endif

#include "hwy/contrib/sort/order.h"
#include "hwy/contrib/sort/vqsort-inl.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/simd/utils_impl.hpp"
#include "common/simd/vector_stream_impl.hpp"

#endif

export module bioseq.simd.sorting;

#if !defined(__GNUC__) || defined(__clang__)

import bioseq.common.mlogging;
import bioseq.simd.vector_stream;

export {

#include "common/simd/sorting/index_impl.hpp"

}  // export

#endif
