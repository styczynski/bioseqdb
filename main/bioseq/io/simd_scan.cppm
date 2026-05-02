module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

export module bioseq_io.simd_scan;

import mlogging;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::io::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/// Build a Highway mask "byte equals any of `needles`". For NeedleN==1 the
/// expression collapses to a single Eq; for N=2,3 the Eq results are OR'd.
template <std::size_t NeedleN, class D>
HWY_INLINE hn::Mask<D> any_needle_match(
    D d,
    const std::array<std::uint8_t, NeedleN>& needles,
    hn::Vec<D> chunk) noexcept {
    static_assert(NeedleN >= 1 && NeedleN <= 3,
                  "scan_bytes supports 1..3 needles");
    if constexpr (NeedleN == 1) {
        return hn::Eq(chunk, hn::Set(d, needles[0]));
    } else if constexpr (NeedleN == 2) {
        return hn::Or(hn::Eq(chunk, hn::Set(d, needles[0])),
                      hn::Eq(chunk, hn::Set(d, needles[1])));
    } else {
        return hn::Or(hn::Or(hn::Eq(chunk, hn::Set(d, needles[0])),
                             hn::Eq(chunk, hn::Set(d, needles[1]))),
                      hn::Eq(chunk, hn::Set(d, needles[2])));
    }
}

/// Find the first byte in `[data, data+size)` matching ANY of `needles`.
/// Returns `size` if absent. The hot loop does one Lanes(d)-vector per
/// iteration; the tail uses a final overlap-load when `cur < end` and
/// `end - cur < lanes`. For v1, no 4× unroll — correctness first;
/// per-target unroll factor is a phase-2 optimisation. Spec §8.1, §8.2.
template <std::size_t NeedleN, class D>
[[nodiscard]] HWY_INLINE std::size_t scan_for_first(
    D d,
    const std::array<std::uint8_t, NeedleN>& needles,
    const std::uint8_t* HWY_RESTRICT data,
    std::size_t size) noexcept {
    const std::size_t lanes = hn::Lanes(d);

    // Tiny-haystack scalar fast path
    if (size < lanes) {
        for (std::size_t i = 0; i < size; ++i) {
            for (std::size_t k = 0; k < NeedleN; ++k) {
                if (data[i] == needles[k]) return i;
            }
        }
        return size;
    }

    std::size_t cur = 0;
    while (cur + lanes <= size) {
        const auto chunk = hn::LoadU(d, data + cur);
        const auto match = any_needle_match<NeedleN>(d, needles, chunk);
        if (!hn::AllFalse(d, match)) {
            const auto lane = hn::FindFirstTrue(d, match);
            return cur + static_cast<std::size_t>(lane);
        }
        cur += lanes;
    }

    // Tail-overlap trick: rewind so the load ends exactly at `data+size`.
    if (cur < size) {
        const std::size_t overlap_start = size - lanes;
        const auto chunk = hn::LoadU(d, data + overlap_start);
        const auto match = any_needle_match<NeedleN>(d, needles, chunk);
        if (!hn::AllFalse(d, match)) {
            // Mask out lanes that were already searched (lanes [0, cur - overlap_start))
            const std::size_t already_searched = cur - overlap_start;
            const auto suppress = hn::FirstN(d, already_searched);
            const auto live = hn::AndNot(suppress, match);
            if (!hn::AllFalse(d, live)) {
                const auto lane = hn::FindFirstTrue(d, live);
                return overlap_start + static_cast<std::size_t>(lane);
            }
        }
    }
    return size;
}

/// Iterate every offset in `[data, data+size)` whose byte matches any
/// needle, calling `visit(offset)` per match in ascending order.
/// Used by FASTA scanner / line-offset bookkeeping. Spec §8.4.
template <std::size_t NeedleN, class D, typename Visit>
HWY_INLINE void scan_for_each(
    D d,
    const std::array<std::uint8_t, NeedleN>& needles,
    const std::uint8_t* HWY_RESTRICT data,
    std::size_t size,
    Visit&& visit) noexcept {
    const std::size_t lanes = hn::Lanes(d);
    if (size < lanes) {
        for (std::size_t i = 0; i < size; ++i) {
            for (std::size_t k = 0; k < NeedleN; ++k) {
                if (data[i] == needles[k]) {
                    visit(i);
                    break;
                }
            }
        }
        return;
    }
    std::size_t cur = 0;
    while (cur + lanes <= size) {
        const auto chunk = hn::LoadU(d, data + cur);
        const auto match = any_needle_match<NeedleN>(d, needles, chunk);
        if (!hn::AllFalse(d, match)) {
            const std::uint64_t bits = hn::BitsFromMask(d, match);
            std::uint64_t remaining = bits;
            while (remaining != 0) {
                const std::size_t lane = static_cast<std::size_t>(
                    std::countr_zero(remaining));
                visit(cur + lane);
                remaining &= remaining - 1;
            }
        }
        cur += lanes;
    }
    // Tail (scalar to avoid double-counting overlap)
    for (std::size_t i = cur; i < size; ++i) {
        for (std::size_t k = 0; k < NeedleN; ++k) {
            if (data[i] == needles[k]) {
                visit(i);
                break;
            }
        }
    }
}

}  // namespace bioseq::io::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
