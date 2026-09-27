/**
 * @file index.cppm
 * @brief SIMD sorting functions built on Highway.
 *
 * The public module declares the sorting functions and selects stream widths
 * at compile time. Highway's CPU-specific implementation headers are included
 * in the implementation unit
 */
module;

#include <hwy/base.h>

#include <cstddef>
#include <cstdint>

export module bioseq.simd.sorting;

export namespace bioseq::simd::sorting {

/** @brief Minimum input size at which callers should consider Highway VQSort. */
inline constexpr std::size_t vqsort_minimum_bytes = 100u * 1024u;

/**
 * @brief Minimum key count corresponding to `vqsort_minimum_bytes`.
 * @tparam p_key_type Key representation stored in the input array.
 */
template <class p_key_type>
inline constexpr std::size_t vqsort_minimum_keys =
    vqsort_minimum_bytes / sizeof(p_key_type);

/**
 * Automatic VQSort selection  is disabled for 128-bit keys. A NEON vector
 * holds just one such key, so the default sorter compares several independent
 * vectors at a time. The explicit VQSort entry points remain available.
 */
template <>
inline constexpr std::size_t vqsort_minimum_keys<hwy::uint128_t> =
    static_cast<std::size_t>(-1);

/** @brief Number of independent vectors compared together by the default 128-bit sorter. */
inline constexpr std::size_t unsigned_128_default_stream_width = 2u;

/** @brief Largest 128-bit key count handled by the scalar insertion-sort path. */
inline constexpr std::size_t unsigned_128_scalar_key_limit = 8u;

/**
 * @brief Allows one to four independent vectors per partition scan.
 */
template <std::size_t p_stream_width>
concept supported_unsigned_128_stream_width =
    p_stream_width >= 1u && p_stream_width <= 4u;

namespace detail {

/** @brief Sorts 128-bit keys ascending with a stream width of one. */
auto sort_unsigned_128_ascending_stream_width_1(hwy::uint128_t* HWY_RESTRICT keys,
                                                std::size_t key_count) -> void;

/** @brief Sorts 128-bit keys ascending with a stream width of two. */
auto sort_unsigned_128_ascending_stream_width_2(hwy::uint128_t* HWY_RESTRICT keys,
                                                std::size_t key_count) -> void;

/** @brief Sorts 128-bit keys ascending with a stream width of three. */
auto sort_unsigned_128_ascending_stream_width_3(hwy::uint128_t* HWY_RESTRICT keys,
                                                std::size_t key_count) -> void;

/** @brief Sorts 128-bit keys ascending with a stream width of four. */
auto sort_unsigned_128_ascending_stream_width_4(hwy::uint128_t* HWY_RESTRICT keys,
                                                std::size_t key_count) -> void;

/** @brief Sorts 128-bit keys descending with a stream width of one. */
auto sort_unsigned_128_descending_stream_width_1(hwy::uint128_t* HWY_RESTRICT keys,
                                                 std::size_t key_count) -> void;

/** @brief Sorts 128-bit keys descending with a stream width of two. */
auto sort_unsigned_128_descending_stream_width_2(hwy::uint128_t* HWY_RESTRICT keys,
                                                 std::size_t key_count) -> void;

/** @brief Sorts 128-bit keys descending with a stream width of three. */
auto sort_unsigned_128_descending_stream_width_3(hwy::uint128_t* HWY_RESTRICT keys,
                                                 std::size_t key_count) -> void;

/** @brief Sorts 128-bit keys descending with a stream width of four. */
auto sort_unsigned_128_descending_stream_width_4(hwy::uint128_t* HWY_RESTRICT keys,
                                                 std::size_t key_count) -> void;

/** @brief Calls the ascending sortr for the chosen stream width. */
template <std::size_t p_stream_width>
inline auto dispatch_unsigned_128_ascending_stream(hwy::uint128_t* HWY_RESTRICT keys,
                                                   std::size_t key_count) -> void {
    static_assert(supported_unsigned_128_stream_width<p_stream_width>);
    if constexpr (p_stream_width == 1u) {
        sort_unsigned_128_ascending_stream_width_1(keys, key_count);
    } else if constexpr (p_stream_width == 2u) {
        sort_unsigned_128_ascending_stream_width_2(keys, key_count);
    } else if constexpr (p_stream_width == 3u) {
        sort_unsigned_128_ascending_stream_width_3(keys, key_count);
    } else {
        sort_unsigned_128_ascending_stream_width_4(keys, key_count);
    }
}

/** @brief Calls the descending sorter for the chosen stream  width. */
template <std::size_t p_stream_width>
inline auto dispatch_unsigned_128_descending_stream(hwy::uint128_t* HWY_RESTRICT keys,
                                                    std::size_t key_count) -> void {
    static_assert(supported_unsigned_128_stream_width<p_stream_width>);
    if constexpr (p_stream_width == 1u) {
        sort_unsigned_128_descending_stream_width_1(keys, key_count);
    } else if constexpr (p_stream_width == 2u) {
        sort_unsigned_128_descending_stream_width_2(keys, key_count);
    } else if constexpr (p_stream_width == 3u) {
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
auto sort_unsigned_64_ascending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) -> void;

/**
 * @brief Sorts unsigned 64-bit keys in descending order.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
auto sort_unsigned_64_descending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) -> void;

/**
 * @brief Sorts unsigned 128-bit keys ascending with Highway VQSort.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
auto sort_unsigned_128_ascending_vqsort(hwy::uint128_t* HWY_RESTRICT keys, std::size_t key_count)
    -> void;

/**
 * @brief Sorts unsigned 128-bit keys descending with Highway VQSort.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
auto sort_unsigned_128_descending_vqsort(hwy::uint128_t* HWY_RESTRICT keys, std::size_t key_count)
    -> void;

/**
 * @brief Sorts unsigned 128-bit keys in ascending order.
 * @tparam p_stream_width Number of independent vectors compared per scan, from 1 to 4.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 *
 * The sorter compares `hi` first, then `lo` when the high halves are equal.
 * Partition scans process several keys together using SIMD comparisons.
 */
template <std::size_t p_stream_width = unsigned_128_default_stream_width>
    requires supported_unsigned_128_stream_width<p_stream_width>
inline auto sort_unsigned_128_ascending_vector_stream(hwy::uint128_t* HWY_RESTRICT keys,
                                                      std::size_t key_count) -> void {
    detail::dispatch_unsigned_128_ascending_stream<p_stream_width>(
        keys,
        key_count);
}

/**
 * @brief Sorts unsigned 128-bit keys in descending order.
 * @tparam p_stream_width Number of independent vectors compared per scan, from 1 to 4.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 *
 * The sorter compares `hi` first, then `lo` when the high halves are equal.
 * Larger keys come first.
 */
template <std::size_t p_stream_width = unsigned_128_default_stream_width>
    requires supported_unsigned_128_stream_width<p_stream_width>
inline auto sort_unsigned_128_descending_vector_stream(hwy::uint128_t* HWY_RESTRICT keys,
                                                       std::size_t key_count) -> void {
    detail::dispatch_unsigned_128_descending_stream<p_stream_width>(
        keys,
        key_count);
}

/**
 * @brief Sorts unsigned 128-bit keys ascending with the default stream width.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
auto sort_unsigned_128_ascending(hwy::uint128_t* HWY_RESTRICT keys, std::size_t key_count) -> void;

/**
 * @brief Sorts unsigned 128-bit keys descending with the default stream width.
 * @param keys Mutable key array.
 * @param key_count Number of keys.
 */
auto sort_unsigned_128_descending(hwy::uint128_t* HWY_RESTRICT keys, std::size_t key_count) -> void;

/**
 * @brief Sorts packed 32-bit key/value records ascending by key.
 * @param records Mutable `K32V32` array.
 * @param record_count Number of records.
 *
 * Each record stores its key in the upper 32 bits and value in the lower
 * 32 bits. Signed keys must be biased before packing when signed order is
 * required.
 */
auto sort_key_value_32_ascending(hwy::K32V32* HWY_RESTRICT records, std::size_t record_count)
    -> void;

/**
 * @brief Sorts packed 32-bit key/value records descending by key.
 * @param records Mutable `K32V32` array.
 * @param record_count Number of records.
 */
auto sort_key_value_32_descending(hwy::K32V32* HWY_RESTRICT records, std::size_t record_count)
    -> void;

/**
 * @brief Places records with the largest 32-bit keys first, in  descending order.
 * @param records Mutable `K32V32` array.
 * @param record_count Number of records.
 * @param selected_count Number of records to sort at the front.
 */
auto partial_sort_key_value_32_descending(hwy::K32V32* HWY_RESTRICT records,
                                          std::size_t record_count,
                                          std::size_t selected_count) -> void;

/**
 * @brief Sorts 64-bit key/value records ascending by key.
 * @param records Mutable `K64V64` array.
 * @param record_count Number of records.
 */
auto sort_key_value_64_ascending(hwy::K64V64* HWY_RESTRICT records, std::size_t record_count)
    -> void;

/**
 * @brief Sorts 64-bit key/value records descending by key.
 * @param records Mutable `K64V64` array.
 * @param record_count Number of records.
 */
auto sort_key_value_64_descending(hwy::K64V64* HWY_RESTRICT records, std::size_t record_count)
    -> void;

} // namespace bioseq::simd::sorting
