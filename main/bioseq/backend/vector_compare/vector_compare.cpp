module;

#include "common_simd.h"

#include <cstdint>
#include <span>

module bioseq.column.vector_compare;

HWY_BEFORE_NAMESPACE();
namespace bioseq::column::vec::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

HWY_INLINE void mask_and_impl(std::span<bool> lhs, std::span<const bool> rhs) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t N = hn::Lanes(d);
    const std::size_t n = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    std::uint8_t* HWY_RESTRICT lp = reinterpret_cast<std::uint8_t*>(lhs.data());
    const std::uint8_t* HWY_RESTRICT rp = reinterpret_cast<const std::uint8_t*>(rhs.data());
    std::size_t i = 0;
    for (; i + N <= n; i += N) {
        hn::StoreU(hn::And(hn::LoadU(d, lp + i), hn::LoadU(d, rp + i)), d, lp + i);
    }
    if (i < n) {
        const std::size_t rem = n - i;
        hn::StoreN(hn::And(hn::LoadN(d, lp + i, rem), hn::LoadN(d, rp + i, rem)), d, lp + i, rem);
    }
}

HWY_INLINE void mask_or_impl(std::span<bool> lhs, std::span<const bool> rhs) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t N = hn::Lanes(d);
    const std::size_t n = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    std::uint8_t* HWY_RESTRICT lp = reinterpret_cast<std::uint8_t*>(lhs.data());
    const std::uint8_t* HWY_RESTRICT rp = reinterpret_cast<const std::uint8_t*>(rhs.data());
    std::size_t i = 0;
    for (; i + N <= n; i += N) {
        hn::StoreU(hn::Or(hn::LoadU(d, lp + i), hn::LoadU(d, rp + i)), d, lp + i);
    }
    if (i < n) {
        const std::size_t rem = n - i;
        hn::StoreN(hn::Or(hn::LoadN(d, lp + i, rem), hn::LoadN(d, rp + i, rem)), d, lp + i, rem);
    }
}

}  // namespace bioseq::column::vec::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

namespace bioseq::column::vec {

void mask_and(std::span<bool> lhs, std::span<const bool> rhs) noexcept {
    bioseq::column::vec::HWY_NAMESPACE::mask_and_impl(lhs, rhs);
}

void mask_or(std::span<bool> lhs, std::span<const bool> rhs) noexcept {
    bioseq::column::vec::HWY_NAMESPACE::mask_or_impl(lhs, rhs);
}

}  // namespace bioseq::column::vec
