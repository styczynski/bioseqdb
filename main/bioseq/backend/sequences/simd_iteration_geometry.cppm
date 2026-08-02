/** Target-independent arithmetic shared by static and generated SIMD owners. */
module;

export module bioseq.sequences.simd_iteration_geometry;

import std;

export namespace bioseq::simd::iteration_geometry {

template<class T>
[[nodiscard]] inline constexpr std::pair<T, T> int_div_ceil(
    const T& value, const T& divisor)
{
    const T quotient = (value + divisor - 1) / divisor;
    return {quotient, quotient * divisor - value};
}

template<std::size_t MaxLanes>
[[nodiscard]] inline std::tuple<
    std::size_t, std::array<std::size_t, MaxLanes>>
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
    std::array<std::size_t, MaxLanes> offsets{};
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
