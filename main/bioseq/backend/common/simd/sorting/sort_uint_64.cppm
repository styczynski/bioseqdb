/**
 * @file sort_uint_64.cppm
 * @brief Public re-export of the `uint64_t` VQSort entry points.
 *
 * Drop-in for sites that sort packed `uint64_t` keys (e.g. the
 * `coverage_intervals` array in `mm_set_parent`). Callers can either
 * `import bioseq_simd.sorting;` for the full surface, or
 * `import bioseq_simd.sorting.uint64;` when they only need the u64
 * helpers.
 */
export module bioseq.simd.sorting.uint64;

#if !defined(__GNUC__) || defined(__clang__)
import bioseq.simd.sorting;

export namespace bioseq::simd::sorting::uint64 {

using ::bioseq::simd::sorting::sort_u64_ascending;
using ::bioseq::simd::sorting::sort_u64_descending;
using ::bioseq::simd::sorting::vqsort_threshold_bytes;
using ::bioseq::simd::sorting::vqsort_threshold_keys;

}  // namespace bioseq::simd::sorting::uint64
#endif
