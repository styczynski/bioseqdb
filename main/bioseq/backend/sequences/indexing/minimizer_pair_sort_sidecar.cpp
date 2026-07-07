#include "common.h"

#include <hwy/highway.h>

#include <cstddef>

import bioseq.simd.sorting;

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
