/**
 * @file parallel_gzip_decoder.cppm
 * @brief Copies decoded gzip chunks and verifies each member's footer at its exact boundary.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

export module bioseq.io.compression.parallel_gzip_decoder;

import bioseq.io.sources;
import bioseq.io.simd;
import bioseq.io.compression.chunk;
import bioseq.io.compression.chunk_fetcher;
import bioseq.io.compression.crc32;
import bioseq.io.compression.bit_reader;
import bioseq.io.compression.gzip_format;

export namespace bioseq::io::compression {

/// Tracks checksum and size state while draining one gzip member.
struct gzip_member_checksum_state {
    std::uint64_t observed_decompressed_size = 0;
    std::uint32_t running_crc32 = iso_hdlc_initial_value ^ iso_hdlc_final_xor;
    bool started = false;
};

/// Copies decoded gzip chunks to the caller in file order.
///
/// The compressed view must outlive the decoder. The fetcher owns each chunk
/// until all its bytes have been copied. ISIZE is always checked; CRC checking
/// follows the caller's existing verify_crc option for every concatenated member.
class parallel_gzip_decoder {
  public:
    /// Construct a decoder over, caller-owned compressed bytes.
    ///
    /// @param[in] compressed_view Bytes the caller keeps alive for the decoder lifetime
    /// @param[in] worker_count Maximum concurrent chunk decoders
    /// @param[in] chunk_decompressed_target_bytes Target output bytes per chunk
    /// @param[in] verify_crc Whether to verify member CRC32 values while draining
    parallel_gzip_decoder(std::span<const std::uint8_t> compressed_view,
                          std::size_t worker_count,
                          std::size_t chunk_decompressed_target_bytes,
                          bool verify_crc) noexcept
        : _compressed_view(compressed_view),
          _fetcher(compressed_view, chunk_decompressed_target_bytes, worker_count, verify_crc),
          _verify_crc(verify_crc) {}

    /// Decoders exclusively own their fetcher state.
    parallel_gzip_decoder(const parallel_gzip_decoder&) = delete;

    /// Prevents copy assignment of the decoder and its active fetcher.
    parallel_gzip_decoder& operator=(const parallel_gzip_decoder&) = delete;

    /// Copy up to `cap` decompressed bytes into caller-owned storage.
    ///
    /// @param[out] dst Destination containing at least `cap` writable bytes
    /// @param[in] cap Maximum byte count to copy
    /// @return Read count together with EOF and error state
    [[nodiscard]] source_read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        if (_is_at_end) {
            return source_read_result{0, true, !_last_error_message.empty()};
        }
        std::size_t emitted = 0;
        while (emitted < cap) {
            if (_current_chunk == nullptr) {
                const auto [chunk_ptr, has_error] = _fetcher.get_next_chunk();
                _current_chunk = chunk_ptr;
                _current_chunk_drain_offset = 0;
                if (chunk_ptr == nullptr) {
                    _is_at_end = true;
                    if (has_error) {
                        _last_error_message = _fetcher.last_error_message();
                        return source_read_result{emitted, true, true};
                    }
                    if (_current_member.started) {
                        _last_error_message = "gzip member ended without a final block and footer";
                        return source_read_result{emitted, true, true};
                    }
                    if (_next_member_byte != _compressed_view.size()) {
                        _last_error_message = "gzip stream contains unconsumed member or trailing bytes";
                        return source_read_result{emitted, true, true};
                    }
                    return source_read_result{emitted, true, false};
                }
                if (chunk_ptr->state == chunk_state::decode_error) {
                    _is_at_end = true;
                    _last_error_message = std::string{chunk_ptr->error_message.view()};
                    return source_read_result{emitted, true, true};
                }
                if (!_current_member.started) {
                    if (_next_member_byte > _compressed_view.size()
                        || _next_member_byte > std::numeric_limits<std::size_t>::max() / 8u) {
                        _is_at_end = true;
                        _last_error_message = "gzip member boundary exceeds compressed input";
                        return source_read_result{emitted, true, true};
                    }
                    bit_reader header_reader{_compressed_view};
                    header_reader.seek_to_bit(_next_member_byte * 8u);
                    const auto header = parse_gzip_header(header_reader, _compressed_view);
                    if (!header || header->end_bit_offset != chunk_ptr->encoded_start_bit_offset) {
                        _is_at_end = true;
                        _last_error_message = header ? "gzip chunks do not begin at the next member boundary" : header.error();
                        return source_read_result{emitted, true, true};
                    }
                    _current_member.started = true;
                }
            }
            const auto& chunk = *_current_chunk;
            const std::size_t available =
                chunk.decompressed_size_bytes - _current_chunk_drain_offset;
            const std::size_t to_copy = std::min(available, cap - emitted);

            if (chunk.uses_marker_storage) {
                // The fetcher has resolved every marker. Copy the low byte of each
                // 16-bit entry into the caller's buffer.
                bioseq::io::simd::narrow_lower_bytes_to_dst(chunk.marker_storage.data.get() +
                                                                _current_chunk_drain_offset,
                                                            dst + emitted,
                                                            to_copy);
            } else {
                // Byte storage  can be copied directly into the caller's buffer.
                std::memcpy(dst + emitted,
                            chunk.byte_storage.data.get() + _current_chunk_drain_offset,
                            to_copy);
            }

            if (_verify_crc) {
                // The Highway kernel uses zlib for small inputs and a
                // carryless-multiply fold for larger chunks.
                _current_member.running_crc32 = bioseq::io::simd::crc32_iso_hdlc_simd(
                    _current_member.running_crc32, dst + emitted, to_copy);
            }

            emitted += to_copy;
            _current_chunk_drain_offset += to_copy;
            if (to_copy > std::numeric_limits<std::uint64_t>::max() - _current_member.observed_decompressed_size) {
                _is_at_end = true;
                _last_error_message = "gzip member decompressed length overflows";
                return source_read_result{emitted, true, true};
            }
            _current_member.observed_decompressed_size += to_copy;

            if (_current_chunk_drain_offset == chunk.decompressed_size_bytes) {
                const bool was_member_final = chunk.is_member_final;
                if (was_member_final) {
                    bit_reader footer_reader{_compressed_view};
                    footer_reader.seek_to_bit(chunk.encoded_end_bit_offset);
                    const auto footer = parse_gzip_footer(footer_reader);
                    if (!footer) _last_error_message = footer.error();
                    else if (footer->isize_low32 != static_cast<std::uint32_t>(_current_member.observed_decompressed_size))
                        _last_error_message = "gzip member ISIZE does not match decompressed bytes";
                    else if (_verify_crc && footer->crc32 != _current_member.running_crc32)
                        _last_error_message = "gzip member CRC32 does not match decompressed bytes";
                    else _next_member_byte = footer_reader.bit_position() / 8u;
                }
                _fetcher.release_chunk(_current_chunk);
                _current_chunk = nullptr;
                if (!_last_error_message.empty()) {
                    _is_at_end = true;
                    return source_read_result{emitted, true, true};
                }
                if (was_member_final) {
                    // Start fresh CRC and size counts for the next gzip member.
                    _current_member = gzip_member_checksum_state{};
                }
            }
        }
        return source_read_result{emitted, false, false};
    }

    /// Return the terminal decoder diagnostic, or an empty string on success.
    [[nodiscard]] std::string last_error_message() const noexcept {
        return _last_error_message;
    }

    /// Return no borrowed view because decoded chunks are drained by copy.
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> next_decoded_chunk_view() noexcept {
        return std::nullopt;
    }
    /// No actipn is needed because callers never borrow a chunk.
    void release_chunk() noexcept {}

  private:
    std::span<const std::uint8_t> _compressed_view;
    chunk_fetcher _fetcher;
    gzip_member_checksum_state _current_member;
    const decoded_chunk* _current_chunk = nullptr;
    std::size_t _current_chunk_drain_offset = 0;
    std::size_t _next_member_byte = 0;
    bool _is_at_end = false;
    bool _verify_crc;
    std::string _last_error_message;
};

} // namespace bioseq::io::compression
