/**
 * @file index.cpp
 * @brief Same-module implementation unit for bioseq.simd.sorting.
 */
module;

#include <hwy/highway.h>

#include "common.h"

#ifndef VQSORT_ONLY_STATIC
#define VQSORT_ONLY_STATIC 1
#endif

#include "hwy/contrib/sort/order.h"
#include "hwy/contrib/sort/vqsort-inl.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

module bioseq.simd.sorting;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::sorting {
namespace hn = hwy::HWY_NAMESPACE;

namespace detail {

[[nodiscard]] inline bool u128_less_ascending(
    const hwy::uint128_t& a,
    const hwy::uint128_t& b) noexcept
{
    return (a.hi == b.hi) ? (a.lo < b.lo) : (a.hi < b.hi);
}

template <bool Ascending>
[[nodiscard]] inline bool u128_ordered_less(
    const hwy::uint128_t& a,
    const hwy::uint128_t& b) noexcept
{
    if constexpr (Ascending) {
        return u128_less_ascending(a, b);
    } else {
        return u128_less_ascending(b, a);
    }
}

template <class D>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_u128_key(D d, const hwy::uint128_t& key) noexcept {
    return hn::LoadU(d, &key.lo);
}

template <bool Ascending>
inline void sort_u128_std_fallback(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    std::sort(keys, keys + key_count,
        [](const hwy::uint128_t& a, const hwy::uint128_t& b) {
            return u128_ordered_less<Ascending>(a, b);
        });
}

template <bool Ascending>
inline void sort_u128_small_scalar(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count) noexcept
{
    for (std::size_t i = 1; i < key_count; ++i) {
        const hwy::uint128_t value = keys[i];
        std::size_t j = i;
        while (j > 0 && u128_ordered_less<Ascending>(value, keys[j - 1])) {
            keys[j] = keys[j - 1];
            --j;
        }
        keys[j] = value;
    }
}

#if HWY_TARGET != HWY_SCALAR || HWY_IDE

// Scan one compile-time slot at a time instead of collecting vectors or masks
// into a bundle. Highway vectors are sizeless on SVE/RVV, so they may be
// locals and function arguments but cannot be elements of arrays or other
// aggregate storage. Forced inlining turns this recursion into the same
// straight-line sequence of up to four comparisons used by the old stream
// implementation without giving a sizeless vector an object representation.
template <bool Ascending, bool PivotFirst, class D>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto u128_ordered_less_vec(
    D d,
    hn::Vec<D> key,
    hn::Vec<D> pivot) noexcept
{
    if constexpr (Ascending) {
        if constexpr (PivotFirst) {
            return hn::Lt128(d, pivot, key);
        } else {
            return hn::Lt128(d, key, pivot);
        }
    } else {
        if constexpr (PivotFirst) {
            return hn::Lt128(d, key, pivot);
        } else {
            return hn::Lt128(d, pivot, key);
        }
    }
}

template <std::size_t Slot,
          std::size_t StreamWidth,
          bool Ascending,
          bool PivotFirst,
          bool Reverse,
          class D>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::size_t first_false_u128_block_slot(
    D d,
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::ptrdiff_t base,
    hn::Vec<D> pivot) noexcept
{
    static_assert(StreamWidth >= 1 && StreamWidth <= 4);
    static_assert(Slot <= StreamWidth);

    if constexpr (Slot == StreamWidth) {
        return StreamWidth;
    } else {
        constexpr auto offset = static_cast<std::ptrdiff_t>(Slot);
        const std::ptrdiff_t index = Reverse ? base - offset : base + offset;
        const auto key = load_u128_key(
            d, keys[static_cast<std::size_t>(index)]);

        const auto ordered_less =
            u128_ordered_less_vec<Ascending, PivotFirst>(d, key, pivot);
        if (hn::AllFalse(d, ordered_less)) {
            return Slot;
        }

        return first_false_u128_block_slot<
            Slot + 1, StreamWidth, Ascending, PivotFirst, Reverse>(
                d, keys, base, pivot);
    }
}

template <bool Ascending>
[[nodiscard]] inline hwy::uint128_t choose_u128_pivot(
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count) noexcept
{
    hwy::uint128_t a = keys[0];
    hwy::uint128_t b = keys[key_count / 2];
    hwy::uint128_t c = keys[key_count - 1];
    if (u128_ordered_less<Ascending>(b, a)) std::swap(a, b);
    if (u128_ordered_less<Ascending>(c, b)) std::swap(b, c);
    if (u128_ordered_less<Ascending>(b, a)) std::swap(a, b);
    return b;
}

template <std::size_t StreamWidth, bool Ascending, class D>
[[nodiscard]] HWY_INLINE std::size_t partition_u128_stream(
    D d,
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    hwy::uint128_t pivot) noexcept
{
    std::ptrdiff_t i = 0;
    std::ptrdiff_t j = static_cast<std::ptrdiff_t>(key_count) - 1;
    const auto pivot_vec = load_u128_key(d, pivot);

    while (i <= j) {
        while (i <= j) {
            while (j - i + 1 >= static_cast<std::ptrdiff_t>(StreamWidth)) {
                const std::size_t first_false =
                    first_false_u128_block_slot<
                        0, StreamWidth, Ascending, false, false>(
                            d, keys, i, pivot_vec);
                if (first_false == StreamWidth) {
                    i += static_cast<std::ptrdiff_t>(StreamWidth);
                    continue;
                }
                i += static_cast<std::ptrdiff_t>(first_false);
                break;
            }
            while (i <= j &&
                   u128_ordered_less<Ascending>(
                       keys[static_cast<std::size_t>(i)], pivot)) {
                ++i;
            }
            break;
        }

        while (i <= j) {
            while (j - i + 1 >= static_cast<std::ptrdiff_t>(StreamWidth)) {
                const std::size_t first_false =
                    first_false_u128_block_slot<
                        0, StreamWidth, Ascending, true, true>(
                            d, keys, j, pivot_vec);
                if (first_false == StreamWidth) {
                    j -= static_cast<std::ptrdiff_t>(StreamWidth);
                    continue;
                }
                j -= static_cast<std::ptrdiff_t>(first_false);
                break;
            }
            while (i <= j &&
                   u128_ordered_less<Ascending>(
                       pivot, keys[static_cast<std::size_t>(j)])) {
                --j;
            }
            break;
        }

        if (i > j) {
            break;
        }

        std::swap(keys[static_cast<std::size_t>(i)],
                  keys[static_cast<std::size_t>(j)]);
        ++i;
        --j;
    }

    return static_cast<std::size_t>(i);
}

[[nodiscard]] inline std::size_t introsort_depth(std::size_t key_count) noexcept {
    std::size_t depth = 0;
    while (key_count > 1) {
        key_count >>= 1;
        ++depth;
    }
    return depth * 2;
}

template <std::size_t StreamWidth, bool Ascending>
void sort_u128_vec_stream_impl(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t depth)
{
    const hn::CappedTag<std::uint64_t, 2> d;

    while (key_count > 8) {
        if (depth == 0) [[unlikely]] {
            sort_u128_std_fallback<Ascending>(keys, key_count);
            return;
        }
        --depth;

        const hwy::uint128_t pivot =
            choose_u128_pivot<Ascending>(keys, key_count);
        const std::size_t cut =
            partition_u128_stream<StreamWidth, Ascending>(d, keys, key_count, pivot);
        if (cut == 0 || cut == key_count) [[unlikely]] {
            sort_u128_std_fallback<Ascending>(keys, key_count);
            return;
        }

        const std::size_t right_count = key_count - cut;
        if (cut < right_count) {
            sort_u128_vec_stream_impl<StreamWidth, Ascending>(keys, cut, depth);
            keys += cut;
            key_count = right_count;
        } else {
            sort_u128_vec_stream_impl<StreamWidth, Ascending>(
                keys + cut, right_count, depth);
            key_count = cut;
        }
    }

    sort_u128_small_scalar<Ascending>(keys, key_count);
}

#else

template <std::size_t StreamWidth, bool Ascending>
void sort_u128_vec_stream_impl(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t)
{
    sort_u128_std_fallback<Ascending>(keys, key_count);
}

#endif  // HWY_TARGET != HWY_SCALAR || HWY_IDE

template <std::size_t StreamWidth, bool Ascending>
inline void sort_u128_vec_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    if (key_count <= 1) {
        return;
    }
    if (key_count <= u128_sort_scalar_fast_path_keys) {
        sort_u128_small_scalar<Ascending>(keys, key_count);
        return;
    }
    sort_u128_vec_stream_impl<StreamWidth, Ascending>(
        keys, key_count, introsort_depth(key_count));
}

void sort_u128_ascending_vec_stream_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<1, true>(keys, key_count);
}

void sort_u128_ascending_vec_stream_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<2, true>(keys, key_count);
}

void sort_u128_ascending_vec_stream_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<3, true>(keys, key_count);
}

void sort_u128_ascending_vec_stream_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<4, true>(keys, key_count);
}

void sort_u128_descending_vec_stream_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<1, false>(keys, key_count);
}

void sort_u128_descending_vec_stream_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<2, false>(keys, key_count);
}

void sort_u128_descending_vec_stream_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<3, false>(keys, key_count);
}

void sort_u128_descending_vec_stream_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_vec_stream<4, false>(keys, key_count);
}

}  // namespace detail

void sort_u64_ascending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

void sort_u64_descending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

void sort_u128_ascending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

void sort_u128_descending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

void sort_u128_ascending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_u128_vec_stream<2, true>(keys, key_count);
}

void sort_u128_descending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_u128_vec_stream<2, false>(keys, key_count);
}

void sort_k32v32_ascending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

void sort_k32v32_descending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

void partial_sort_k32v32_descending(
    hwy::K32V32* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t k_keys)
{
    hwy::HWY_NAMESPACE::VQPartialSortStatic(
        keys, key_count, k_keys, hwy::SortDescending());
}

void sort_k64v64_ascending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

void sort_k64v64_descending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

}  // namespace bioseq::simd::sorting
HWY_AFTER_NAMESPACE();
