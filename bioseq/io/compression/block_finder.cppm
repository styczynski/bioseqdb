/**
 * @file block_finder.cppm
 * @brief DEFLATE dynamic-Huffman + uncompressed block scanners.
 *
 * Two scanner functions probe a `bit_reader`'s view for candidate
 * DEFLATE block starts. Used by `chunk_fetcher` to split a single
 * gzip-member stream into chunks that workers decode in parallel.
 *
 * `seek_dynamic_huffman_candidate` uses the 32 KiB skip-LUT
 * (`dynamic_huffman_skip_table.cppm`) to advance through the bitstream
 * 15 bits at a time. Negative LUT entries flag candidate offsets;
 * `validate_precode` then runs `precode_virtual_leaf_count_ok` to
 * weed out false candidates.
 *
 * `seek_uncompressed_block_candidate` probes for the LEN/NLEN sentinel
 * pattern at the start of an uncompressed block.
 */
module;


#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

export module bioseq.io.compression.block_finder;

import bioseq.io.compression.bit_reader;
import bioseq.io.compression.dynamic_huffman_skip_table;
import bioseq.io.compression.precode_validation;

export namespace bioseq::io::compression {

inline constexpr std::size_t block_finder_sentinel =
    std::numeric_limits<std::size_t>::max();

/// Validate a candidate dynamic-Huffman block by attempting to read the
/// 14-bit header (HLIT, HDIST, HCLEN) then checking that the precode
/// code-length table forms a complete prefix code. The validator is
/// uses a bounded LUT reduction before full table construction. Takes a copy
/// of the bit reader
/// — the caller's cursor is preserved.
[[nodiscard]] inline bool
validate_precode(bit_reader reader_copy) noexcept {
    // 3-bit block header BFINAL+BTYPE; we only care that BTYPE=10
    // (dynamic Huffman). Caller already filtered on that.
    if (reader_copy.bits_buffered() < 3 + 14) {
        reader_copy.refill();
    }
    const std::uint64_t header = reader_copy.read<3>();
    const std::uint64_t btype = (header >> 1) & 0x3u;
    if (btype != 0x2u) return false;

    // HLIT (5b), HDIST (5b), HCLEN (4b)
    const std::uint64_t hlit  = reader_copy.read<5>();
    const std::uint64_t hdist = reader_copy.read<5>();
    const std::uint64_t hclen = reader_copy.read<4>();
    (void)hlit;
    (void)hdist;
    const std::size_t precode_count = static_cast<std::size_t>(hclen) + 4;
    if (precode_count > 19) return false;

    // Read the precode code-length table (precode_count × 3 bits) and
    // run the virtual-leaf-count check.
    std::uint64_t packed = 0;
    for (std::size_t entry = 0; entry < precode_count; ++entry) {
        const std::uint64_t code_length = reader_copy.read<3>();
        packed |= (code_length << (entry * 3));
    }
    return precode_virtual_leaf_count_ok(packed);
}

/// Walk `reader` forward until either a candidate dynamic-Huffman block
/// is detected (returns its bit offset) or the search hits
/// `until_bit_offset`. Returns `block_finder_sentinel` on no-find.
///
/// The skip LUT consumes 15 bits at a time. Negative entries flag a
/// candidate; positive entries advance the search cursor by that many
/// bits.
[[nodiscard]] inline std::size_t seek_dynamic_huffman_candidate(
    bit_reader& reader, std::size_t until_bit_offset) noexcept {
    while (reader.bit_position() + 15 <= until_bit_offset) {
        reader.refill();
        if (reader.bits_buffered() < 15) break;
        const std::uint64_t key = reader.peek<15>();
        const std::int8_t entry =
            dynamic_huffman_skip_table[static_cast<std::size_t>(key)];
        if (entry < 0) {
            const std::size_t candidate_bit_offset =
                reader.bit_position()
                + static_cast<std::size_t>(-static_cast<std::int32_t>(entry));
            if (validate_precode(reader)) {
                return reader.bit_position();
            }
            // Not a real candidate; advance one bit and re-probe.
            reader.skip(1);
            (void)candidate_bit_offset;
        } else {
            reader.skip(static_cast<std::size_t>(entry));
        }
    }
    return block_finder_sentinel;
}

/// Walk `reader` for an uncompressed-block candidate. Returns
/// `(bit_offset_of_block_header, len_field_in_bytes)` or
/// `(block_finder_sentinel, 0)` on no-find.
[[nodiscard]] inline std::pair<std::size_t, std::size_t>
seek_uncompressed_block_candidate(
    bit_reader& reader, std::size_t until_bit_offset) noexcept {
    // Scan byte-aligned positions for `BTYPE=00` followed by the
    // LEN/NLEN sentinel.
    while (reader.bit_position() + 4 * 8 <= until_bit_offset) {
        const std::size_t starting_bit = reader.bit_position();
        reader.refill();
        if (reader.bits_buffered() < 3) break;
        const std::uint64_t header = reader.peek<3>();
        if ((header & 0x6u) != 0x0u) {
            reader.skip(1);
            continue;
        }
        // Align to byte boundary, read 4 bytes (LEN, NLEN).
        bit_reader probe_copy = reader;
        probe_copy.skip(3);
        probe_copy.align_to_byte_boundary();
        probe_copy.refill();
        const std::uint64_t len_field = probe_copy.read<16>();
        const std::uint64_t nlen_field = probe_copy.read<16>();
        if ((len_field ^ nlen_field) == 0xFFFFu) {
            return {starting_bit, static_cast<std::size_t>(len_field)};
        }
        reader.skip(1);
    }
    return {block_finder_sentinel, 0};
}

}  // namespace bioseq::io::compression
