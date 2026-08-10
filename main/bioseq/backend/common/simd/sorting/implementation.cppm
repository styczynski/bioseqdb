/**
 * @file implementation.cppm
 * @brief Highway-target implementation for `bioseq.simd.sorting`.
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
/// Active Highway target namespace for the static sorting implementation.
namespace highway_target = hwy::HWY_NAMESPACE;

namespace detail {

/// Active target namespace for shared vector-stream primitives.
namespace vector_stream_target = ::bioseq::simd::HWY_NAMESPACE;

/** Returns whether `left` precedes `right` in ascending 128-bit order. */
[[nodiscard]] inline bool unsigned_128_less(
    const hwy::uint128_t& left,
    const hwy::uint128_t& right) noexcept
{
    return left.hi == right.hi
        ? left.lo < right.lo
        : left.hi < right.hi;
}

/** Returns whether `left` precedes `right` in the requested order. */
template <bool ascending>
[[nodiscard]] inline bool unsigned_128_precedes(
    const hwy::uint128_t& left,
    const hwy::uint128_t& right) noexcept
{
    if constexpr (ascending) {
        return unsigned_128_less(left, right);
    } else {
        return unsigned_128_less(right, left);
    }
}

/** Loads one 128-bit key as two adjacent unsigned 64-bit lanes. */
template <class descriptor_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_unsigned_128(
    descriptor_type descriptor,
    const hwy::uint128_t& key) noexcept
{
    return highway_target::LoadU(descriptor, &key.lo);
}

/** Sorts 128-bit keys with the standard library in the requested order. */
template <bool ascending>
inline void sort_unsigned_128_with_standard_library(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    std::sort(
        keys,
        keys + key_count,
        [](const hwy::uint128_t& left, const hwy::uint128_t& right) {
            return unsigned_128_precedes<ascending>(left, right);
        });
}

/** Insertion-sorts a small 128-bit key array in the requested order. */
template <bool ascending>
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
            && unsigned_128_precedes<ascending>(
                value,
                keys[destination_index - 1u])) {
            keys[destination_index] = keys[destination_index - 1u];
            --destination_index;
        }
        keys[destination_index] = value;
    }
}

#if HWY_TARGET != HWY_SCALAR || HWY_IDE

/** Vector-stream representation of independent 128-bit keys. */
template <class descriptor_type, std::size_t stream_width>
using unsigned_128_vector_stream = vector_stream_target::vector_stream<
    descriptor_type,
    std::uint64_t,
    stream_width>;

/** Compares each pair of 128-bit vector-stream keys in the requested order. */
template <bool ascending,
          class descriptor_type,
          std::size_t stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto unsigned_128_stream_precedes(
    descriptor_type descriptor,
    unsigned_128_vector_stream<descriptor_type, stream_width> left,
    unsigned_128_vector_stream<descriptor_type, stream_width> right) noexcept
{
    return [&]<std::size_t... stream_indices>(
        std::index_sequence<stream_indices...>) [[gnu::always_inline]] {
        if constexpr (ascending) {
            return vector_stream_target::mask_stream<
                descriptor_type,
                stream_width>::pack(
                    highway_target::Lt128(
                        descriptor,
                        left[stream_indices],
                        right[stream_indices])...);
        } else {
            return vector_stream_target::mask_stream<
                descriptor_type,
                stream_width>::pack(
                    highway_target::Lt128(
                        descriptor,
                        right[stream_indices],
                        left[stream_indices])...);
        }
    }(std::make_index_sequence<stream_width>{});
}

/** Broadcasts one 128-bit key across every slot in a vector stream. */
template <class descriptor_type, std::size_t stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto broadcast_unsigned_128_stream(
    descriptor_type descriptor,
    const hwy::uint128_t& key) noexcept
{
    const auto vector = load_unsigned_128(descriptor, key);
    return vector_stream_target::make_vector_stream<
        descriptor_type,
        std::uint64_t,
        stream_width>(
            [&](auto) [[gnu::always_inline]] { return vector; });
}

/** Loads consecutive 128-bit keys into a forward vector stream. */
template <class descriptor_type, std::size_t stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_unsigned_128_forward_stream(
    descriptor_type descriptor,
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::ptrdiff_t base_index) noexcept
{
    return vector_stream_target::make_vector_stream<
        descriptor_type,
        std::uint64_t,
        stream_width>(
            [&](auto stream_index_constant) [[gnu::always_inline]] {
                constexpr std::size_t stream_index =
                    decltype(stream_index_constant)::value;
                return load_unsigned_128(
                    descriptor,
                    keys[static_cast<std::size_t>(base_index) + stream_index]);
            });
}

/** Loads consecutive 128-bit keys in reverse into a vector stream. */
template <class descriptor_type, std::size_t stream_width>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_unsigned_128_reverse_stream(
    descriptor_type descriptor,
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::ptrdiff_t base_index) noexcept
{
    return vector_stream_target::make_vector_stream<
        descriptor_type,
        std::uint64_t,
        stream_width>(
            [&](auto stream_index_constant) [[gnu::always_inline]] {
                constexpr std::size_t stream_index =
                    decltype(stream_index_constant)::value;
                const auto offset =
                    static_cast<std::ptrdiff_t>(stream_index);
                return load_unsigned_128(
                    descriptor,
                    keys[static_cast<std::size_t>(base_index - offset)]);
            });
}

/** Returns the first stream slot whose mask has no set lanes. */
template <class descriptor_type,
          std::size_t stream_width,
          class mask_stream_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::size_t first_all_false_mask_slot(
    descriptor_type descriptor,
    const mask_stream_type& masks) noexcept
{
    std::size_t first_false_slot = stream_width;
    vector_stream_target::for_each_stream<stream_width>(
        [&](auto stream_index_constant) [[gnu::always_inline]] {
            constexpr std::size_t stream_index =
                decltype(stream_index_constant)::value;
            if (
                first_false_slot == stream_width
                && highway_target::AllFalse(
                    descriptor,
                    masks[stream_index])) {
                first_false_slot = stream_index;
            }
        });
    return first_false_slot;
}

/** Chooses the median of the first, middle, and final 128-bit keys. */
template <bool ascending>
[[nodiscard]] inline hwy::uint128_t choose_unsigned_128_pivot(
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count) noexcept
{
    hwy::uint128_t first_candidate = keys[0];
    hwy::uint128_t middle_candidate = keys[key_count / 2u];
    hwy::uint128_t final_candidate = keys[key_count - 1u];
    if (unsigned_128_precedes<ascending>(
            middle_candidate,
            first_candidate)) {
        std::swap(first_candidate, middle_candidate);
    }
    if (unsigned_128_precedes<ascending>(
            final_candidate,
            middle_candidate)) {
        std::swap(middle_candidate, final_candidate);
    }
    if (unsigned_128_precedes<ascending>(
            middle_candidate,
            first_candidate)) {
        std::swap(first_candidate, middle_candidate);
    }
    return middle_candidate;
}

/** Partitions 128-bit keys around a pivot with streamed comparison scans. */
template <std::size_t stream_width,
          bool ascending,
          class descriptor_type>
[[nodiscard]] HWY_INLINE std::size_t partition_unsigned_128_stream(
    descriptor_type descriptor,
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    hwy::uint128_t pivot) noexcept
{
    std::ptrdiff_t left_index = 0;
    std::ptrdiff_t right_index =
        static_cast<std::ptrdiff_t>(key_count) - 1;
    const auto pivot_stream =
        broadcast_unsigned_128_stream<descriptor_type, stream_width>(
            descriptor,
            pivot);

    while (left_index <= right_index) {
        while (left_index <= right_index) {
            while (
                right_index - left_index + 1
                >= static_cast<std::ptrdiff_t>(stream_width)) {
                const auto block =
                    load_unsigned_128_forward_stream<
                        descriptor_type,
                        stream_width>(
                            descriptor,
                            keys,
                            left_index);
                const auto precedes_pivot =
                    unsigned_128_stream_precedes<
                        ascending,
                        descriptor_type,
                        stream_width>(
                            descriptor,
                            block,
                            pivot_stream);
                const std::size_t first_nonmatching_slot =
                    first_all_false_mask_slot<
                        descriptor_type,
                        stream_width>(
                            descriptor,
                            precedes_pivot);
                if (first_nonmatching_slot == stream_width) {
                    left_index +=
                        static_cast<std::ptrdiff_t>(stream_width);
                    continue;
                }
                left_index +=
                    static_cast<std::ptrdiff_t>(first_nonmatching_slot);
                break;
            }
            while (
                left_index <= right_index
                && unsigned_128_precedes<ascending>(
                    keys[static_cast<std::size_t>(left_index)],
                    pivot)) {
                ++left_index;
            }
            break;
        }

        while (left_index <= right_index) {
            while (
                right_index - left_index + 1
                >= static_cast<std::ptrdiff_t>(stream_width)) {
                const auto block =
                    load_unsigned_128_reverse_stream<
                        descriptor_type,
                        stream_width>(
                            descriptor,
                            keys,
                            right_index);
                const auto pivot_precedes =
                    unsigned_128_stream_precedes<
                        ascending,
                        descriptor_type,
                        stream_width>(
                            descriptor,
                            pivot_stream,
                            block);
                const std::size_t first_nonmatching_slot =
                    first_all_false_mask_slot<
                        descriptor_type,
                        stream_width>(
                            descriptor,
                            pivot_precedes);
                if (first_nonmatching_slot == stream_width) {
                    right_index -=
                        static_cast<std::ptrdiff_t>(stream_width);
                    continue;
                }
                right_index -=
                    static_cast<std::ptrdiff_t>(first_nonmatching_slot);
                break;
            }
            while (
                left_index <= right_index
                && unsigned_128_precedes<ascending>(
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

/** Computes the depth limit used by the streamed introsort. */
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

/** Implements streamed 128-bit introsort for a non-scalar Highway target. */
template <std::size_t stream_width, bool ascending>
void sort_unsigned_128_vector_stream_impl(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t depth_limit)
{
    const highway_target::CappedTag<std::uint64_t, 2> descriptor;

    while (key_count > unsigned_128_scalar_key_limit) {
        if (depth_limit == 0u) [[unlikely]] {
            sort_unsigned_128_with_standard_library<ascending>(
                keys,
                key_count);
            return;
        }
        --depth_limit;

        const hwy::uint128_t pivot =
            choose_unsigned_128_pivot<ascending>(keys, key_count);
        const std::size_t partition_index =
            partition_unsigned_128_stream<stream_width, ascending>(
                descriptor,
                keys,
                key_count,
                pivot);
        if (
            partition_index == 0u
            || partition_index == key_count) [[unlikely]] {
            sort_unsigned_128_with_standard_library<ascending>(
                keys,
                key_count);
            return;
        }

        const std::size_t right_count = key_count - partition_index;
        if (partition_index < right_count) {
            sort_unsigned_128_vector_stream_impl<stream_width, ascending>(
                keys,
                partition_index,
                depth_limit);
            keys += partition_index;
            key_count = right_count;
        } else {
            sort_unsigned_128_vector_stream_impl<stream_width, ascending>(
                keys + partition_index,
                right_count,
                depth_limit);
            key_count = partition_index;
        }
    }

    insertion_sort_unsigned_128<ascending>(keys, key_count);
}

#else

/** Uses standard sorting when the active Highway target is scalar. */
template <std::size_t stream_width, bool ascending>
void sort_unsigned_128_vector_stream_impl(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t)
{
    sort_unsigned_128_with_standard_library<ascending>(keys, key_count);
}

#endif // HWY_TARGET != HWY_SCALAR || HWY_IDE

/** Selects the scalar or streamed 128-bit sorting path by input size. */
template <std::size_t stream_width, bool ascending>
inline void sort_unsigned_128_vector_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    if (key_count <= 1u) {
        return;
    }
    if (key_count <= unsigned_128_scalar_key_limit) {
        insertion_sort_unsigned_128<ascending>(keys, key_count);
        return;
    }
    sort_unsigned_128_vector_stream_impl<stream_width, ascending>(
        keys,
        key_count,
        introsort_depth_limit(key_count));
}

/** Ascending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_ascending_stream_width_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<1u, true>(keys, key_count);
}

/** Ascending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_ascending_stream_width_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<2u, true>(keys, key_count);
}

/** Ascending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_ascending_stream_width_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<3u, true>(keys, key_count);
}

/** Ascending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_ascending_stream_width_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<4u, true>(keys, key_count);
}

/** Descending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_descending_stream_width_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<1u, false>(keys, key_count);
}

/** Descending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_descending_stream_width_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<2u, false>(keys, key_count);
}

/** Descending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_descending_stream_width_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<3u, false>(keys, key_count);
}

/** Descending fixed-width bridge used by the module-interface dispatcher. */
void sort_unsigned_128_descending_stream_width_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_unsigned_128_vector_stream<4u, false>(keys, key_count);
}

} // namespace detail

/** Sorts unsigned 64-bit keys ascending through the static Highway target. */
void sort_unsigned_64_ascending(
    std::uint64_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortAscending());
}

/** Sorts unsigned 64-bit keys descending through the static Highway target. */
void sort_unsigned_64_descending(
    std::uint64_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortDescending());
}

/** Sorts unsigned 128-bit keys ascending with static-target VQSort. */
void sort_unsigned_128_ascending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortAscending());
}

/** Sorts unsigned 128-bit keys descending with static-target VQSort. */
void sort_unsigned_128_descending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        keys,
        key_count,
        hwy::SortDescending());
}

/** Sorts unsigned 128-bit keys ascending with the default vector stream. */
void sort_unsigned_128_ascending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_unsigned_128_vector_stream<
        unsigned_128_default_stream_width,
        true>(keys, key_count);
}

/** Sorts unsigned 128-bit keys descending with the default vector stream. */
void sort_unsigned_128_descending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_unsigned_128_vector_stream<
        unsigned_128_default_stream_width,
        false>(keys, key_count);
}

/** Sorts packed 32-bit key/value records ascending. */
void sort_key_value_32_ascending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        records,
        record_count,
        hwy::SortAscending());
}

/** Sorts packed 32-bit key/value records descending. */
void sort_key_value_32_descending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        records,
        record_count,
        hwy::SortDescending());
}

/** Partially sorts packed 32-bit key/value records descending. */
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

/** Sorts 64-bit key/value records ascending. */
void sort_key_value_64_ascending(
    hwy::K64V64* HWY_RESTRICT records,
    std::size_t record_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(
        records,
        record_count,
        hwy::SortAscending());
}

/** Sorts 64-bit key/value records descending. */
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
