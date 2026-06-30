#include "common.h"

#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__GNUC__) && !defined(__clang__)
#ifndef VQSORT_ONLY_STATIC
#define VQSORT_ONLY_STATIC 1
#endif
#include "hwy/contrib/sort/order.h"
#include "hwy/contrib/sort/vqsort-inl.h"
#include "common/simd/utils_impl.hpp"
#include "common/simd/vector_stream_impl.hpp"
#include "common/simd/sorting/index_impl.hpp"
#else
import bioseq.simd.sorting;
#endif

extern "C" void bioseq_sort_pair_run_u128_keys(
        hwy::uint128_t* keys,
        std::size_t key_count,
        int sort_impl) noexcept {
    switch (sort_impl) {
    case 1:
        bioseq::simd::sorting::sort_u128_ascending_vqsort(keys, key_count);
        return;
    case 2:
        bioseq::simd::sorting::sort_u128_ascending_vec_stream<1>(keys, key_count);
        return;
    case 3:
        bioseq::simd::sorting::sort_u128_ascending_vec_stream<2>(keys, key_count);
        return;
    case 4:
        bioseq::simd::sorting::sort_u128_ascending_vec_stream<4>(keys, key_count);
        return;
    default:
        bioseq::simd::sorting::sort_u128_ascending(keys, key_count);
        return;
    }
}
