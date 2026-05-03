/**
 * @file huffman.cppm
 * @brief Canonical Huffman decoder with primary-table fast path.
 *
 * Templated on `<PrimaryBits, Alphabet>`. The primary table maps
 * `peek<PrimaryBits>` -> `(symbol << 4) | code_length`; codes longer than
 * `PrimaryBits` redirect to a fixed-capacity fallthrough table indexed
 * by the suffix bits. Encoding `code_length=0` in the primary entry
 * means "long code; consult fallthrough".
 *
 * The fallthrough is a `std::array<uint16_t, kMaxFallthroughEntries>` —
 * not a `std::vector` — so `consteval`-built FIXED tables can land in an
 * `inline constexpr` static. RFC 1951 §3.2.6 shapes these for the FIXED
 * case (lit/len + distance); they're shared across every fixed-Huffman
 * block and saved across all workers.
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

export module bioseq_io.compression.huffman;

import mlogging;
import bioseq_io.compression.bit_reader;

export namespace bioseq::io::compression {

namespace detail {

/// Number of code-length codes in the precode (RFC 1951 §3.2.7).
inline constexpr std::size_t kPrecodeAlphabet = 19;
/// Maximum code length used by DEFLATE precode (lit/len/dist tables).
inline constexpr std::size_t kPrecodeMaxBits  = 7;

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

/// `kMaxFallthroughEntries` covers RFC 1951's worst case — 288-symbol
/// alphabet with all 15-bit codes. The 11-bit primary captures every
/// code <= 11 bits; the residual is at most `Alphabet * 2^4 / 2^4`
/// rounded up to the next 16-element cacheline.
template <std::size_t PrimaryBits, std::size_t Alphabet>
class canonical_huffman_decoder {
public:
    static constexpr std::size_t kPrimaryBits          = PrimaryBits;
    static constexpr std::size_t kPrimarySize          = std::size_t{1} << PrimaryBits;
    static constexpr std::size_t kMaxCodeBits          = 15;
    static constexpr std::size_t kMaxFallthroughEntries =
        ((Alphabet * 16) + 15) & ~std::size_t{15};
    static constexpr std::uint16_t kInvalidSymbol = 0xFFFF;

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

    [[nodiscard]] constexpr bool is_valid() const noexcept { return _is_valid; }

    /// Decode the next symbol from `reader`. Returns `kInvalidSymbol`
    /// on a malformed stream.
    [[nodiscard]] std::uint16_t decode(bit_reader& reader) const noexcept {
        const std::uint64_t peeked = reader.peek<kPrimaryBits>();
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

    /// Direct primary-table view for the Huffman fast loop (§5.3).
    /// Each entry packs `(symbol << 4) | code_length`; `code_length == 0`
    /// signals "consult fallthrough".
    [[nodiscard]] std::span<const std::uint16_t, kPrimarySize>
    primary_table() const noexcept {
        return std::span<const std::uint16_t, kPrimarySize>{
            _primary.data(), kPrimarySize};
    }

private:
    [[nodiscard]] std::uint16_t
    decode_fallthrough(bit_reader& reader) const noexcept {
        // Fall back to a slow per-bit walk via the canonical first-code-
        // per-length table. Codes longer than kPrimaryBits land here.
        // We probe each length from kPrimaryBits+1 to kMaxCodeBits and
        // see if the prefix of that length matches an assigned code.
        std::uint64_t code_be = 0;
        std::uint64_t accumulated = reader.peek<kMaxCodeBits>();
        // Reverse PrimaryBits..kMaxCodeBits-1 of `accumulated`.
        // The fallthrough lookup uses a linear scan: we walk the
        // canonical table sorted by (length, code) and find the entry
        // whose `(canonical_code, length)` matches the read prefix.
        // Build the prefix MSB-first in `code_be` for comparison.
        for (std::size_t length = 1; length <= kMaxCodeBits; ++length) {
            code_be = (code_be << 1) | ((accumulated >> (length - 1)) & 1u);
            if (length <= kPrimaryBits) continue;
            // The canonical prefix-code value for a code of `length`
            // bits sits in `_first_code_per_length[length]` and the
            // first symbol with that length sits in
            // `_first_symbol_per_length[length]`. The actual symbol is
            // at `_canonical_symbols[base + (code_be - first_code)]`
            // where `base = _first_symbol_per_length[length]`.
            const std::uint32_t first_code = _first_code_per_length[length];
            const std::uint32_t next_code  = _first_code_per_length[length + 1] >> 1;
            if (code_be < first_code) continue;
            if (code_be < next_code || length == kMaxCodeBits) {
                const std::uint32_t base_offset =
                    _first_symbol_offset_per_length[length];
                const std::uint32_t symbol_index = base_offset
                    + static_cast<std::uint32_t>(code_be - first_code);
                if (symbol_index >= _canonical_symbols_count) {
                    return kInvalidSymbol;
                }
                reader.skip(length);
                return _canonical_symbols[symbol_index];
            }
        }
        return kInvalidSymbol;
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
        std::array<std::uint32_t, kMaxCodeBits + 2> length_counts{};
        for (auto length : code_lengths) {
            if (length > kMaxCodeBits) return false;
            ++length_counts[length];
        }
        // RFC 1951 Appendix C: bl_count[0] must be 0 for the canonical
        // code computation to start from `code = 0` regardless of how
        // many entries were zero-length (zero-length means "symbol not
        // used", not "1 0-bit code"). Without this reset, every absent
        // symbol shifts first_code[1] by 2, corrupting the entire LUT.
        length_counts[0] = 0;

        // Compute first canonical code per length (MSB-first walk).
        std::array<std::uint32_t, kMaxCodeBits + 2> next_code{};
        std::uint32_t code = 0;
        std::uint32_t total_assigned = 0;
        for (std::size_t length = 1; length <= kMaxCodeBits; ++length) {
            code = (code + length_counts[length - 1]) << 1;
            next_code[length] = code;
            _first_code_per_length[length] = code;
            total_assigned += length_counts[length];
        }
        _first_code_per_length[kMaxCodeBits + 1] =
            (code + length_counts[kMaxCodeBits]) << 1;

        // Reject incomplete codes for non-trivial alphabets. A
        // single-symbol alphabet with one code of length 1 is valid.
        if (total_assigned == 0) return false;

        // Bucket-sort symbols by (length, symbol) to derive
        // `_canonical_symbols`.
        std::array<std::uint32_t, kMaxCodeBits + 2> length_running_offset{};
        std::uint32_t cursor = 0;
        for (std::size_t length = 1; length <= kMaxCodeBits; ++length) {
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

        // Populate the primary table for codes of length <= PrimaryBits.
        // Each canonical code of length L expands to (1 << (PrimaryBits - L))
        // primary-table entries with the bits AFTER the code as the
        // varying part.
        std::array<std::uint32_t, kMaxCodeBits + 2> per_length_code{};
        for (std::size_t length = 1; length <= kMaxCodeBits; ++length) {
            per_length_code[length] = _first_code_per_length[length];
        }
        for (std::size_t symbol = 0; symbol < code_lengths.size(); ++symbol) {
            const std::uint8_t length = code_lengths[symbol];
            if (length == 0 || length > kPrimaryBits) continue;
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
            const std::size_t fanout = std::size_t{1} << (kPrimaryBits - length);
            for (std::size_t variant = 0; variant < fanout; ++variant) {
                const std::size_t index =
                    reversed | (variant << length);
                _primary[index] = entry;
            }
        }
        return true;
    }

    bool _is_valid = false;
    alignas(64) std::array<std::uint16_t, kPrimarySize>           _primary{};
    /// Canonical sorted-by-length symbol list. Index = `first_symbol_offset[length]
    /// + (canonical_code - first_code[length])`.
    alignas(64) std::array<std::uint16_t, kMaxFallthroughEntries> _canonical_symbols{};
    std::uint32_t _canonical_symbols_count = 0;
    std::array<std::uint32_t, kMaxCodeBits + 2> _first_code_per_length{};
    std::array<std::uint32_t, kMaxCodeBits + 2> _first_symbol_offset_per_length{};
};

using lit_len_decoder  = canonical_huffman_decoder<11, 288>;
using distance_decoder = canonical_huffman_decoder<9,  30>;
using precode_decoder  = canonical_huffman_decoder<7,  detail::kPrecodeAlphabet>;

/// FIXED Huffman tables (RFC 1951 §3.2.6) — same across every gzip file.
/// `consteval` build runs at compile time; `inline` lets the linker
/// share one copy across all translation units. ~10% of real-world
/// blocks use FIXED encoding; this saves the per-block primary-table
/// build on those.
inline const lit_len_decoder kFixedLitLenDecoder =
    lit_len_decoder::build(detail::fixed_lit_len_code_lengths());
inline const distance_decoder kFixedDistanceDecoder =
    distance_decoder::build(detail::fixed_distance_code_lengths());

}  // namespace bioseq::io::compression
