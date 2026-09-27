/**
 * @file
 * @brief Target-local Highway lane materialization utilities.
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

/// @brief Makes the canonical compile-time index loop available to SIMD consumers.
using ::bioseq::common::templating::for_each_index;

/**
 * @brief Materializes the active lanes of a Highway vector.
 *
 * @tparam p_value_type Scalar lane type.
 * @tparam p_simd_tag Highway descriptor type.
 * @param[in] simd_tag Highway descriptor defining the active lane count.
 * @param[in] vector Source vector.
 * @return Scalar values in increasing lane order.
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
 * @brief Materializes a Highway mask as zero or all-one scalar lanes.
 *
 * @tparam p_value_type Scalar lane type.
 * @tparam p_simd_tag Highway descriptor type.
 * @param[in] simd_tag Highway descriptor defining the active lane count.
 * @param[in] mask Source mask.
 * @return Mask lanes in Highway's vector-mask scalar representation.
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
