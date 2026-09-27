/**
 * @file precode_validation.cppm
 * @brief Lookup-table filter for candidate DEFLATE code-length headers.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.precode_validation;

export namespace bioseq::io::compression {

namespace detail {

/// Number of input bits used to select one precode lookup-table entry.
inline constexpr std::size_t precode_validation_table_bits = 14;
inline constexpr std::size_t precode_validation_table_size = std::size_t{1}
                                                             << precode_validation_table_bits;

/// Mask for one four-bit frequency field.
inline constexpr std::uint32_t four_bit_frequency_mask = 0x0Fu;

/// Adds two packed words with ordinary unsigned addition Carries are preserved.
[[nodiscard]] constexpr std::uint32_t add_packed_4bit_frequencies(std::uint32_t lhs,
                                                                  std::uint32_t rhs) noexcept {
    return lhs + rhs;
}

/// Counts the deepest tree slots occupied by a code of this length.
/// Zero or, unsupported lengths occupy none.
[[nodiscard]] constexpr std::uint32_t leaf_weight_for_length(std::uint32_t code_length,
                                                             std::uint32_t max_bits) noexcept {
    if (code_length == 0 || code_length > max_bits)
        return 0;
    return std::uint32_t{1} << (max_bits - code_length);
}

} // namespace detail

/// Builds the table of  tree-slot counts for four three-bit lengths.
/// Each 14-bit lookup key uses its low 12 bits.
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
        // Keep the stored count within the 16-bit table entry.
        lut[key] = static_cast<std::uint16_t>(total > 0xFFFFu ? 0xFFFFu : total);
    }
    return lut;
}

inline constexpr auto precode_validation_table = build_precode_validation_lut();

/// Filters a candidate header by comparing its lookup sum with the tkee capacity.
/// The current 14-bit chunking crosses three-bit code-length boundaries, so
/// callers still need a full Huffman-table validation.
[[nodiscard]] inline bool precode_virtual_leaf_count_ok(std::uint64_t precode_bits) noexcept {
    constexpr std::uint64_t low_mask =
        (std::uint64_t{1} << detail::precode_validation_table_bits) - 1;
    std::uint32_t total = 0;
    for (std::size_t shift = 0;
         shift < detail::precode_validation_table_bits * 5; // five lookup chunks
         shift += detail::precode_validation_table_bits) {
        const std::size_t key = static_cast<std::size_t>((precode_bits >> shift) & low_mask);
        total += precode_validation_table[key];
    }
    constexpr std::uint32_t full_prefix_code_budget = 1u << 7;
    // A complete tree with maximum code length seven has 128 deepest slots.
    return total == full_prefix_code_budget;
}

} // namespace bioseq::io::compression
