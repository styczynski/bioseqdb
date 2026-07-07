/**
 * @file index.cppm
 * @brief SIMD sorting wrappers exposed as a C++26 module.
 *
 * The exported module interface intentionally contains only declarations and
 * light stream-width dispatchers. Highway/VQSort implementation headers stay
 * in the same-module implementation unit so GCC does not serialize their
 * target-local internals into this BMI.
 */
module;

#include <hwy/base.h>

#include <cstddef>
#include <cstdint>

export module bioseq.simd.sorting;

export namespace bioseq::simd::sorting {

/// Recommended `n * sizeof(key)` VQSort cutover (in bytes) below which the
/// per-call VQSort overhead absorbs the asymptotic win and `std::sort` is
/// faster. Mirrors Highway's documented "at least 100 KiB" guidance.
inline constexpr std::size_t vqsort_threshold_bytes = 100u * 1024u;

/// Convenience: minimum key count that clears the byte threshold for a given
/// key type. Use with `if (n >= vqsort_threshold_keys<Key>) ...`.
template <typename Key>
inline constexpr std::size_t vqsort_threshold_keys =
    vqsort_threshold_bytes / sizeof(Key);

/// The legacy VQSort u128 path remains opt-in only. NEON-128 holds one
/// 128-bit key per vector, so callers should benchmark the vec_stream sorter
/// below before enabling any u128 SIMD path by default.
template <>
inline constexpr std::size_t vqsort_threshold_keys<hwy::uint128_t> =
    static_cast<std::size_t>(-1);

/// Default stream width for the u128 vec_stream sorter. Apple NEON-128 only
/// carries one 128-bit key per hardware vector, but two independent key
/// vectors fit the ABI/register-pressure envelope proven by the two-stack
/// minimizer collector.
inline constexpr std::size_t u128_sort_default_stream_width = 2;

/// Small arrays stay scalar to avoid repeating the earlier NEON short-sort
/// regression; larger arrays use vec_stream in partition scans.
inline constexpr std::size_t u128_sort_scalar_fast_path_keys = 8;

template <std::size_t StreamWidth>
concept u128_sort_stream_width_ok = (StreamWidth >= 1 && StreamWidth <= 4);

namespace detail {

void sort_u128_ascending_vec_stream_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);
void sort_u128_ascending_vec_stream_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);
void sort_u128_ascending_vec_stream_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);
void sort_u128_ascending_vec_stream_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

void sort_u128_descending_vec_stream_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);
void sort_u128_descending_vec_stream_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);
void sort_u128_descending_vec_stream_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);
void sort_u128_descending_vec_stream_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

template <std::size_t StreamWidth>
inline void sort_u128_ascending_vec_stream_dispatch(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    static_assert(u128_sort_stream_width_ok<StreamWidth>);
    if constexpr (StreamWidth == 1) {
        sort_u128_ascending_vec_stream_1(keys, key_count);
    } else if constexpr (StreamWidth == 2) {
        sort_u128_ascending_vec_stream_2(keys, key_count);
    } else if constexpr (StreamWidth == 3) {
        sort_u128_ascending_vec_stream_3(keys, key_count);
    } else {
        sort_u128_ascending_vec_stream_4(keys, key_count);
    }
}

template <std::size_t StreamWidth>
inline void sort_u128_descending_vec_stream_dispatch(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    static_assert(u128_sort_stream_width_ok<StreamWidth>);
    if constexpr (StreamWidth == 1) {
        sort_u128_descending_vec_stream_1(keys, key_count);
    } else if constexpr (StreamWidth == 2) {
        sort_u128_descending_vec_stream_2(keys, key_count);
    } else if constexpr (StreamWidth == 3) {
        sort_u128_descending_vec_stream_3(keys, key_count);
    } else {
        sort_u128_descending_vec_stream_4(keys, key_count);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// uint64 / int64
// ---------------------------------------------------------------------------

/// Sort `n` `uint64_t` keys ascending. Drop-in for the
/// `coverage_intervals` site (packed `(start << 32) | end`).
void sort_u64_ascending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count);

/// Sort `n` `uint64_t` keys descending. Symmetric counterpart of
/// `sort_u64_ascending`.
void sort_u64_descending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count);

// ---------------------------------------------------------------------------
// uint128
// ---------------------------------------------------------------------------

/// Legacy Highway VQSort u128 entry point, retained for explicit benchmark
/// comparisons.
void sort_u128_ascending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

void sort_u128_descending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/// Sort `n` `uint128_t` keys ascending via a vec_stream-backed introsort.
/// `StreamWidth` controls how many independent 128-bit partition-scan
/// comparisons are issued as one stream bundle. The sort order is full
/// lexicographic `(hi, lo)`, matching `hwy::uint128_t::operator<`.
template <std::size_t StreamWidth = u128_sort_default_stream_width>
requires (u128_sort_stream_width_ok<StreamWidth>)
inline void sort_u128_ascending_vec_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_u128_ascending_vec_stream_dispatch<StreamWidth>(
        keys, key_count);
}

template <std::size_t StreamWidth = u128_sort_default_stream_width>
requires (u128_sort_stream_width_ok<StreamWidth>)
inline void sort_u128_descending_vec_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_u128_descending_vec_stream_dispatch<StreamWidth>(
        keys, key_count);
}

void sort_u128_ascending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

void sort_u128_descending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

// ---------------------------------------------------------------------------
// K32V32 (key in upper 32 bits of u64, value in lower 32 bits)
// ---------------------------------------------------------------------------

/// Sort `n` `K32V32` packed keys ascending. The caller is responsible for
/// packing `(key << 32) | value` and for sign-biasing signed keys by
/// `+0x80000000` so unsigned comparison matches signed semantics.
void sort_k32v32_ascending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count);

/// Sort `n` `K32V32` packed keys descending. Used for the `Chain` sort
/// site (sort by score descending).
void sort_k32v32_descending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count);

/// Top-`k` partial sort for `K32V32` descending. The first `k` entries after
/// the call are the `k` largest, ordered.
void partial_sort_k32v32_descending(
    hwy::K32V32* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t k_keys);

// ---------------------------------------------------------------------------
// K64V64 (value at offset 0, key at offset 8)
// ---------------------------------------------------------------------------

void sort_k64v64_ascending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count);

void sort_k64v64_descending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count);

}  // namespace bioseq::simd::sorting
