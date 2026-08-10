/**
 * @file index.cppm
 * @brief Primary interface for Highway-backed SIMD sorting.
 *
 * The interface exposes stable declarations and compile-time stream-width
 * dispatch only. Highway implementation headers remain in the same-module
 * implementation unit, keeping target-local definitions out of this BMI.
 */
module;

#include <hwy/base.h>

#include <cstddef>
#include <cstdint>

export module bioseq.simd.sorting;

export namespace bioseq::simd::sorting {

/** Minimum input size at which callers should consider Highway VQSort. */
inline constexpr std::size_t vqsort_minimum_bytes = 100u * 1024u;

/**
 * @brief Minimum key count corresponding to `vqsort_minimum_bytes`.
 * @tparam key_type Key representation stored in the input array.
 */
template <class key_type>
inline constexpr std::size_t vqsort_minimum_keys =
    vqsort_minimum_bytes / sizeof(key_type);

/**
 * VQSort selection for 128-bit keys is explicitly disabled. A 128-bit NEON
 * vector contains one key, so callers use the vector-stream sorter unless they
 * invoke the VQSort-specific entry point directly.
 */
template <>
inline constexpr std::size_t vqsort_minimum_keys<hwy::uint128_t> =
    static_cast<std::size_t>(-1);

/** Default independent-vector count for streamed 128-bit partition scans. */
inline constexpr std::size_t unsigned_128_default_stream_width = 2u;

/** Largest 128-bit key count handled by the scalar insertion-sort path. */
inline constexpr std::size_t unsigned_128_scalar_key_limit = 8u;

/**
 * @brief Accepts stream widths implemented by the fixed dispatch bridge.
 * @tparam stream_width Independent-vector count.
 */
template <std::size_t stream_width>
concept supported_unsigned_128_stream_width =
    stream_width >= 1u && stream_width <= 4u;

namespace detail {

/** Sorts 128-bit keys ascending with a stream width of one. */
void sort_unsigned_128_ascending_stream_width_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Sorts 128-bit keys ascending with a stream width of two. */
void sort_unsigned_128_ascending_stream_width_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Sorts 128-bit keys ascending with a stream width of three. */
void sort_unsigned_128_ascending_stream_width_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Sorts 128-bit keys ascending with a stream width of four. */
void sort_unsigned_128_ascending_stream_width_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Sorts 128-bit keys descending with a stream width of one. */
void sort_unsigned_128_descending_stream_width_1(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Sorts 128-bit keys descending with a stream width of two. */
void sort_unsigned_128_descending_stream_width_2(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Sorts 128-bit keys descending with a stream width of three. */
void sort_unsigned_128_descending_stream_width_3(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Sorts 128-bit keys descending with a stream width of four. */
void sort_unsigned_128_descending_stream_width_4(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/** Dispatches an ascending streamed sort to its fixed-width implementation. */
template <std::size_t stream_width>
inline void dispatch_unsigned_128_ascending_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    static_assert(supported_unsigned_128_stream_width<stream_width>);
    if constexpr (stream_width == 1u) {
        sort_unsigned_128_ascending_stream_width_1(keys, key_count);
    } else if constexpr (stream_width == 2u) {
        sort_unsigned_128_ascending_stream_width_2(keys, key_count);
    } else if constexpr (stream_width == 3u) {
        sort_unsigned_128_ascending_stream_width_3(keys, key_count);
    } else {
        sort_unsigned_128_ascending_stream_width_4(keys, key_count);
    }
}

/** Dispatches a descending streamed sort to its fixed-width implementation. */
template <std::size_t stream_width>
inline void dispatch_unsigned_128_descending_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    static_assert(supported_unsigned_128_stream_width<stream_width>);
    if constexpr (stream_width == 1u) {
        sort_unsigned_128_descending_stream_width_1(keys, key_count);
    } else if constexpr (stream_width == 2u) {
        sort_unsigned_128_descending_stream_width_2(keys, key_count);
    } else if constexpr (stream_width == 3u) {
        sort_unsigned_128_descending_stream_width_3(keys, key_count);
    } else {
        sort_unsigned_128_descending_stream_width_4(keys, key_count);
    }
}

} // namespace detail

/**
 * @brief Sorts unsigned 64-bit keys in ascending order.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
void sort_unsigned_64_ascending(
    std::uint64_t* HWY_RESTRICT keys,
    std::size_t key_count);

/**
 * @brief Sorts unsigned 64-bit keys in descending order.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
void sort_unsigned_64_descending(
    std::uint64_t* HWY_RESTRICT keys,
    std::size_t key_count);

/**
 * @brief Sorts unsigned 128-bit keys ascending with Highway VQSort.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
void sort_unsigned_128_ascending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/**
 * @brief Sorts unsigned 128-bit keys descending with Highway VQSort.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
void sort_unsigned_128_descending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/**
 * @brief Sorts unsigned 128-bit keys ascending with streamed partition scans.
 * @tparam stream_width Independent-vector count used by each partition scan.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 *
 * Ordering is lexicographic by `(hi, lo)`, matching Highway's 128-bit
 * comparison convention.
 */
template <
    std::size_t stream_width = unsigned_128_default_stream_width>
    requires supported_unsigned_128_stream_width<stream_width>
inline void sort_unsigned_128_ascending_vector_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::dispatch_unsigned_128_ascending_stream<stream_width>(
        keys,
        key_count);
}

/**
 * @brief Sorts unsigned 128-bit keys descending with streamed partition scans.
 * @tparam stream_width Independent-vector count used by each partition scan.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 *
 * Ordering is reverse lexicographic by `(hi, lo)`.
 */
template <
    std::size_t stream_width = unsigned_128_default_stream_width>
    requires supported_unsigned_128_stream_width<stream_width>
inline void sort_unsigned_128_descending_vector_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::dispatch_unsigned_128_descending_stream<stream_width>(
        keys,
        key_count);
}

/**
 * @brief Sorts unsigned 128-bit keys ascending with the default stream width.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
void sort_unsigned_128_ascending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/**
 * @brief Sorts unsigned 128-bit keys descending with the default stream width.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
void sort_unsigned_128_descending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count);

/**
 * @brief Sorts packed 32-bit key/value records ascending by key.
 * @param records Mutable `K32V32` array.
 * @param record_count Number of records.
 *
 * Each record stores its key in the upper 32 bits and value in the lower
 * 32 bits. Signed keys must be biased before packing when signed order is
 * required.
 */
void sort_key_value_32_ascending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count);

/**
 * @brief Sorts packed 32-bit key/value records descending by key.
 * @param records Mutable `K32V32` array.
 * @param record_count Number of records.
 */
void sort_key_value_32_descending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count);

/**
 * @brief Places the largest packed 32-bit key/value records first.
 * @param records Mutable `K32V32` array.
 * @param record_count Number of records.
 * @param selected_count Number of largest records to order at the front.
 */
void partial_sort_key_value_32_descending(
    hwy::K32V32* HWY_RESTRICT records,
    std::size_t record_count,
    std::size_t selected_count);

/**
 * @brief Sorts 64-bit key/value records ascending by key.
 * @param records Mutable `K64V64` array.
 * @param record_count Number of records.
 */
void sort_key_value_64_ascending(
    hwy::K64V64* HWY_RESTRICT records,
    std::size_t record_count);

/**
 * @brief Sorts 64-bit key/value records descending by key.
 * @param records Mutable `K64V64` array.
 * @param record_count Number of records.
 */
void sort_key_value_64_descending(
    hwy::K64V64* HWY_RESTRICT records,
    std::size_t record_count);

} // namespace bioseq::simd::sorting
