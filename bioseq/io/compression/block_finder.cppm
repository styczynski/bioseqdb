/**
 * @file block_finder.cppm
 * @brief Search compressed bytes for possible DEFLATE block starts.
 *
 * Dynamic-Huffman candidates pass a hmader and precode check. Stored-block
 * candidates have complementary LEN and NLEN fields. Further decoding is
 * needed to confirm, a candidate.
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

inline constexpr std::size_t block_finder_sentinel = std::numeric_limits<std::size_t>::max();

/// Check a possible dynamic-Huffman header and precode using a reader copy.
[[nodiscard]] inline bool validate_precode(bit_reader reader_copy) noexcept {
    // Check the block type before reading dynamic-Huffman fields.
    if (reader_copy.bits_buffered() < 3 + 14) {
        reader_copy.refill();
    }
    const std::uint64_t header = reader_copy.read<3>();
    const std::uint64_t btype = (header >> 1) & 0x3u;
    if (btype != 0x2u)
        return false;

    // HLIT (5b), HDIST (5b), HCLEN (4b)
    const std::uint64_t hlit = reader_copy.read<5>();
    const std::uint64_t hdist = reader_copy.read<5>();
    const std::uint64_t hclen = reader_copy.read<4>();
    (void)hlit;
    (void)hdist;
    const std::size_t precode_count = static_cast<std::size_t>(hclen) + 4;
    if (precode_count > 19)
        return false;

    // Check the packed precode lengths before accepting this candidate.
    std::uint64_t packed = 0;
    for (std::size_t entry = 0; entry < precode_count; ++entry) {
        const std::uint64_t code_length = reader_copy.read<3>();
        packed |= (code_length << (entry * 3));
    }
    return precode_virtual_leaf_count_ok(packed);
}

/// Search  for a possible dynamic-Huffman block and return its bit offset.
/// Return block_finder_sentinel when the search reaches until_bit_offset
/// without finding one. A 15-bit lookup chooses how far to skip before
/// trying the next position.
[[nodiscard]] inline std::size_t
seek_dynamic_huffman_candidate(bit_reader& reader, std::size_t until_bit_offset) noexcept {
    while (reader.bit_position() + 15 <= until_bit_offset) {
        reader.refill();
        if (reader.bits_buffered() < 15)
            break;
        const std::uint64_t key = reader.peek<15>();
        const std::int8_t entry = dynamic_huffman_skip_table[static_cast<std::size_t>(key)];
        if (entry < 0) {
            const std::size_t candidate_bit_offset =
                reader.bit_position() + static_cast<std::size_t>(-static_cast<std::int32_t>(entry));
            if (validate_precode(reader)) {
                return reader.bit_position();
            }
            // Advance one bit and re-probe a rejected candidate.
            reader.skip(1);
            (void)candidate_bit_offset;
        } else {
            reader.skip(static_cast<std::size_t>(entry));
        }
    }
    return block_finder_sentinel;
}

/// Search for a stored-block header and matching LEN/NLEN fields. Return
/// the header bit offset and byte length, or (block_finder_sentinel, 0).
[[nodiscard]] inline std::pair<std::size_t, std::size_t>
seek_uncompressed_block_candidate(bit_reader& reader, std::size_t until_bit_offset) noexcept {
    // Try each bit position. A stored block aligns its length fields to bytes.
    while (reader.bit_position() + 4 * 8 <= until_bit_offset) {
        const std::size_t starting_bit = reader.bit_position();
        reader.refill();
        if (reader.bits_buffered() < 3)
            break;
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

} // namespace bioseq::io::compression
