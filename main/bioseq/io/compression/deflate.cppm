/**
 * @file deflate.cppm
 * @brief DEFLATE block decoder. **Initial implementation: zlib inflate
 *        backend.**
 *
 * The full marker-mode DEFLATE port from upstream rapidgzip is the
 * forever-goal, but landing it in one PR is impractical (~2000 LOC of
 * correctness-critical code). This initial cut wires up the spec's
 * `block` interface but delegates the actual DEFLATE bit-stream
 * decoding to zlib's `inflate(2)` with `windowBits = -15` (raw mode).
 *
 * This shape gets the rest of the bundle (chunk_fetcher,
 * parallel_gzip_decoder, parallel_gzip_source, bench wiring, SIMD
 * kernels) compiling and the bench cell producing valid samples. A
 * follow-up PR can replace the inflate call with the full
 * marker-mode-aware port without touching any other module.
 */
module;

#include "common.h"

#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <utility>

export module bioseq_io.compression.deflate;

import mlogging;
import bioseq_io.compression.bit_reader;
import bioseq_io.compression.huffman;

export namespace bioseq::io::compression {

enum class block_compression : std::uint8_t {
    UNCOMPRESSED   = 0,
    FIXED_HUFFMAN  = 1,
    DYNAMIC_HUFFMAN = 2,
    RESERVED       = 3,
};

enum class decode_status : std::uint8_t {
    OK               = 0,
    END_OF_BLOCK     = 1,
    END_OF_STREAM    = 2,
    INVALID_CODE     = 3,
    INVALID_DISTANCE = 4,
    OUT_OF_INPUT     = 5,
    GENERIC_ERROR    = 6,
};

[[nodiscard]] constexpr const char*
decode_status_name(decode_status status) noexcept {
    switch (status) {
        case decode_status::OK:               return "ok";
        case decode_status::END_OF_BLOCK:     return "end_of_block";
        case decode_status::END_OF_STREAM:    return "end_of_stream";
        case decode_status::INVALID_CODE:     return "invalid_code";
        case decode_status::INVALID_DISTANCE: return "invalid_distance";
        case decode_status::OUT_OF_INPUT:     return "out_of_input";
        case decode_status::GENERIC_ERROR:    return "generic_error";
    }
    return "?";
}

/// Single-block FSM. The initial implementation forwards to zlib's
/// inflate; the public interface is kept stable so that a later port
/// of the marker-mode decoder lands without touching callers.
class block {
public:
    static constexpr std::size_t kMaxWindowSize = 32 * 1024;
    using pre_decoded_buffer_t =
        std::array<std::uint16_t, 2 * kMaxWindowSize>;

    block() = default;
    block(const block&) = delete;
    block& operator=(const block&) = delete;
    block(block&&) = delete;
    block& operator=(block&&) = delete;
    ~block() = default;

    /// Read the 3-bit DEFLATE block header (BFINAL + BTYPE). Returns
    /// the compression type or a `decode_status` error.
    [[nodiscard]] std::expected<block_compression, decode_status>
    read_header(bit_reader& reader) noexcept {
        if (reader.bits_buffered() < 3) reader.refill();
        if (reader.bits_buffered() < 3) {
            return std::unexpected(decode_status::OUT_OF_INPUT);
        }
        const std::uint64_t bfinal = reader.read<1>();
        const std::uint64_t btype  = reader.read<2>();
        _is_end_of_stream = (bfinal != 0);
        switch (btype) {
            case 0: _compression_type = block_compression::UNCOMPRESSED;    break;
            case 1: _compression_type = block_compression::FIXED_HUFFMAN;   break;
            case 2: _compression_type = block_compression::DYNAMIC_HUFFMAN; break;
            default:
                _compression_type = block_compression::RESERVED;
                return std::unexpected(decode_status::INVALID_CODE);
        }
        return _compression_type;
    }

    /// Read dynamic-Huffman code-length tables. Implemented for
    /// API compatibility — actual tree state is opaque under the
    /// zlib backend.
    [[nodiscard]] decode_status
    read_dynamic_huffman(bit_reader& /*reader*/) noexcept {
        return decode_status::OK;
    }

    /// Decode up to `n_max_to_decode` bytes from `reader` into the
    /// internal output buffer. Initial cut is a no-op shim — actual
    /// decode happens in `chunk.cppm` via zlib.
    [[nodiscard]] std::pair<std::size_t, decode_status>
    decode(bit_reader& /*reader*/,
           std::size_t /*n_max_to_decode*/ =
               std::numeric_limits<std::size_t>::max()) noexcept {
        return {0, decode_status::OK};
    }

    void set_initial_window(std::span<const std::uint8_t> /*prior_window*/) noexcept {}

    void reset(std::span<const std::uint8_t> /*prior_window*/ = {}) noexcept {
        _output_cursor = 0;
        _contains_markers = false;
        _is_end_of_stream = false;
        _compression_type = block_compression::RESERVED;
    }

    [[nodiscard]] bool contains_marker_bytes() const noexcept {
        return _contains_markers;
    }
    [[nodiscard]] bool is_end_of_stream() const noexcept {
        return _is_end_of_stream;
    }
    [[nodiscard]] block_compression compression_type() const noexcept {
        return _compression_type;
    }
    [[nodiscard]] std::span<const std::uint16_t> output_view_uint16() const noexcept {
        return {_window16.data(), _output_cursor};
    }
    [[nodiscard]] std::span<const std::uint8_t> output_view_uint8() const noexcept {
        return {reinterpret_cast<const std::uint8_t*>(_window16.data()),
                _output_cursor * sizeof(std::uint16_t)};
    }

private:
    alignas(64) pre_decoded_buffer_t _window16{};
    std::size_t       _output_cursor      = 0;
    bool              _contains_markers   = false;
    bool              _is_end_of_stream   = false;
    block_compression _compression_type   = block_compression::RESERVED;
};

}  // namespace bioseq::io::compression
