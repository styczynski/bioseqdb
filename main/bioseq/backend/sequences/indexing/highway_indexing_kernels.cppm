/**
 * @file highway_indexing_kernels.cppm
 * @brief Canonical generator input for grouped32 fixed-target validation.
 *
 * This module is scanned by bioseq_codegen and is not a final production
 * provider. The generator replaces the annotated `target` namespace with one
 * namespace per compiled Highway target and adds HWY_ATTR to every callable.
 */
module;

#include <hwy/highway.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

export module bioseq.highway.sources.indexing;

import std;
import bioseq.indexing.grouped32_contract;

export namespace bioseq::indexing {
namespace [[bioseq::simd]] target {

namespace hn = hwy::HWY_NAMESPACE;

namespace grouped32_target_detail {

[[nodiscard]] inline std::uint32_t read_local_position(
        std::span<const std::byte> encoded_bytes,
        std::size_t byte_offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(
        &value,
        encoded_bytes.data() + byte_offset,
        sizeof(value));
    return value;
}

/// Shift `current` left by one lane and borrow the final lane from `previous`.
template <class D>
[[nodiscard]] inline hn::Vec<D> lanes_shift_left_1_borrow(
        D d,
        hn::Vec<D> current,
        hn::Vec<D> previous) noexcept {
    constexpr std::size_t shift = hn::MaxLanes(d) - 1u;
    if constexpr (hn::MaxLanes(d) <= 64u) {
        return hn::CombineShiftRightLanes<shift>(d, current, previous);
    } else {
        const auto shifted = hn::SlideUpLanes(d, current, 1u);
        const auto borrowed = hn::Set(
            d, hn::ExtractLane(previous, hn::Lanes(d) - 1u));
        return hn::IfThenElse(hn::FirstN(d, 1u), borrowed, shifted);
    }
}

}  // namespace grouped32_target_detail

/// Validate one already-bounds-qualified non-scatter grouped32 position span.
/// Short spans and unsupported shapes deliberately request the reader's exact
/// scalar replay; a SIMD rejection likewise replays scalar to identify the
/// canonical first wire error.
[[nodiscard]] inline MinimizerGrouped32NonScatterValidationResult
validate_non_scatter_local32_w2(
        std::span<const std::byte> encoded_bytes,
        const MinimizerGrouped32Control& control,
        const MinimizerPositionSegmentCore& core) noexcept {
    MinimizerGrouped32NonScatterValidationResult result{
        .selected_hwy_target = static_cast<std::uint64_t>(HWY_TARGET),
    };
    if constexpr (HWY_TARGET == HWY_SCALAR) {
        (void)encoded_bytes;
        (void)control;
        (void)core;
        return result;
    } else if constexpr (std::endian::native != std::endian::little) {
        return result;
    } else {
        if (static_cast<std::uint64_t>(control.position_offset)
                > (std::numeric_limits<std::size_t>::max)()
            || static_cast<std::uint64_t>(control.position_count)
                > (std::numeric_limits<std::size_t>::max)()) {
            return result;
        }

        using D8 = hn::ScalableTag<std::uint8_t>;
        using D32 = hn::Repartition<std::uint32_t, D8>;
        constexpr std::size_t stream_width = 2u;
        const D8 d8;
        const D32 d32;
        const std::size_t lanes = hn::Lanes(d32);
        const std::size_t count = static_cast<std::size_t>(
            control.position_count);
        if (count == 0u
            || count - 1u < stream_width * lanes
            || core.begin_bp >= core.end_bp
            || core.end_bp - core.begin_bp
                > static_cast<std::uint64_t>(
                    kMinimizerGrouped32MaxLocalBp) + 1u) {
            return result;
        }

        const std::size_t first_element = static_cast<std::size_t>(
            control.position_offset);
        const std::size_t available_words = encoded_bytes.size()
            / kMinimizerGrouped32PositionEncodedBytes;
        if (first_element > available_words
            || count > available_words - first_element) {
            return result;
        }
        const std::size_t first_byte = first_element
            * kMinimizerGrouped32PositionEncodedBytes;
        const auto segment_width = static_cast<std::uint32_t>(
            core.end_bp - core.begin_bp);
        const std::uint32_t first =
            grouped32_target_detail::read_local_position(
                encoded_bytes, first_byte);
        if ((first >> 1u) >= segment_width) {
            result.validation =
                MinimizerGrouped32NonScatterValidation::rejected;
            return result;
        }

        const auto* span_bytes = reinterpret_cast<const std::uint8_t*>(
            encoded_bytes.data() + first_byte);
        std::size_t position_index = 1u;
        auto previous_vector = hn::Set(d32, first);
        auto invalid_0 = hn::MaskFalse(d32);
        auto invalid_1 = hn::MaskFalse(d32);
        const auto widths = hn::Set(d32, segment_width);
        const std::size_t words_per_iteration = stream_width * lanes;
        result.selected_vector_stream_width =
            static_cast<std::uint8_t>(stream_width);
        do {
            const auto current_0 = hn::BitCast(
                d32,
                hn::LoadU(
                    d8,
                    span_bytes + position_index
                        * kMinimizerGrouped32PositionEncodedBytes));
            const auto current_1 = hn::BitCast(
                d32,
                hn::LoadU(
                    d8,
                    span_bytes + (position_index + lanes)
                        * kMinimizerGrouped32PositionEncodedBytes));
            const auto predecessor_0 =
                grouped32_target_detail::lanes_shift_left_1_borrow(
                    d32, current_0, previous_vector);
            const auto predecessor_1 =
                grouped32_target_detail::lanes_shift_left_1_borrow(
                    d32, current_1, current_0);
            invalid_0 = hn::Or(
                invalid_0,
                hn::Or(
                    hn::Le(current_0, predecessor_0),
                    hn::Ge(hn::ShiftRight<1>(current_0), widths)));
            invalid_1 = hn::Or(
                invalid_1,
                hn::Or(
                    hn::Le(current_1, predecessor_1),
                    hn::Ge(hn::ShiftRight<1>(current_1), widths)));
            previous_vector = current_1;
            position_index += words_per_iteration;
            result.vectorized_words += words_per_iteration;
        } while (count - position_index >= words_per_iteration);

        if (!hn::AllFalse(d32, invalid_0)
            || !hn::AllFalse(d32, invalid_1)) {
            result.validation =
                MinimizerGrouped32NonScatterValidation::rejected;
            return result;
        }

        std::uint32_t previous = hn::ExtractLane(
            previous_vector, hn::Lanes(d32) - 1u);
        while (position_index < count) {
            const std::uint32_t encoded =
                grouped32_target_detail::read_local_position(
                    encoded_bytes,
                    first_byte + position_index
                        * kMinimizerGrouped32PositionEncodedBytes);
            if (encoded <= previous
                || (encoded >> 1u) >= segment_width) {
                result.validation =
                    MinimizerGrouped32NonScatterValidation::rejected;
                return result;
            }
            previous = encoded;
            ++position_index;
        }
        result.validation =
            MinimizerGrouped32NonScatterValidation::accepted;
        return result;
    }
}

}  // namespace target
}  // namespace bioseq::indexing
