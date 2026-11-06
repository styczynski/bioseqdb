/**
 * @file gzip_format.cppm
 * @brief Reads gzip member headers and footers.
 *
 * Optional header fields are skipped according to the flags. The footer stores
 * the CRC-32 checksum and the uncompressed byte count modulo 2^32.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

export module bioseq.io.compression.gzip_format;

import bioseq.common.logging;
import bioseq.io.compression.bit_reader;
import bioseq.io.simd;

export namespace bioseq::io::compression {

inline constexpr std::uint8_t gzip_magic_first_byte = 0x1F;
inline constexpr std::uint8_t gzip_magic_second_byte = 0x8B;
inline constexpr std::uint8_t gzip_deflate_method = 0x08;

/// Decoded gzip FLG bits that control optional header fields.
struct gzip_header_flags {
    bool ftext = false;
    bool fhcrc = false;
    bool fextra = false;
    bool fname = false;
    bool fcomment = false;
};

/// Parsed fixed header fields and the first DEFLATE bit offset.
struct gzip_header {
    std::size_t end_bit_offset = 0;
    gzip_header_flags flags;
    std::uint8_t operating_system = 0;
    std::uint8_t extra_flags = 0;
    std::uint32_t modification_time = 0;
    /// Total BGZF member bytes from BC/BSIZE, including header and trailer.
    std::optional<std::uint32_t> bgzf_member_bytes;
};

/// Parsed checksum and modulo-2^32 size from a gzip member footer.
struct gzip_footer {
    std::uint32_t crc32 = 0;
    std::uint32_t isize_low32 = 0;
};

/// Validate the complete XLEN payload and return the optional unique BGZF member size.
/// Shared by mapped-header parsing and bounded sequential BGZF framing.
[[nodiscard]] inline std::expected<std::optional<std::uint32_t>, std::string>
parse_gzip_extra_fields(std::span<const std::uint8_t> extra) {
    std::optional<std::uint32_t> member_bytes;
    for (std::size_t cursor = 0; cursor < extra.size();) {
        if (extra.size() - cursor < 4)
            return std::unexpected(std::string{"gzip header: truncated extra subfield"});
        const bool is_bgzf = extra[cursor] == 'B' && extra[cursor + 1] == 'C';
        const auto field_length = static_cast<std::size_t>(extra[cursor + 2])
            | (static_cast<std::size_t>(extra[cursor + 3]) << 8);
        cursor += 4;
        if (field_length > extra.size() - cursor)
            return std::unexpected(std::string{"gzip header: extra subfield exceeds XLEN"});
        if (is_bgzf) {
            if (member_bytes || field_length != 2)
                return std::unexpected(std::string{"BGZF header: duplicate or invalid BC field"});
            member_bytes = (static_cast<std::uint32_t>(extra[cursor])
                | (static_cast<std::uint32_t>(extra[cursor + 1]) << 8)) + 1;
        }
        cursor += field_length;
    }
    return member_bytes;
}

/// Reads and validates a complete gzip header at the current byte boundary.
/// Every access is bounded by compressed_view; BC and FHCRC are validated here
/// for ordinary gzip and BGZF consumers. The reader advances only on success.
[[nodiscard]] inline std::expected<gzip_header, std::string>
parse_gzip_header(bit_reader& reader, std::span<const std::uint8_t> compressed_view) {
    const auto initial_bit = reader.bit_position();
    if (initial_bit % 8 != 0)
        return std::unexpected(std::string{"gzip header: cursor is not byte aligned"});
    const auto begin = initial_bit / 8;
    if (begin > compressed_view.size() || compressed_view.size() - begin < 10)
        return std::unexpected(std::string{"gzip header: truncated fixed header"});
    if (compressed_view[begin] != gzip_magic_first_byte
        || compressed_view[begin + 1] != gzip_magic_second_byte)
        return std::unexpected(std::string{"gzip header: invalid magic"});
    if (compressed_view[begin + 2] != gzip_deflate_method)
        return std::unexpected(std::string{"gzip header: unsupported compression method"});
    const auto flag_byte = compressed_view[begin + 3];
    if ((flag_byte & 0xE0u) != 0)
        return std::unexpected(std::string{"gzip header: reserved flags are set"});
    gzip_header header;
    header.flags.ftext = (flag_byte & 0x01u) != 0;
    header.flags.fhcrc = (flag_byte & 0x02u) != 0;
    header.flags.fextra = (flag_byte & 0x04u) != 0;
    header.flags.fname = (flag_byte & 0x08u) != 0;
    header.flags.fcomment = (flag_byte & 0x10u) != 0;
    for (std::size_t byte = 0; byte < 4; ++byte)
        header.modification_time |= static_cast<std::uint32_t>(compressed_view[begin + 4 + byte])
            << (8 * byte);
    header.extra_flags = compressed_view[begin + 8];
    header.operating_system = compressed_view[begin + 9];
    std::size_t cursor = begin + 10;
    if (header.flags.fextra) {
        if (compressed_view.size() - cursor < 2)
            return std::unexpected(std::string{"gzip header: truncated XLEN"});
        const auto extra_length = static_cast<std::size_t>(compressed_view[cursor])
            | (static_cast<std::size_t>(compressed_view[cursor + 1]) << 8);
        cursor += 2;
        if (extra_length > compressed_view.size() - cursor)
            return std::unexpected(std::string{"gzip header: truncated extra field"});
        const auto parsed_extra = parse_gzip_extra_fields(compressed_view.subspan(cursor, extra_length));
        if (!parsed_extra) return std::unexpected(parsed_extra.error());
        header.bgzf_member_bytes = *parsed_extra;
        cursor += extra_length;
    }
    const auto skip_terminated = [&compressed_view, &cursor, begin, &header]()
        -> std::expected<void, std::string> {
        auto available = compressed_view.size() - cursor;
        if (header.bgzf_member_bytes) {
            const auto used = cursor - begin;
            if (used >= *header.bgzf_member_bytes)
                return std::unexpected(std::string{"BGZF header: optional field exceeds member"});
            available = std::min(available,
                static_cast<std::size_t>(*header.bgzf_member_bytes) - used);
        }
        const auto distance = bioseq::io::simd::scan_first_byte(
            std::uint8_t{0}, compressed_view.data() + cursor, available);
        if (distance == available)
            return std::unexpected(std::string{"gzip header: unterminated FNAME or FCOMMENT"});
        cursor += distance + 1;
        return {};
    };
    if (header.flags.fname) {
        const auto skipped = skip_terminated();
        if (!skipped) return std::unexpected(skipped.error());
    }
    if (header.flags.fcomment) {
        const auto skipped = skip_terminated();
        if (!skipped) return std::unexpected(skipped.error());
    }
    if (header.flags.fhcrc) {
        if (compressed_view.size() - cursor < 2)
            return std::unexpected(std::string{"gzip header: truncated FHCRC"});
        const auto expected = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(compressed_view[cursor])
            | (static_cast<std::uint16_t>(compressed_view[cursor + 1]) << 8));
        const auto observed = static_cast<std::uint16_t>(bioseq::io::simd::crc32_iso_hdlc_simd(
            0, compressed_view.data() + begin, cursor - begin));
        if (expected != observed)
            return std::unexpected(std::string{"gzip header: FHCRC mismatch"});
        cursor += 2;
    }
    if (header.bgzf_member_bytes
        && (*header.bgzf_member_bytes < 8 || cursor - begin > *header.bgzf_member_bytes - 8))
        return std::unexpected(std::string{"BGZF header: BSIZE cannot contain header and trailer"});
    if (cursor > std::numeric_limits<std::size_t>::max() / 8)
        return std::unexpected(std::string{"gzip header: bit offset overflow"});
    header.end_bit_offset = cursor * 8;
    reader.seek_to_bit(header.end_bit_offset);
    return header;
}

/// Aligns to the next byte boundary and reads a complete eight-byte gzip footer.
[[nodiscard]] inline std::expected<gzip_footer, std::string> parse_gzip_footer(bit_reader& reader) {
    reader.align_to_byte_boundary();
    const auto byte_position = reader.bit_position() / 8;
    if (byte_position > reader.total_bytes() || reader.total_bytes() - byte_position < 8)
        return std::unexpected(std::string{"gzip footer: truncated checksum or size"});
    gzip_footer footer;
    footer.crc32 = static_cast<std::uint32_t>(reader.read<32>());
    footer.isize_low32 = static_cast<std::uint32_t>(reader.read<32>());
    return footer;
}

} // namespace bioseq::io::compression
