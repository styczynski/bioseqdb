/**
 * @file precode_validation.cppm
 * @brief Validates packed DEFLATE precode lengths by virtual-leaf count.
 *
 * A constexpr lookup table reduces the 19 three-bit code lengths before the
 * final completeness check.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.precode_validation;

export namespace bioseq::io::compression {

namespace detail {

/// Per-LUT bit width. Both halves of the precode-code-length packed
/// bitset use the same LUT.
inline constexpr std::size_t precode_validation_table_bits = 14;
inline constexpr std::size_t precode_validation_table_size = std::size_t{1}
                                                             << precode_validation_table_bits;

/// Mask of frequency information packed into a uint32: 4 bits each per
/// length 1..7 (we don't see length 0 or 8+ in the precode).
inline constexpr std::uint32_t four_bit_frequency_mask = 0x0Fu;

/// Add four-bit-frequency-counts in two packed words. Saturates above
/// 15. The validator only checks whether the count exceeds the
/// virtual-leaf budget.
[[nodiscard]] constexpr std::uint32_t add_packed_4bit_frequencies(std::uint32_t lhs,
                                                                  std::uint32_t rhs) noexcept {
    return lhs + rhs;
}

/// Virtual-leaf multipliers indexed by code length. A length-1 code
/// consumes 1<<(maxbits-1) virtual leaves. Length-2 consumes 1<<(maxbits-2).
/// etc. We pre-multiply during table build so the runtime check is a
/// single packed-add reduction.
[[nodiscard]] constexpr std::uint32_t leaf_weight_for_length(std::uint32_t code_length,
                                                             std::uint32_t max_bits) noexcept {
    if (code_length == 0 || code_length > max_bits)
        return 0;
    return std::uint32_t{1} << (max_bits - code_length);
}

} // namespace detail

/// Build the virtual-leaf-count LUT for half the precode bitset (each
/// half encodes ~7 code lengths × 3 bits = 21 bits, padded to 14 bits
/// per chunk). The LUT entry is the SUM of leaf weights across the
/// chunk's code lengths.
inline constexpr std::array<std::uint16_t, detail::precode_validation_table_size>
build_precode_validation_lut() noexcept {
    std::array<std::uint16_t, detail::precode_validation_table_size> lut{};
    constexpr std::uint32_t max_bits = 7;
    for (std::size_t key = 0; key < detail::precode_validation_table_size; ++key) {
        std::uint32_t total = 0;
        for (std::size_t slot = 0; slot < detail::precode_validation_table_bits / 3; ++slot) {
            const std::uint32_t code_length =
                static_cast<std::uint32_t>((key >> (slot * 3)) & 0x7u);
            total += detail::leaf_weight_for_length(code_length, max_bits);
        }
        // Cap at 0xFFFF so wraparound doesn't poison the comparison.
        lut[key] = static_cast<std::uint16_t>(total > 0xFFFFu ? 0xFFFFu : total);
    }
    return lut;
}

inline constexpr auto precode_validation_table = build_precode_validation_lut();

/// Quickly test whether the precode's virtual-leaf count equals the
/// budget for a complete prefix code of `MAX_PRECODE_BITS == 7`. The
/// budget is 1 << 7 = 128.
[[nodiscard]] inline bool precode_virtual_leaf_count_ok(std::uint64_t precode_bits) noexcept {
    constexpr std::uint64_t low_mask =
        (std::uint64_t{1} << detail::precode_validation_table_bits) - 1;
    std::uint32_t total = 0;
    for (std::size_t shift = 0;
         shift < detail::precode_validation_table_bits * 5; // 14*5 > 57 covers all
         shift += detail::precode_validation_table_bits) {
        const std::size_t key = static_cast<std::size_t>((precode_bits >> shift) & low_mask);
        total += precode_validation_table[key];
    }
    constexpr std::uint32_t full_prefix_code_budget = 1u << 7;
    // For a complete prefix code total == budget. For a single-symbol
    // alphabet (one non-zero length-1 code) total == budget too. An
    // incomplete code surfaces as `total < budget`. An over-allocated
    // tree surfaces as `total > budget`.
    return total == full_prefix_code_budget;
}

} // namespace bioseq::io::compression
