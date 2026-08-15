/**
 * @file parallel_gzip_decoder.cppm
 * @brief Ordered chunk drain with optional gzip member checksum tracking.
 */
module;

#include <cstddef>
#include <cstdint>
#include <cstring>
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

export namespace bioseq::io::compression {

/// Tracks checksum and size state while draining one gzip member.
struct gzip_member_checksum_state {
    std::size_t header_end_bit_offset = 0;
    std::size_t footer_start_bit_offset = 0;
    std::uint32_t expected_crc32 = 0;
    std::uint32_t expected_isize_low32 = 0;
    std::uint64_t observed_decompressed_size = 0;
    std::uint32_t running_crc32 = iso_hdlc_initial_value ^ iso_hdlc_final_xor;
    bool seen_bfinal_block = false;
};

/// Presents ordered decompressed bytes from independently decoded gzip chunks.
///
/// The compressed view must outlive the decoder. Chunks remain owned by the
/// fetcher until their bytes have been copied to the caller.
class parallel_gzip_decoder {
  public:
    /// Construct a decoder over caller-owned compressed bytes.
    ///
    /// @param[in] compressed_view Bytes retained for the decoder lifetime
    /// @param[in] worker_count Maximum concurrent chunk decoders
    /// @param[in] chunk_decompressed_target_bytes Target output bytes per chunk
    /// @param[in] verify_crc Whether to update member checksums while draining
    parallel_gzip_decoder(std::span<const std::uint8_t> compressed_view,
                          std::size_t worker_count,
                          std::size_t chunk_decompressed_target_bytes,
                          bool verify_crc) noexcept
        : _compressed_view(compressed_view),
          _fetcher(compressed_view, chunk_decompressed_target_bytes, worker_count, verify_crc),
          _verify_crc(verify_crc) {}

    /// Decoders exclusively own their fetcher state.
    parallel_gzip_decoder(const parallel_gzip_decoder&) = delete;

    /// Decoders cannot share or replace active fetcher state.
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
                    return source_read_result{emitted, true, false};
                }
                if (chunk_ptr->state == chunk_state::decode_error) {
                    _is_at_end = true;
                    _last_error_message = std::string{chunk_ptr->error_message.view()};
                    return source_read_result{emitted, true, true};
                }
            }
            const auto& chunk = *_current_chunk;
            const std::size_t available =
                chunk.decompressed_size_bytes - _current_chunk_drain_offset;
            const std::size_t to_copy = std::min(available, cap - emitted);

            if (chunk.uses_marker_storage) {
                // Marker-mode chunk: narrow uint16 → uint8 into dst.
                // Reserved for the marker-mode parallel decode path.
                bioseq::io::simd::narrow_lower_bytes_to_dst(chunk.marker_storage.data.get() +
                                                                _current_chunk_drain_offset,
                                                            dst + emitted,
                                                            to_copy);
            } else {
                // Direct-decode chunk: bytes already resolved. Single
                // memcpy into the caller's slab.
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
            _current_member.observed_decompressed_size += to_copy;

            if (_current_chunk_drain_offset == chunk.decompressed_size_bytes) {
                const bool was_member_final = chunk.is_member_final;
                _fetcher.release_chunk(_current_chunk);
                _current_chunk = nullptr;
                if (was_member_final) {
                    // Reset the member CRC/ISIZE accumulators for the
                    // next member. zlib auto-handles multi-member
                    // streams under our backend; we reset our own
                    // tracking.
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
    /// Complete the source protocol when no borrowed chunk is exposed.
    void release_chunk() noexcept {}

  private:
    std::span<const std::uint8_t> _compressed_view;
    chunk_fetcher _fetcher;
    gzip_member_checksum_state _current_member;
    const decoded_chunk* _current_chunk = nullptr;
    std::size_t _current_chunk_drain_offset = 0;
    bool _is_at_end = false;
    bool _verify_crc;
    std::string _last_error_message;
};

} // namespace bioseq::io::compression
