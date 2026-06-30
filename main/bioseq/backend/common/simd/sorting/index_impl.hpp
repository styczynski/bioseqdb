HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::sorting {
namespace hn = hwy::HWY_NAMESPACE;

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

namespace detail {

namespace vs = ::bioseq::simd::HWY_NAMESPACE;

inline constexpr std::size_t u128_sort_scalar_fast_path_keys = 8;

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

template <class D, std::size_t StreamWidth>
using u128_vec_stream_t = vs::vec_stream<D, std::uint64_t, StreamWidth>;

template <bool Ascending, class D, std::size_t StreamWidth>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto u128_ordered_less_stream(
    D d,
    u128_vec_stream_t<D, StreamWidth> a,
    u128_vec_stream_t<D, StreamWidth> b) noexcept
{
    return [&]<std::size_t... S>(std::index_sequence<S...>)
        [[gnu::always_inline]] {
        if constexpr (Ascending) {
            return vs::mask_stream<D, StreamWidth>::pack(
                hn::Lt128(d, a[S], b[S])...);
        } else {
            return vs::mask_stream<D, StreamWidth>::pack(
                hn::Lt128(d, b[S], a[S])...);
        }
    }(std::make_index_sequence<StreamWidth>{});
}

template <class D, std::size_t StreamWidth>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto splat_u128_key_stream(D d, const hwy::uint128_t& key) noexcept {
    const auto v = load_u128_key(d, key);
    return vs::make_vec_stream<D, std::uint64_t, StreamWidth>(
        [&](auto) [[gnu::always_inline]] { return v; });
}

template <class D, std::size_t StreamWidth>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_u128_forward_stream(
    D d,
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::ptrdiff_t base) noexcept
{
    return vs::make_vec_stream<D, std::uint64_t, StreamWidth>(
        [&](auto s_const) [[gnu::always_inline]] {
            constexpr std::size_t s = decltype(s_const)::value;
            return load_u128_key(d, keys[static_cast<std::size_t>(base) + s]);
        });
}

template <class D, std::size_t StreamWidth>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto load_u128_reverse_stream(
    D d,
    const hwy::uint128_t* HWY_RESTRICT keys,
    std::ptrdiff_t base) noexcept
{
    return vs::make_vec_stream<D, std::uint64_t, StreamWidth>(
        [&](auto s_const) [[gnu::always_inline]] {
            constexpr std::size_t s = decltype(s_const)::value;
            const auto offset = static_cast<std::ptrdiff_t>(s);
            return load_u128_key(d, keys[static_cast<std::size_t>(base - offset)]);
        });
}

template <class D, std::size_t StreamWidth, class MaskStream>
[[nodiscard, gnu::always_inline]] HWY_INLINE
std::size_t first_false_u128_mask_slot(D d, const MaskStream& masks) noexcept {
    std::size_t first_false = StreamWidth;
    vs::for_each_stream<StreamWidth>([&](auto s_const) [[gnu::always_inline]] {
        constexpr std::size_t s = decltype(s_const)::value;
        if (first_false == StreamWidth && hn::AllFalse(d, masks[s])) {
            first_false = s;
        }
    });
    return first_false;
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
    const auto pivot_stream = splat_u128_key_stream<D, StreamWidth>(d, pivot);

    while (i <= j) {
        while (i <= j) {
            while (j - i + 1 >= static_cast<std::ptrdiff_t>(StreamWidth)) {
                const auto block = load_u128_forward_stream<D, StreamWidth>(d, keys, i);
                const auto less =
                    u128_ordered_less_stream<Ascending, D, StreamWidth>(
                        d, block, pivot_stream);
                const std::size_t first_false =
                    first_false_u128_mask_slot<D, StreamWidth>(d, less);
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
                const auto block = load_u128_reverse_stream<D, StreamWidth>(d, keys, j);
                const auto greater =
                    u128_ordered_less_stream<Ascending, D, StreamWidth>(
                        d, pivot_stream, block);
                const std::size_t first_false =
                    first_false_u128_mask_slot<D, StreamWidth>(d, greater);
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

}  // namespace detail

/// Default stream width for the u128 vec_stream sorter. Apple NEON-128 only
/// carries one 128-bit key per hardware vector, but two independent key
/// vectors fit the ABI/register-pressure envelope proven by the two-stack
/// minimizer collector.
inline constexpr std::size_t u128_sort_default_stream_width = 2;

/// Small arrays stay scalar to avoid repeating the earlier NEON short-sort
/// regression; larger arrays use vec_stream in partition scans.
inline constexpr std::size_t u128_sort_scalar_fast_path_keys =
    detail::u128_sort_scalar_fast_path_keys;

// ---------------------------------------------------------------------------
// uint64 / int64
// ---------------------------------------------------------------------------

/// Sort `n` `uint64_t` keys ascending. Drop-in for the
/// `coverage_intervals` site (packed `(start << 32) | end`).
inline void sort_u64_ascending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

/// Sort `n` `uint64_t` keys descending. Symmetric counterpart of
/// `sort_u64_ascending`.
inline void sort_u64_descending(std::uint64_t* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

// ---------------------------------------------------------------------------
// uint128
// ---------------------------------------------------------------------------

/// Legacy Highway VQSort u128 entry point, retained for explicit benchmark
/// comparisons.
inline void sort_u128_ascending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

inline void sort_u128_descending_vqsort(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

/// Sort `n` `uint128_t` keys ascending via a vec_stream-backed introsort.
/// `StreamWidth` controls how many independent 128-bit partition-scan
/// comparisons are issued as one stream bundle. The sort order is full
/// lexicographic `(hi, lo)`, matching `hwy::uint128_t::operator<`.
template <std::size_t StreamWidth = u128_sort_default_stream_width>
requires (::bioseq::simd::HWY_NAMESPACE::stream_width_ok<StreamWidth>)
inline void sort_u128_ascending_vec_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_u128_vec_stream<StreamWidth, true>(keys, key_count);
}

template <std::size_t StreamWidth = u128_sort_default_stream_width>
requires (::bioseq::simd::HWY_NAMESPACE::stream_width_ok<StreamWidth>)
inline void sort_u128_descending_vec_stream(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    detail::sort_u128_vec_stream<StreamWidth, false>(keys, key_count);
}

inline void sort_u128_ascending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_ascending_vec_stream<u128_sort_default_stream_width>(
        keys, key_count);
}

inline void sort_u128_descending(
    hwy::uint128_t* HWY_RESTRICT keys,
    std::size_t key_count)
{
    sort_u128_descending_vec_stream<u128_sort_default_stream_width>(
        keys, key_count);
}

// ---------------------------------------------------------------------------
// K32V32 (key in upper 32 bits of u64, value in lower 32 bits)
// ---------------------------------------------------------------------------

/// Sort `n` `K32V32` packed keys ascending. The caller is responsible for
/// packing `(key << 32) | value` and for sign-biasing signed keys by
/// `+0x80000000` so unsigned comparison matches signed semantics.
inline void sort_k32v32_ascending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

/// Sort `n` `K32V32` packed keys descending. Used for the `Chain` sort
/// site (sort by score descending).
inline void sort_k32v32_descending(hwy::K32V32* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

/// Top-`k` partial sort for `K32V32` descending. The first `k` entries after
/// the call are the `k` largest, ordered.
inline void partial_sort_k32v32_descending(
    hwy::K32V32* HWY_RESTRICT keys,
    std::size_t key_count,
    std::size_t k_keys)
{
    hwy::HWY_NAMESPACE::VQPartialSortStatic(
        keys, key_count, k_keys, hwy::SortDescending());
}

// ---------------------------------------------------------------------------
// K64V64 (value at offset 0, key at offset 8)
// ---------------------------------------------------------------------------

inline void sort_k64v64_ascending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortAscending());
}

inline void sort_k64v64_descending(hwy::K64V64* HWY_RESTRICT keys, std::size_t key_count) {
    hwy::HWY_NAMESPACE::VQSortStatic(keys, key_count, hwy::SortDescending());
}

}  // namespace bioseq::simd::sorting
HWY_AFTER_NAMESPACE();
