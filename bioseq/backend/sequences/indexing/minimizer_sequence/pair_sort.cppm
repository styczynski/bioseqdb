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

/**
 * @brief Sorts packed minimizer pairs through the selected SIMD policy.
 * @param[in,out] keys Array of `key_count` packed keys sorted in ascending order
 * @param[in] key_count Number of keys available at `keys`
 * @param[in] sort_implementation Stable sorter ordinal from `pair_run_sort_implementation`
 */
void sort_minimizer_pair_run_keys(hwy::uint128_t* keys,
                                  std::size_t key_count,
                                  int sort_implementation) noexcept {
    switch (sort_implementation) {
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

/**
 * @brief Sorts packed minimizer pairs for the module caller using the C linkage boundary.
 * @param[in,out] keys Array of `key_count` packed keys sorted in ascending order
 * @param[in] key_count Number of keys available at `keys`
 * @param[in] sort_implementation Stable sorter ordinal from `pair_run_sort_implementation`
 */
extern "C" void bioseq_sort_pair_run_u128_keys(hwy::uint128_t* keys,
                                               std::size_t key_count,
                                               int sort_implementation) noexcept {
    bioseq::indexing::detail::sort_minimizer_pair_run_keys(keys, key_count, sort_implementation);
}
