/**
 * @file pair_sort.cppm
 * @brief SIMD sorting implementation for packed minimizer-pair runs.
 */
module;

#include <hwy/highway.h>

#include <cstddef>

module bioseq.indexing.minimizer_sequence;

import bioseq.simd.sorting;

namespace bioseq::indexing::detail {

/// Sort packed minimizer pairs through the selected SIMD policy.
void sort_minimizer_pair_run_keys(
        hwy::uint128_t* keys,
        std::size_t key_count,
        int sort_impl) noexcept {
    switch (sort_impl) {
    case 1:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vqsort(
            keys, key_count);
        return;
    case 2:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vector_stream<1>(
            keys, key_count);
        return;
    case 3:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vector_stream<2>(
            keys, key_count);
        return;
    case 4:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vector_stream<4>(
            keys, key_count);
        return;
    default:
        bioseq::simd::sorting::sort_unsigned_128_ascending(keys, key_count);
        return;
    }
}

} // namespace bioseq::indexing::detail
