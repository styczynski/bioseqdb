/**
 * @file pair_sort.cppm
 * @brief Sorts packed minimizer hash/position pairs with the selected SIMD sorter.
 */
module;

#include <cstddef>
#include <hwy/highway.h>

module bioseq.indexing.minimizer_sequence.admitted_runs;

import bioseq.simd.sorting;

namespace bioseq::indexing::detail {

/**
 * Sort unsigned 128-bit pair keys in ascending order using the selected sorter.
 * The hash occupies the high half, so pairs are ordered by hash and then position.
 * @param keys Array of key_count keys to sort in place.
 * @param key_count Number of keys.
 * @param sort_implementation Value from pair_run_sort_implementation.
 */
void sort_minimizer_pair_run_keys(hwy::uint128_t* keys,
                                  std::size_t key_count,
                                  int sort_implementation) noexcept {
    switch (sort_implementation) {
    case 1:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vqsort(keys, key_count);
        return;
    case 2:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vector_stream<1>(keys, key_count);
        return;
    case 3:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vector_stream<2>(keys, key_count);
        return;
    case 4:
        bioseq::simd::sorting::sort_unsigned_128_ascending_vector_stream<4>(keys, key_count);
        return;
    default:
        bioseq::simd::sorting::sort_unsigned_128_ascending(keys, key_count);
        return;
    }
}

} // namespace bioseq::indexing::detail

/** Sort packed hash/position keys through the C-linkage entry point. */
extern "C" void bioseq_sort_pair_run_u128_keys(hwy::uint128_t* keys,
                                               std::size_t key_count,
                                               int sort_implementation) noexcept {
    bioseq::indexing::detail::sort_minimizer_pair_run_keys(keys, key_count, sort_implementation);
}
