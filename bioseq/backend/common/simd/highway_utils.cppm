/**
 * @file
 * @brief SIMD lane and matrix helpers compiled for each Highway CPU target.
 */
module;

#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>

export module bioseq.highway.sources.simd_utils;

import std;
import bioseq.common.templating;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/// @brief Reuses the compile-time index loop in generated kernels.
using ::bioseq::common::templating::for_each_index;

/**
 * @brief Allocates an array through Highway's aligned allocator.
 *
 * @tparam p_value_type Array element type.
 * @tparam p_buffer_type Owning buffer type returned by Highway.
 */
template <class p_value_type, class p_buffer_type>
struct simd_array_allocator {
    /**
     * @brief Allocates aligned storage for a requested element count.
     *
     * @param[in] length Number of elements to allocate.
     * @return Owning Highway-aligned buffer.
     */
    [[nodiscard]] static auto allocate(std::size_t length) -> p_buffer_type {
        return hwy::AllocateAligned<p_value_type>(length);
    }
};

/**
 * @brief Extracts a lane counted backward from the active vector tail.
 *
 * @tparam p_simd_tag Highway descriptor type.
 * @tparam p_tail_offset Zero-based distance from the final active lane.
 * @tparam p_value_type Scalar lane type.
 * @param[in] simd_tag Highway descriptor defining the active lane count.
 * @param[in] vector Source vector.
 * @return Selected scalar lane.
 * @pre `p_tail_offset < hn::Lanes(simd_tag)`.
 */
template <class p_simd_tag,
          std::size_t p_tail_offset = 0u,
          class p_value_type = hn::TFromD<p_simd_tag>>
HWY_INLINE auto extract_last_lane(p_simd_tag simd_tag, hn::Vec<p_simd_tag> vector) -> p_value_type {
    return hn::ExtractLane(
        vector,
        hn::Lanes(simd_tag) - 1u - p_tail_offset);
}

/**
 * @brief Copies a tail lane into every lane of the vector.
 * @tparam p_tail_offset Distance from the final lane.
 * @param[in] simd_tag SIMD lane type and width.
 * @param[in] vector Source vector.
 * @pre The selected lane exists. For tags with at most 64 maximum lanes,
 * the lane index is based on MaxLanes, so the active width must match it.
 */
template <class p_simd_tag, std::size_t p_tail_offset = 0u>
HWY_INLINE auto broadcast_last_lane(p_simd_tag simd_tag, hn::Vec<p_simd_tag> vector)
    -> hn::Vec<p_simd_tag> {
    constexpr std::size_t k_lane_index =
        hn::MaxLanes(p_simd_tag{}) - 1u - p_tail_offset;
    if constexpr (hn::MaxLanes(p_simd_tag{}) <= 64u) {
        return hn::BroadcastLane<k_lane_index>(vector);
    } else {
        return hn::Set(
            simd_tag,
            extract_last_lane<p_simd_tag, p_tail_offset>(simd_tag, vector));
    }
}

/**
 * @brief Replaces the first p_first_lane_count lanes with first_value.
 *
 * Other lanes keep their original values.
 */
template <class p_simd_tag,
          std::size_t p_first_lane_count = 1u,
          class p_value_type = hn::TFromD<p_simd_tag>>
HWY_INLINE auto replace_first_lanes(p_simd_tag simd_tag,
                                    hn::Vec<p_simd_tag> vector,
                                    p_value_type first_value) -> hn::Vec<p_simd_tag> {
    return hn::IfThenElse(
        hn::FirstN(simd_tag, p_first_lane_count),
        hn::Set(simd_tag, first_value),
        vector);
}

/**
 * @brief Extracts one runtime-selected vector lane.
 *
 * @tparam p_simd_tag Highway descriptor type.
 * @tparam p_value_type Scalar lane type.
 * @param[in] simd_tag Highway descriptor associated with the vector.
 * @param[in] vector Source vector.
 * @param[in] lane_index Zero-based active-lane index.
 * @return Selected scalar lane.
 * @pre `lane_index < hn::Lanes(simd_tag)`.
 */
template <class p_simd_tag, class p_value_type = hn::TFromD<p_simd_tag>>
HWY_INLINE auto extract_lane([[maybe_unused]] p_simd_tag simd_tag,
                             hn::Vec<p_simd_tag> vector,
                             std::size_t lane_index) -> p_value_type {
    return hn::ExtractLane(vector, lane_index);
}

/**
 * @brief Returns the first lane equal to the maximum supplied by the caller.
 *
 * Aborts if no lane contains that value.
 */
template <class p_simd_tag, class p_value_type = hn::TFromD<p_simd_tag>>
HWY_INLINE auto find_first_maximum_lane(p_simd_tag simd_tag,
                                        hn::Vec<p_simd_tag> vector,
                                        p_value_type maximum) -> std::size_t {
    const auto lane_index = hn::FindFirstTrue(
        simd_tag,
        hn::Eq(vector, hn::Set(simd_tag, maximum)));
    if (lane_index < 0) [[unlikely]] {
        std::abort();
    }
    return static_cast<std::size_t>(lane_index);
}

/**
 * @brief Shifts lanes left within each 128-bit block, filling with zeros.
 *
 * Alignment prefix scans depend on values staying inside their blocks.
 * @tparam p_lane_count Number of lanes to shift.
 */
template <std::size_t p_lane_count, class p_simd_tag>
HWY_INLINE auto shift_lanes_left_zero(p_simd_tag simd_tag, hn::Vec<p_simd_tag> vector)
    -> hn::Vec<p_simd_tag> {
    return hn::ShiftLeftBytes<
        p_lane_count * sizeof(hn::TFromD<p_simd_tag>)>(simd_tag, vector);
}

/**
 * @brief Shifts current one lane left and inserts the predecessor's last lane.
 *
 * This carries a value across consecutive vectors.
 * @param[in] simd_tag SIMD lane type and width.
 * @param[in] current Vector to shift.
 * @param[in] predecessor Vector immediately before current.
 */
template <class p_simd_tag>
HWY_INLINE auto shift_lanes_left_with_predecessor(p_simd_tag simd_tag,
                                                  hn::Vec<p_simd_tag> current,
                                                  hn::Vec<p_simd_tag> predecessor)
    -> hn::Vec<p_simd_tag> {
    constexpr std::size_t k_vector_byte_count =
        hn::MaxLanes(p_simd_tag{}) * sizeof(hn::TFromD<p_simd_tag>);
    if constexpr (k_vector_byte_count <= 16u) {
        constexpr std::size_t k_shift_lane_count =
            hn::MaxLanes(p_simd_tag{}) - 1u;
        return hn::CombineShiftRightLanes<k_shift_lane_count>(
            simd_tag,
            current,
            predecessor);
    } else {
        const auto shifted = hn::SlideUpLanes(simd_tag, current, 1u);
        return hn::IfThenElse(
            hn::FirstN(simd_tag, 1u),
            hn::Set(simd_tag, extract_last_lane(simd_tag, predecessor)),
            shifted);
    }
}

/**
 * @brief Approximates log2 for positive 32-bit integer lanes.
 *
 * Converts to float and combines the exponent with a polynomial estimate
 * for the fractional part. Used when scoring chaining gaps.
 * @param[in] integer_simd_tag 32-bit integer tag.
 * @param[in] float_simd_tag Float tag with the same lane count.
 * @param[in] integer_values Positive values to evaluate.
 * @return Approximate base-two logarithm in each float lane.
 */
template <class p_integer_simd_tag, class p_float_simd_tag>
HWY_INLINE auto approximate_log2(p_integer_simd_tag integer_simd_tag,
                                 p_float_simd_tag float_simd_tag,
                                 hn::Vec<p_integer_simd_tag> integer_values)
    -> hn::Vec<p_float_simd_tag> {
    const auto float_values = hn::ConvertTo(float_simd_tag, integer_values);
    const auto integer_bit_values = hn::BitCast(integer_simd_tag, float_values);
    const auto raw_exponents = hn::And(
        hn::ShiftRight<23>(integer_bit_values),
        hn::Set(integer_simd_tag, 255));
    const auto biased_exponents = hn::Sub(
        raw_exponents,
        hn::Set(integer_simd_tag, 128));
    const auto float_exponents = hn::ConvertTo(float_simd_tag, biased_exponents);
    const auto mantissa_bits = hn::Add(
        hn::And(
            integer_bit_values,
            hn::Set(integer_simd_tag, ~(255 << 23))),
        hn::Set(integer_simd_tag, 127 << 23));
    const auto normalized_mantissas = hn::BitCast(float_simd_tag, mantissa_bits);
    const auto inner_terms = hn::MulAdd(
        hn::Set(float_simd_tag, -0.34484843F),
        normalized_mantissas,
        hn::Set(float_simd_tag, 2.02466578F));
    const auto fractional_parts = hn::MulSub(
        inner_terms,
        normalized_mantissas,
        hn::Set(float_simd_tag, 0.67487759F));
    return hn::Add(float_exponents, fractional_parts);
}

/**
 * @brief Checks whether a lane type remains at most 64 bits after three widenings.
 *
 * @tparam p_simd_tag Highway descriptor type.
 */
template <typename p_simd_tag>
concept simd_lane_widenable_three_times =
    sizeof(hn::TFromD<p_simd_tag>) <= 1u;

/**
 * @brief Checks whether a lane type remains at most 64 bits after two widenings.
 *
 * @tparam p_simd_tag Highway descriptor type.
 */
template <typename p_simd_tag>
concept simd_lane_widenable_twice =
    sizeof(hn::TFromD<p_simd_tag>) <= 2u;

/**
 * @brief Checks whether a lane type remains at most 64 bits after one widening.
 *
 * @tparam p_simd_tag Highway descriptor type.
 */
template <typename p_simd_tag>
concept simd_lane_widenable_once =
    sizeof(hn::TFromD<p_simd_tag>) <= 4u;

#if HWY_TARGET == HWY_SCALAR

// Scalar builds need these declarations to compile shared callers.
// Returning false prevents use of the multi-lane transpose path.
/// @brief Reports that the scalar target cannot use the u64 transpose path.
/// @return Always false.
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool u64_lane_transpose_shape_supported(p_simd_tag) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    return false;
}

/// @brief Leaves the matrix unchanged on the scalar target.
/// @return Always false.
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool transpose_u64_lane_matrix(
    p_simd_tag,
    hn::Vec<p_simd_tag>* HWY_RESTRICT) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    return false;
}

#else

/**
 * @brief Checks whether this target supports the 64-bit matrix transpose.
 *
 * The lane count must be 2, 4, 8 or 16, and each half-width tag must
 * have exactly half as many lanes. Check this before choosing the path.
 * @param d Tag for unsigned 64-bit lanes.
 * @return True when the recursive transpose can handle the active width.
 */
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool u64_lane_transpose_shape_supported(p_simd_tag d) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    const std::size_t lanes = hn::Lanes(d);
    if (lanes < 2 || lanes > 16) return false;
    if (lanes == 2) return true;
    if ((lanes & (lanes - 1)) != 0) return false;

    if constexpr (hn::MaxLanes(p_simd_tag{}) < 4) {
        return false;
    } else {
        using half_simd_tag_type = hn::Half<p_simd_tag>;
        const half_simd_tag_type dh;
        return hn::Lanes(dh) * 2 == lanes
            && u64_lane_transpose_shape_supported(dh);
    }
}

/**
 * @brief Transposes a square matrix of unsigned 64-bit lanes using SIMD.
 *
 * Splits the matrix into four smaller squares, transposes them recursively
 * and combines the halves. The two-lane case uses interleaving. This avoids
 * requiring a wider lane type than uint64_t.
 *
 * @param d Tag defining the matrix width.
 * @param matrix Array containing at least Lanes(d) vectors.
 * @return True when the matrix was transposed, or false for an unsupported width.
 */
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE bool transpose_u64_lane_matrix(
    p_simd_tag d,
    hn::Vec<p_simd_tag>* HWY_RESTRICT matrix) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint64_t>);
    const std::size_t lanes = hn::Lanes(d);
    if (lanes == 2) {
        const hn::Vec<p_simd_tag> column0 = matrix[0];
        const hn::Vec<p_simd_tag> column1 = matrix[1];
        matrix[0] = hn::InterleaveLower(d, column0, column1);
        matrix[1] = hn::InterleaveUpper(d, column0, column1);
        return true;
    }
    if (lanes < 4 || lanes > 16 || (lanes & (lanes - 1)) != 0) {
        return false;
    }

    if constexpr (hn::MaxLanes(p_simd_tag{}) < 4) {
        return false;
    } else {
        using half_simd_tag_type = hn::Half<p_simd_tag>;
        using half_vector_type = hn::Vec<half_simd_tag_type>;
        constexpr std::size_t half_capacity = hn::MaxLanes(half_simd_tag_type{});
        const half_simd_tag_type dh;
        const std::size_t half_lanes = lanes / 2;
        if (hn::Lanes(dh) != half_lanes) return false;

        HWY_ALIGN std::array<half_vector_type, half_capacity> lower_left{};
        HWY_ALIGN std::array<half_vector_type, half_capacity> lower_right{};
        HWY_ALIGN std::array<half_vector_type, half_capacity> upper_left{};
        HWY_ALIGN std::array<half_vector_type, half_capacity> upper_right{};
        for (std::size_t column = 0; column < half_lanes; ++column) {
            lower_left[column] = hn::LowerHalf(dh, matrix[column]);
            upper_left[column] = hn::UpperHalf(dh, matrix[column]);
            lower_right[column] =
                hn::LowerHalf(dh, matrix[column + half_lanes]);
            upper_right[column] =
                hn::UpperHalf(dh, matrix[column + half_lanes]);
        }

        if (!transpose_u64_lane_matrix(dh, lower_left.data())
            || !transpose_u64_lane_matrix(dh, lower_right.data())
            || !transpose_u64_lane_matrix(dh, upper_left.data())
            || !transpose_u64_lane_matrix(dh, upper_right.data())) {
            return false;
        }
        for (std::size_t row = 0; row < half_lanes; ++row) {
            matrix[row] = hn::Combine(
                d, lower_right[row], lower_left[row]);
            matrix[row + half_lanes] = hn::Combine(
                d, upper_right[row], upper_left[row]);
        }
        return true;
    }
}

#endif

/**
 * @brief Loads and transposes a square matrix stored in SIMD vectors.
 *
 * The wrapper borrows its storage. The caller keeps that storage alive
 * and prevents concurrent changes. Transposition supports 1, 2, 4, 8 or
 * 16 lanes, with element-width limits checked by each implementation.
 * Unsupported combinations abort.
 *
 * @tparam p_simd_tag Highway lane tag.
 * @tparam p_simd_vector Vector type for the tag.
 * @tparam p_matrix_storage Indexable handle to the caller's storage.
 */
template <
    class p_simd_tag,
    class p_simd_vector = hn::Vec<p_simd_tag>,
    typename p_matrix_storage = p_simd_vector*>
class square_lane_matrix {
private:
    p_matrix_storage matrix_storage_;

    /**
     * @brief Leaves a one-lane matrix unchanged.
     *
     * @param[in] simd_tag Active Highway descriptor.
     * @param[in,out] matrix_storage Borrowed matrix storage.
     */
    static constexpr auto transpose_one_lane([[maybe_unused]] p_simd_tag& simd_tag,
                                             [[maybe_unused]] p_matrix_storage& matrix_storage)
        -> void {}

    /**
     * @brief Transposes a two-lane square matrix in place.
     *
     * @param[in] simd_tag Active Highway descriptor.
     * @param[in,out] matrix_storage Borrowed matrix storage.
     */
    static constexpr auto transpose_two_lanes(p_simd_tag& simd_tag,
                                              p_matrix_storage& matrix_storage) -> void {
        const p_simd_vector first_row = matrix_storage[0];
        const p_simd_vector second_row = matrix_storage[1];
        const p_simd_vector first_column =
            hn::InterleaveLower(simd_tag, first_row, second_row);
        const p_simd_vector second_column =
            hn::InterleaveUpper(simd_tag, first_row, second_row);
        matrix_storage[0] = first_column;
        matrix_storage[1] = second_column;
    }

    /**
     * @brief Transposes a four-lane square matrix in place.
     *
     * @param[in] simd_tag Active Highway descriptor.
     * @param[in,out] matrix_storage Borrowed matrix storage.
     * @warning Terminates when the lane type cannot be widened once.
     */
    static constexpr auto transpose_four_lanes(p_simd_tag& simd_tag,
                                               p_matrix_storage& matrix_storage) -> void {
        if constexpr (!simd_lane_widenable_once<p_simd_tag>) {
            std::abort();
        } else {
            const hn::RepartitionToWide<p_simd_tag> wide_simd_tag;

            const p_simd_vector row_zero = matrix_storage[0];
            const p_simd_vector row_one = matrix_storage[1];
            const p_simd_vector row_two = matrix_storage[2];
            const p_simd_vector row_three = matrix_storage[3];

            const p_simd_vector lower_top_pair =
                hn::InterleaveLower(simd_tag, row_zero, row_one);
            const p_simd_vector lower_bottom_pair =
                hn::InterleaveLower(simd_tag, row_two, row_three);
            const p_simd_vector upper_top_pair =
                hn::InterleaveUpper(simd_tag, row_zero, row_one);
            const p_simd_vector upper_bottom_pair =
                hn::InterleaveUpper(simd_tag, row_two, row_three);

            matrix_storage[0] = hn::BitCast(
                simd_tag,
                hn::InterleaveLower(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, lower_top_pair),
                    hn::BitCast(wide_simd_tag, lower_bottom_pair)));
            matrix_storage[1] = hn::BitCast(
                simd_tag,
                hn::InterleaveUpper(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, lower_top_pair),
                    hn::BitCast(wide_simd_tag, lower_bottom_pair)));
            matrix_storage[2] = hn::BitCast(
                simd_tag,
                hn::InterleaveLower(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, upper_top_pair),
                    hn::BitCast(wide_simd_tag, upper_bottom_pair)));
            matrix_storage[3] = hn::BitCast(
                simd_tag,
                hn::InterleaveUpper(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, upper_top_pair),
                    hn::BitCast(wide_simd_tag, upper_bottom_pair)));
        }
    }

    /**
     * @brief Transposes an eight-lane square matrix in place.
     *
     * @param[in] simd_tag Active Highway descriptor.
     * @param[in,out] matrix_storage Borrowed matrix storage.
     * @warning Terminates when the lane type exceeds 32 bits.
     */
    static constexpr auto transpose_eight_lanes(p_simd_tag& simd_tag,
                                                p_matrix_storage& matrix_storage) -> void {
        if constexpr (sizeof(hn::TFromD<p_simd_tag>) > 4u) {
            std::abort();
        } else {
            const hn::RepartitionToWide<p_simd_tag> wide_simd_tag;

            const p_simd_vector row_zero = matrix_storage[0];
            const p_simd_vector row_one = matrix_storage[1];
            const p_simd_vector row_two = matrix_storage[2];
            const p_simd_vector row_three = matrix_storage[3];
            const p_simd_vector row_four = matrix_storage[4];
            const p_simd_vector row_five = matrix_storage[5];
            const p_simd_vector row_six = matrix_storage[6];
            const p_simd_vector row_seven = matrix_storage[7];

            const p_simd_vector lower_pair_zero =
                hn::InterleaveLower(simd_tag, row_zero, row_one);
            const p_simd_vector lower_pair_one =
                hn::InterleaveLower(simd_tag, row_two, row_three);
            const p_simd_vector lower_pair_two =
                hn::InterleaveLower(simd_tag, row_four, row_five);
            const p_simd_vector lower_pair_three =
                hn::InterleaveLower(simd_tag, row_six, row_seven);
            const p_simd_vector upper_pair_zero =
                hn::InterleaveUpper(simd_tag, row_zero, row_one);
            const p_simd_vector upper_pair_one =
                hn::InterleaveUpper(simd_tag, row_two, row_three);
            const p_simd_vector upper_pair_two =
                hn::InterleaveUpper(simd_tag, row_four, row_five);
            const p_simd_vector upper_pair_three =
                hn::InterleaveUpper(simd_tag, row_six, row_seven);

            const p_simd_vector lower_block_zero = hn::BitCast(
                simd_tag,
                hn::InterleaveLower(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, lower_pair_zero),
                    hn::BitCast(wide_simd_tag, lower_pair_one)));
            const p_simd_vector lower_block_one = hn::BitCast(
                simd_tag,
                hn::InterleaveUpper(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, lower_pair_zero),
                    hn::BitCast(wide_simd_tag, lower_pair_one)));
            const p_simd_vector lower_block_two = hn::BitCast(
                simd_tag,
                hn::InterleaveLower(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, lower_pair_two),
                    hn::BitCast(wide_simd_tag, lower_pair_three)));
            const p_simd_vector lower_block_three = hn::BitCast(
                simd_tag,
                hn::InterleaveUpper(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, lower_pair_two),
                    hn::BitCast(wide_simd_tag, lower_pair_three)));
            const p_simd_vector upper_block_zero = hn::BitCast(
                simd_tag,
                hn::InterleaveLower(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, upper_pair_zero),
                    hn::BitCast(wide_simd_tag, upper_pair_one)));
            const p_simd_vector upper_block_one = hn::BitCast(
                simd_tag,
                hn::InterleaveUpper(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, upper_pair_zero),
                    hn::BitCast(wide_simd_tag, upper_pair_one)));
            const p_simd_vector upper_block_two = hn::BitCast(
                simd_tag,
                hn::InterleaveLower(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, upper_pair_two),
                    hn::BitCast(wide_simd_tag, upper_pair_three)));
            const p_simd_vector upper_block_three = hn::BitCast(
                simd_tag,
                hn::InterleaveUpper(
                    wide_simd_tag,
                    hn::BitCast(wide_simd_tag, upper_pair_two),
                    hn::BitCast(wide_simd_tag, upper_pair_three)));

            if constexpr (simd_lane_widenable_twice<p_simd_tag>) {
                const hn::RepartitionToWideX2<p_simd_tag> double_wide_simd_tag;

                matrix_storage[0] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveLower(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, lower_block_zero),
                        hn::BitCast(double_wide_simd_tag, lower_block_two)));
                matrix_storage[1] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveUpper(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, lower_block_zero),
                        hn::BitCast(double_wide_simd_tag, lower_block_two)));
                matrix_storage[2] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveLower(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, lower_block_one),
                        hn::BitCast(double_wide_simd_tag, lower_block_three)));
                matrix_storage[3] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveUpper(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, lower_block_one),
                        hn::BitCast(double_wide_simd_tag, lower_block_three)));
                matrix_storage[4] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveLower(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, upper_block_zero),
                        hn::BitCast(double_wide_simd_tag, upper_block_two)));
                matrix_storage[5] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveUpper(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, upper_block_zero),
                        hn::BitCast(double_wide_simd_tag, upper_block_two)));
                matrix_storage[6] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveLower(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, upper_block_one),
                        hn::BitCast(double_wide_simd_tag, upper_block_three)));
                matrix_storage[7] = hn::BitCast(
                    simd_tag,
                    hn::InterleaveUpper(
                        double_wide_simd_tag,
                        hn::BitCast(double_wide_simd_tag, upper_block_one),
                        hn::BitCast(double_wide_simd_tag, upper_block_three)));
            } else {
                const hn::Half<p_simd_tag> half_simd_tag;

                matrix_storage[0] = hn::Combine(
                    simd_tag,
                    hn::LowerHalf(half_simd_tag, lower_block_two),
                    hn::LowerHalf(half_simd_tag, lower_block_zero));
                matrix_storage[1] = hn::Combine(
                    simd_tag,
                    hn::LowerHalf(half_simd_tag, lower_block_three),
                    hn::LowerHalf(half_simd_tag, lower_block_one));
                matrix_storage[2] = hn::Combine(
                    simd_tag,
                    hn::LowerHalf(half_simd_tag, upper_block_two),
                    hn::LowerHalf(half_simd_tag, upper_block_zero));
                matrix_storage[3] = hn::Combine(
                    simd_tag,
                    hn::LowerHalf(half_simd_tag, upper_block_three),
                    hn::LowerHalf(half_simd_tag, upper_block_one));
                matrix_storage[4] = hn::Combine(
                    simd_tag,
                    hn::UpperHalf(half_simd_tag, lower_block_two),
                    hn::UpperHalf(half_simd_tag, lower_block_zero));
                matrix_storage[5] = hn::Combine(
                    simd_tag,
                    hn::UpperHalf(half_simd_tag, lower_block_three),
                    hn::UpperHalf(half_simd_tag, lower_block_one));
                matrix_storage[6] = hn::Combine(
                    simd_tag,
                    hn::UpperHalf(half_simd_tag, upper_block_two),
                    hn::UpperHalf(half_simd_tag, upper_block_zero));
                matrix_storage[7] = hn::Combine(
                    simd_tag,
                    hn::UpperHalf(half_simd_tag, upper_block_three),
                    hn::UpperHalf(half_simd_tag, upper_block_one));
            }
        }
    }

    /**
     * @brief Combines columns from the two eight-row halves of a matrix.
     * @tparam p_select_upper Select the upper interleave result when true.
     * @param[in] simd_tag Original lane tag.
     * @param[in] triple_wide_simd_tag Tag whose lanes hold eight original elements.
     * @param[in] upper_column Column from the first half.
     * @param[in] lower_column Column from the second half.
     * @return One column of the sixteen-row matrix.
     */
    template <bool p_select_upper, class p_triple_wide_simd_tag>
    static constexpr auto
    interleave_matrix_halves(p_simd_tag& simd_tag,
                             const p_triple_wide_simd_tag& triple_wide_simd_tag,
                             p_simd_vector upper_column,
                             p_simd_vector lower_column) -> p_simd_vector {
        const auto upper_wide =
            hn::BitCast(triple_wide_simd_tag, upper_column);
        const auto lower_wide =
            hn::BitCast(triple_wide_simd_tag, lower_column);
        if constexpr (p_select_upper) {
            return hn::BitCast(
                simd_tag,
                hn::InterleaveUpper(
                    triple_wide_simd_tag,
                    upper_wide,
                    lower_wide));
        } else {
            return hn::BitCast(
                simd_tag,
                hn::InterleaveLower(
                    triple_wide_simd_tag,
                    upper_wide,
                    lower_wide));
        }
    }

    /**
     * @brief Transposes a sixteen-lane square matrix in place.
     *
     * @param[in] simd_tag Active Highway descriptor.
     * @param[in,out] matrix_storage Borrowed matrix storage.
     * @warning Terminates when the lane type cannot be widened three times.
     */
    static constexpr auto transpose_sixteen_lanes(p_simd_tag& simd_tag,
                                                  p_matrix_storage& matrix_storage) -> void {
        if constexpr (!simd_lane_widenable_three_times<p_simd_tag>) {
            std::abort();
        } else {
            auto lower_half_storage = &matrix_storage[8];
            transpose_eight_lanes(simd_tag, matrix_storage);
            transpose_eight_lanes(simd_tag, lower_half_storage);

            const p_simd_vector upper_column_zero = matrix_storage[0];
            const p_simd_vector upper_column_one = matrix_storage[1];
            const p_simd_vector upper_column_two = matrix_storage[2];
            const p_simd_vector upper_column_three = matrix_storage[3];
            const p_simd_vector upper_column_four = matrix_storage[4];
            const p_simd_vector upper_column_five = matrix_storage[5];
            const p_simd_vector upper_column_six = matrix_storage[6];
            const p_simd_vector upper_column_seven = matrix_storage[7];
            const p_simd_vector lower_column_zero = matrix_storage[8];
            const p_simd_vector lower_column_one = matrix_storage[9];
            const p_simd_vector lower_column_two = matrix_storage[10];
            const p_simd_vector lower_column_three = matrix_storage[11];
            const p_simd_vector lower_column_four = matrix_storage[12];
            const p_simd_vector lower_column_five = matrix_storage[13];
            const p_simd_vector lower_column_six = matrix_storage[14];
            const p_simd_vector lower_column_seven = matrix_storage[15];

            const hn::RepartitionToWideX3<p_simd_tag> triple_wide_simd_tag;

            matrix_storage[0] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_zero,
                lower_column_zero);
            matrix_storage[1] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_zero,
                lower_column_zero);
            matrix_storage[2] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_one,
                lower_column_one);
            matrix_storage[3] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_one,
                lower_column_one);
            matrix_storage[4] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_two,
                lower_column_two);
            matrix_storage[5] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_two,
                lower_column_two);
            matrix_storage[6] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_three,
                lower_column_three);
            matrix_storage[7] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_three,
                lower_column_three);
            matrix_storage[8] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_four,
                lower_column_four);
            matrix_storage[9] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_four,
                lower_column_four);
            matrix_storage[10] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_five,
                lower_column_five);
            matrix_storage[11] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_five,
                lower_column_five);
            matrix_storage[12] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_six,
                lower_column_six);
            matrix_storage[13] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_six,
                lower_column_six);
            matrix_storage[14] = interleave_matrix_halves<false>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_seven,
                lower_column_seven);
            matrix_storage[15] = interleave_matrix_halves<true>(
                simd_tag,
                triple_wide_simd_tag,
                upper_column_seven,
                lower_column_seven);
        }
    }

public:
    /**
     * @brief Binds a matrix wrapper to caller-owned vector storage.
     *
     * @param[in,out] matrix_storage Storage for at least `hn::Lanes(p_simd_tag{})` vectors.
     */
    explicit square_lane_matrix(p_matrix_storage matrix_storage)
        : matrix_storage_(matrix_storage) {}

    /**
     * @brief Loads one SIMD row from each of the active row offsets.
     *
     * Smaller unsigned source elements are widened to the matrix lane type.
     * @param[in] simd_tag Tag defining the number of rows and columns.
     * @param[in] offsets Source element offset for each row.
     * @param[in] source Readable source elements.
     * @return Number of loaded rows.
     * @pre There are at least Lanes(simd_tag) offsets and every row is readable.
     */
    template <typename p_value_type, std::size_t p_offset_count>
    constexpr auto load_rows_at_offsets(p_simd_tag& simd_tag,
                                        const std::array<std::size_t, p_offset_count> offsets,
                                        const p_value_type* source) -> std::size_t {
        const std::size_t lane_count = hn::Lanes(simd_tag);
        for (std::size_t row_index = 0; row_index < lane_count; ++row_index) {
            if constexpr (std::same_as<std::remove_cv_t<p_value_type>,
                                       hn::TFromD<p_simd_tag>>) {
                matrix_storage_[row_index] =
                    hn::LoadU(simd_tag, &source[offsets[row_index]]);
            } else {
                using lane_type = hn::TFromD<p_simd_tag>;
                using source_type = std::remove_cv_t<p_value_type>;
                using destination_simd_tag = std::remove_cvref_t<p_simd_tag>;
                using source_simd_tag = hn::Rebind<source_type, destination_simd_tag>;
                static_assert(std::unsigned_integral<source_type>
                              && sizeof(lane_type) >= sizeof(p_value_type),
                    "matrix lane conversion only supports unsigned widening");
                static_assert(hn::MaxLanes(source_simd_tag{})
                                  == hn::MaxLanes(p_simd_tag{}),
                    "rebound source descriptor must preserve lane count");
                const source_simd_tag source_tag;
                matrix_storage_[row_index] = hn::PromoteTo(
                    simd_tag,
                    hn::LoadU(source_tag, &source[offsets[row_index]]));
            }
        }
        return lane_count;
    }

    /**
     * @brief Loads rows from available source elements and fills missing lanes with zero.
     *
     * Processes every supplied offset, including offsets beyond the active
     * matrix width. Smaller unsigned source elements are widened.
     *
     * @param[in] simd_tag Tag defining row width.
     * @param[in] available_value_count Readable source element count.
     * @param[in] offsets Source element offset for each row.
     * @param[in] source Source elements.
     * @return Full row count and valid lane count of the last partial row.
     * @pre At least Lanes(simd_tag) offsets are required. Matrix storage must
     * hold p_offset_count rows, and adding a lane count to an offset must not overflow.
     */
    template <typename p_value_type, std::size_t p_offset_count>
    constexpr auto
    load_bounded_rows_at_offsets(p_simd_tag& simd_tag,
                                 std::size_t available_value_count,
                                 const std::array<std::size_t, p_offset_count> offsets,
                                 const p_value_type* source)
        -> std::pair<std::size_t, std::size_t> {
        const std::size_t lane_count = hn::Lanes(simd_tag);
        if (p_offset_count < lane_count) [[unlikely]] {
            std::abort();
        }

        std::size_t partial_lane_count = 0u;
        std::size_t full_row_count = 0u;
        std::size_t row_index = 0u;
        using lane_type = hn::TFromD<p_simd_tag>;
        const auto load_source_values = [&simd_tag](const p_value_type* values,
                                                    std::size_t value_count) {
            if constexpr (std::same_as<std::remove_cv_t<p_value_type>, lane_type>) {
                return value_count == hn::Lanes(simd_tag)
                    ? hn::LoadU(simd_tag, values)
                    : hn::LoadN(simd_tag, values, value_count);
            } else {
                using source_type = std::remove_cv_t<p_value_type>;
                using destination_simd_tag = std::remove_cvref_t<p_simd_tag>;
                using source_simd_tag = hn::Rebind<source_type, destination_simd_tag>;
                static_assert(std::unsigned_integral<source_type>
                              && sizeof(lane_type) >= sizeof(p_value_type),
                    "matrix lane conversion only supports unsigned widening");
                static_assert(hn::MaxLanes(source_simd_tag{})
                                  == hn::MaxLanes(p_simd_tag{}),
                    "rebound source descriptor must preserve lane count");
                const source_simd_tag source_tag;
                const auto source_values = value_count == hn::Lanes(source_tag)
                    ? hn::LoadU(source_tag, values)
                    : hn::LoadN(source_tag, values, value_count);
                return hn::PromoteTo(simd_tag, source_values);
            }
        };
        for (; row_index < p_offset_count; ++row_index) {
            const std::size_t source_offset = offsets[row_index];
            if (source_offset + lane_count > available_value_count) [[unlikely]] {
                if (source_offset >= available_value_count) [[unlikely]] {
                    matrix_storage_[row_index] = hn::Zero(simd_tag);
                } else {
                    const std::size_t available_lane_count =
                        available_value_count - source_offset;
                    matrix_storage_[row_index] = load_source_values(
                        &source[source_offset], available_lane_count);
                    partial_lane_count = available_lane_count;
                }
            } else {
                matrix_storage_[row_index] = load_source_values(
                    &source[source_offset], lane_count);
                ++full_row_count;
            }
        }
        for (; row_index < lane_count; ++row_index) {
            matrix_storage_[row_index] = hn::Zero(simd_tag);
        }
        return {full_row_count, partial_lane_count};
    }

    /**
     * @brief Transposes the bound square lane matrix in place.
     *
     * @param[in] simd_tag Highway descriptor defining matrix dimensions.
     * @warning Terminates for unsupported lane counts or element widths.
     */
    constexpr auto transpose_in_place(p_simd_tag& simd_tag) -> void {
        const std::size_t lane_count = hn::Lanes(simd_tag);
        if constexpr (std::same_as<hn::TFromD<p_simd_tag>, std::uint64_t>) {
            if (lane_count > 2u) {
                // Use the half-width transpose for uint64_t because Highway has no
                // wider integer lane type for the widening-based transpose.
                if (!transpose_u64_lane_matrix(simd_tag, &matrix_storage_[0])) {
                    std::abort();
                }
                return;
            }
        }
        if (lane_count == 1u) {
            transpose_one_lane(simd_tag, matrix_storage_);
        } else if (lane_count == 2u) {
            transpose_two_lanes(simd_tag, matrix_storage_);
        } else if (lane_count == 4u) {
            transpose_four_lanes(simd_tag, matrix_storage_);
        } else if (lane_count == 8u) {
            transpose_eight_lanes(simd_tag, matrix_storage_);
        } else if (lane_count == 16u) {
            transpose_sixteen_lanes(simd_tag, matrix_storage_);
        } else {
            std::abort();
        }
    }
};

} // namespace target
} // namespace bioseq::simd
