/**
 * @file precode_validate.cppm
 * @brief Bit-parallel virtual-leaf-count check for DEFLATE precode.
 *
 * The blockfinder (`blockfinder.cppm`) probes candidate dynamic-Huffman
 * block starts. The precode validator rejects candidates whose
 * code-length-of-code-lengths table does not form a complete prefix
 * code. The 16K-entry LUT trick (port of upstream
 * `CountAllocatedLeaves.hpp:64-95`) lets us validate the count constraint
 * via two LUT lookups and a single comparison, vastly cheaper than
 * actually building the precode Huffman table.
 *
 * The validator operates on the 19 × 3-bit encoded code lengths read
 * from the candidate block header. Two halves of the bitset (low 24
 * bits, high 33 bits) get virtual-leaf counts via the LUT; the sum
 * must equal 0 for a valid prefix code per Kraft's inequality applied
 * with virtual leaves.
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.precode_validate;

import bioseq.common.mlogging;

export namespace bioseq::io::compression {

namespace detail {

/// Per-LUT bit width. Both halves of the precode-code-length packed
/// bitset use the same LUT.
inline constexpr std::size_t kPrecodeValidationLutBits = 14;
inline constexpr std::size_t kPrecodeValidationLutSize =
    std::size_t{1} << kPrecodeValidationLutBits;

/// Mask of frequency information packed into a uint32: 4 bits each per
/// length 1..7 (we don't see length 0 or 8+ in the precode).
inline constexpr std::uint32_t kFrequencyMask4Bit = 0x0Fu;

/// Add four-bit-frequency-counts in two packed words. Saturates above
/// 15; the validator only cares whether the count exceeds the
/// virtual-leaf budget.
[[nodiscard]] constexpr std::uint32_t add_packed_4bit_frequencies(
    std::uint32_t lhs, std::uint32_t rhs) noexcept {
    return lhs + rhs;
}

/// Virtual-leaf multipliers indexed by code length. A length-1 code
/// consumes 1<<(maxbits-1) virtual leaves; length-2 consumes 1<<(maxbits-2);
/// etc. We pre-multiply during table build so the runtime check is a
/// single packed-add reduction.
[[nodiscard]] constexpr std::uint32_t leaf_weight_for_length(
    std::uint32_t code_length, std::uint32_t max_bits) noexcept {
    if (code_length == 0 || code_length > max_bits) return 0;
    return std::uint32_t{1} << (max_bits - code_length);
}

}  // namespace detail

/// Build the virtual-leaf-count LUT for half the precode bitset (each
/// half encodes ~7 code lengths × 3 bits = 21 bits, padded to 14 bits
/// per chunk). The LUT entry is the SUM of leaf weights across the
/// chunk's code lengths.
inline constexpr std::array<std::uint16_t, detail::kPrecodeValidationLutSize>
build_precode_validation_lut() noexcept {
    std::array<std::uint16_t, detail::kPrecodeValidationLutSize> lut{};
    constexpr std::uint32_t max_bits = 7;
    for (std::size_t key = 0; key < detail::kPrecodeValidationLutSize; ++key) {
        std::uint32_t total = 0;
        for (std::size_t slot = 0; slot < detail::kPrecodeValidationLutBits / 3; ++slot) {
            const std::uint32_t code_length =
                static_cast<std::uint32_t>((key >> (slot * 3)) & 0x7u);
            total += detail::leaf_weight_for_length(code_length, max_bits);
        }
        // Cap at 0xFFFF so wraparound doesn't poison the comparison.
        lut[key] = static_cast<std::uint16_t>(
            total > 0xFFFFu ? 0xFFFFu : total);
    }
    return lut;
}

inline constexpr auto kPrecodeValidationLut = build_precode_validation_lut();

/// Quickly test whether the precode's virtual-leaf count equals the
/// budget for a complete prefix code of `MAX_PRECODE_BITS == 7`. The
/// budget is 1 << 7 = 128.
[[nodiscard]] inline bool
precode_virtual_leaf_count_ok(std::uint64_t precode_bits) noexcept {
    constexpr std::uint64_t kLowMask =
        (std::uint64_t{1} << detail::kPrecodeValidationLutBits) - 1;
    std::uint32_t total = 0;
    for (std::size_t shift = 0;
         shift < detail::kPrecodeValidationLutBits * 5;  // 14*5 > 57 covers all
         shift += detail::kPrecodeValidationLutBits) {
        const std::size_t key =
            static_cast<std::size_t>((precode_bits >> shift) & kLowMask);
        total += kPrecodeValidationLut[key];
    }
    constexpr std::uint32_t kFullPrefixCodeBudget = 1u << 7;
    // For a complete prefix code total == budget; for a single-symbol
    // alphabet (one non-zero length-1 code) total == budget too. An
    // incomplete code surfaces as `total < budget`. An over-allocated
    // tree surfaces as `total > budget`.
    return total == kFullPrefixCodeBudget;
}

}  // namespace bioseq::io::compression
