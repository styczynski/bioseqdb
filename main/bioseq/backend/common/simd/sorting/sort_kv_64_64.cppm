/**
 * @file sort_kv_64_64.cppm
 * @brief Public re-export of the `K64V64` VQSort entry points.
 *
 * `K64V64` is a 128-bit struct with the 64-bit value at offset 0 (LE
 * low lane) and the 64-bit key at offset 8 (LE high lane). Provided as
 * a fallback for sites where the value field could exceed 32 bits
 * (e.g. anchor indices in pathological large-input runs).
 */
export module bioseq_simd.sorting.kv64v64;

import bioseq_simd.sorting;

export namespace bioseq::simd::sorting::kv64v64 {

using ::bioseq::simd::sorting::sort_k64v64_ascending;
using ::bioseq::simd::sorting::sort_k64v64_descending;
using ::bioseq::simd::sorting::vqsort_threshold_bytes;
using ::bioseq::simd::sorting::vqsort_threshold_keys;

}  // namespace bioseq::simd::sorting::kv64v64
