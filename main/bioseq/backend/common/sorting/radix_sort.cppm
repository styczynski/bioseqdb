/**
 * @file radix_sort.cppm
 * @brief Port of minimap2's `radix_sort_*` family from `ksort.h`.
 *
 * Reproduces the LSD-then-MSD recursion used by minimap2 for sorting
 * (anchor, chain, coverage_intervals, scored_indices) buckets in the
 * post-sketch pipeline. The algorithm is:
 *
 *   - 8 bits per pass, MSD-first; one bucket array (256 entries) per
 *     recursion level, allocated on the stack.
 *   - In-place permutation: for each non-empty bucket, follow a swap
 *     chain through the bucket index until the cycle closes. This
 *     leaves every element in its destination bucket without an
 *     auxiliary buffer.
 *   - Below `RS_MIN_SIZE` (= 64) elements, fall back to insertion sort
 *     because the bucket-walking overhead dominates radix's linear scan.
 *
 * The minimap2 macros generate one specialised function per
 * (type, key-extractor, key-size) tuple; we replicate that with a
 * function template and let the compiler inline the key extractor at
 * each call site. The output is byte-identical to a call to the
 * generated `radix_sort_*` variant in `bench/tool_minimap2/minimap2/`.
 *
 * The recursion depth is bounded by `(8 * sizeof(key)) / RS_MAX_BITS`,
 * which is 8 for a 64-bit key, so a hand-converted recursive
 * implementation has no stack-blow risk; we keep it recursive for
 * exact parity with the upstream implementation.
 */
module;

#include "common.h"

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <utility>

export module bioseq.common.radix_sort;

export namespace bioseq::sorting {

inline constexpr std::size_t kRadixMinSize  = 64;
inline constexpr std::size_t kRadixMaxBits  = 8;

namespace detail {

template <typename T, typename KeyFn>
void rs_insertsort(T* beg, T* end, KeyFn key) {
    for (T* i = beg + 1; i < end; ++i) {
        if (key(*i) < key(*(i - 1))) {
            T tmp = *i;
            T* j = i;
            while (j > beg && key(tmp) < key(*(j - 1))) {
                *j = *(j - 1);
                --j;
            }
            *j = tmp;
        }
    }
}

template <typename T, typename KeyFn>
void rs_sort(T* beg, T* end, int n_bits, int s, KeyFn key) {
    using KeyT = std::decay_t<decltype(key(*beg))>;
    static_assert(std::is_unsigned_v<KeyT>,
                  "radix sort requires an unsigned key");

    struct Bucket { T* b; T* e; };

    const int size = 1 << n_bits;
    const KeyT m = static_cast<KeyT>(size) - 1;

    Bucket b[1 << kRadixMaxBits];
    Bucket* const be = b + size;

    for (Bucket* k = b; k != be; ++k) {
        k->b = beg;
        k->e = beg;
    }
    for (T* i = beg; i != end; ++i) {
        ++b[(key(*i) >> s) & m].e;
    }
    for (Bucket* k = b + 1; k != be; ++k) {
        k->e += (k - 1)->e - beg;
        k->b = (k - 1)->e;
    }
    for (Bucket* k = b; k != be; ) {
        if (k->b != k->e) {
            Bucket* l = b + ((key(*k->b) >> s) & m);
            if (l != k) {
                T tmp = *k->b;
                T sw;
                do {
                    sw = tmp;
                    tmp = *l->b;
                    *l->b++ = sw;
                    l = b + ((key(tmp) >> s) & m);
                } while (l != k);
                *k->b++ = tmp;
            } else {
                ++k->b;
            }
        } else {
            ++k;
        }
    }
    b->b = beg;
    for (Bucket* k = b + 1; k != be; ++k) {
        k->b = (k - 1)->e;
    }
    if (s != 0) {
        const int next_s = s > n_bits ? s - n_bits : 0;
        for (Bucket* k = b; k != be; ++k) {
            const std::ptrdiff_t span = k->e - k->b;
            if (span > static_cast<std::ptrdiff_t>(kRadixMinSize)) {
                rs_sort(k->b, k->e, n_bits, next_s, key);
            } else if (span > 1) {
                rs_insertsort(k->b, k->e, key);
            }
        }
    }
}

}  // namespace detail

/// Generic minimap2-style radix sort.
///
/// `key` returns an unsigned integer view of `T` (e.g. `[](const Anchor& a)
/// { return a.ref_bits; }`). The sort is in-place, unstable, and matches
/// minimap2's `KRADIX_SORT_INIT(name, T, key, sizeof_key)` byte-for-byte
/// in observable output (modulo the order of elements within each
/// equal-key bucket, which is unspecified for both implementations).
template <typename T, typename KeyFn>
void radix_sort(T* beg, T* end, KeyFn key) {
    using KeyT = std::decay_t<decltype(key(*beg))>;
    static_assert(std::is_unsigned_v<KeyT>,
                  "radix sort requires an unsigned key");
    constexpr int sizeof_key = sizeof(KeyT);
    if (end - beg <= static_cast<std::ptrdiff_t>(kRadixMinSize)) {
        detail::rs_insertsort(beg, end, key);
    } else {
        detail::rs_sort(beg, end, kRadixMaxBits,
                        (sizeof_key - 1) * static_cast<int>(kRadixMaxBits),
                        key);
    }
}

/// Specialised wrapper for sorting raw `uint64_t` ascending — equivalent
/// to minimap2's `radix_sort_64`.
inline void radix_sort_u64(std::uint64_t* beg, std::uint64_t* end) {
    radix_sort(beg, end, [](std::uint64_t v) { return v; });
}

}  // namespace bioseq::sorting
