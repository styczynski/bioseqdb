/**
 * @file gzip_format.cppm
 * @brief gzip header / footer parsers per RFC 1952.
 *
 * Header layout:
 *   ID1 (1B) ID2 (1B) CM (1B) FLG (1B) MTIME (4B) XFL (1B) OS (1B)
 *   [FEXTRA (2B+N)?] [FNAME (zero-terminated)?] [FCOMMENT (zero-terminated)?]
 *   [FHCRC (2B)?]
 *
 * Footer layout: CRC32 (4B) + ISIZE (4B, uncompressed size mod 2^32).
 *
 * The bit reader is byte-aligned at every entry/exit point; the parser
 * uses `align_to_byte_boundary()` defensively after the variable
 * fields. NUL-terminator scanning for FNAME / FCOMMENT goes through
 * `bioseq::io::simd::scan_first_byte('\0', ...)`.
 */
module;


#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

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
    bool ftext    = false;
    bool fhcrc    = false;
    bool fextra   = false;
    bool fname    = false;
    bool fcomment = false;
};

/// Parsed fixed header fields and the first DEFLATE bit offset.
struct gzip_header {
    std::size_t end_bit_offset = 0;
    gzip_header_flags  flags;
    std::uint8_t  operating_system = 0;
    std::uint8_t  extra_flags      = 0;
    std::uint32_t modification_time = 0;
};

/// Parsed checksum and modulo-2^32 size from a gzip member footer.
struct gzip_footer {
    std::uint32_t crc32 = 0;
    std::uint32_t isize_low32 = 0;
};

/// Parse a gzip member header from `reader`. Reader MUST be byte-aligned
/// on entry. On success, advances the reader past the header and
/// returns a populated `gzip_header`. On failure, the reader is left at
/// some intermediate position; the caller should treat the stream as
/// invalid.
[[nodiscard]] inline std::expected<gzip_header, std::string>
parse_gzip_header(bit_reader& reader,
                  std::span<const std::uint8_t> compressed_view) {
    if (reader.bits_buffered() < 8) reader.refill();
    const std::uint64_t magic0 = reader.read<8>();
    if (magic0 != gzip_magic_first_byte) {
        return std::unexpected(fmt::format(
            "gzip header: bad magic byte 0 ({:#04x} expected {:#04x})",
            magic0, gzip_magic_first_byte));
    }
    const std::uint64_t magic1 = reader.read<8>();
    if (magic1 != gzip_magic_second_byte) {
        return std::unexpected(fmt::format(
            "gzip header: bad magic byte 1 ({:#04x} expected {:#04x})",
            magic1, gzip_magic_second_byte));
    }
    const std::uint64_t method = reader.read<8>();
    if (method != gzip_deflate_method) {
        return std::unexpected(fmt::format(
            "gzip header: unsupported method {} (only DEFLATE supported)",
            method));
    }
    const std::uint8_t flag_byte = static_cast<std::uint8_t>(reader.read<8>());
    gzip_header header;
    header.flags.ftext    = (flag_byte & 0x01u) != 0;
    header.flags.fhcrc    = (flag_byte & 0x02u) != 0;
    header.flags.fextra = (flag_byte & 0x04u) != 0;
    header.flags.fname = (flag_byte & 0x08u) != 0;
    header.flags.fcomment = (flag_byte & 0x10u) != 0;

    header.modification_time = static_cast<std::uint32_t>(reader.read<32>());
    header.extra_flags = static_cast<std::uint8_t>(reader.read<8>());
    header.operating_system = static_cast<std::uint8_t>(reader.read<8>());

    if (header.flags.fextra) {
        const std::uint64_t length = reader.read<16>();
        for (std::uint64_t byte_index = 0; byte_index < length; ++byte_index) {
            (void)reader.read<8>();
        }
    }

    auto skip_zero_terminated = [&compressed_view, &reader]() -> std::expected<void, std::string> {
        // Use SIMD scan when the underlying compressed view's tail is
        // available; fall back to byte-by-byte read.
        const std::size_t bit_pos = reader.bit_position();
        if (bit_pos % 8 == 0) {
            const std::size_t byte_pos = bit_pos / 8;
            if (byte_pos < compressed_view.size()) {
                const std::size_t scanned =
                    bioseq::io::simd::scan_first_byte(std::uint8_t{0},
                                                      compressed_view.data() + byte_pos,
                                                      compressed_view.size() - byte_pos);
                if (scanned >= compressed_view.size() - byte_pos) {
                    return std::unexpected(
                        std::string{"gzip header: NUL terminator missing in FNAME / FCOMMENT"});
                }
                // Advance past the NUL.
                reader.seek_to_bit((byte_pos + scanned + 1) * 8);
                return {};
            }
        }
        // Fallback path.
        for (std::size_t safety = 0; safety < 65536; ++safety) {
            const std::uint64_t byte = reader.read<8>();
            if (byte == 0)
                return {};
        }
        return std::unexpected(std::string{
            "gzip header: NUL terminator absent in FNAME / FCOMMENT (>64K)"});
    };

    if (header.flags.fname) {
        if (auto result = skip_zero_terminated(); !result) {
            return std::unexpected(std::move(result.error()));
        }
    }
    if (header.flags.fcomment) {
        if (auto result = skip_zero_terminated(); !result) {
            return std::unexpected(std::move(result.error()));
        }
    }
    if (header.flags.fhcrc) {
        (void)reader.read<16>();
    }

    header.end_bit_offset = reader.bit_position();
    return header;
}

/// Parse the 8-byte gzip footer. Reader MUST be byte-aligned on entry.
[[nodiscard]] inline std::expected<gzip_footer, std::string>
parse_gzip_footer(bit_reader& reader) {
    reader.align_to_byte_boundary();
    gzip_footer footer;
    footer.crc32       = static_cast<std::uint32_t>(reader.read<32>());
    footer.isize_low32 = static_cast<std::uint32_t>(reader.read<32>());
    return footer;
}

}  // namespace bioseq::io::compression
