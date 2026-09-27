/**
 * @file highway_vector_stream.cppm
 * @brief Applies SIMD operations to small groups of independent vectors.
 *
 * The Highway generator compiles a copy for each CPU target and keeps
 * the vector_stream name in its generated source records.
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
 * @brief Allows groups of one to four SIMD vectors or masks.
 */
template <std::size_t p_stream_count>
concept supported_vector_stream_width = p_stream_count >= 1 && p_stream_count <= 4;

/**
 * @brief Declares a group of SIMD vectors with a fixed size.
 */
template <class p_simd_tag, class p_storage_type, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
struct vector_stream;

/**
 * @brief Declares a group of SIMD masks with a fixed size.
 */
template <class p_simd_tag, std::size_t p_stream_count>
requires supported_vector_stream_width<p_stream_count>
struct mask_stream;

/** @brief Type trait recognizing `vector_stream` specializations. */
template <class p_value_type>
struct is_vector_stream : std::false_type {};

/** @brief Recognizes vector_stream types. */
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

/** @brief Recognizes mask_stream types. */
template <class p_simd_tag, std::size_t p_stream_count>
struct is_mask_stream<mask_stream<p_simd_tag, p_stream_count>> : std::true_type {};

/** @brief Reports whether a type is a `mask_stream` specialization. */
template <class p_value_type>
inline constexpr bool k_is_mask_stream =
    is_mask_stream<std::remove_cvref_t<p_value_type>>::value;

/** @brief Checks whether a type is a vector stream or mask stream. */
template <class p_value_type>
inline constexpr bool k_is_any_stream =
    k_is_vector_stream<p_value_type> || k_is_mask_stream<p_value_type>;

/** @brief Requires at least one stream in the argument list. */
template <class... p_argument_types>
concept has_stream_argument =
    (k_is_any_stream<p_argument_types> || ... || false);


/**
 * @brief Holds independent SIMD vectors so one operation can process each in turn.
 *
 * The group contains one to four vectors with the same lane type and width.
 * @tparam p_simd_tag Highway lane tag.
 * @tparam p_storage_type Scalar lane type.
 * @tparam p_stream_count Number of vectors.
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

    /** @brief Scalar lane type used to choose result types. */
    using storage_type = p_storage_type;

    /** @brief Highway vector type for this CPU target. */
    using vector_type = hn::Vec<tag_type>;

    /** @brief Number of independent vectors in the bundle. */
    static constexpr std::size_t k_stream_count = p_stream_count;

private:
    // Highway vectors supply their own alignment. Copies are written here
    // so the generator can give them the same CPU target as the vectors.
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
    /** @brief Default-constructs each vector using code compiled for this CPU target. */
    HWY_INLINE constexpr vector_stream() noexcept
        : vector_stream(std::make_index_sequence<p_stream_count>{}) {}

    // Copy elements explicitly because std::array copy functions do not
    // carry the CPU target attributes added to this module.
    /** @brief Copies all vector slots. */
    HWY_INLINE constexpr vector_stream(const vector_stream& other) noexcept
        : vector_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copies the vector slots from the source being moved. */
    HWY_INLINE constexpr vector_stream(vector_stream&& other) noexcept
        : vector_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copies all vector slots into this stream. */
    HWY_INLINE constexpr vector_stream& operator=(
        const vector_stream& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Copies vector slots from the source being moved into this stream. */
    HWY_INLINE constexpr vector_stream& operator=(vector_stream&& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Returns a mutable vector slot without checking its index. */
    [[nodiscard]] constexpr vector_type& operator[](std::size_t slot_index) noexcept {
        return vectors_[slot_index];
    }

    /** @brief Returns a read-only vector slot without checking its index. */
    [[nodiscard]] constexpr const vector_type& operator[](
        std::size_t slot_index) const noexcept {
        return vectors_[slot_index];
    }

    /** @brief Returns the array of vector slots. */
    [[nodiscard]] constexpr std::array<vector_type, p_stream_count>& data() noexcept {
        return vectors_;
    }

    /** @brief Returns the array of vector slots as read-only storage. */
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
    // Pass small groups by value so the compiler can keep their vectors
    // in registers without taking the group's address.
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

    /** @brief Returns equality masks for corresponding vector lanes. */
    [[nodiscard]] friend HWY_INLINE auto operator==(
        vector_stream left, vector_stream right) noexcept {
        return compare_equal_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Returns inequality masks for corresponding vector lanes. */
    [[nodiscard]] friend HWY_INLINE auto operator!=(
        vector_stream left, vector_stream right) noexcept {
        return compare_not_equal_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Returns masks for lanes where left is less than right. */
    [[nodiscard]] friend HWY_INLINE auto operator<(
        vector_stream left, vector_stream right) noexcept {
        return compare_less_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Returns masks for lanes where left is greater than right. */
    [[nodiscard]] friend HWY_INLINE auto operator>(
        vector_stream left, vector_stream right) noexcept {
        return compare_greater_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Returns masks for lanes where left is at most right. */
    [[nodiscard]] friend HWY_INLINE auto operator<=(
        vector_stream left, vector_stream right) noexcept {
        return compare_less_equal_slots(
            left, right, std::make_index_sequence<p_stream_count>{});
    }

    /** @brief Returns masks for lanes where left is at least right. */
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
 * @brief Holds one SIMD mask for each vector in a stream.
 * @tparam p_simd_tag Highway lane tag.
 * @tparam p_stream_count Number of masks, from one to four.
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

    /** @brief Highway mask type for this CPU target. */
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
    /** @brief Default-constructs each mask using code compiled for this CPU target. */
    HWY_INLINE constexpr mask_stream() noexcept
        : mask_stream(std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copies all mask slots. */
    HWY_INLINE constexpr mask_stream(const mask_stream& other) noexcept
        : mask_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copies the mask slots from the source being moved. */
    HWY_INLINE constexpr mask_stream(mask_stream&& other) noexcept
        : mask_stream(other, std::make_index_sequence<p_stream_count>{}) {}

    /** @brief Copies all mask slots into this stream. */
    HWY_INLINE constexpr mask_stream& operator=(const mask_stream& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Copies mask slots from the source being moved into this stream. */
    HWY_INLINE constexpr mask_stream& operator=(mask_stream&& other) noexcept {
        assign_from(other, std::make_index_sequence<p_stream_count>{});
        return *this;
    }

    /** @brief Returns a mutable mask slot without checking its index. */
    [[nodiscard]] constexpr mask_type& operator[](std::size_t slot_index) noexcept {
        return masks_[slot_index];
    }

    /** @brief Returns a read-only mask slot without checking its index. */
    [[nodiscard]] constexpr const mask_type& operator[](
        std::size_t slot_index) const noexcept {
        return masks_[slot_index];
    }

    /** @brief Returns the array of mask slots. */
    [[nodiscard]] constexpr std::array<mask_type, p_stream_count>& data() noexcept {
        return masks_;
    }

    /** @brief Returns the array of mask slots as read-only storage. */
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

/** @brief Returns a reference to a slot selected at compile time. */
template <std::size_t p_slot_index, class p_stream_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE
constexpr decltype(auto) get_stream_slot(p_stream_type&& stream) noexcept {
    static_assert(
        p_slot_index < k_stream_count<p_stream_type>,
        "stream slot index out of bounds");
    return std::forward<p_stream_type>(stream)[p_slot_index];
}

/** @brief Builds a vector stream from one vector per slot. */
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

/** @brief Builds a mask stream from one mask per slot. */
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

// Apply one operation separately to each stream slot.

namespace vector_stream_detail {

/**
 * @brief Selects one stream slot, or reuses an argument that is not a stream.
 *
 * Returns a reference in both cases so repeated calls reuse the same value.
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

/** @brief Finds the largest slot count among stream arguments. */
template <class... p_argument_types>
struct deduced_stream_count;

/** @brief Empty packs contain no streams. */
template <>
struct deduced_stream_count<> {
    static constexpr std::size_t k_value = 0;
};

/** @brief Selects the largest stream count from this argument and the rest. */
template <class p_first_type, class... p_remaining_types>
struct deduced_stream_count<p_first_type, p_remaining_types...> {
    static constexpr std::size_t k_tail_value =
        deduced_stream_count<p_remaining_types...>::k_value;
    static constexpr std::size_t k_value =
        k_stream_count_for_type<p_first_type> > k_tail_value
            ? k_stream_count_for_type<p_first_type>
            : k_tail_value;
};

/** @brief Slot count selected from the argument types. */
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

/** @brief Highway tag for a stream type, or void for another type. */
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
 * @brief Calls an operation for one stream slot and returns its result.
 *
 * Arguments outside streams are reused unchanged.
 */
template <std::size_t p_slot_index, class p_operation_type, class... p_argument_types>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto evaluate_lifted_slot(
    p_operation_type& operation,
    p_argument_types&&... arguments) {
    return operation(project_stream_slot<p_slot_index>(arguments)...);
}

/** @brief Calls an operation for one stream slot when no result is returned. */
template <std::size_t p_slot_index, class p_operation_type, class... p_argument_types>
[[gnu::always_inline]] HWY_INLINE void apply_lifted_slot(
    p_operation_type& operation,
    p_argument_types&&... arguments) {
    operation(project_stream_slot<p_slot_index>(arguments)...);
}

/** @brief Collects each slot's vector result into a vector stream. */
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

/** @brief Collects each slot's mask result into a mask stream. */
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

/** @brief Calls an operation for each slot without collecting results. */
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
 * @brief Applies one operation separately to every slot in compatible streams.
 *
 * All stream arguments must have the same slot count and Highway tag.
 * Other arguments are reused by reference for each call, including values
 * originally passed as temporaries. Vector or mask results are grouped
 * into a matching stream. Operations returning void run once per slot.
 *
 * @param[in] operation Function to call for each slot.
 * @param[in] arguments Streams and shared arguments.
 * @return Grouped results when the operation returns vectors or masks.
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
 * @brief Applies an operation adapter to each slot in compatible streams.
 * @tparam p_operation_type Type with a static apply() function.
 * @param[in] arguments Streams and arguments shared by every slot.
 * @return Grouped vector or mask results, or void.
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

// These wrappers call CPU-specific Highway functions at runtime.
// Keep their bodies in source so the generator can add target attributes.

// Two-argument operations accept the vector or mask types supported by Highway.
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

/** @brief Computes (~left) & right for corresponding lanes. */
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

/** @brief Shifts every lane left by the same runtime bit count. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto shift_left_same(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::shift_left_same_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Shifts every lane right by the same runtime bit count. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto shift_right_same(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::shift_right_same_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

// Single-argument operations.
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

// Conditional selection with stream arguments or values reused across slots.
/** @brief Selects second where the mask is true and third where it is false. */
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

/** @brief Keeps lanes where the mask is true and writes zero elsewhere. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto if_then_else_zero(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::if_then_else_zero_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

/** @brief Writes zero where the mask is true and keeps other lanes. */
template <class p_left_type, class p_right_type>
requires has_stream_argument<p_left_type, p_right_type>
[[nodiscard]] HWY_INLINE auto if_then_zero_else(
    p_left_type&& left,
    p_right_type&& right) {
    return apply_lifted<vector_stream_detail::if_then_zero_else_operation>(
        std::forward<p_left_type>(left), std::forward<p_right_type>(right));
}

// Shifts and rotations with a bit count chosen at compile time.

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

/** @brief Reinterprets one vector slot's bits using another lane tag. */
template <class p_destination_tag, class p_input_type, std::size_t p_slot_index>
[[nodiscard, gnu::always_inline]] HWY_INLINE auto bit_cast_slot(
    p_destination_tag destination_tag,
    const p_input_type& input) noexcept {
    return hn::BitCast(destination_tag, get_stream_slot<p_slot_index>(input));
}

/** @brief Groups the reinterpreted vector slots into a new stream. */
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
 * @brief Reinterprets every vector's bits using the destination lane type.
 *
 * Values are not converted numerically.
 * @param[in] destination_tag Destination Highway tag.
 * @param[in] input Source vector stream.
 * @return Stream with the same number of vector slots.
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

/** @brief Expands masks to vectors with all bits set in true lanes and zero in false lanes. */
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
 * @brief Checks each mask separately for all-false lanes.
 * @return One boolean per stream slot. The caller can combine these results.
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

/** @brief Builds a vector stream by calling the factory once per slot. */
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
