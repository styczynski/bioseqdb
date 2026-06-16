/**
 * @file instrumentation/histogram.cppm
 * @brief Log-linear histogram template (Part 19.11 of the design doc).
 *
 * Histogram16 (16-bucket pure log2) is the default flavour for region
 * aggregates and lives in bioseq_instr.aggregate. This module exposes the
 * configurable log-linear variant for opt-in regions that need finer
 * percentile resolution.
 */
module;

#include <array>
#include <bit>
#include <cstdint>

export module bioseq.instr.histogram;

export namespace bioseq::instr {

/// Log-linear sketch: MagnitudeBits magnitudes × (1 << SubBits) sub-buckets.
/// Defaults give 64 magnitudes × 8 sub-buckets = 512 buckets, ~2 KiB.
template<unsigned MagnitudeBits = 64u, unsigned SubBits = 3u>
struct Histogram {
    static_assert(MagnitudeBits <= 64u);
    static_assert(SubBits < 8u);
    static constexpr unsigned NumBuckets = MagnitudeBits << SubBits;

    std::array<std::uint32_t, NumBuckets> buckets{};

    [[gnu::always_inline]]
    void add(std::uint64_t v) noexcept {
        if (v == 0) {
            ++buckets[0];
            return;
        }
        const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v));
        const unsigned shift = msb >= SubBits ? msb - SubBits : 0u;
        const unsigned sub = static_cast<unsigned>(
            (v >> shift) & ((1ull << SubBits) - 1ull));
        const unsigned idx = (msb << SubBits) | sub;
        if (idx < NumBuckets) ++buckets[idx];
    }
};

}  // namespace bioseq::instr
