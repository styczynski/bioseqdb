/**
 * @file highway_vector_stream_operations.cppm
 * @brief Defines lifted Highway operation adapters for vector streams.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.vector_stream_operations;

import std;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

namespace vector_stream_detail {

// The structural generator inserts a target attribute into every callable in
// this namespace. Keep adapter bodies directly source-spelled so Clang exposes
// one writable declaration/body geometry instead of macro expansion locations.
/** @brief Adapts Highway addition for stream lifting. */
struct add_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Add(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway subtraction for stream lifting. */
struct subtract_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Sub(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway multiplication for stream lifting. */
struct multiply_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Mul(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway lane-wise minimum for stream lifting. */
struct minimum_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Min(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway lane-wise maximum for stream lifting. */
struct maximum_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Max(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway bitwise AND for stream lifting. */
struct bit_and_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::And(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway bitwise OR for stream lifting. */
struct bit_or_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Or(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway bitwise XOR for stream lifting. */
struct bit_xor_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Xor(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway bitwise AND-NOT for stream lifting. */
struct bit_and_not_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::AndNot(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway bitwise complement for stream lifting. */
struct bit_not_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Not(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway arithmetic negation for stream lifting. */
struct negate_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Neg(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway absolute value for stream lifting. */
struct absolute_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Abs(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway equality comparison for stream lifting. */
struct equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Eq(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway inequality comparison for stream lifting. */
struct not_equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Ne(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway less-than comparison for stream lifting. */
struct less_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Lt(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway greater-than comparison for stream lifting. */
struct greater_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Gt(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway less-than-or-equal comparison for stream lifting. */
struct less_equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Le(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway greater-than-or-equal comparison for stream lifting. */
struct greater_equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Ge(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway variable-count left shift for stream lifting. */
struct shift_left_same_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::ShiftLeftSame(
            std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway variable-count right shift for stream lifting. */
struct shift_right_same_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::ShiftRightSame(
            std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Adapts Highway vector-to-mask conversion for stream lifting. */
struct mask_from_vector_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::MaskFromVec(
            std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Preserves a target-native predicate for conditional Highway operations. */
template <class p_mask_type>
[[gnu::always_inline]] HWY_INLINE decltype(auto)
native_predicate(p_mask_type&& mask_bits) {
    return std::forward<p_mask_type>(mask_bits);
}

/** @brief Adapts Highway's three-argument conditional selection. */
struct if_then_else_operation {
    template <class p_mask_type, class p_yes_type, class p_no_type>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_mask_type&& mask,
        p_yes_type&& yes,
        p_no_type&& no) {
        return hn::IfThenElse(
            native_predicate(std::forward<p_mask_type>(mask)),
            std::forward<p_yes_type>(yes),
            std::forward<p_no_type>(no));
    }
};

/** @brief Adapts Highway's two-argument true-or-zero selection. */
struct if_then_else_zero_operation {
    template <class p_mask_type, class p_yes_type>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_mask_type&& mask,
        p_yes_type&& yes) {
        return hn::IfThenElseZero(
            native_predicate(std::forward<p_mask_type>(mask)),
            std::forward<p_yes_type>(yes));
    }
};

/** @brief Adapts Highway's two-argument zero-or-false selection. */
struct if_then_zero_else_operation {
    template <class p_mask_type, class p_no_type>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_mask_type&& mask,
        p_no_type&& no) {
        return hn::IfThenZeroElse(
            native_predicate(std::forward<p_mask_type>(mask)),
            std::forward<p_no_type>(no));
    }
};

} // namespace vector_stream_detail

} // namespace target
} // namespace bioseq::simd
