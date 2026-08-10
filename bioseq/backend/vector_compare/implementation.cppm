/**
 * @file vector_compare.cppm
 * @brief Highway implementation of Boolean column-mask composition.
 */
module;

#include <hwy/highway.h>

#include <cstddef>
#include <cstdint>

module bioseq.column.vector_compare;

HWY_BEFORE_NAMESPACE();
namespace bioseq::column::vec::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/** Intersect two byte-addressable Boolean masks for the active target. */
HWY_INLINE void mask_and_impl(
    bool* left,
    const bool* right,
    std::uint32_t element_count) {
    const hn::ScalableTag<std::uint8_t> simd_tag;
    const std::size_t active_lanes = hn::Lanes(simd_tag);
    auto* HWY_RESTRICT left_bytes = reinterpret_cast<std::uint8_t*>(left);
    const auto* HWY_RESTRICT right_bytes =
        reinterpret_cast<const std::uint8_t*>(right);
    std::size_t index = 0;
    for (; index + active_lanes <= element_count; index += active_lanes) {
        hn::StoreU(
            hn::And(
                hn::LoadU(simd_tag, left_bytes + index),
                hn::LoadU(simd_tag, right_bytes + index)),
            simd_tag,
            left_bytes + index);
    }
    if (index < element_count) {
        const std::size_t remainder = element_count - index;
        hn::StoreN(
            hn::And(
                hn::LoadN(simd_tag, left_bytes + index, remainder),
                hn::LoadN(simd_tag, right_bytes + index, remainder)),
            simd_tag,
            left_bytes + index,
            remainder);
    }
}

/** Union two byte-addressable Boolean masks for the active target. */
HWY_INLINE void mask_or_impl(
    bool* left,
    const bool* right,
    std::uint32_t element_count) {
    const hn::ScalableTag<std::uint8_t> simd_tag;
    const std::size_t active_lanes = hn::Lanes(simd_tag);
    auto* HWY_RESTRICT left_bytes = reinterpret_cast<std::uint8_t*>(left);
    const auto* HWY_RESTRICT right_bytes =
        reinterpret_cast<const std::uint8_t*>(right);
    std::size_t index = 0;
    for (; index + active_lanes <= element_count; index += active_lanes) {
        hn::StoreU(
            hn::Or(
                hn::LoadU(simd_tag, left_bytes + index),
                hn::LoadU(simd_tag, right_bytes + index)),
            simd_tag,
            left_bytes + index);
    }
    if (index < element_count) {
        const std::size_t remainder = element_count - index;
        hn::StoreN(
            hn::Or(
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
    bioseq::column::vec::HWY_NAMESPACE::mask_and_impl(
        left,
        right,
        element_count);
}

/** @copydoc mask_or */
void mask_or(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept {
    bioseq::column::vec::HWY_NAMESPACE::mask_or_impl(
        left,
        right,
        element_count);
}

}  // namespace bioseq::column::vec
