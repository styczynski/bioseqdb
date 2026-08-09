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

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

export module bioseq.highway.sources.indexing;

import std;
import bioseq.indexing.grouped32_contract;
import bioseq.highway.sources.simd_utils;

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
    constexpr std::size_t vector_bytes =
        hn::MaxLanes(D{}) * sizeof(hn::TFromD<D>);
    if constexpr (vector_bytes <= 16) {
        constexpr std::size_t shift = hn::MaxLanes(D{}) - 1u;
        return hn::CombineShiftRightLanes<shift>(d, current, previous);
    } else {
        const auto shifted = hn::SlideUpLanes(d, current, 1u);
        const auto borrowed = hn::Set(
            d, hn::ExtractLane(previous, hn::Lanes(d) - 1u));
        return hn::IfThenElse(hn::FirstN(d, 1u), borrowed, shifted);
    }
}

template <class D8, class D32, std::size_t... Slots>
[[nodiscard]] inline std::array<hn::Vec<D32>, sizeof...(Slots)>
load_local_position_streams(
        D8 d8,
        D32 d32,
        const std::uint8_t* span_bytes,
        std::size_t position_index,
        std::size_t lanes,
        std::index_sequence<Slots...>) noexcept {
    return {{
        hn::BitCast(
            d32,
            hn::LoadU(
                d8,
                span_bytes + (position_index + Slots * lanes)
                    * kMinimizerGrouped32PositionEncodedBytes))...
    }};
}

template <class D, std::size_t... Slots>
[[nodiscard]] inline std::array<hn::Mask<D>, sizeof...(Slots)>
make_false_masks(D d, std::index_sequence<Slots...>) noexcept {
    return {{
        (static_cast<void>(Slots), hn::MaskFalse(d))...
    }};
}

template <std::size_t Slot, class D, std::size_t StreamWidth>
inline void accumulate_invalid_local_positions(
        D d,
        const std::array<hn::Vec<D>, StreamWidth>& current,
        hn::Vec<D> previous_vector,
        hn::Vec<D> widths,
        std::array<hn::Mask<D>, StreamWidth>& invalid_masks) noexcept {
    const auto predecessor = [&]() noexcept {
        if constexpr (Slot == 0u) {
            return lanes_shift_left_1_borrow(
                d, current[Slot], previous_vector);
        } else {
            return lanes_shift_left_1_borrow(
                d, current[Slot], current[Slot - 1u]);
        }
    }();
    invalid_masks[Slot] = hn::Or(
        invalid_masks[Slot],
        hn::Or(
            hn::Le(current[Slot], predecessor),
            hn::Ge(hn::ShiftRight<1>(current[Slot]), widths)));
}

template <class D, std::size_t StreamWidth, std::size_t... Slots>
inline void accumulate_invalid_local_position_streams(
        D d,
        const std::array<hn::Vec<D>, StreamWidth>& current,
        hn::Vec<D> previous_vector,
        hn::Vec<D> widths,
        std::array<hn::Mask<D>, StreamWidth>& invalid_masks,
        std::index_sequence<Slots...>) noexcept {
    (accumulate_invalid_local_positions<Slots>(
        d, current, previous_vector, widths, invalid_masks), ...);
}

template <class D, std::size_t StreamWidth, std::size_t... Slots>
[[nodiscard]] inline bool any_invalid_local_position_stream(
        D d,
        const std::array<hn::Mask<D>, StreamWidth>& invalid_masks,
        std::index_sequence<Slots...>) noexcept {
    return (... || !hn::AllFalse(d, invalid_masks[Slots]));
}

/// Compile-time stream-width form of the production W2 loop.  The public
/// production entry point below remains fixed at W2; W1/W4 are instantiated
/// only by the benchmark-owned fixed-target probe.
template <std::size_t StreamWidth>
[[nodiscard]] inline MinimizerGrouped32NonScatterValidationResult
validate_non_scatter_local32_core(
        std::span<const std::byte> encoded_bytes,
        const MinimizerGrouped32Control& control,
        const MinimizerPositionSegmentCore& core) noexcept {
    static_assert(
        StreamWidth == 1u || StreamWidth == 2u || StreamWidth == 4u,
        "grouped32 stream width is deliberately bounded to W1/W2/W4");
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
        const D8 d8;
        const D32 d32;
        const std::size_t lanes = hn::Lanes(d32);
        const std::size_t count = static_cast<std::size_t>(
            control.position_count);
        if (count == 0u
            || count - 1u < StreamWidth * lanes
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
        const std::uint32_t first = read_local_position(
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
        auto invalid_masks = make_false_masks(
            d32, std::make_index_sequence<StreamWidth>{});
        const auto widths = hn::Set(d32, segment_width);
        const std::size_t words_per_iteration = StreamWidth * lanes;
        result.selected_vector_stream_width =
            static_cast<std::uint8_t>(StreamWidth);
        do {
            const auto current = load_local_position_streams(
                d8,
                d32,
                span_bytes,
                position_index,
                lanes,
                std::make_index_sequence<StreamWidth>{});
            accumulate_invalid_local_position_streams(
                d32,
                current,
                previous_vector,
                widths,
                invalid_masks,
                std::make_index_sequence<StreamWidth>{});
            previous_vector = current.back();
            position_index += words_per_iteration;
            result.vectorized_words += words_per_iteration;
        } while (count - position_index >= words_per_iteration);

        if (any_invalid_local_position_stream(
                d32,
                invalid_masks,
                std::make_index_sequence<StreamWidth>{})) {
            result.validation =
                MinimizerGrouped32NonScatterValidation::rejected;
            return result;
        }

        std::uint32_t previous = hn::ExtractLane(
            previous_vector, hn::Lanes(d32) - 1u);
        while (position_index < count) {
            const std::uint32_t encoded = read_local_position(
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
    return grouped32_target_detail::validate_non_scatter_local32_core<2u>(
        encoded_bytes, control, core);
}

}  // namespace target
}  // namespace bioseq::indexing
