/**
 * @file sort_uint_128.cppm
 * @brief Public re-export of the `uint128_t` VQSort entry points.
 *
 * Used for the `Anchor` sort site after the layout swap that puts the
 * primary key (`ref_bits`) in the upper 64 bits and the secondary key
 * (`query_bits`) in the lower 64 bits — the convention `Lt128` enforces.
 */
export module bioseq_simd.sorting.uint128;

import bioseq_simd.sorting;

export namespace bioseq::simd::sorting::uint128 {

using ::bioseq::simd::sorting::sort_u128_ascending;
using ::bioseq::simd::sorting::sort_u128_descending;
using ::bioseq::simd::sorting::vqsort_threshold_bytes;
using ::bioseq::simd::sorting::vqsort_threshold_keys;

}  // namespace bioseq::simd::sorting::uint128
