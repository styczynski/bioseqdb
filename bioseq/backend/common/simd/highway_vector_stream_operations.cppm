/**
 * @file highway_vector_stream_operations.cppm
 * @brief Highway operations called once for each vector-stream slot.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.vector_stream_operations;

import std;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

namespace vector_stream_detail {

// The generator adds a CPU target attribute to each function here.
// Write the bodies directly so it can edit their source locations
// without following macro expansions.
/** @brief Adds corresponding SIMD lanes. */
struct add_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Add(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Subtracts corresponding SIMD lanes. */
struct subtract_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Sub(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Multiplies corresponding SIMD lanes. */
struct multiply_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Mul(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Selects the smaller value in each pair of lanes. */
struct minimum_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Min(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Selects the larger value in each pair of lanes. */
struct maximum_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Max(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Applies bitwise AND to vectors or masks. */
struct bit_and_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::And(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Applies bitwise OR to vectors or masks. */
struct bit_or_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Or(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Applies bitwise XOR to vectors or masks. */
struct bit_xor_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Xor(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Computes (~first) & second for vectors or masks. */
struct bit_and_not_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::AndNot(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Complements vector or mask bits. */
struct bit_not_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Not(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Negates each lane. */
struct negate_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Neg(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Takes the absolute value of each lane. */
struct absolute_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Abs(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Returns masks for equal lanes. */
struct equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Eq(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Returns masks for unequal lanes. */
struct not_equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Ne(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Returns masks for lanes where the first value is smaller. */
struct less_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Lt(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Returns masks for lanes where the first value is larger. */
struct greater_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Gt(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Returns masks for lanes where the first value is at most the second. */
struct less_equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Le(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Returns masks for lanes where the first value is at least the second. */
struct greater_equal_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::Ge(std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Shifts each lane left by the same runtime bit count. */
struct shift_left_same_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::ShiftLeftSame(
            std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Shifts each lane right by the same runtime bit count. */
struct shift_right_same_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::ShiftRightSame(
            std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Converts a vector containing mask bits to a Highway mask. */
struct mask_from_vector_operation {
    template <class... p_argument_types>
    [[gnu::always_inline]] HWY_INLINE static auto apply(
        p_argument_types&&... arguments) {
        return hn::MaskFromVec(
            std::forward<p_argument_types>(arguments)...);
    }
};

/** @brief Forwards a Highway mask to conditional operations unchanged. */
template <class p_mask_type>
[[gnu::always_inline]] HWY_INLINE decltype(auto)
native_predicate(p_mask_type&& mask_bits) {
    return std::forward<p_mask_type>(mask_bits);
}

/** @brief Selects yes where the mask is true and no elsewhere. */
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

/** @brief Selects yes where the mask is true and zero elsewhere. */
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

/** @brief Selects zero where the mask is true and no elsewhere. */
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
