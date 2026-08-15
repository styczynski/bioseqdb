/**
 * @file highway_vector_stream.cppm
 * @brief Defines the registered Highway target source for fixed-count vector streams.
 *
 * The authored source remains target-neutral; the Highway module generator materializes
 * target-attributed copies while preserving the `vector_stream` source-record identity.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.vector_stream;

import std;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream_operations;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * @brief Accepts the supported compile-time number of independent SIMD streams.
 * @tparam p_stream_count Number of bundled vector or mask values.
 */
template <std::size_t p_stream_count>
concept supported_vector_stream_width = p_stream_count >= 1 && p_stream_count <= 4;

/**
 * @brief Forward declaration of a fixed-count SIMD vector bundle.
 * @tparam p_simd_tag Highway lane descriptor.
 * @tparam p_storage_type Scalar lane type.
 * @tparam p_stream_count Number of independent vectors.
 */
template <class p_simd_tag, class p_storage_type, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
struct vector_stream;

/**
 * @brief Forward declaration of a fixed-count SIMD mask bundle.
 * @tparam p_simd_tag Highway lane descriptor.
 * @tparam p_stream_count Number of independent masks.
 */
template <class p_simd_tag, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
struct mask_stream;

/** @brief Type trait recognizing `vector_stream` specializations. */
template <class p_value_type>
struct is_vector_stream : std::false_type {};

/** @brief Positive specialization for `vector_stream`. */
template <class p_simd_tag, class p_storage_type, std::size_t p_stream_count>
struct is_vector_stream<vector_stream<p_simd_tag, p_storage_type, p_stream_count>>
    : std::true_type {};

/** @brief Reports whether a type is a `vector_stream` specialization. */
template <class p_value_type>
inline constexpr bool k_is_vector_stream =
    is_vector_stream<std::remove_cvref_t<p_value_type>>::value;

/** @brief Type trait recognizing `mask_stream` specializations. */
template <class p_value_type>
struct is_mask_stream : std::false_type {};

/** @brief Positive specialization for `mask_stream`. */
template <class p_simd_tag, std::size_t p_stream_count>
struct is_mask_stream<mask_stream<p_simd_tag, p_stream_count>> : std::true_type {};

/** @brief Reports whether a type is a `mask_stream` specialization. */
template <class p_value_type>
inline constexpr bool k_is_mask_stream =
    is_mask_stream<std::remove_cvref_t<p_value_type>>::value;

/** @brief Reports whether a type is either supported stream bundle. */
template <class p_value_type>
inline constexpr bool k_is_any_stream =
    k_is_vector_stream<p_value_type> || k_is_mask_stream<p_value_type>;

/** @brief Accepts an argument pack containing at least one stream bundle. */
template <class... p_argument_types>
concept has_stream_argument =
    (k_is_any_stream<p_argument_types> || ... || false);

// ----------------------------------------------------------------------------
/**
 * @brief Bundles independent Highway vectors in target-attributed project callables.
 * @tparam p_simd_tag Highway lane descriptor.
 * @tparam p_storage_type Scalar lane type.
 * @tparam p_stream_count Number of independent vectors.
 */
template <class p_simd_tag, class p_storage_type, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
struct
#if defined(__clang__)
[[clang::trivial_abi]]
#endif
vector_stream {
    /** @brief Unqualified Highway lane descriptor. */
    using tag_type = std::remove_cvref_t<p_simd_tag>;

    /** @brief Scalar lane type carried for lifted result deduction. */
    using storage_type = p_storage_type;

    /** @brief Target-native Highway vector type. */
    using vector_type = hn::Vec<tag_type>;

    /** @brief Number of independent vectors in the bundle. */
    static constexpr std::size_t k_stream_count = p_stream_count;

private:
    // Target vectors provide their own alignment. Every copy remains in an attributed
    // project callable so fixed targets never cross a targetless std::array copy boundary.
    std::array<vector_type, p_stream_count> vectors_;

    template <class... p_vector_types>
    requires (
        sizeof...(p_vector_types) == p_stream_count
        && (std::is_convertible_v<p_vector_types, vector_type> && ...))
    explicit HWY_INLINE constexpr vector_stream(p_vector_types... vectors) noexcept
        : vectors_{static_cast<vector_type>(vectors)...} {}

    template <std::size_t... p_slot_indices>
    explicit HWY_INLINE constexpr vector_stream(
        std::index_sequence<p_slot_indices...>) noexcept
        : vectors_{((void)p_slot_indices, vector_type{})...} {}

    template <std::size_t... p_slot_indices>
    HWY_INLINE constexpr vector_stream(
        const vector_stream& other,
        std::index_sequence<p_slot_indices...>) noexcept
        : vectors_{other.vectors_[p_slot_indices]...} {}

    template <std::size_t... p_slot_indices>
    HWY_INLINE constexpr void assign_from(
        const vector_stream& other,
        std::index_sequence<p_slot_indices...>) noexcept {
        ((vectors_[p_slot_indices] = other.vectors_[p_slot_indices]), ...);
    }

public:
    /** @brief Default-constructs each target vector inside generated project code. */
    HWY_INLINE constexpr vector_stream() noexcept
        : vector_stream(std::make_index_sequence<p_stream_count>{}) {}

    // Array special members are not target-attributed, so every copy stays inside
    // generated project callables and avoids a targetless boundary for fixed targets.
    /** @brief Copies each target vector inside generated project code. */
    HWY_INLINE constexpr vector_stream(const vector_stream& other) noexcept
        : vector_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Moves each target vector through the target-attributed copy path. */
    HWY_INLINE constexpr vector_stream(vector_stream&& other) noexcept
        : vector_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copy-assigns each target vector inside generated project code. */
    HWY_INLINE constexpr vector_stream& operator=(
        const vector_stream& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Move-assigns each target vector through the target-attributed copy path. */
    HWY_INLINE constexpr vector_stream& operator=(vector_stream&& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Returns a mutable target vector slot. */
    [[nodiscard]] constexpr vector_type& operator[](std::size_t slot_index) noexcept {
        return vectors_[slot_index];
    }

    /** @brief Returns a const target vector slot. */
    [[nodiscard]] constexpr const vector_type& operator[](
        std::size_t slot_index) const noexcept {
        return vectors_[slot_index];
    }

    /** @brief Returns the mutable inline vector representation. */
    [[nodiscard]] constexpr std::array<vector_type, p_stream_count>& data() noexcept {
        return vectors_;
    }

    /** @brief Returns the const inline vector representation. */
    [[nodiscard]] constexpr const std::array<vector_type, p_stream_count>&
    data() const noexcept {
        return vectors_;
    }

    /** @brief Packs exactly one target vector per stream slot. */
    template <class... p_vector_types>
    requires (
        sizeof...(p_vector_types) == p_stream_count
        && (std::is_convertible_v<p_vector_types, vector_type> && ...))
    [[nodiscard]] static HWY_INLINE constexpr vector_stream pack(
        p_vector_types... vectors) noexcept {
        return vector_stream{static_cast<vector_type>(vectors)...};
    }

    /** @brief Replicates one scalar value across all lanes and stream slots. */
    [[nodiscard]] static HWY_INLINE vector_stream
    splat(tag_type simd_tag, storage_type value) noexcept {
        return splat_impl(
            hn::Set(simd_tag, value), std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Replicates zero across all lanes and stream slots. */
    [[nodiscard]] static HWY_INLINE vector_stream splat_zero(tag_type simd_tag) noexcept {
        return splat(simd_tag, storage_type{0});
    }

private:
    template <std::size_t... p_slot_indices>
    [[nodiscard]] static HWY_INLINE vector_stream splat_impl(
        vector_type vector,
        std::index_sequence<p_slot_indices...>) noexcept {
        return pack(((void)p_slot_indices, vector)...);
    }
public:
    // Small bundles intentionally pass by value. On M1 NEON they remain homogeneous
    // vector aggregates in V0/V1 registers, avoiding an address escape that can inhibit SROA.
    /** @brief Adds corresponding vectors in two bundles. */
    [[nodiscard]] friend HWY_INLINE auto operator+(
        vector_stream left, vector_stream right) noexcept {
        return add_slots(left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Subtracts corresponding vectors in two bundles. */
    [[nodiscard]] friend HWY_INLINE auto operator-(
        vector_stream left, vector_stream right) noexcept {
        return subtract_slots(left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Multiplies corresponding vectors in two bundles. */
    [[nodiscard]] friend HWY_INLINE auto operator*(
        vector_stream left, vector_stream right) noexcept {
        return multiply_slots(left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Compares corresponding vectors for equality. */
    [[nodiscard]] friend HWY_INLINE auto operator==(
        vector_stream left, vector_stream right) noexcept {
        return compare_equal_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Compares corresponding vectors for inequality. */
    [[nodiscard]] friend HWY_INLINE auto operator!=(
        vector_stream left, vector_stream right) noexcept {
        return compare_not_equal_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Compares corresponding vectors with less-than. */
    [[nodiscard]] friend HWY_INLINE auto operator<(
        vector_stream left, vector_stream right) noexcept {
        return compare_less_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Compares corresponding vectors with greater-than. */
    [[nodiscard]] friend HWY_INLINE auto operator>(
        vector_stream left, vector_stream right) noexcept {
        return compare_greater_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Compares corresponding vectors with less-than-or-equal. */
    [[nodiscard]] friend HWY_INLINE auto operator<=(
        vector_stream left, vector_stream right) noexcept {
        return compare_less_equal_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Compares corresponding vectors with greater-than-or-equal. */
    [[nodiscard]] friend HWY_INLINE auto operator>=(
        vector_stream left, vector_stream right) noexcept {
        return compare_greater_equal_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

private:
    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE vector_stream add_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return pack(hn::Add(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE vector_stream subtract_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return pack(hn::Sub(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE vector_stream multiply_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return pack(hn::Mul(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream<
        tag_type,
        p_stream_count>
    compare_equal_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return mask_stream<tag_type, p_stream_count>::pack(
            hn::Eq(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream<
        tag_type,
        p_stream_count>
    compare_not_equal_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return mask_stream<tag_type, p_stream_count>::pack(
            hn::Ne(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream<
        tag_type,
        p_stream_count>
    compare_less_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return mask_stream<tag_type, p_stream_count>::pack(
            hn::Lt(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream<
        tag_type,
        p_stream_count>
    compare_greater_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return mask_stream<tag_type, p_stream_count>::pack(
            hn::Gt(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream<
        tag_type,
        p_stream_count>
    compare_less_equal_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return mask_stream<tag_type, p_stream_count>::pack(
            hn::Le(left[p_slot_indices], right[p_slot_indices])...);
    }

    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream<
        tag_type,
        p_stream_count>
    compare_greater_equal_slots(
        vector_stream left,
        vector_stream right,
        std::index_sequence<p_slot_indices...>) noexcept {
        return mask_stream<tag_type, p_stream_count>::pack(
            hn::Ge(left[p_slot_indices], right[p_slot_indices])...);
    }
};

/**
 * @brief Bundles independent Highway masks in target-attributed project callables.
 * @tparam p_simd_tag Highway lane descriptor.
 * @tparam p_stream_count Number of independent masks.
 */
template <class p_simd_tag, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
struct
#if defined(__clang__)
[[clang::trivial_abi]]
#endif
mask_stream {
    /** @brief Unqualified Highway lane descriptor. */
    using tag_type = std::remove_cvref_t<p_simd_tag>;

    /** @brief Target-native Highway mask type. */
    using mask_type = hn::Mask<tag_type>;

    /** @brief Number of independent masks in the bundle. */
    static constexpr std::size_t k_stream_count = p_stream_count;

private:
    std::array<mask_type, p_stream_count> masks_;

    template <class... p_mask_types>
    requires (
        sizeof...(p_mask_types) == p_stream_count
        && (std::is_convertible_v<p_mask_types, mask_type> && ...))
    explicit HWY_INLINE constexpr mask_stream(p_mask_types... masks) noexcept
        : masks_{static_cast<mask_type>(masks)...} {}

    template <std::size_t... p_slot_indices>
    explicit HWY_INLINE constexpr mask_stream(
        std::index_sequence<p_slot_indices...>) noexcept
        : masks_{((void)p_slot_indices, mask_type{})...} {}

    template <std::size_t... p_slot_indices>
    HWY_INLINE constexpr mask_stream(
        const mask_stream& other,
        std::index_sequence<p_slot_indices...>) noexcept
        : masks_{other.masks_[p_slot_indices]...} {}

    template <std::size_t... p_slot_indices>
    HWY_INLINE constexpr void assign_from(
        const mask_stream& other,
        std::index_sequence<p_slot_indices...>) noexcept {
        ((masks_[p_slot_indices] = other.masks_[p_slot_indices]), ...);
    }

public:
    /** @brief Default-constructs each target mask inside generated project code. */
    HWY_INLINE constexpr mask_stream() noexcept
        : mask_stream(std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copies each target mask inside generated project code. */
    HWY_INLINE constexpr mask_stream(const mask_stream& other) noexcept
        : mask_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Moves each target mask through the target-attributed copy path. */
    HWY_INLINE constexpr mask_stream(mask_stream&& other) noexcept
        : mask_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copy-assigns each target mask inside generated project code. */
    HWY_INLINE constexpr mask_stream& operator=(const mask_stream& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Move-assigns each target mask through the target-attributed copy path. */
    HWY_INLINE constexpr mask_stream& operator=(mask_stream&& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Returns a mutable target mask slot. */
    [[nodiscard]] constexpr mask_type& operator[](std::size_t slot_index) noexcept {
        return masks_[slot_index];
    }

    /** @brief Returns a const target mask slot. */
    [[nodiscard]] constexpr const mask_type& operator[](
        std::size_t slot_index) const noexcept {
        return masks_[slot_index];
    }

    /** @brief Returns the mutable inline mask representation. */
    [[nodiscard]] constexpr std::array<mask_type, p_stream_count>& data() noexcept {
        return masks_;
    }

    /** @brief Returns the const inline mask representation. */
    [[nodiscard]] constexpr const std::array<mask_type, p_stream_count>&
    data() const noexcept {
        return masks_;
    }

    /** @brief Packs exactly one target mask per stream slot. */
    template <class... p_mask_types>
    requires (
        sizeof...(p_mask_types) == p_stream_count
        && (std::is_convertible_v<p_mask_types, mask_type> && ...))
    [[nodiscard]] static HWY_INLINE constexpr mask_stream pack(
        p_mask_types... masks) noexcept {
        return mask_stream{static_cast<mask_type>(masks)...};
    }

    /** @brief Replicates a false mask across all stream slots. */
    [[nodiscard]] static HWY_INLINE mask_stream splat_false(tag_type simd_tag) noexcept {
        return splat_false_impl(
            hn::MaskFalse(simd_tag), std::make_index_sequence<p_stream_count>{});
    }

private:
    template <std::size_t... p_slot_indices>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream
    splat_false_impl(
        mask_type mask,
        std::index_sequence<p_slot_indices...>) noexcept {
        return pack(((void)p_slot_indices, mask)...);
    }
};

/** @brief Returns a stream bundle type's compile-time slot count. */
template <class p_stream_type>
inline constexpr std::size_t k_stream_count =
    std::remove_cvref_t<p_stream_type>::k_stream_count;

/** @brief Names a stream bundle type's unqualified Highway tag. */
template <class p_stream_type>
using stream_tag_type = typename std::remove_cvref_t<p_stream_type>::tag_type;

/** @brief Names a vector stream's scalar lane type. */
template <class p_stream_type>
using stream_storage_type =
    typename std::remove_cvref_t<p_stream_type>::storage_type;

/** @brief Accesses one compile-time stream slot while preserving value category. */
template <std::size_t p_slot_index, class p_stream_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE
constexpr decltype(auto) get_stream_slot(p_stream_type&& stream) noexcept {
    static_assert(
        p_slot_index < k_stream_count<p_stream_type>,
        "stream slot index out of bounds");
    return std::forward<p_stream_type>(stream)[p_slot_index];
}

/** @brief Packs target vectors through the generated source's stable factory surface. */
template <
    class p_simd_tag,
    class p_storage_type,
    std::size_t p_stream_count,
    class... p_vector_types>
requires (
    supported_vector_stream_width<p_stream_count>
    && sizeof...(p_vector_types) == p_stream_count)
[[nodiscard, gnu::always_inline]] HWY_INLINE constexpr
vector_stream<p_simd_tag, p_storage_type, p_stream_count> pack_vector_stream(
    p_simd_tag simd_tag,
    p_vector_types... vectors) noexcept {
    (void)simd_tag;
    return vector_stream<p_simd_tag, p_storage_type, p_stream_count>::pack(vectors...);
}

/** @brief Packs target masks through the generated source's stable factory surface. */
template <class p_simd_tag, std::size_t p_stream_count, class... p_mask_types>
requires (
    supported_vector_stream_width<p_stream_count>
    && sizeof...(p_mask_types) == p_stream_count)
[[nodiscard, gnu::always_inline]] HWY_INLINE constexpr
mask_stream<p_simd_tag, p_stream_count> pack_mask_stream(
    p_simd_tag simd_tag,
    p_mask_types... masks) noexcept {
    (void)simd_tag;
    return mask_stream<p_simd_tag, p_stream_count>::pack(masks...);
}

/** @brief Broadcasts one scalar across every vector stream slot. */
template <class p_simd_tag, class p_storage_type, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
[[nodiscard, gnu::always_inline]] HWY_INLINE
vector_stream<p_simd_tag, p_storage_type, p_stream_count> splat_vector_stream(
    p_simd_tag simd_tag,
    p_storage_type value) noexcept {
    return vector_stream<p_simd_tag, p_storage_type, p_stream_count>::splat(
        simd_tag, value);
}

/** @brief Broadcasts a false mask across every mask stream slot. */
template <class p_simd_tag, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
[[nodiscard, gnu::always_inline]] HWY_INLINE
mask_stream<p_simd_tag, p_stream_count> splat_false_mask_stream(
    p_simd_tag simd_tag) noexcept {
    return mask_stream<p_simd_tag, p_stream_count>::splat_false(simd_tag);
}

// ============================================================================
// Lifting machinery
// ============================================================================

namespace vector_stream_detail {

/**
 * @brief Projects one stream slot or broadcasts a non-stream argument.
 *
 * @tparam p_slot_index Compile-time stream slot.
 * @tparam p_argument_type Projected argument type.
 * @param[in] argument Stream bundle or broadcast argument.
 * @return Selected slot reference for streams, otherwise the original lvalue reference.
 */
template <std::size_t p_slot_index, class p_argument_type>
[[gnu::always_inline]] HWY_INLINE constexpr decltype(auto)
project_stream_slot(p_argument_type& argument) {
    if constexpr (k_is_any_stream<p_argument_type>) {
        return get_stream_slot<p_slot_index>(argument);
    } else {
        return (argument);
    }
}

/** @brief Maps a non-stream type to a zero stream count. */
template <class p_value_type>
inline constexpr std::size_t k_stream_count_for_type = 0;

/** @brief Maps a stream type to its compile-time stream count. */
template <class p_value_type>
requires k_is_any_stream<p_value_type>
inline constexpr std::size_t k_stream_count_for_type<p_value_type> =
    std::remove_cvref_t<p_value_type>::k_stream_count;

/** @brief Deduces the greatest stream count in an argument pack. */
template <class... p_argument_types>
struct deduced_stream_count;

/** @brief Empty packs contain no streams. */
template <>
struct deduced_stream_count<> {
    static constexpr std::size_t k_value = 0;
};

/** @brief Recursively selects the greatest stream argument count. */
template <class p_first_type, class... p_remaining_types>
struct deduced_stream_count<p_first_type, p_remaining_types...> {
    static constexpr std::size_t k_tail_value =
        deduced_stream_count<p_remaining_types...>::k_value;
    static constexpr std::size_t k_value =
        k_stream_count_for_type<p_first_type> > k_tail_value
            ? k_stream_count_for_type<p_first_type>
            : k_tail_value;
};

/** @brief Exposes the deduced stream count for an argument pack. */
template <class... p_argument_types>
inline constexpr std::size_t k_deduced_stream_count =
    deduced_stream_count<std::remove_cvref_t<p_argument_types>...>::k_value;

/** @brief Maps non-stream types to `void` for tag deduction. */
template <class p_value_type>
struct simd_tag_for_type {
    using simd_tag_type = void;
};

/** @brief Maps stream types to their unqualified Highway tag. */
template <class p_value_type>
requires k_is_any_stream<p_value_type>
struct simd_tag_for_type<p_value_type> {
    using simd_tag_type = typename std::remove_cvref_t<p_value_type>::tag_type;
};

/** @brief Semantic alias for a candidate type's deduced SIMD tag. */
template <class p_value_type>
using simd_tag_type_for = typename simd_tag_for_type<p_value_type>::simd_tag_type;

/** @brief Deduces the first stream tag in an argument pack. */
template <class... p_argument_types>
struct deduced_simd_tag;

template <>
struct deduced_simd_tag<> {
    using simd_tag_type = void;
};

template <class p_first_type, class... p_remaining_types>
requires k_is_any_stream<p_first_type>
struct deduced_simd_tag<p_first_type, p_remaining_types...> {
    using simd_tag_type = typename std::remove_cvref_t<p_first_type>::tag_type;
};

template <class p_first_type, class... p_remaining_types>
requires (!k_is_any_stream<p_first_type>)
struct deduced_simd_tag<p_first_type, p_remaining_types...>
    : deduced_simd_tag<p_remaining_types...> {};

/** @brief Deduces the first vector stream's storage type. */
template <class... p_argument_types>
struct deduced_storage_type;

template <>
struct deduced_storage_type<> {
    using storage_type = void;
};

template <class p_first_type, class... p_remaining_types>
requires k_is_vector_stream<p_first_type>
struct deduced_storage_type<p_first_type, p_remaining_types...> {
    using storage_type = typename std::remove_cvref_t<p_first_type>::storage_type;
};

template <class p_first_type, class... p_remaining_types>
requires (!k_is_vector_stream<p_first_type>)
struct deduced_storage_type<p_first_type, p_remaining_types...>
    : deduced_storage_type<p_remaining_types...> {};

/** @brief Reports whether all stream arguments have the same stream count. */
template <class... p_argument_types>
inline constexpr bool k_stream_counts_consistent =
    ((!k_is_any_stream<p_argument_types>
      || k_stream_count_for_type<p_argument_types>
          == k_deduced_stream_count<p_argument_types...>)
     && ... && true);

/** @brief Reports whether all stream arguments have the same Highway tag. */
template <class... p_argument_types>
inline constexpr bool k_stream_tags_consistent =
    ((!k_is_any_stream<p_argument_types>
      || std::is_same_v<
          simd_tag_type_for<p_argument_types>,
          typename deduced_simd_tag<
              std::remove_cvref_t<p_argument_types>...>::simd_tag_type>)
     && ... && true);

/**
 * @brief Evaluates a lifted operation for one stream slot.
 * @tparam p_slot_index Compile-time stream slot.
 * @tparam p_operation_type Lifted callable type.
 * @tparam p_argument_types Operation argument types.
 * @param[in] operation Lifted callable.
 * @param[in] arguments Stream or broadcast arguments.
 * @return Per-slot operation result.
 */
template <std::size_t p_slot_index, class p_operation_type, class... p_argument_types>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto evaluate_lifted_slot(
    p_operation_type& operation,
    p_argument_types&&... arguments) {
    return operation(project_stream_slot<p_slot_index>(arguments)...);
}

/** @brief Applies a void lifted operation for one stream slot. */
template <std::size_t p_slot_index, class p_operation_type, class... p_argument_types>
[[gnu::always_inline]] HWY_INLINE void apply_lifted_slot(
    p_operation_type& operation,
    p_argument_types&&... arguments) {
    operation(project_stream_slot<p_slot_index>(arguments)...);
}

/** @brief Packs per-slot vector results from a lifted operation. */
template <
    class p_simd_tag,
    class p_storage_type,
    std::size_t p_stream_count,
    std::size_t... p_slot_indices,
    class p_operation_type,
    class... p_argument_types>
[[nodiscard, gnu::always_inline]] HWY_INLINE
vector_stream<p_simd_tag, p_storage_type, p_stream_count> pack_lifted_vectors(
    std::index_sequence<p_slot_indices...>,
    p_operation_type& operation,
    p_argument_types&&... arguments) {
    return pack_vector_stream<p_simd_tag, p_storage_type, p_stream_count>(
        p_simd_tag{},
        evaluate_lifted_slot<p_slot_indices>(
            operation, std::forward<p_argument_types>(arguments)...)...);
}

/** @brief Packs per-slot mask results from a lifted operation. */
template <
    class p_simd_tag,
    std::size_t p_stream_count,
    std::size_t... p_slot_indices,
    class p_operation_type,
    class... p_argument_types>
[[nodiscard, gnu::always_inline]] HWY_INLINE
mask_stream<p_simd_tag, p_stream_count> pack_lifted_masks(
    std::index_sequence<p_slot_indices...>,
    p_operation_type& operation,
    p_argument_types&&... arguments) {
    return pack_mask_stream<p_simd_tag, p_stream_count>(
        p_simd_tag{},
        evaluate_lifted_slot<p_slot_indices>(
            operation, std::forward<p_argument_types>(arguments)...)...);
}

/** @brief Applies a void lifted operation to every stream slot. */
template <
    std::size_t... p_slot_indices,
    class p_operation_type,
    class... p_argument_types>
[[gnu::always_inline]] HWY_INLINE void apply_lifted_operation(
    std::index_sequence<p_slot_indices...>,
    p_operation_type& operation,
    p_argument_types&&... arguments) {
    (apply_lifted_slot<p_slot_indices>(
         operation, std::forward<p_argument_types>(arguments)...),
     ...);
}

/**
 * @brief Lifts one Highway operation over every slot in compatible stream arguments.
 *
 * The element result selects a vector stream, mask stream, or void application. Non-stream
 * arguments are broadcast as lvalues to every slot so rvalues are never moved more than once.
 *
 * @tparam p_operation_type Lifted callable type.
 * @tparam p_argument_types Stream and broadcast argument types.
 * @param[in] operation Lifted callable.
 * @param[in] arguments Operation arguments.
 * @return Bundled per-slot result for non-void operations.
 */
template <class p_operation_type, class... p_argument_types>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto lift_operation(p_operation_type&& operation, p_argument_types&&... arguments) {
    static_assert(
        has_stream_argument<p_argument_types...>,
        "lift_operation requires at least one stream argument");
    static_assert(
        k_stream_counts_consistent<p_argument_types...>,
        "lift_operation requires equal stream counts");
    static_assert(
        k_stream_tags_consistent<p_argument_types...>,
        "lift_operation requires equal SIMD tags");

    constexpr std::size_t stream_count =
        k_deduced_stream_count<p_argument_types...>;
    using simd_tag_type = typename deduced_simd_tag<
        std::remove_cvref_t<p_argument_types>...>::simd_tag_type;
    static_assert(!std::is_void_v<simd_tag_type>, "SIMD tag deduction failed");

    using element_result_type = std::remove_cvref_t<decltype(
        operation(project_stream_slot<0>(arguments)...))>;

    if constexpr (std::is_same_v<element_result_type, hn::Vec<simd_tag_type>>) {
        using argument_storage_type = typename deduced_storage_type<
            std::remove_cvref_t<p_argument_types>...>::storage_type;
        using result_storage_type = std::conditional_t<
            std::is_void_v<argument_storage_type>,
            hn::TFromD<simd_tag_type>,
            argument_storage_type>;
        return pack_lifted_vectors<simd_tag_type, result_storage_type, stream_count>(
            std::make_index_sequence<stream_count>{},
            operation,
            std::forward<p_argument_types>(arguments)...);
    } else if constexpr (std::is_same_v<element_result_type, hn::Mask<simd_tag_type>>) {
        return pack_lifted_masks<simd_tag_type, stream_count>(
            std::make_index_sequence<stream_count>{},
            operation,
            std::forward<p_argument_types>(arguments)...);
    } else {
        static_assert(
            std::is_same_v<element_result_type, void>,
            "lift_operation result must be a vector, mask, or void");
        apply_lifted_operation(
            std::make_index_sequence<stream_count>{},
            operation,
            std::forward<p_argument_types>(arguments)...);
    }
}

} // namespace vector_stream_detail

/**
 * @brief Applies a registered operation to compatible stream arguments.
 * @tparam p_operation_type Operation adapter type.
 * @tparam p_argument_types Stream and broadcast argument types.
 * @param[in] arguments Operation arguments.
 * @return Bundled operation result.
 */
template <class p_operation_type, class... p_argument_types>
requires has_stream_argument<p_argument_types...>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto apply_lifted(p_argument_types&&... arguments) {
    return vector_stream_detail::lift_operation(
        []<class... p_inner_argument_types>(
            p_inner_argument_types&&... inner_arguments) [[gnu::always_inline]] {
            return p_operation_type::apply(
                std::forward<p_inner_argument_types>(inner_arguments)...);
        },
        std::forward<p_argument_types>(arguments)...);
}

// Wrappers remain non-constexpr because Highway operations are target callables.
// These bodies are likewise source-spelled for structural target attribution.

// Binary lifts — both vec/vec and mask/mask are handled here; the constraint
// Each wrapper accepts either vector or mask streams as supported by its Highway operation.
/** @brief Adds corresponding lanes across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto add(p_left_type&& left, p_right_type&& right) {
    return apply_lifted<vector_stream_detail::add_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Subtracts corresponding lanes across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto subtract(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::subtract_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Multiplies corresponding lanes across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto multiply(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::multiply_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Computes lane-wise minima across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto minimum(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::minimum_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Computes lane-wise maxima across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto maximum(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::maximum_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Computes lane-wise bitwise AND across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto bit_and(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::bit_and_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Computes lane-wise bitwise OR across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto bit_or(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::bit_or_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Computes lane-wise bitwise XOR across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto bit_xor(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::bit_xor_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Computes lane-wise bitwise AND-NOT across compatible stream arguments. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto bit_and_not(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::bit_and_not_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Compares corresponding lanes for equality. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto equal(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::equal_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Compares corresponding lanes for inequality. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto not_equal(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::not_equal_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Compares corresponding lanes with less-than. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto less(p_left_type&& left, p_right_type&& right) {
    return apply_lifted<vector_stream_detail::less_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Compares corresponding lanes with greater-than. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto greater(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::greater_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Compares corresponding lanes with less-than-or-equal. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto less_equal(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::less_equal_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Compares corresponding lanes with greater-than-or-equal. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto greater_equal(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::greater_equal_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Shifts lanes left by per-lane counts across stream slots. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto shift_left_same(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::shift_left_same_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Shifts lanes right by per-lane counts across stream slots. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto shift_right_same(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::shift_right_same_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

// Unary lifts.
/** @brief Complements every lane in a stream argument. */
template <class p_input_type>
requires k_is_any_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto bit_not(p_input_type&& input) {
    return apply_lifted<vector_stream_detail::bit_not_operation>(
        std::forward<p_input_type>(input));
}

/** @brief Negates every lane in a stream argument. */
template <class p_input_type>
requires k_is_any_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto negate(p_input_type&& input) {
    return apply_lifted<vector_stream_detail::negate_operation>(
        std::forward<p_input_type>(input));
}

/** @brief Computes the absolute value of every stream lane. */
template <class p_input_type>
requires k_is_any_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto absolute(p_input_type&& input) {
    return apply_lifted<vector_stream_detail::absolute_operation>(
        std::forward<p_input_type>(input));
}

/** @brief Converts every vector slot to a mask slot. */
template <class p_input_type>
requires k_is_any_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto mask_from_vector(p_input_type&& input) {
    return apply_lifted<vector_stream_detail::mask_from_vector_operation>(
        std::forward<p_input_type>(input));
}

// Ternary lifts (full {stream, scalar}^3 = 27 cases handled in one wrapper).
/** @brief Selects lanes conditionally across compatible stream arguments. */
template <class p_first_type, class p_second_type, class p_third_type>
requires has_stream_argument<p_first_type, p_second_type, p_third_type>
[[nodiscard]] HWY_INLINE auto if_then_else(
    p_first_type&& first,
    p_second_type&& second,
    p_third_type&& third) {
    return apply_lifted<vector_stream_detail::if_then_else_operation>(
        std::forward<p_first_type>(first),
        std::forward<p_second_type>(second),
        std::forward<p_third_type>(third));
}

/** @brief Applies true-or-zero conditional selection across stream slots. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto if_then_else_zero(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::if_then_else_zero_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Applies zero-or-false conditional selection across stream slots. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto if_then_zero_else(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::if_then_zero_else_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

// ============================================================================
// Templated shifts — non-type template parameter, written long-hand.
// ============================================================================

/** @brief Shifts every vector lane left by a compile-time bit count. */
template <int p_bit_count, class p_input_type>
requires k_is_vector_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto shift_left(p_input_type&& input) {
    return vector_stream_detail::lift_operation(
        [](auto&& value) [[gnu::always_inline]] {
            return hn::ShiftLeft<p_bit_count>(value);
        },
        std::forward<p_input_type>(input));
}

/** @brief Shifts every vector lane right by a compile-time bit count. */
template <int p_bit_count, class p_input_type>
requires k_is_vector_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto shift_right(p_input_type&& input) {
    return vector_stream_detail::lift_operation(
        [](auto&& value) [[gnu::always_inline]] {
            return hn::ShiftRight<p_bit_count>(value);
        },
        std::forward<p_input_type>(input));
}

/** @brief Rotates every vector lane left by a compile-time bit count. */
template <int p_bit_count, class p_input_type>
requires k_is_vector_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto rotate_left(p_input_type&& input) {
    return vector_stream_detail::lift_operation(
        [](auto&& value) [[gnu::always_inline]] {
            return hn::RotateLeft<p_bit_count>(value);
        },
        std::forward<p_input_type>(input));
}

/** @brief Rotates every vector lane right by a compile-time bit count. */
template <int p_bit_count, class p_input_type>
requires k_is_vector_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto rotate_right(p_input_type&& input) {
    return vector_stream_detail::lift_operation(
        [](auto&& value) [[gnu::always_inline]] {
            return hn::RotateRight<p_bit_count>(value);
        },
        std::forward<p_input_type>(input));
}

namespace vector_stream_bit_cast_detail {

/** @brief Bit-casts one vector stream slot to a destination tag. */
template <class p_destination_tag, class p_input_type, std::size_t p_slot_index>
[[nodiscard, gnu::always_inline]] HWY_INLINE auto bit_cast_slot(
    p_destination_tag destination_tag,
    const p_input_type& input) noexcept {
    return hn::BitCast(destination_tag, get_stream_slot<p_slot_index>(input));
}

/** @brief Packs bit-cast vectors for every stream slot. */
template <
    class p_destination_tag,
    class p_destination_storage_type,
    std::size_t p_stream_count,
    class p_input_type,
    std::size_t... p_slot_indices>
[[nodiscard, gnu::always_inline]] HWY_INLINE
vector_stream<p_destination_tag, p_destination_storage_type, p_stream_count>
pack_bit_cast_slots(
    p_destination_tag destination_tag,
    const p_input_type& input,
    std::index_sequence<p_slot_indices...>) noexcept {
    return pack_vector_stream<
        p_destination_tag,
        p_destination_storage_type,
        p_stream_count>(
        destination_tag,
        bit_cast_slot<p_destination_tag, p_input_type, p_slot_indices>(
            destination_tag, input)...);
}

} // namespace vector_stream_bit_cast_detail

/**
 * @brief Bit-casts every vector slot to a destination Highway tag.
 * @tparam p_destination_tag Destination lane descriptor.
 * @tparam p_input_type Source vector stream type.
 * @param[in] destination_tag Destination lane descriptor.
 * @param[in] input Source vector stream.
 * @return Bit-cast destination vector stream.
 */
template <class p_destination_tag, class p_input_type>
requires k_is_vector_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto bit_cast(
    p_destination_tag destination_tag,
    p_input_type&& input) {
    using input_type = std::remove_cvref_t<p_input_type>;
    constexpr std::size_t stream_count = input_type::k_stream_count;
    using destination_storage_type = hn::TFromD<p_destination_tag>;
    return vector_stream_bit_cast_detail::pack_bit_cast_slots<
        p_destination_tag,
        destination_storage_type,
        stream_count>(
        destination_tag, input, std::make_index_sequence<stream_count>{});
}

/** @brief Converts every mask slot to a target-native vector slot. */
template <class p_simd_tag, class p_input_type>
requires k_is_mask_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto vector_from_mask(
    p_simd_tag simd_tag,
    p_input_type&& input) {
    using input_type = std::remove_cvref_t<p_input_type>;
    static_assert(
        std::is_same_v<typename input_type::tag_type, p_simd_tag>,
        "vector_from_mask requires matching SIMD tags");
    return vector_stream_detail::lift_operation(
        [simd_tag](auto&& mask) [[gnu::always_inline]] {
            return hn::VecFromMask(simd_tag, mask);
        },
        std::forward<p_input_type>(input));
}

/** @brief Stores each vector slot to its corresponding aligned base pointer. */
template <class p_simd_tag, class p_input_type, class p_storage_type>
requires k_is_vector_stream<p_input_type>
HWY_INLINE void store_stream(
    const p_input_type& input,
    p_simd_tag simd_tag,
    std::array<
        p_storage_type*,
        std::remove_cvref_t<p_input_type>::k_stream_count> base_pointers) {
    using input_type = std::remove_cvref_t<p_input_type>;
    static_assert(
        std::is_same_v<typename input_type::tag_type, p_simd_tag>,
        "store_stream requires matching SIMD tags");
    for_each_index<input_type::k_stream_count>(
        [&input, simd_tag, &base_pointers](auto slot_constant) [[gnu::always_inline]] {
            constexpr std::size_t slot_index = decltype(slot_constant)::value;
            hn::Store(get_stream_slot<slot_index>(input), simd_tag, base_pointers[slot_index]);
        });
}

/** @brief Stores each vector slot to its corresponding unaligned base pointer. */
template <class p_simd_tag, class p_input_type, class p_storage_type>
requires k_is_vector_stream<p_input_type>
HWY_INLINE void store_unaligned_stream(
    const p_input_type& input,
    p_simd_tag simd_tag,
    std::array<
        p_storage_type*,
        std::remove_cvref_t<p_input_type>::k_stream_count> base_pointers) {
    using input_type = std::remove_cvref_t<p_input_type>;
    static_assert(
        std::is_same_v<typename input_type::tag_type, p_simd_tag>,
        "store_unaligned_stream requires matching SIMD tags");
    for_each_index<input_type::k_stream_count>(
        [&input, simd_tag, &base_pointers](auto slot_constant) [[gnu::always_inline]] {
            constexpr std::size_t slot_index = decltype(slot_constant)::value;
            hn::StoreU(get_stream_slot<slot_index>(input), simd_tag, base_pointers[slot_index]);
        });
}

/**
 * @brief Reduces each mask slot independently with Highway `AllFalse`.
 * @return One boolean per stream slot; cross-stream reduction remains caller-owned.
 */
template <class p_simd_tag, class p_input_type>
requires k_is_mask_stream<p_input_type>
[[nodiscard]] HWY_INLINE auto collect_all_false_per_stream(
    p_simd_tag simd_tag,
    const p_input_type& input)
    -> std::array<bool, std::remove_cvref_t<p_input_type>::k_stream_count> {
    using input_type = std::remove_cvref_t<p_input_type>;
    static_assert(
        std::is_same_v<typename input_type::tag_type, p_simd_tag>,
        "collect_all_false_per_stream requires matching SIMD tags");
    std::array<bool, input_type::k_stream_count> reductions{};
    for_each_index<input_type::k_stream_count>(
        [simd_tag, &input, &reductions](auto slot_constant) [[gnu::always_inline]] {
            constexpr std::size_t slot_index = decltype(slot_constant)::value;
            reductions[slot_index] = hn::AllFalse(
                simd_tag, get_stream_slot<slot_index>(input));
        });
    return reductions;
}

/** @brief Invokes a callable once for every compile-time stream slot. */
template <std::size_t p_stream_count, class p_function_type>
[[gnu::always_inline]] HWY_INLINE constexpr void for_each_stream(
    p_function_type&& function) {
    for_each_index<p_stream_count>(std::forward<p_function_type>(function));
}

/** @brief Builds a vector stream by invoking one factory per compile-time slot. */
template <
    class p_simd_tag,
    class p_storage_type,
    std::size_t p_stream_count,
    class p_factory_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE auto make_vector_stream(
    p_factory_type&& factory) noexcept {
    return [&factory]<std::size_t... p_slot_indices>(
               std::index_sequence<p_slot_indices...>) [[gnu::always_inline]] {
        return pack_vector_stream<p_simd_tag, p_storage_type, p_stream_count>(
        p_simd_tag{},
            factory(std::integral_constant<std::size_t, p_slot_indices>{})...);
    }(std::make_index_sequence<p_stream_count>{});
}

/** @brief Packs one matrix row from every stream into a vector bundle. */
template <
    class p_simd_tag,
    class p_storage_type,
    std::size_t p_stream_count,
    class p_matrix_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE auto make_vector_stream_from_rows(
    const p_matrix_type& matrices,
    std::size_t row_index) noexcept {
    return make_vector_stream<p_simd_tag, p_storage_type, p_stream_count>(
        [&matrices, row_index](auto slot_constant) [[gnu::always_inline]] {
            return matrices[decltype(slot_constant)::value][row_index];
        });
}


} // namespace target
} // namespace bioseq::simd
