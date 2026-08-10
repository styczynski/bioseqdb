/**
 * @file simd_iteration_geometry.cppm
 * @brief Target-independent lane geometry for packed-sequence iteration.
 */
module;

export module bioseq.sequences.simd_iteration_geometry;

import std;

export namespace bioseq::simd::iteration_geometry {

/**
 * @brief Divide upward and report the padding introduced by rounding.
 * @tparam p_value_type Integer-like arithmetic type.
 * @param value Dividend.
 * @param divisor Non-zero divisor.
 * @return Pair of rounded quotient and `quotient * divisor - value`.
 */
template<class p_value_type>
[[nodiscard]] inline constexpr std::pair<p_value_type, p_value_type> int_div_ceil(
    const p_value_type& value, const p_value_type& divisor)
{
    const p_value_type quotient = (value + divisor - 1) / divisor;
    return {quotient, quotient * divisor - value};
}

/**
 * @brief Partition a packed sequence into balanced lane start offsets.
 *
 * Offsets are returned in ascending order and unused compile-time lanes stay
 * zero. The caller must provide `0 < lane_count <= p_maximum_lanes` and
 * `sequence_length >= context_units`.
 *
 * @tparam p_maximum_lanes Compile-time capacity of the offset array.
 * @param sequence_length Packed storage units in the sequence view.
 * @param lane_count Runtime lanes available for the active target.
 * @param context_units Packed units required as right-hand context.
 * @return Units assigned to each lane and the fixed-capacity offset array.
 */
template<std::size_t p_maximum_lanes>
[[nodiscard]] inline std::tuple<
    std::size_t, std::array<std::size_t, p_maximum_lanes>>
calculate_iteration_offsets(
    const std::size_t sequence_length,
    const std::size_t lane_count,
    const std::size_t context_units)
{
    const auto [units_per_lane, unused_padding] =
        int_div_ceil(sequence_length - context_units, lane_count);
    (void)unused_padding;
    const std::size_t max_offset =
        sequence_length - context_units - units_per_lane;
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
