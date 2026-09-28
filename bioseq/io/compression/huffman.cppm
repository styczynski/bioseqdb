/**
 * @file huffman.cppm
 * @brief Decodes canonical Huffman codes with a lookup table for short codes.
 *
 * Longer codes are found from the first code and symbol index for each length.
 * All tables are stored inside the decoder.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

export module bioseq.io.compression.huffman;

import bioseq.io.compression.bit_reader;

export namespace bioseq::io::compression {

namespace detail {

/// Number of code-length codes in the precode (RFC 1951 §3.2.7).
inline constexpr std::size_t precode_alphabet = 19;
/// Maximum code length in  the DEFLATE code-length alphabet.
inline constexpr std::size_t precode_maximum_bits = 7;

/// FIXED lit/len Huffman code lengths from RFC 1951 §3.2.6:
///   symbols   0-143 → 8 bits
///   symbols 144-255 → 9 bits
///   symbols 256-279 → 7 bits
///   symbols 280-287 → 8 bits
[[nodiscard]] consteval std::array<std::uint8_t, 288> fixed_lit_len_code_lengths() noexcept {
    std::array<std::uint8_t, 288> code_lengths{};
    for (std::size_t symbol = 0; symbol < 144; ++symbol)
        code_lengths[symbol] = 8;
    for (std::size_t symbol = 144; symbol < 256; ++symbol)
        code_lengths[symbol] = 9;
    for (std::size_t symbol = 256; symbol < 280; ++symbol)
        code_lengths[symbol] = 7;
    for (std::size_t symbol = 280; symbol < 288; ++symbol)
        code_lengths[symbol] = 8;
    return code_lengths;
}

/// Five-bit lengths for the 30 supported fixed distance symbols.
[[nodiscard]] consteval std::array<std::uint8_t, 30> fixed_distance_code_lengths() noexcept {
    std::array<std::uint8_t, 30> code_lengths{};
    for (auto& code_length : code_lengths)
        code_length = 5;
    return code_lengths;
}

} // namespace detail

/// Decodes Huffman symbols using a prefix table and canonical code order.
/// Short codes need one table lookup. Longer codes are checked by length.
/// The template arguments set the prefix width and storage for the alphabet.
template <std::size_t p_primary_bits, std::size_t p_alphabet_size> class canonical_huffman_decoder {
  public:
    static constexpr std::size_t primary_bit_count = p_primary_bits;
    static constexpr std::size_t primary_table_size = std::size_t{1} << p_primary_bits;
    static constexpr std::size_t maximum_code_bits = 15;
    static constexpr std::size_t maximum_fallthrough_entries =
        ((p_alphabet_size * 16) + 15) & ~std::size_t{15};
    static constexpr std::uint16_t invalid_symbol = 0xFFFF;

    /// Construct an invalid decoder with empty lookup tables.
    constexpr canonical_huffman_decoder() noexcept = default;

    /// Builds a decoder from one code length per alphabet symbol.
    /// Complete code trees and a single one-bit code are accepted. Check
    /// `is_valid()` before decoding. Zero lengths mark unused symbols.
    /// @pre The span contains at most `p_alphabet_size` symbols.
    [[nodiscard]] static constexpr canonical_huffman_decoder
    build(std::span<const std::uint8_t> code_lengths) noexcept {
        canonical_huffman_decoder decoder{};
        decoder._is_valid = decoder.build_in_place(code_lengths);
        return decoder;
    }

    /// Validate code lengths without constructing lookup tables. This
    /// returns the same validity result as `build()`.
    [[nodiscard]] static constexpr bool
    is_valid_code_lengths(std::span<const std::uint8_t> code_lengths) noexcept {
        std::array<std::uint32_t, maximum_code_bits + 2> length_counts{};
        for (auto length : code_lengths) {
            if (length > maximum_code_bits)
                return false;
            ++length_counts[length];
        }
        length_counts[0] = 0;

        std::uint32_t total_assigned = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            total_assigned += length_counts[length];
        }
        if (total_assigned == 0)
            return false;

        std::uint32_t kraft_sum = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            kraft_sum += length_counts[length] * (std::uint32_t{1} << (maximum_code_bits - length));
        }
        const std::uint32_t kraft_target = std::uint32_t{1} << maximum_code_bits;
        const bool single_one_bit = (total_assigned == 1u) && (length_counts[1] == 1u);
        return kraft_sum == kraft_target || single_one_bit;
    }

    /// Reports whether building accepted the code lengths.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return _is_valid;
    }

    /// Reads the  next symbol, or returns `invalid_symbol` for an unmatched code.
    /// Call only on a valid decoder with, enough input bits available.
    [[nodiscard]] std::uint16_t decode(bit_reader& reader) const noexcept {
        const std::uint64_t peeked = reader.peek<primary_bit_count>();
        const std::uint16_t entry = _primary[peeked];
        const std::uint8_t code_length = static_cast<std::uint8_t>(entry & 0x0Fu);
        const std::uint16_t symbol = static_cast<std::uint16_t>(entry >> 4);
        if (code_length != 0) {
            reader.skip(code_length);
            return symbol;
        }
        return decode_fallthrough(reader);
    }

    /// Direct primary-table view for the Huffman fast loop.
    /// Each entry packs `(symbol << 4) | code_length`. `code_length == 0`
    /// signals "consult fallthrough".
    [[nodiscard]] std::span<const std::uint16_t, primary_table_size>
    primary_table() const noexcept {
        return std::span<const std::uint16_t, primary_table_size>{_primary.data(),
                                                                  primary_table_size};
    }

  private:
    /// Decode a symbol whose code is longer than the primary-table width.
    [[nodiscard]] std::uint16_t decode_fallthrough(bit_reader& reader) const noexcept {
        // Test longer prefixes against the canonical code range for each length.
        std::uint64_t code_be = 0;
        std::uint64_t accumulated = reader.peek<maximum_code_bits>();
        // Build the prefix in Huffman code order as input bits are read.
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            code_be = (code_be << 1) | ((accumulated >> (length - 1)) & 1u);
            if (length <= primary_bit_count)
                continue;
            // Codes of the same length occupy consecutive symbol entries. Use the
            // differece from the first code to find the matching symbol
            const std::uint32_t first_code = _first_code_per_length[length];
            const std::uint32_t next_code = _first_code_per_length[length + 1] >> 1;
            if (code_be < first_code)
                continue;
            if (code_be < next_code || length == maximum_code_bits) {
                const std::uint32_t base_offset = _first_symbol_offset_per_length[length];
                const std::uint32_t symbol_index =
                    base_offset + static_cast<std::uint32_t>(code_be - first_code);
                if (symbol_index >= _canonical_symbols_count) {
                    return invalid_symbol;
                }
                reader.skip(length);
                return _canonical_symbols[symbol_index];
            }
        }
        return invalid_symbol;
    }

    /// Fills the lookup tables and returns whether the  code lengths are supported.
    [[nodiscard]] constexpr bool
    build_in_place(std::span<const std::uint8_t> code_lengths) noexcept {
        // Reset state.
        for (auto& entry : _primary)
            entry = 0;
        for (auto& entry : _canonical_symbols)
            entry = 0;
        for (auto& entry : _first_code_per_length)
            entry = 0;
        for (auto& entry : _first_symbol_offset_per_length)
            entry = 0;
        _canonical_symbols_count = 0;

        // Count how many codes have each length.
        std::array<std::uint32_t, maximum_code_bits + 2> length_counts{};
        for (auto length : code_lengths) {
            if (length > maximum_code_bits)
                return false;
            ++length_counts[length];
        }
        // A zero length means the symbol is absent. Exclude these entries so
        // they do not shift the first assigned code.
        length_counts[0] = 0;

        // Compute first canonical code per length (MSB-first walk).
        std::array<std::uint32_t, maximum_code_bits + 2> next_code{};
        std::uint32_t code = 0;
        std::uint32_t total_assigned = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            code = (code + length_counts[length - 1]) << 1;
            next_code[length] = code;
            _first_code_per_length[length] = code;
            total_assigned += length_counts[length];
        }
        _first_code_per_length[maximum_code_bits + 1] = (code + length_counts[maximum_code_bits])
                                                        << 1;

        // Reject an alphabet with no assigned codes.
        if (total_assigned == 0)
            return false;

        // Check that the codes fill a binary tree without overlapping. A code
        // of length L occupies 2^(maximum_code_bits-L) slots at the deepest
        // level. Their sum must fill that level exactly. DEFLATE also allows
        // one symbol with a one-bit code, which fills half the tree.
        std::uint32_t kraft_sum = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            kraft_sum += length_counts[length] * (std::uint32_t{1} << (maximum_code_bits - length));
        }
        const std::uint32_t kraft_target = std::uint32_t{1} << maximum_code_bits;
        const bool single_one_bit = (total_assigned == 1u) && (length_counts[1] == 1u);
        if (kraft_sum != kraft_target && !single_one_bit) {
            return false;
        }

        // Bucket-sort symbols by (length, symbol) to derive
        // `_canonical_symbols`.
        std::array<std::uint32_t, maximum_code_bits + 2> length_running_offset{};
        std::uint32_t cursor = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            length_running_offset[length] = cursor;
            _first_symbol_offset_per_length[length] = cursor;
            cursor += length_counts[length];
        }
        _canonical_symbols_count = cursor;

        for (std::size_t symbol = 0; symbol < code_lengths.size(); ++symbol) {
            const std::uint8_t length = code_lengths[symbol];
            if (length == 0)
                continue;
            _canonical_symbols[length_running_offset[length]++] =
                static_cast<std::uint16_t>(symbol);
        }

        // Repeat each short-code entry for every  possible suffix in the
        // lookup window, so one lookup works without knowing the code length.
        std::array<std::uint32_t, maximum_code_bits + 2> per_length_code{};
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            per_length_code[length] = _first_code_per_length[length];
        }
        for (std::size_t symbol = 0; symbol < code_lengths.size(); ++symbol) {
            const std::uint8_t length = code_lengths[symbol];
            if (length == 0 || length > primary_bit_count)
                continue;
            const std::uint32_t canonical_code = per_length_code[length]++;
            // Reverse `canonical_code`'s low `length` bits to get the
            // LSB-first index into the primary table.
            std::uint32_t reversed = 0;
            for (std::size_t bit = 0; bit < length; ++bit) {
                reversed |= ((canonical_code >> bit) & 1u) << (length - 1 - bit);
            }
            const std::uint16_t entry =
                static_cast<std::uint16_t>((static_cast<std::uint32_t>(symbol) << 4) | length);
            const std::size_t fanout = std::size_t{1} << (primary_bit_count - length);
            for (std::size_t variant = 0; variant < fanout; ++variant) {
                const std::size_t index = reversed | (variant << length);
                _primary[index] = entry;
            }
        }
        return true;
    }

    bool _is_valid = false;
    alignas(64) std::array<std::uint16_t, primary_table_size> _primary{};
    /// Symbols in length order, then symbol order, for decoding long codes.
    alignas(64) std::array<std::uint16_t, maximum_fallthrough_entries> _canonical_symbols{};
    std::uint32_t _canonical_symbols_count = 0;
    std::array<std::uint32_t, maximum_code_bits + 2> _first_code_per_length{};
    std::array<std::uint32_t, maximum_code_bits + 2> _first_symbol_offset_per_length{};
};

using lit_len_decoder = canonical_huffman_decoder<11, 288>;
using distance_decoder = canonical_huffman_decoder<9, 30>;
using precode_decoder = canonical_huffman_decoder<7, detail::precode_alphabet>;

/// Shared fixed-code decoder objects. The DEFLATE reader uses the literal
/// table and decodes fixed distance symbols directly fom their five bits.
inline const lit_len_decoder fixed_literal_length_decoder =
    lit_len_decoder::build(detail::fixed_lit_len_code_lengths());
inline const distance_decoder fixed_distance_decoder =
    distance_decoder::build(detail::fixed_distance_code_lengths());

} // namespace bioseq::io::compression
