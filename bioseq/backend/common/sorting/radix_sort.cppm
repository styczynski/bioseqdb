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

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.common.radix_sort;

export namespace bioseq::sorting {

/** @brief Largest range handled by the insertion-sort fast path. */
inline constexpr std::size_t radix_sort_insertion_limit = 64;

/** @brief Number of key bits consumed by each radix pass. */
inline constexpr std::size_t radix_sort_bits_per_pass = 8;

}  // namespace bioseq::sorting

namespace bioseq::sorting::radix_sort_detail {

/**
 * @brief Sort a small non-empty range by its extracted unsigned key.
 *
 * @tparam p_value_type Element type stored in the range.
 * @tparam p_key_function Callable returning an unsigned key for an element.
 * @param[in,out] begin First element in the range.
 * @param[in] end One past the final element in the range.
 * @param[in] key Key extractor invoked at each comparison site.
 * @pre `[begin, end)` is a valid, non-empty mutable range.
 */
template <typename p_value_type, typename p_key_function>
void insertion_sort(
    p_value_type* begin,
    p_value_type* end,
    p_key_function key)
{
    for (p_value_type* current = begin + 1;
         current < end;
         ++current) {
        if (key(*current) < key(*(current - 1))) {
            p_value_type value = *current;
            p_value_type* insertion = current;
            while (insertion > begin
                   && key(value) < key(*(insertion - 1))) {
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
 * @param[in,out] begin First element in the range.
 * @param[in] end One past the final element in the range.
 * @param[in] bits_per_pass Number of key bits assigned to the bucket index.
 * @param[in] shift Least-significant bit of the current key digit.
 * @param[in] key Key extractor invoked at each radix-extraction site.
 * @pre `[begin, end)` is a valid, non-empty mutable range.
 * @pre `bits_per_pass` is positive and no greater than
 * `radix_sort_bits_per_pass`.
 */
template <typename p_value_type, typename p_key_function>
void sort_suffix(
    p_value_type* begin,
    p_value_type* end,
    int bits_per_pass,
    int shift,
    p_key_function key)
{
    using key_type = std::decay_t<decltype(key(*begin))>;
    static_assert(std::is_unsigned_v<key_type>,
                  "radix sort requires an unsigned key");

    struct bucket {
        p_value_type* begin;
        p_value_type* end;
    };

    const int bucket_count = 1 << bits_per_pass;
    const key_type bucket_mask =
        static_cast<key_type>(bucket_count) - 1;

    // Only the active bucket prefix is initialized and visited for this pass.
    std::array<bucket, std::size_t{1} << radix_sort_bits_per_pass> buckets;
    bucket* const buckets_begin = buckets.data();
    bucket* const buckets_end = buckets_begin + bucket_count;

    for (bucket* current_bucket = buckets_begin;
         current_bucket != buckets_end;
         ++current_bucket) {
        current_bucket->begin = begin;
        current_bucket->end = begin;
    }
    for (p_value_type* current = begin;
         current != end;
         ++current) {
        ++buckets[static_cast<std::size_t>(
            (key(*current) >> shift) & bucket_mask)].end;
    }
    for (bucket* current_bucket = buckets_begin + 1;
         current_bucket != buckets_end;
         ++current_bucket) {
        current_bucket->end +=
            (current_bucket - 1)->end - begin;
        current_bucket->begin = (current_bucket - 1)->end;
    }
    for (bucket* current_bucket = buckets_begin;
         current_bucket != buckets_end;) {
        if (current_bucket->begin != current_bucket->end) {
            bucket* destination = buckets_begin
                + static_cast<std::size_t>(
                    (key(*current_bucket->begin) >> shift)
                    & bucket_mask);
            if (destination != current_bucket) {
                p_value_type pending = *current_bucket->begin;
                p_value_type displaced;
                do {
                    displaced = pending;
                    pending = *destination->begin;
                    *destination->begin++ = displaced;
                    destination = buckets_begin
                        + static_cast<std::size_t>(
                            (key(pending) >> shift) & bucket_mask);
                } while (destination != current_bucket);
                *current_bucket->begin++ = pending;
            } else {
                ++current_bucket->begin;
            }
        } else {
            ++current_bucket;
        }
    }
    buckets.front().begin = begin;
    for (bucket* current_bucket = buckets_begin + 1;
         current_bucket != buckets_end;
         ++current_bucket) {
        current_bucket->begin = (current_bucket - 1)->end;
    }
    if (shift != 0) {
        const int next_shift = shift > bits_per_pass
            ? shift - bits_per_pass
            : 0;
        for (bucket* current_bucket = buckets_begin;
             current_bucket != buckets_end;
             ++current_bucket) {
            const std::ptrdiff_t span =
                current_bucket->end - current_bucket->begin;
            if (span > static_cast<std::ptrdiff_t>(
                    radix_sort_insertion_limit)) {
                sort_suffix(
                    current_bucket->begin,
                    current_bucket->end,
                    bits_per_pass,
                    next_shift,
                    key);
            } else if (span > 1) {
                insertion_sort(
                    current_bucket->begin,
                    current_bucket->end,
                    key);
            }
        }
    }
}

}  // namespace bioseq::sorting::radix_sort_detail

export namespace bioseq::sorting {

/**
 * @brief Sorts a half-open range in place by an unsigned extracted key.
 *
 * Equal-key ordering is unspecified. The key function is invoked inline at
 * each comparison and radix extraction site.
 *
 * @tparam p_value_type Element type stored in the range.
 * @tparam p_key_function Callable returning an unsigned key for an element.
 * @param[in,out] begin First element in the range.
 * @param[in] end One past the final element in the range.
 * @param[in] key Key extractor invoked at each comparison and radix site.
 * @pre `[begin, end)` is a valid, non-empty mutable range.
 */
template <typename p_value_type, typename p_key_function>
void radix_sort(
    p_value_type* begin,
    p_value_type* end,
    p_key_function key)
{
    using key_type = std::decay_t<decltype(key(*begin))>;
    static_assert(std::is_unsigned_v<key_type>,
                  "radix sort requires an unsigned key");
    constexpr int key_bytes = sizeof(key_type);
    if (end - begin <= static_cast<std::ptrdiff_t>(
            radix_sort_insertion_limit)) {
        radix_sort_detail::insertion_sort(begin, end, key);
    } else {
        radix_sort_detail::sort_suffix(
            begin,
            end,
            static_cast<int>(radix_sort_bits_per_pass),
            (key_bytes - 1)
                * static_cast<int>(radix_sort_bits_per_pass),
            key);
    }
}

/**
 * @brief Sorts a range whose more-significant key bytes are already partitioned.
 *
 * @tparam p_shift Least-significant bit of the next key byte to consume.
 * @tparam p_value_type Element type stored in the range.
 * @tparam p_key_function Callable returning an unsigned key for an element.
 * @param[in,out] begin First element in the range.
 * @param[in] end One past the final element in the range.
 * @param[in] key Key extractor invoked at each comparison and radix site.
 * @pre `[begin, end)` is a valid, non-empty mutable range.
 * @pre Every key byte above `p_shift` is equal throughout the range.
 */
template <int p_shift, typename p_value_type, typename p_key_function>
void radix_sort_from_shift(
    p_value_type* begin,
    p_value_type* end,
    p_key_function key)
{
    using key_type = std::decay_t<decltype(key(*begin))>;
    static_assert(std::is_unsigned_v<key_type>,
                  "radix sort requires an unsigned key");
    static_assert(p_shift >= 0, "radix shift must be non-negative");
    static_assert(
        p_shift % static_cast<int>(radix_sort_bits_per_pass) == 0,
                  "radix shift must select a byte boundary");
    static_assert(p_shift < static_cast<int>(sizeof(key_type) * 8u),
                  "radix shift exceeds the key width");
    if (end - begin <= static_cast<std::ptrdiff_t>(
            radix_sort_insertion_limit)) {
        radix_sort_detail::insertion_sort(begin, end, key);
    } else {
        radix_sort_detail::sort_suffix(
            begin,
            end,
            static_cast<int>(radix_sort_bits_per_pass),
            p_shift,
            key);
    }
}

/**
 * @brief Sort raw unsigned 64-bit values in ascending order.
 *
 * @param[in,out] begin First value in the range.
 * @param[in] end One past the final value in the range.
 * @pre `[begin, end)` is a valid, non-empty mutable range.
 */
inline void radix_sort_unsigned_64(
    std::uint64_t* begin,
    std::uint64_t* end)
{
    radix_sort(
        begin,
        end,
        [](std::uint64_t value) { return value; });
}

}  // namespace bioseq::sorting
