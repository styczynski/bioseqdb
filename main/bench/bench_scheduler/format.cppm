/**
 * @file format.cppm
 * @brief Number-to-string helpers shared across bench harnesses.
 *
 * Every bench in the project formats counts, durations, byte
 * volumes, IPC ratios, and percentages the same way: fixed-width
 * columns, locale-independent thousands separators, auto-scaled
 * units (k/M/G/T or KB/MB/GB/s, ns/µs/ms/s). Pulling the helpers
 * into one module avoids the "did the chart builder format ns the
 * same way the report did?" cross-file audit — there is one
 * implementation of each.
 *
 * Naming: the helpers are kept under their pre-refactor names
 * (`fmt_duration_10`, `fmt_count`, ...) so the aligner-side report
 * code that calls them doesn't need to chase a rename. The `_10` /
 * `_8` digit suffixes encode the column width the helper targets so
 * the reader can size table headers without re-counting characters.
 */
module;

#include "common.h"

#include <cstdint>
#include <string>

export module bench.scheduler.format;

import mlogging;
import bench.scheduler.stats;

export {

namespace bioseq::bench_scheduler::format {

/// Format a nanosecond duration into a 10-char unit-aware string.
/// The cut-overs match Catch2's own BENCHMARK table so identical
/// magnitudes always render with identical units across the two
/// layers of output.
[[nodiscard]] inline std::string fmt_duration_10(std::uint64_t ns) noexcept {
    if (ns < 10'000ull)
        return fmt::format("{:>7} ns", ns);
    if (ns < 10'000'000ull)
        return fmt::format("{:>6.2f} us", static_cast<double>(ns) / 1'000.0);
    if (ns < 10'000'000'000ull)
        return fmt::format("{:>6.3f} ms", static_cast<double>(ns) / 1'000'000.0);
    return fmt::format("{:>7.3f} s", static_cast<double>(ns) / 1'000'000'000.0);
}

/// Format a count to a right-aligned, width-padded, thousands-separated
/// string. Wraps `bench.scheduler.stats::with_thousands_separator`.
[[nodiscard]] inline std::string fmt_count(std::uint64_t value, int width) {
    return fmt::format("{:>{}}", stats::with_thousands_separator(value), width);
}

/// Format a non-negative byte count with thousands separators and a
/// trailing " B" inside a fixed-width column.
[[nodiscard]] inline std::string fmt_bytes(std::uint64_t bytes, int width) {
    return fmt::format("{:>{}}", stats::with_thousands_separator(bytes) + " B", width);
}

/// Format a signed delta percentage with a forced sign so columns align.
[[nodiscard]] inline std::string fmt_signed_pct(double pct) {
    return fmt::format("{:+8.2f}%", pct);
}

/// Format an unsigned percentage from a fraction in [0, 1].
[[nodiscard]] inline std::string fmt_pct(double frac01) {
    return fmt::format("{:6.2f}%", frac01 * 100.0);
}

/// Format a throughput rate (work units / time) with auto-scaled units.
/// `bytes/ns == GB/s` (1 GB == 1e9 bytes, 1 s == 1e9 ns), so for byte-
/// typed loops `total_work_bytes / total_ns` *is* GB/s. The bench-level
/// loop's per-pair work is small enough that GB/s rounds to 0.000;
/// auto-step down to MB/s / KB/s / B/s so the column is informative
/// across magnitudes. Returns `-` when units are not bytes (caller
/// passes `is_byte_unit=false`) — GB/s on iteration-counted loops is
/// meaningless.
[[nodiscard]] inline std::string fmt_throughput_10(
    std::uint64_t total_work, std::uint64_t total_ns,
    bool is_byte_unit)
{
    if (!is_byte_unit) return fmt::format("{:>10}", "-");
    if (total_ns == 0) return fmt::format("{:>10}", "-");
    const double bytes_per_ns =
        static_cast<double>(total_work) / static_cast<double>(total_ns);
    if (bytes_per_ns >= 1.0)        return fmt::format("{:6.2f} GB/s", bytes_per_ns);
    if (bytes_per_ns >= 0.001)      return fmt::format("{:6.2f} MB/s", bytes_per_ns * 1000.0);
    if (bytes_per_ns >= 0.000001)   return fmt::format("{:6.2f} kB/s", bytes_per_ns * 1'000'000.0);
    return fmt::format("{:6.2f}  B/s", bytes_per_ns * 1'000'000'000.0);
}

/// Format an iter/s rate inside a 14-char column.
[[nodiscard]] inline std::string fmt_iters_per_sec(
    std::uint64_t total_iters, std::uint64_t total_ns)
{
    if (total_ns == 0) return fmt::format("{:>14}", "-");
    const double iters_per_sec =
        static_cast<double>(total_iters) * 1e9 / static_cast<double>(total_ns);
    return fmt::format("{:14.2f}", iters_per_sec);
}

/// Compact representation of a counter in k/M/G/T scale so wide PMU
/// columns fit in 12 chars without scientific notation.
[[nodiscard]] inline std::string fmt_compact_count(std::uint64_t value) {
    if (value == 0) return fmt::format("{:>12}", "0");
    if (value < 1'000ull)             return fmt::format("{:>12}", value);
    if (value < 1'000'000ull)         return fmt::format("{:>9.2f} k", value / 1'000.0);
    if (value < 1'000'000'000ull)     return fmt::format("{:>9.2f} M", value / 1'000'000.0);
    if (value < 1'000'000'000'000ull) return fmt::format("{:>9.2f} G", value / 1'000'000'000.0);
    return fmt::format("{:>9.2f} T", value / 1'000'000'000'000.0);
}

/// IPC = total_instructions / total_cycles. Returns `-` when cycles == 0
/// (no PMU data, or zero-time region).
[[nodiscard]] inline std::string fmt_ipc(
    std::uint64_t total_cycles, std::uint64_t total_instructions)
{
    if (total_cycles == 0) return fmt::format("{:>7}", "-");
    const double ipc =
        static_cast<double>(total_instructions) / static_cast<double>(total_cycles);
    return fmt::format("{:>7.3f}", ipc);
}

/// Branch miss rate as a percent of total branches.
[[nodiscard]] inline std::string fmt_branch_miss_pct(
    std::uint64_t total_branches, std::uint64_t total_branch_misses)
{
    if (total_branches == 0) return fmt::format("{:>8}", "-");
    const double pct =
        100.0 * static_cast<double>(total_branch_misses)
              / static_cast<double>(total_branches);
    return fmt::format("{:>7.3f}%", pct);
}

/// Mean cycles per visit. Surfaces "this region runs for 30k cycles
/// per visit, 408 visits, no wonder it's hot" at a glance.
[[nodiscard]] inline std::string fmt_per_visit(
    std::uint64_t total, std::uint64_t visits)
{
    if (visits == 0) return fmt::format("{:>12}", "-");
    return fmt_compact_count(total / visits);
}

/// Width of the wide tables. Picked to match Catch2's own BENCHMARK
/// rule width (`CATCH_CONFIG_CONSOLE_WIDTH-1 == 199`) so the reader
/// sees a single visual column when the report block follows a
/// Catch2 BENCHMARK section.
inline constexpr int kTableWidth = 199;

/// Horizontal rule of `kTableWidth` dashes.
[[nodiscard]] inline std::string rule() {
    return std::string(kTableWidth, '-');
}

}  // namespace bioseq::bench_scheduler::format

}  // export
