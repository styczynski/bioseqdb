/**
 * @file radix_sort.cppm
 * @brief In-place most-significant-byte radix sorting for unsigned keys.
 *
 * The algorithm uses eight bits per pass and a 256-entry stack bucket table.
 * Each non-empty bucket is permuted in place by following its destination
 * cycle. Small ranges use insertion sort to avoid bucket-table overhead.
 *
 * Recursion consumes one key byte per level, bounding depth by the key width.
 */
module;

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.common.radix_sort;

export namespace bioseq::sorting {

/** Largest range handled by the insertion-sort fast path. */
inline constexpr std::size_t radix_sort_insertion_limit = 64;

/** Number of key bits consumed by each radix pass. */
inline constexpr std::size_t radix_sort_bits_per_pass = 8;

}  // namespace bioseq::sorting

namespace bioseq::sorting::radix_sort_detail {

/**
 * @brief Sort a small non-empty range by its extracted unsigned key.
 *
 * @tparam p_value_type Element type stored in the range.
 * @tparam p_key_function Callable returning an unsigned key for an element.
 * @param p_begin First element in the range.
 * @param p_end One past the final element in the range.
 * @param p_key Key extractor invoked at each comparison site.
 * @pre `[p_begin, p_end)` is a valid, non-empty mutable range.
 */
template <typename p_value_type, typename p_key_function>
void insertion_sort(
    p_value_type* p_begin,
    p_value_type* p_end,
    p_key_function p_key)
{
    for (p_value_type* current = p_begin + 1;
         current < p_end;
         ++current) {
        if (p_key(*current) < p_key(*(current - 1))) {
            p_value_type value = *current;
            p_value_type* insertion = current;
            while (insertion > p_begin
                   && p_key(value) < p_key(*(insertion - 1))) {
                *insertion = *(insertion - 1);
                --insertion;
            }
            *insertion = value;
        }
    }
}

/**
 * @brief Partition a range by one key byte and recursively sort each bucket.
 *
 * @tparam p_value_type Element type stored in the range.
 * @tparam p_key_function Callable returning an unsigned key for an element.
 * @param p_begin First element in the range.
 * @param p_end One past the final element in the range.
 * @param p_bits_per_pass Number of key bits assigned to the bucket index.
 * @param p_shift Least-significant bit of the current key digit.
 * @param p_key Key extractor invoked at each radix-extraction site.
 * @pre `[p_begin, p_end)` is a valid, non-empty mutable range.
 * @pre `p_bits_per_pass` is positive and no greater than
 * `radix_sort_bits_per_pass`.
 */
template <typename p_value_type, typename p_key_function>
void sort_suffix(
    p_value_type* p_begin,
    p_value_type* p_end,
    int p_bits_per_pass,
    int p_shift,
    p_key_function p_key)
{
    using key_type = std::decay_t<decltype(p_key(*p_begin))>;
    static_assert(std::is_unsigned_v<key_type>,
                  "radix sort requires an unsigned key");

    struct bucket {
        p_value_type* begin;
        p_value_type* end;
    };

    const int bucket_count = 1 << p_bits_per_pass;
    const key_type bucket_mask =
        static_cast<key_type>(bucket_count) - 1;

    bucket buckets[1 << radix_sort_bits_per_pass];
    bucket* const buckets_end = buckets + bucket_count;

    for (bucket* current_bucket = buckets;
         current_bucket != buckets_end;
         ++current_bucket) {
        current_bucket->begin = p_begin;
        current_bucket->end = p_begin;
    }
    for (p_value_type* current = p_begin;
         current != p_end;
         ++current) {
        ++buckets[(p_key(*current) >> p_shift) & bucket_mask].end;
    }
    for (bucket* current_bucket = buckets + 1;
         current_bucket != buckets_end;
         ++current_bucket) {
        current_bucket->end +=
            (current_bucket - 1)->end - p_begin;
        current_bucket->begin = (current_bucket - 1)->end;
    }
    for (bucket* current_bucket = buckets;
         current_bucket != buckets_end;) {
        if (current_bucket->begin != current_bucket->end) {
            bucket* destination = buckets
                + ((p_key(*current_bucket->begin) >> p_shift)
                   & bucket_mask);
            if (destination != current_bucket) {
                p_value_type pending = *current_bucket->begin;
                p_value_type displaced;
                do {
                    displaced = pending;
                    pending = *destination->begin;
                    *destination->begin++ = displaced;
                    destination = buckets
                        + ((p_key(pending) >> p_shift) & bucket_mask);
                } while (destination != current_bucket);
                *current_bucket->begin++ = pending;
            } else {
                ++current_bucket->begin;
            }
        } else {
            ++current_bucket;
        }
    }
    buckets->begin = p_begin;
    for (bucket* current_bucket = buckets + 1;
         current_bucket != buckets_end;
         ++current_bucket) {
        current_bucket->begin = (current_bucket - 1)->end;
    }
    if (p_shift != 0) {
        const int next_shift = p_shift > p_bits_per_pass
            ? p_shift - p_bits_per_pass
            : 0;
        for (bucket* current_bucket = buckets;
             current_bucket != buckets_end;
             ++current_bucket) {
            const std::ptrdiff_t span =
                current_bucket->end - current_bucket->begin;
            if (span > static_cast<std::ptrdiff_t>(
                    radix_sort_insertion_limit)) {
                sort_suffix(
                    current_bucket->begin,
                    current_bucket->end,
                    p_bits_per_pass,
                    next_shift,
                    p_key);
            } else if (span > 1) {
                insertion_sort(
                    current_bucket->begin,
                    current_bucket->end,
                    p_key);
            }
        }
    }
}

}  // namespace bioseq::sorting::radix_sort_detail

export namespace bioseq::sorting {

/**
 * Sort a half-open range in place by an unsigned extracted key.
 *
 * Equal-key ordering is unspecified. The key function is invoked inline at
 * each comparison and radix extraction site.
 *
 * @tparam p_value_type Element type stored in the range.
 * @tparam p_key_function Callable returning an unsigned key for an element.
 * @param p_begin First element in the range.
 * @param p_end One past the final element in the range.
 * @param p_key Key extractor invoked at each comparison and radix site.
 * @pre `[p_begin, p_end)` is a valid, non-empty mutable range.
 */
template <typename p_value_type, typename p_key_function>
void radix_sort(
    p_value_type* p_begin,
    p_value_type* p_end,
    p_key_function p_key)
{
    using key_type = std::decay_t<decltype(p_key(*p_begin))>;
    static_assert(std::is_unsigned_v<key_type>,
                  "radix sort requires an unsigned key");
    constexpr int key_bytes = sizeof(key_type);
    if (p_end - p_begin <= static_cast<std::ptrdiff_t>(
            radix_sort_insertion_limit)) {
        radix_sort_detail::insertion_sort(p_begin, p_end, p_key);
    } else {
        radix_sort_detail::sort_suffix(
            p_begin,
            p_end,
            static_cast<int>(radix_sort_bits_per_pass),
            (key_bytes - 1)
                * static_cast<int>(radix_sort_bits_per_pass),
            p_key);
    }
}

/**
 * Sort a range whose more-significant key bytes are already partitioned.
 *
 * @tparam p_shift Least-significant bit of the next key byte to consume.
 * @tparam p_value_type Element type stored in the range.
 * @tparam p_key_function Callable returning an unsigned key for an element.
 * @param p_begin First element in the range.
 * @param p_end One past the final element in the range.
 * @param p_key Key extractor invoked at each comparison and radix site.
 * @pre `[p_begin, p_end)` is a valid, non-empty mutable range.
 * @pre Every key byte above `p_shift` is equal throughout the range.
 */
template <int p_shift, typename p_value_type, typename p_key_function>
void radix_sort_from_shift(
    p_value_type* p_begin,
    p_value_type* p_end,
    p_key_function p_key)
{
    using key_type = std::decay_t<decltype(p_key(*p_begin))>;
    static_assert(std::is_unsigned_v<key_type>,
                  "radix sort requires an unsigned key");
    static_assert(p_shift >= 0, "radix shift must be non-negative");
    static_assert(
        p_shift % static_cast<int>(radix_sort_bits_per_pass) == 0,
                  "radix shift must select a byte boundary");
    static_assert(p_shift < static_cast<int>(sizeof(key_type) * 8u),
                  "radix shift exceeds the key width");
    if (p_end - p_begin <= static_cast<std::ptrdiff_t>(
            radix_sort_insertion_limit)) {
        radix_sort_detail::insertion_sort(p_begin, p_end, p_key);
    } else {
        radix_sort_detail::sort_suffix(
            p_begin,
            p_end,
            static_cast<int>(radix_sort_bits_per_pass),
            p_shift,
            p_key);
    }
}

/**
 * @brief Sort raw unsigned 64-bit values in ascending order.
 *
 * @param p_begin First value in the range.
 * @param p_end One past the final value in the range.
 * @pre `[p_begin, p_end)` is a valid, non-empty mutable range.
 */
inline void radix_sort_unsigned_64(
    std::uint64_t* p_begin,
    std::uint64_t* p_end)
{
    radix_sort(
        p_begin,
        p_end,
        [](std::uint64_t value) { return value; });
}

}  // namespace bioseq::sorting
