/**
 * @file radix_sort.cppm
 * @brief Sorts unsigned keys in place, starting with their highest byte.
 *
 * Each pass groups values by one key byte, then sorts each group by the next
 * byte. Moving values directly between their destination groups avoids an
 * extra array. Small groups use insertion sort.
 *
 * The bucket table has 256 entries on the stack. Recursion stops after the
 * lowest key byte.
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
 * @brief Sorts a small range by moving each value into its sorted position.
 * @param[in] key Returns an unsigned sorting key.
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
 * @brief Groups values by the selected key bits, then sorts the lower bits.
 * @param[in] bits_per_pass Number of key bits used for each bucket.
 * @param[in] shift Lowest bit of the current group of key bits.
 * @param[in] key Returns an unsigned sorting key.
 * @pre `[begin, end)` is a valid, non-empty mutable range.
 * @pre `bits_per_pass` is positive and at most `radix_sort_bits_per_pass`.
 *
 * Bucket counts determine each destination range. Values are moved in cycles
 * until each range contains its own bucket, then each range is sorted further.
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

    // This pass uses only the first `bucket_count` entries.
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
 * @brief Sorts `[begin, end)` in place by an unsigned key.
 * @param[in] key Extracts the sorting key from an element.
 * @pre The range is valid, mutable, and non-empty.
 *
 * The order of equal keys is unspecified. Keys are read during sorting, so
 * the extractor must return a consistent value for each element.
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
 * @brief Sorts a range starting at a selected key byte.
 * @tparam p_shift Lowest bit of the next byte to sort. Must be a valid byte boundary.
 * @param[in] key Extracts an unsigned sorting key.
 * @pre `[begin, end)` is a valid, non-empty mutable range.
 * @pre All key bytes above the selected byte are equal throughout the range.
 *
 * Use this after higher key bytes have already placed the values in one bucket.
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
