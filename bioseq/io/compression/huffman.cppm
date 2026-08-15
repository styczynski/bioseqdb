/**
 * @file huffman.cppm
 * @brief Canonical Huffman decoder with primary-table fast path.
 *
 * Templated on `<p_primary_bits, p_alphabet_size>`. The primary table maps
 * `peek<p_primary_bits>` -> `(symbol << 4) | code_length`; codes longer than
 * `p_primary_bits` redirect to a fixed-capacity fallthrough table indexed
 * by the suffix bits. Encoding `code_length=0` in the primary entry
 * means "long code; consult fallthrough".
 *
 * The fallthrough is a `std::array<uint16_t, maximum_fallthrough_entries>` —
 * not a `std::vector` — so `consteval`-built FIXED tables can land in an
 * `inline constexpr` static. RFC 1951 §3.2.6 shapes these for the FIXED
 * case (lit/len + distance); they're shared across every fixed-Huffman
 * block and saved across all workers.
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
/// Maximum code length used by DEFLATE precode (lit/len/dist tables).
inline constexpr std::size_t precode_maximum_bits  = 7;

/// FIXED lit/len Huffman code lengths from RFC 1951 §3.2.6:
///   symbols   0-143 → 8 bits
///   symbols 144-255 → 9 bits
///   symbols 256-279 → 7 bits
///   symbols 280-287 → 8 bits
[[nodiscard]] consteval std::array<std::uint8_t, 288>
fixed_lit_len_code_lengths() noexcept {
    std::array<std::uint8_t, 288> code_lengths{};
    for (std::size_t symbol = 0;   symbol < 144; ++symbol) code_lengths[symbol] = 8;
    for (std::size_t symbol = 144; symbol < 256; ++symbol) code_lengths[symbol] = 9;
    for (std::size_t symbol = 256; symbol < 280; ++symbol) code_lengths[symbol] = 7;
    for (std::size_t symbol = 280; symbol < 288; ++symbol) code_lengths[symbol] = 8;
    return code_lengths;
}

/// FIXED distance Huffman code lengths: all 5 bits per RFC 1951 §3.2.6.
[[nodiscard]] consteval std::array<std::uint8_t, 30>
fixed_distance_code_lengths() noexcept {
    std::array<std::uint8_t, 30> code_lengths{};
    for (auto& code_length : code_lengths) code_length = 5;
    return code_lengths;
}

}  // namespace detail

/// `maximum_fallthrough_entries` covers RFC 1951's worst case — 288-symbol
/// alphabet with all 15-bit codes. The 11-bit primary captures every
/// code <= 11 bits; the residual is at most `p_alphabet_size * 2^4 / 2^4`
/// rounded up to the next 16-element cacheline.
template <std::size_t p_primary_bits, std::size_t p_alphabet_size>
class canonical_huffman_decoder {
public:
    static constexpr std::size_t primary_bit_count          = p_primary_bits;
    static constexpr std::size_t primary_table_size          = std::size_t{1} << p_primary_bits;
    static constexpr std::size_t maximum_code_bits          = 15;
    static constexpr std::size_t maximum_fallthrough_entries =
        ((p_alphabet_size * 16) + 15) & ~std::size_t{15};
    static constexpr std::uint16_t invalid_symbol = 0xFFFF;

    constexpr canonical_huffman_decoder() noexcept = default;

    /// Build a decoder from a span of code lengths. Returns a
    /// default-constructed (invalid) decoder if the code lengths don't
    /// form a complete prefix code; the caller surfaces that as a
    /// stream error.
    [[nodiscard]] static constexpr canonical_huffman_decoder build(
        std::span<const std::uint8_t> code_lengths) noexcept {
        canonical_huffman_decoder decoder{};
        decoder._is_valid = decoder.build_in_place(code_lengths);
        return decoder;
    }

    /// Validate code lengths without constructing lookup tables. This
    /// returns the same validity result as `build()`.
    [[nodiscard]] static constexpr bool is_valid_code_lengths(
        std::span<const std::uint8_t> code_lengths) noexcept {
        std::array<std::uint32_t, maximum_code_bits + 2> length_counts{};
        for (auto length : code_lengths) {
            if (length > maximum_code_bits) return false;
            ++length_counts[length];
        }
        length_counts[0] = 0;

        std::uint32_t total_assigned = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            total_assigned += length_counts[length];
        }
        if (total_assigned == 0) return false;

        std::uint32_t kraft_sum = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            kraft_sum += length_counts[length]
                       * (std::uint32_t{1} << (maximum_code_bits - length));
        }
        const std::uint32_t kraft_target =
            std::uint32_t{1} << maximum_code_bits;
        const bool single_one_bit =
            (total_assigned == 1u) && (length_counts[1] == 1u);
        return kraft_sum == kraft_target || single_one_bit;
    }

    /// Return whether construction produced a complete supported code tree.
    [[nodiscard]] constexpr bool is_valid() const noexcept { return _is_valid; }

    /// Decode the next symbol from `reader`. Returns `invalid_symbol`
    /// on a malformed stream.
    [[nodiscard]] std::uint16_t decode(bit_reader& reader) const noexcept {
        const std::uint64_t peeked = reader.peek<primary_bit_count>();
        const std::uint16_t entry  = _primary[peeked];
        const std::uint8_t  code_length =
            static_cast<std::uint8_t>(entry & 0x0Fu);
        const std::uint16_t symbol      =
            static_cast<std::uint16_t>(entry >> 4);
        if (code_length != 0) {
            reader.skip(code_length);
            return symbol;
        }
        return decode_fallthrough(reader);
    }

    /// Direct primary-table view for the Huffman fast loop.
    /// Each entry packs `(symbol << 4) | code_length`; `code_length == 0`
    /// signals "consult fallthrough".
    [[nodiscard]] std::span<const std::uint16_t, primary_table_size>
    primary_table() const noexcept {
        return std::span<const std::uint16_t, primary_table_size>{
            _primary.data(), primary_table_size};
    }

private:
    [[nodiscard]] std::uint16_t
    decode_fallthrough(bit_reader& reader) const noexcept {
        // Fall back to a slow per-bit walk via the canonical first-code-
        // per-length table. Codes longer than primary_bit_count land here.
        // We probe each length from primary_bit_count+1 to maximum_code_bits and
        // see if the prefix of that length matches an assigned code.
        std::uint64_t code_be = 0;
        std::uint64_t accumulated = reader.peek<maximum_code_bits>();
        // Reverse p_primary_bits..maximum_code_bits-1 of `accumulated`.
        // The fallthrough lookup uses a linear scan: we walk the
        // canonical table sorted by (length, code) and find the entry
        // whose `(canonical_code, length)` matches the read prefix.
        // Build the prefix MSB-first in `code_be` for comparison.
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            code_be = (code_be << 1) | ((accumulated >> (length - 1)) & 1u);
            if (length <= primary_bit_count) continue;
            // The canonical prefix-code value for a code of `length`
            // bits sits in `_first_code_per_length[length]` and the
            // first symbol with that length sits in
            // `_first_symbol_per_length[length]`. The actual symbol is
            // at `_canonical_symbols[base + (code_be - first_code)]`
            // where `base = _first_symbol_per_length[length]`.
            const std::uint32_t first_code = _first_code_per_length[length];
            const std::uint32_t next_code  = _first_code_per_length[length + 1] >> 1;
            if (code_be < first_code) continue;
            if (code_be < next_code || length == maximum_code_bits) {
                const std::uint32_t base_offset =
                    _first_symbol_offset_per_length[length];
                const std::uint32_t symbol_index = base_offset
                    + static_cast<std::uint32_t>(code_be - first_code);
                if (symbol_index >= _canonical_symbols_count) {
                    return invalid_symbol;
                }
                reader.skip(length);
                return _canonical_symbols[symbol_index];
            }
        }
        return invalid_symbol;
    }

    /// In-place build. Returns true on success.
    [[nodiscard]] constexpr bool build_in_place(
        std::span<const std::uint8_t> code_lengths) noexcept {
        // Reset state.
        for (auto& entry : _primary)               entry = 0;
        for (auto& entry : _canonical_symbols)     entry = 0;
        for (auto& entry : _first_code_per_length) entry = 0;
        for (auto& entry : _first_symbol_offset_per_length) entry = 0;
        _canonical_symbols_count = 0;

        // Count how many codes have each length.
        std::array<std::uint32_t, maximum_code_bits + 2> length_counts{};
        for (auto length : code_lengths) {
            if (length > maximum_code_bits) return false;
            ++length_counts[length];
        }
        // RFC 1951 Appendix C: bl_count[0] must be 0 for the canonical
        // code computation to start from `code = 0` regardless of how
        // many entries were zero-length (zero-length means "symbol not
        // used", not "1 0-bit code"). Without this reset, every absent
        // symbol shifts first_code[1] by 2, corrupting the entire LUT.
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
        _first_code_per_length[maximum_code_bits + 1] =
            (code + length_counts[maximum_code_bits]) << 1;

        // Reject incomplete codes for non-trivial alphabets. A
        // single-symbol alphabet with one code of length 1 is valid.
        if (total_assigned == 0) return false;

        // Completeness check (Kraft inequality). For a complete binary
        // prefix tree, sum_{L=1..MAX} N_L · 2^(MAX-L) must equal
        // 2^MAX exactly — anything less leaves uncovered leaves and
        // means some bit patterns map to no symbol (invalid_symbol on
        // decode). Anything more would imply collisions which the
        // build couldn't represent. Without this, a precode-valid
        // dynamic block can still build malformed literal/distance
        // tables that pass `read_header` but produce invalid_huffman_code
        // halfway through body decode.
        //
        // Special case: a single 1-bit code (the spec's degenerate
        // single-symbol alphabet, e.g. an all-zeros distance table
        // with one symbol of length 1) covers only half the tree but
        // is still considered well-formed by RFC 1951 §3.2.7.
        std::uint32_t kraft_sum = 0;
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            kraft_sum += length_counts[length]
                       * (std::uint32_t{1} << (maximum_code_bits - length));
        }
        const std::uint32_t kraft_target =
            std::uint32_t{1} << maximum_code_bits;
        const bool single_one_bit =
            (total_assigned == 1u) && (length_counts[1] == 1u);
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
            if (length == 0) continue;
            _canonical_symbols[length_running_offset[length]++] =
                static_cast<std::uint16_t>(symbol);
        }

        // Populate the primary table for codes of length <= p_primary_bits.
        // Each canonical code of length L expands to (1 << (p_primary_bits - L))
        // primary-table entries with the bits AFTER the code as the
        // varying part.
        std::array<std::uint32_t, maximum_code_bits + 2> per_length_code{};
        for (std::size_t length = 1; length <= maximum_code_bits; ++length) {
            per_length_code[length] = _first_code_per_length[length];
        }
        for (std::size_t symbol = 0; symbol < code_lengths.size(); ++symbol) {
            const std::uint8_t length = code_lengths[symbol];
            if (length == 0 || length > primary_bit_count) continue;
            const std::uint32_t canonical_code = per_length_code[length]++;
            // Reverse `canonical_code`'s low `length` bits to get the
            // LSB-first index into the primary table.
            std::uint32_t reversed = 0;
            for (std::size_t bit = 0; bit < length; ++bit) {
                reversed |= ((canonical_code >> bit) & 1u)
                          << (length - 1 - bit);
            }
            const std::uint16_t entry = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(symbol) << 4) | length);
            const std::size_t fanout = std::size_t{1} << (primary_bit_count - length);
            for (std::size_t variant = 0; variant < fanout; ++variant) {
                const std::size_t index =
                    reversed | (variant << length);
                _primary[index] = entry;
            }
        }
        return true;
    }

    bool _is_valid = false;
    alignas(64) std::array<std::uint16_t, primary_table_size>           _primary{};
    /// Canonical sorted-by-length symbol list. Index = `first_symbol_offset[length]
    /// + (canonical_code - first_code[length])`.
    alignas(64) std::array<std::uint16_t, maximum_fallthrough_entries> _canonical_symbols{};
    std::uint32_t _canonical_symbols_count = 0;
    std::array<std::uint32_t, maximum_code_bits + 2> _first_code_per_length{};
    std::array<std::uint32_t, maximum_code_bits + 2> _first_symbol_offset_per_length{};
};

using lit_len_decoder  = canonical_huffman_decoder<11, 288>;
using distance_decoder = canonical_huffman_decoder<9,  30>;
using precode_decoder  = canonical_huffman_decoder<7,  detail::precode_alphabet>;

/// FIXED Huffman tables (RFC 1951 §3.2.6) — same across every gzip file.
/// `consteval` build runs at compile time; `inline` lets the linker
/// share one copy across all translation units and avoid rebuilding
/// the primary tables for fixed-Huffman blocks.
inline const lit_len_decoder fixed_literal_length_decoder =
    lit_len_decoder::build(detail::fixed_lit_len_code_lengths());
inline const distance_decoder fixed_distance_decoder =
    distance_decoder::build(detail::fixed_distance_code_lengths());

}  // namespace bioseq::io::compression
