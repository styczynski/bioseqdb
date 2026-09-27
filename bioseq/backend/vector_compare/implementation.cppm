/**
 * @file implementation.cppm
 * @brief Combines column Boolean masks using Highway SIMD.
 */
module;

#include <hwy/highway.h>

#include <cstddef>
#include <cstdint>

module bioseq.column.vector_compare;

HWY_BEFORE_NAMESPACE();
namespace bioseq::column::vec::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/** Operation used to combine two mask vectors. */
enum class mask_composition_operation : std::uint8_t {
    intersection,  ///< Applies a lane-wise Boolean AND.
    union_,        ///< Applies a lane-wise Boolean OR.
};

/**
 * Combines mask bytes with the AND or OR selected at compile time
 * @return A vector containing the result for each byte.
 */
template <mask_composition_operation p_operation, class p_vector_type>
[[nodiscard]] HWY_INLINE p_vector_type compose_mask_vectors(
    p_vector_type left,
    p_vector_type right) noexcept {
    if constexpr (p_operation == mask_composition_operation::intersection) {
        return hn::And(left, right);
    } else {
        return hn::Or(left, right);
    }
}

/**
 * Combines Boolean masks in place using the selected Highway target.
 *
 * Processes full vectors, then loads and stores only the remaining bytes.
 * @param left Destination and left input mask.
 * @param right Right input mask.
 * @param element_count Number of elements in each mask.
 */
template <mask_composition_operation p_operation>
HWY_INLINE void compose_mask_impl(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept {
    const hn::ScalableTag<std::uint8_t> simd_tag;
    const std::size_t active_lanes = hn::Lanes(simd_tag);
    auto* HWY_RESTRICT left_bytes = reinterpret_cast<std::uint8_t*>(left);
    const auto* HWY_RESTRICT right_bytes =
        reinterpret_cast<const std::uint8_t*>(right);
    std::size_t index = 0;
    for (; index + active_lanes <= element_count; index += active_lanes) {
        hn::StoreU(
            compose_mask_vectors<p_operation>(
                hn::LoadU(simd_tag, left_bytes + index),
                hn::LoadU(simd_tag, right_bytes + index)),
            simd_tag,
            left_bytes + index);
    }
    if (index < element_count) {
        const std::size_t remainder = element_count - index;
        hn::StoreN(
            compose_mask_vectors<p_operation>(
                hn::LoadN(simd_tag, left_bytes + index, remainder),
                hn::LoadN(simd_tag, right_bytes + index, remainder)),
            simd_tag,
            left_bytes + index,
            remainder);
    }
}

}  // namespace bioseq::column::vec::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

namespace bioseq::column::vec {

/** @copydoc mask_and */
void mask_and(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept {
    bioseq::column::vec::HWY_NAMESPACE::compose_mask_impl<
        bioseq::column::vec::HWY_NAMESPACE::mask_composition_operation::intersection>(
        left,
        right,
        element_count);
}

/** @copydoc mask_or */
void mask_or(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept {
    bioseq::column::vec::HWY_NAMESPACE::compose_mask_impl<
        bioseq::column::vec::HWY_NAMESPACE::mask_composition_operation::union_>(
        left,
        right,
        element_count);
}

}  // namespace bioseq::column::vec
