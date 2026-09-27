/**
 * @file
 * @brief Copies Highway vector lanes into ordinary C++ vectors.
 */
module;

#include <array>
#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>
#include <vector>

export module bioseq.simd.utils;

import bioseq.common.templating;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/// @brief Makes the shared compile-time index loop available in this SIMD namespace.
using ::bioseq::common::templating::for_each_index;

/**
 * @brief Copies the active SIMD lanes into a C++ vector in lane order.
 * @param[in] simd_tag Highway descriptor giving the active lane count.
 * @param[in] vector Vector to copy.
 * @tparam p_value_type Scalar lane type.
 */
template <typename p_value_type, typename p_simd_tag>
HWY_INLINE auto extract_lanes(const p_simd_tag& simd_tag, const hn::Vec<p_simd_tag>& vector)
    -> std::vector<p_value_type> {
#if HWY_TARGET == HWY_RVV
    auto storage = hwy::AllocateAligned<p_value_type>(hn::Lanes(simd_tag));
    p_value_type* HWY_RESTRICT storage_buffer = storage.get();
#else
    HWY_ALIGN std::array<p_value_type, hn::MaxLanes(p_simd_tag{})> storage;
    p_value_type* storage_buffer = storage.data();
#endif
    hn::StoreU(vector, simd_tag, storage_buffer);
    return std::vector<p_value_type>(
        storage_buffer,
        storage_buffer + hn::Lanes(simd_tag));
}

/**
 * @brief Copies a SIMD mask into a C++ vector.
 *
 * Each false lane has all bits clear. Each true lane has all bits set, using
 * Highway's vector representation of the mask. Lane order is preserved.
 */
template <typename p_value_type, typename p_simd_tag>
HWY_INLINE auto extract_lanes(const p_simd_tag& simd_tag, const hn::Mask<p_simd_tag>& mask)
    -> std::vector<p_value_type> {
    return extract_lanes<p_value_type>(
        simd_tag,
        hn::VecFromMask(simd_tag, mask));
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}
