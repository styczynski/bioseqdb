/**
 * @file index.cppm
 * @brief Highway VQSort wrappers exposed as a C++26 module.
 *
 * Vendors the relevant subset of `hwy/contrib/sort/` into our tree. The
 * upstream library is plain headers (`vqsort-inl.h`, `traits-inl.h`, ...)
 * sitting under `hwy_contrib/hwy/contrib/...` so the `#include` paths in the
 * vendored headers continue to resolve unchanged. The headers default to
 * `VQSORT_ONLY_STATIC=1` so we do not have to link the upstream `vqsort.cc`
 * translation unit (which provides cryptographic seed material we do not
 * need for genome workloads).
 *
 * Public API: drop-in `std::sort` replacements for the four hot sort sites
 * the SIMD-sort research report identifies. Each entry point is a thin
 * `inline` wrapper around `hwy::HWY_NAMESPACE::VQSortStatic` so the
 * inliner can fold the call through into the caller. Threshold gating
 * (per-site fall back to `std::sort` for small `N`) is the caller's
 * responsibility — see `bioseq::simd::sorting::vqsort_threshold_keys` for
 * the recommended cutover.
 *
 * Layout requirements (see report §2.5):
 *   * `uint128_t` (used for `Anchor`): primary key in the upper 64 bits,
 *     secondary key in the lower 64 bits (`Lt128` reads `a[1]` first).
 *   * `K32V32`: key in the upper 32 bits of one `uint64_t`, value in the
 *     lower 32. Sign-bias signed scores by `+0x80000000` before packing.
 *   * `K64V64`: value at offset 0, key at offset 8 (LE high lane).
 */
module;

#include <hwy/highway.h>

#include "common.h"

// The vendored Highway sort code is treated as a normal include tree.
// Defaulting `VQSORT_ONLY_STATIC=1` happens inside the patched
// `vqsort-inl.h` itself, but we set it before the include too so any
// future translation unit that pulls `vqsort-inl.h` from elsewhere in
// the tree gets the same default.
#ifndef VQSORT_ONLY_STATIC
#define VQSORT_ONLY_STATIC 1
#endif

#include "hwy/contrib/sort/order.h"
#include "hwy/contrib/sort/vqsort-inl.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq_simd.sorting;

import mlogging;

export {

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::sorting {

/// Recommended `n × sizeof(key)` cutover (in bytes) below which the
/// per-call VQSort overhead absorbs the asymptotic win and `std::sort`
/// is faster. Mirrors the documented Highway recommendation
/// (`vqsort.h:25`: "use this for arrays whose size is at least 100 KiB").
inline constexpr std::size_t vqsort_threshold_bytes = 100u * 1024u;

/// Convenience: minimum key count that clears the byte threshold for a
/// given key type. Use with `if (n >= vqsort_threshold_keys<Key>) ...`.
template <typename Key>
inline constexpr std::size_t vqsort_threshold_keys =
    vqsort_threshold_bytes / sizeof(Key);

/// Per-key-type override for targets where the per-vector parallelism
/// is too low for VQSort's asymptotic win to recover the per-call
/// overhead. NEON-128 holds **one** 128-bit key per vector, so the
/// base-case sorting network sorts only 8 keys at a time and `Compare1`
/// lowers to scalar 64-bit ops. Empirical measurement on the
/// BENCHMARKING_SPEEDUP corpus (Apple M1, NEON-128, LONG / VERY_LONG /
/// XL pairs) showed the VQSort u128 path more than doubles the
/// `collect_seed_hits` phase time at 100 k–500 k anchors regardless of
/// whether the input is reinterpreted in place or staged through a
/// 16-byte-aligned temporary. We therefore pin the u128 cutover to a
/// value above the largest anchor population the bench ever produces
/// — the kernel only fires on synthetic / future-large workloads where
/// it has been re-validated. For AVX-512 hosts the override should be
/// removed (4 u128 lanes per vector, the original 6 400-key cutover
/// applies).
template <>
inline constexpr std::size_t vqsort_threshold_keys<hwy::uint128_t> =
    static_cast<std::size_t>(-1);

// ---------------------------------------------------------------------------
// uint64 / int64
// ---------------------------------------------------------------------------

/// Sort `n` `uint64_t` keys ascending. Drop-in for the
/// `coverage_intervals` site (packed `(start << 32) | end`).
inline void sort_u64_ascending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

/// Sort `n` `uint64_t` keys descending. Symmetric counterpart of
/// `sort_u64_ascending` — provided for completeness.
inline void sort_u64_descending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

// ---------------------------------------------------------------------------
// uint128
// ---------------------------------------------------------------------------

/// Sort `n` `uint128_t` keys ascending. Used for the `Anchor` site after
/// the layout swap that puts `ref_bits` (primary key) in the upper 64
/// bits and `query_bits` in the lower 64 bits.
inline void sort_u128_ascending(hwy::uint128_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

inline void sort_u128_descending(hwy::uint128_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

// ---------------------------------------------------------------------------
// K32V32 (key in upper 32 bits of u64, value in lower 32 bits)
// ---------------------------------------------------------------------------

/// Sort `n` `K32V32` packed keys ascending. The caller is responsible for
/// packing `(key << 32) | value` and for sign-biasing signed keys by
/// `+0x80000000` so unsigned comparison matches signed semantics.
inline void sort_k32v32_ascending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

/// Sort `n` `K32V32` packed keys descending. Used for the `Chain` sort
/// site (sort by score descending).
inline void sort_k32v32_descending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

/// Top-`k` partial sort for `K32V32` descending. The first `k` entries
/// after the call are the `k` largest, ordered. Useful when `mm_gen_regs`
/// only needs the top-K chains.
inline void partial_sort_k32v32_descending(
    hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count, std::size_t k_keys)
{
    hwy::HWY_NAMESPACE::VQPartialSortStatic(
        keys, key_count, k_keys, hwy::SortDescending());
}

// ---------------------------------------------------------------------------
// K64V64 (value at offset 0, key at offset 8 — fallback for >2^31 indices)
// ---------------------------------------------------------------------------

inline void sort_k64v64_ascending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

inline void sort_k64v64_descending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

}  // namespace bioseq::simd::sorting
HWY_AFTER_NAMESPACE();

}  // export
