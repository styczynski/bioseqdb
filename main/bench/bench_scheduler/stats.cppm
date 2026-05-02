/**
 * @file stats.cppm
 * @brief Domain-agnostic numerical helpers shared across bench reports.
 *
 * The aligner bench's chart builder and the regression-rule engine
 * both want a percentile estimator over a sorted vector and an
 * arithmetic mean. The IO bench will want the same. Pulling them
 * into a single `bench_scheduler` module avoids the "did the chart
 * builder's mean match the regression checker's mean?" cross-file
 * audit — there is one implementation of each.
 *
 * Everything here is `noexcept` and operates on `std::span<const
 * double>` so callers can pass `std::vector`, `std::array`, or any
 * range without forcing a copy.
 */
module;

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>

export module bench.scheduler.stats;

export {

namespace bioseq::bench_scheduler::stats {

/**
 * @brief Linear-interpolation percentile estimator over a pre-sorted range.
 *
 * `percentile` is in [0, 1]. Returns 0.0 for an empty input — caller
 * decides whether that's "missing data" or a literal zero in their
 * domain. For a single-element input returns that element; for two
 * or more, linear-interpolates between the rank floor and ceiling.
 *
 * Pre-condition: `sorted_values` is sorted ascending. The function
 * does not check (asserting at every chart-emit call site would
 * regress wall-clock noticeably; sort once at the data-collection
 * boundary instead).
 */
[[nodiscard]] inline double percentile_sorted(
    std::span<const double> sorted_values, double percentile) noexcept
{
    if (sorted_values.empty()) return 0.0;
    if (sorted_values.size() == 1) return sorted_values.front();
    const double rank = percentile * static_cast<double>(sorted_values.size() - 1);
    const std::size_t low_index  = static_cast<std::size_t>(std::floor(rank));
    const std::size_t high_index = static_cast<std::size_t>(std::ceil(rank));
    const double fraction = rank - static_cast<double>(low_index);
    return sorted_values[low_index] +
           (sorted_values[high_index] - sorted_values[low_index]) * fraction;
}

/**
 * @brief Arithmetic mean of a numeric range. Returns 0.0 for empty.
 *
 * Plain accumulation; for the bench's input sizes (tens to thousands
 * of values per call) Kahan summation buys nothing measurable, and
 * the single-pass loop vectorises trivially.
 */
[[nodiscard]] inline double mean_of(std::span<const double> values) noexcept {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum / static_cast<double>(values.size());
}

/**
 * @brief Insert thousands separators (single-quote per ISO C++14)
 *        into a non-negative integer's decimal representation.
 *
 * Locale-independent so reports stay bit-identical across hosts.
 * Used by the report's table-emitter and the regression-violations
 * console table where a "12'345'678 cycles" reads cleaner than
 * "12345678".
 */
[[nodiscard]] inline std::string with_thousands_separator(std::uint64_t value) {
    std::string digits = std::to_string(value);
    if (digits.size() <= 3) return digits;
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3;
    std::size_t i = 0;
    if (lead != 0) {
        out.append(digits, 0, lead);
        i = lead;
    }
    while (i < digits.size()) {
        if (!out.empty()) out.push_back('\'');
        out.append(digits, i, 3);
        i += 3;
    }
    return out;
}

}  // namespace bioseq::bench_scheduler::stats

}  // export
