/**
 * @file simd_iteration_geometry.cppm
 * @brief Computes packed-sequence iteration offsets for SIMD lanes.
 */
module;

export module bioseq.sequences.simd_iteration_geometry;

import std;

/** Helpers for dividing packed-sequence work between SIMD lanes. */
export namespace bioseq::simd::iteration_geometry {

/**
 * @brief Divides upward and returns the unused space in the rounded size.
 * @param value Nonnegative number of units.
 * @param divisor Positive units per group.
 * @return Rounded-up group count and its padding.
 * @pre Intermediate sums and products fit p_value_type.
 */
template <class p_value_type>
[[nodiscard]] inline constexpr std::pair<p_value_type, p_value_type>
int_div_ceil(const p_value_type& value, const p_value_type& divisor) {
    const p_value_type quotient = (value + divisor - 1) / divisor;
    return {quotient, quotient * divisor - value};
}

/**
 * @brief Computes lane starts while leaving room for right-hand context.
 *
 * Active lane offsets increase from left to right. Short inputs can make early
 * lanes overlap or start at zero. Unused entries remain zero.
 * @param sequence_length Sequence size in packed storage units.
 * @param lane_count Active lanes, between one and p_maximum_lanes.
 * @param context_units Context required after each lane, at most sequence_length.
 * @return Units per lane and the fixed-size array of start offsets.
 * @pre Rounding arithmetic fits size_t.
 */
template <std::size_t p_maximum_lanes>
[[nodiscard]] inline std::tuple<std::size_t, std::array<std::size_t, p_maximum_lanes>>
calculate_iteration_offsets(const std::size_t sequence_length,
                            const std::size_t lane_count,
                            const std::size_t context_units) {
    const auto [units_per_lane, unused_padding] =
        int_div_ceil(sequence_length - context_units, lane_count);
    (void)unused_padding;
    const std::size_t max_offset = sequence_length - context_units - units_per_lane;
    std::size_t lane = lane_count - 1;
    std::array<std::size_t, p_maximum_lanes> offsets{};
    offsets[lane] = max_offset;
    while (lane >= 1) {
        --lane;
        if (offsets[lane + 1] < units_per_lane) {
            break;
        }
        offsets[lane] = offsets[lane + 1] - units_per_lane;
    }
    for (; lane > 0; --lane) {
        offsets[lane] = 0;
    }
    return {units_per_lane, offsets};
}

} // namespace bioseq::simd::iteration_geometry
