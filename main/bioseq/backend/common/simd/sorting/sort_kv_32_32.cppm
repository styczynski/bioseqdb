/**
 * @file sort_kv_32_32.cppm
 * @brief Public re-export of the `K32V32` VQSort entry points.
 *
 * `K32V32` packs a 32-bit key in the upper 32 bits and a 32-bit value in
 * the lower 32 bits of one `uint64_t`. Used for the `Chain` (sort by
 * score descending) and `scored_indices` (sort by score ascending)
 * sites. Sign-bias signed keys by `+0x80000000` before packing so
 * Highway's unsigned `>>32` comparator matches the desired signed order.
 */
export module bioseq_simd.sorting.kv32v32;

import bioseq_simd.sorting;

export namespace bioseq::simd::sorting::kv32v32 {

using ::bioseq::simd::sorting::sort_k32v32_ascending;
using ::bioseq::simd::sorting::sort_k32v32_descending;
using ::bioseq::simd::sorting::partial_sort_k32v32_descending;
using ::bioseq::simd::sorting::vqsort_threshold_bytes;
using ::bioseq::simd::sorting::vqsort_threshold_keys;

}  // namespace bioseq::simd::sorting::kv32v32
