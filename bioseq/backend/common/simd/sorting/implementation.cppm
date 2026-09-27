/**
 * @file implementation.cppm
 * @brief Sorting implementations for the compiled Highway CPU target.
 */
module;

#include <hwy/highway.h>

#ifndef VQSORT_ONLY_STATIC
#define VQSORT_ONLY_STATIC 1
#endif

#include "hwy/contrib/sort/order.h"
#include "hwy/contrib/sort/vqsort-inl.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

module bioseq.simd.sorting;

import bioseq.simd.vector_stream;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::sorting {
/// Highway functions for the compiled CPU target.
namespace highway_target = hwy::HWY_NAMESPACE;

namespace detail {

/// Vector-stream helpers for the same CPU target.
namespace vector_stream_target = ::bioseq::simd::HWY_NAMESPACE;

/** @brief Returns whether `left` precedes `right` in ascending 128-bit order. */
[[nodiscard]] inline bool unsigned_128_less(
    const hwy::uint128_t& left,
    const hwy::uint128_t& right) noexcept
{
    return left.hi == right.hi
        ? left.lo < right.lo
        : left.hi < right.hi;
}

/** @brief Returns whether `left` precedes `right` in the requested order. */
template <bool p_ascending>
[[nodiscard]] inline bool unsigned_128_precedes(
    const hwy::uint128_t& left,
    const hwy::uint128_t& right) noexcept
{
    if constexpr (p_ascending) {
        return unsigned_128_less(left, right);
    } else {
        return unsigned_128_less(right, left);
    }
}

/** @brief Loads one 128-bit key as two adjacent unsigned 64-bit lanes. */
template <class p_descriptor_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_unsigned_128(
    p_descriptor_type descriptor,
    const hwy::uint128_t& key) noexcept
{
    return highway_target::LoadU(descriptor, &key.lo);
}

/** @brief Sorts 128-bit keys with the standard library in the requested order. */
template <bool p_ascending>
inline void sort_unsigned_128_with_standard_library(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    std::sort(
        keys,
        keys + key_count,
        [](const hwy::uint128_t& left, const hwy::uint128_t& right) {
            return unsigned_128_precedes<p_ascending>(left, right);
        });
}

/** @brief Insertion-sorts a small 128-bit key array in the requested order. */
template <bool p_ascending>
inline void insertion_sort_unsigned_128(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count) noexcept
{
    for (std::size_t insertion_index = 1u;
         insertion_index < key_count;
         ++insertion_index) {
        const hwy::uint128_t value = keys[insertion_index];
        std::size_t destination_index = insertion_index;
        while (
            destination_index > 0u
            && unsigned_128_precedes<p_ascending>(
                value,
                keys[destination_index - 1u])) {
            keys[destination_index] = keys[destination_index - 1u];
            --destination_index;
        }
        keys[destination_index] = value;
    }
}

#if HWY_TARGET != HWY_SCALAR || HWY_IDE

/**
 * @brief A group of vectors, each holding one 128-bit key.
 * @tparam p_descriptor_type Highway descriptor for the two 64-bit halves.
 * @tparam p_stream_width Number of independent key vectors.
 */
template <class p_descriptor_type, std::size_t p_stream_width>
using unsigned_128_vector_stream = vector_stream_target::vector_stream<
    p_descriptor_type,
    std::uint64_t,
    p_stream_width>;

/**
 * @brief Compares corresponding 128-bit keys from two vector groups.
 * @tparam p_ascending Selects ascending or descending order.
 * @return One comparison mask per vector.
 */
template <bool p_ascending,
          class p_descriptor_type,
          std::size_t p_stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto unsigned_128_stream_precedes(
    p_descriptor_type descriptor,
    unsigned_128_vector_stream<p_descriptor_type, p_stream_width> left,
    unsigned_128_vector_stream<p_descriptor_type, p_stream_width> right) noexcept
{
    return [&descriptor, &left, &right]<std::size_t... p_stream_indices>(
        std::index_sequence<p_stream_indices...> p_stream_index_sequence)
        [[gnu::always_inline]] {
        (void)p_stream_index_sequence;
        if constexpr (p_ascending) {
            return vector_stream_target::mask_stream<
                p_descriptor_type,
                p_stream_width>::pack(
                    highway_target::Lt128(
                        descriptor,
                        left[p_stream_indices],
                        right[p_stream_indices])...);
        } else {
            return vector_stream_target::mask_stream<
                p_descriptor_type,
                p_stream_width>::pack(
                    highway_target::Lt128(
                        descriptor,
                        right[p_stream_indices],
                        left[p_stream_indices])...);
        }
    }(std::make_index_sequence<p_stream_width>{});
}

/**
 * @brief Copies one 128-bit key into every vector in the group.
 */
template <class p_descriptor_type, std::size_t p_stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto broadcast_unsigned_128_stream(
    p_descriptor_type descriptor,
    const hwy::uint128_t& key) noexcept
{
    const auto vector = load_unsigned_128(descriptor, key);
    return vector_stream_target::make_vector_stream<
        p_descriptor_type,
        std::uint64_t,
        p_stream_width>(
            [&vector](auto p_stream_index_constant) [[gnu::always_inline]] {
                (void)p_stream_index_constant;
                return vector;
            });
}

/**
 * @brief Loads consecutive keys starting at `base_index`.
 *
 * Each vector holds one key, in increasing array-index order.
 */
template <class p_descriptor_type, std::size_t p_stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_unsigned_128_forward_stream(
    p_descriptor_type descriptor,
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::ptrdiff_t base_index) noexcept
{
    return vector_stream_target::make_vector_stream<
        p_descriptor_type,
        std::uint64_t,
        p_stream_width>(
            [&descriptor, &keys, &base_index](auto p_stream_index_constant)
                [[gnu::always_inline]] {
                constexpr std::size_t stream_index =
                    decltype(p_stream_index_constant)::value;
                return load_unsigned_128(
                    descriptor,
                    keys[static_cast<std::size_t>(base_index) + stream_index]);
            });
}

/**
 * @brief Loads consecutive keys backwards from `base_index`.
 *
 * Each vector holds one key, in decreasing array-index order.
 */
template <class p_descriptor_type, std::size_t p_stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_unsigned_128_reverse_stream(
    p_descriptor_type descriptor,
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::ptrdiff_t base_index) noexcept
{
    return vector_stream_target::make_vector_stream<
        p_descriptor_type,
        std::uint64_t,
        p_stream_width>(
            [&descriptor, &keys, &base_index](auto p_stream_index_constant)
                [[gnu::always_inline]] {
                constexpr std::size_t stream_index =
                    decltype(p_stream_index_constant)::value;
                const auto offset =
                    static_cast<std::ptrdiff_t>(stream_index);
                return load_unsigned_128(
                    descriptor,
                    keys[static_cast<std::size_t>(base_index - offset)]);
            });
}

/**
 * @brief Finds the first vector whose comparison mask is entirely false.
 * @return Its stream index, or `p_stream_width` if every vector has a true lane.
 */
template <class p_descriptor_type,
          std::size_t p_stream_width,
          class p_mask_stream_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::size_t first_all_false_mask_slot(
    p_descriptor_type descriptor,
    const p_mask_stream_type& masks) noexcept
{
    std::size_t first_false_slot = p_stream_width;
    vector_stream_target::for_each_stream<p_stream_width>(
        [&first_false_slot, &descriptor, &masks](auto p_stream_index_constant)
            [[gnu::always_inline]] {
            constexpr std::size_t stream_index =
                decltype(p_stream_index_constant)::value;
            if (
                first_false_slot == p_stream_width
                && highway_target::AllFalse(
                    descriptor,
                    masks[stream_index])) {
                first_false_slot = stream_index;
            }
        });
    return first_false_slot;
}

/** @brief Chooses the median of the first, middle, and final 128-bit keys. */
template <bool p_ascending>
[[nodiscard]] inline hwy::uint128_t choose_unsigned_128_pivot(
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count) noexcept
{
    hwy::uint128_t first_candidate = keys[0];
    hwy::uint128_t middle_candidate = keys[key_count / 2u];
    hwy::uint128_t final_candidate = keys[key_count - 1u];
    if (unsigned_128_precedes<p_ascending>(
            middle_candidate,
            first_candidate)) {
        std::swap(first_candidate, middle_candidate);
    }
    if (unsigned_128_precedes<p_ascending>(
            final_candidate,
            middle_candidate)) {
        std::swap(middle_candidate, final_candidate);
    }
    if (unsigned_128_precedes<p_ascending>(
            middle_candidate,
            first_candidate)) {
        std::swap(first_candidate, middle_candidate);
    }
    return middle_candidate;
}

/**
 * @brief Partitions keys around a pivot by scanning from both ends.
 *
 * SIMD comparisons skip groups already on the correct side. When both
 * scans stop, their keys are swapped. Scanning continues until the indices
 * cross, then the function returns the first index of the right partition.
 */
template <std::size_t p_stream_width,
          bool p_ascending,
          class p_descriptor_type>
[[nodiscard]] HWY_INLINE std::size_t partition_unsigned_128_stream(
    p_descriptor_type descriptor,
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    hwy::uint128_t pivot) noexcept
{
    std::ptrdiff_t left_index = 0;
    std::ptrdiff_t right_index =
        static_cast<std::ptrdiff_t>(key_count) - 1;
    const auto pivot_stream =
        broadcast_unsigned_128_stream<p_descriptor_type, p_stream_width>(
            descriptor,
            pivot);

    while (left_index <= right_index) {
        while (left_index <= right_index) {
            while (
                right_index - left_index + 1
                >= static_cast<std::ptrdiff_t>(p_stream_width)) {
                const auto block =
                    load_unsigned_128_forward_stream<
                        p_descriptor_type,
                        p_stream_width>(
                            descriptor,
                            keys,
                            left_index);
                const auto precedes_pivot =
                    unsigned_128_stream_precedes<
                        p_ascending,
                        p_descriptor_type,
                        p_stream_width>(
                            descriptor,
                            block,
                            pivot_stream);
                const std::size_t first_nonmatching_slot =
                    first_all_false_mask_slot<
                        p_descriptor_type,
                        p_stream_width>(
                            descriptor,
                            precedes_pivot);
                if (first_nonmatching_slot == p_stream_width) {
                    left_index +=
                        static_cast<std::ptrdiff_t>(p_stream_width);
                    continue;
                }
                left_index +=
                    static_cast<std::ptrdiff_t>(first_nonmatching_slot);
                break;
            }
            while (
                left_index <= right_index
                && unsigned_128_precedes<p_ascending>(
                    keys[static_cast<std::size_t>(left_index)],
                    pivot)) {
                ++left_index;
            }
            break;
        }

        while (left_index <= right_index) {
            while (
                right_index - left_index + 1
                >= static_cast<std::ptrdiff_t>(p_stream_width)) {
                const auto block =
                    load_unsigned_128_reverse_stream<
                        p_descriptor_type,
                        p_stream_width>(
                            descriptor,
                            keys,
                            right_index);
                const auto pivot_precedes =
                    unsigned_128_stream_precedes<
                        p_ascending,
                        p_descriptor_type,
                        p_stream_width>(
                            descriptor,
                            pivot_stream,
                            block);
                const std::size_t first_nonmatching_slot =
                    first_all_false_mask_slot<
                        p_descriptor_type,
                        p_stream_width>(
                            descriptor,
                            pivot_precedes);
                if (first_nonmatching_slot == p_stream_width) {
                    right_index -=
                        static_cast<std::ptrdiff_t>(p_stream_width);
                    continue;
                }
                right_index -=
                    static_cast<std::ptrdiff_t>(first_nonmatching_slot);
                break;
            }
            while (
                left_index <= right_index
                && unsigned_128_precedes<p_ascending>(
                    pivot,
                    keys[static_cast<std::size_t>(right_index)])) {
                --right_index;
            }
            break;
        }

        if (left_index > right_index) {
            break;
        }

        std::swap(
            keys[static_cast<std::size_t>(left_index)],
            keys[static_cast<std::size_t>(right_index)]);
        ++left_index;
        --right_index;
    }

    return static_cast<std::size_t>(left_index);
}

/** @brief Limits partition depth so uneven splits can switch to standard sorting. */
[[nodiscard]] inline std::size_t introsort_depth_limit(
    std::size_t key_count) noexcept
{
    std::size_t depth = 0u;
    while (key_count > 1u) {
        key_count >>= 1u;
        ++depth;
    }
    return depth * 2u;
}

/**
 * @brief Sorts 128-bit keys with SIMD partition scans.
 *
 * The pivot is the median of three keys. Recursion handles the smaller
 * partition while the loop handles the larger one, limiting stack use. Small
 * ranges use insertion sort. A depth limit or an unusable split switches to
 * `std::sort`.
 */
template <std::size_t p_stream_width, bool p_ascending>
void sort_unsigned_128_vector_stream_impl(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t depth_limit)
{
    const highway_target::CappedTag<std::uint64_t, 2> descriptor;

    while (key_count > unsigned_128_scalar_key_limit) {
        if (depth_limit == 0u) [[unlikely]] {
            sort_unsigned_128_with_standard_library<p_ascending>(
                keys,
                key_count);
            return;
        }
        --depth_limit;

        const hwy::uint128_t pivot =
            choose_unsigned_128_pivot<p_ascending>(keys, key_count);
        const std::size_t partition_index =
            partition_unsigned_128_stream<p_stream_width, p_ascending>(
                descriptor,
                keys,
                key_count,
                pivot);
        if (
            partition_index == 0u
            || partition_index == key_count) [[unlikely]] {
            sort_unsigned_128_with_standard_library<p_ascending>(
                keys,
                key_count);
            return;
        }

        const std::size_t right_count = key_count - partition_index;
        if (partition_index < right_count) {
            sort_unsigned_128_vector_stream_impl<p_stream_width, p_ascending>(
                keys,
                partition_index,
                depth_limit);
            keys += partition_index;
            key_count = right_count;
        } else {
            sort_unsigned_128_vector_stream_impl<p_stream_width, p_ascending>(
                keys + partition_index,
                right_count,
                depth_limit);
            key_count = partition_index;
        }
    }

    insertion_sort_unsigned_128<p_ascending>(keys, key_count);
}

#else

/** @brief Uses standard sorting when the active Highway target is scalar. */
template <std::size_t p_stream_width, bool p_ascending>
void sort_unsigned_128_vector_stream_impl(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t)
{
    sort_unsigned_128_with_standard_library<p_ascending>(keys, key_count);
}

#endif // HWY_TARGET != HWY_SCALAR || HWY_IDE

/** @brief Uses insertion sort for small arrays and the vector-stream sorter for larger ones. */
template <std::size_t p_stream_width, bool p_ascending>
inline void sort_unsigned_128_vector_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    if (key_count <= 1u) {
        return;
    }
    if (key_count <= unsigned_128_scalar_key_limit) {
        insertion_sort_unsigned_128<p_ascending>(keys, key_count);
        return;
    }
    sort_unsigned_128_vector_stream_impl<p_stream_width, p_ascending>(
        keys,
        key_count,
        introsort_depth_limit(key_count));
}

/** @brief Sorts ascending with one independent vector per scan. */
void sort_unsigned_128_ascending_stream_width_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<1u, true>(keys, key_count);
}

/** @brief Sorts ascending with two independent vectors per scan. */
void sort_unsigned_128_ascending_stream_width_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<2u, true>(keys, key_count);
}

/** @brief Sorts ascending with three independent vectors per scan. */
void sort_unsigned_128_ascending_stream_width_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<3u, true>(keys, key_count);
}

/** @brief Sorts ascending with four independent vectors per scan. */
void sort_unsigned_128_ascending_stream_width_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<4u, true>(keys, key_count);
}

/** @brief Sorts descending with one independent vector per scan. */
void sort_unsigned_128_descending_stream_width_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<1u, false>(keys, key_count);
}

/** @brief Sorts descending with two independent vectors per scan. */
void sort_unsigned_128_descending_stream_width_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<2u, false>(keys, key_count);
}

/** @brief Sorts descending with three independent vectors per scan. */
void sort_unsigned_128_descending_stream_width_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<3u, false>(keys, key_count);
}

/** @brief Sorts descending with four independent vectors per scan. */
void sort_unsigned_128_descending_stream_width_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<4u, false>(keys, key_count);
}

} // namespace detail

/** @brief Sorts unsigned 64-bit keys ascending through the static Highway target. */
void sort_unsigned_64_ascending(
    std::uint64_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortAscending());
}

/** @brief Sorts unsigned 64-bit keys descending through the static Highway target. */
void sort_unsigned_64_descending(
    std::uint64_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortDescending());
}

/** @brief Sorts unsigned 128-bit keys ascending with static-target VQSort. */
void sort_unsigned_128_ascending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortAscending());
}

/** @brief Sorts unsigned 128-bit keys descending with static-target VQSort. */
void sort_unsigned_128_descending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortDescending());
}

/** @brief Sorts unsigned 128-bit keys ascending with the default vector stream. */
void sort_unsigned_128_ascending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_unsigned_128_vector_stream<
        unsigned_128_default_stream_width,
        true>(keys, key_count);
}

/** @brief Sorts unsigned 128-bit keys descending with the default vector stream. */
void sort_unsigned_128_descending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_unsigned_128_vector_stream<
        unsigned_128_default_stream_width,
        false>(keys, key_count);
}

/** @brief Sorts packed 32-bit key/value records ascending. */
void sort_key_value_32_ascending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        records,
        record_count,
        hwy::SortAscending());
}

/** @brief Sorts packed 32-bit key/value records descending. */
void sort_key_value_32_descending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        records,
        record_count,
        hwy::SortDescending());
}

/** @brief Sorts the selected number of largest keys at the front of the array. */
void partial_sort_key_value_32_descending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count,
    std::size_t selected_count)
{
    hwy::HWY_NAMESPACE::VQPartialSortStatic(
        records,
        record_count,
        selected_count,
        hwy::SortDescending());
}

/** @brief Sorts 64-bit key/value records ascending. */
void sort_key_value_64_ascending(
    hwy::K64V64* HWY_RESTRICT records,
    std::size_t record_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        records,
        record_count,
        hwy::SortAscending());
}

/** @brief Sorts 64-bit key/value records descending. */
void sort_key_value_64_descending(
    hwy::K64V64* HWY_RESTRICT records,
    std::size_t record_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        records,
        record_count,
        hwy::SortDescending());
}

} // namespace bioseq::simd::sorting
HWY_AFTER_NAMESPACE();
