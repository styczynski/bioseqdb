/**
 * @file chunk.cppm
 * @brief Store decompressed chunks and their decoding state.
 *
 * A chunk keeps bytes or unresolved references to earlier output, the trailing
 * 32 KiB used by the next chunk, and an error message.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.io.compression.chunk;

import bioseq.io.compression.aligned_byte_buffer;
import bioseq.io.compression.crc32;

export namespace bioseq::io::compression {

inline constexpr std::size_t chunk_trailing_window_bytes = 32 * 1024;

/// Whether a chunk still needs earlier bytes, is ready, or failed to decode.
enum class chunk_state : std::uint8_t {
    pending_resolution = 0,
    resolved = 1,
    directly_decoded = 2,
    decode_error = 3,
};

/// Store a short error message inside the chunk without a separate allocation.
struct inline_error_message {
    static constexpr std::size_t capacity = 256;
    std::array<char, capacity> buffer{};
    std::size_t size = 0;

    /// Replace the current message with a truncated, null-terminated copy.
    void assign(std::string_view message) noexcept {
        size = std::min(message.size(), capacity - 1);
        std::memcpy(buffer.data(), message.data(), size);
        buffer[size] = '\0';
    }
    /// Return the stored message without the terminating null byte.
    [[nodiscard]] std::string_view view() const noexcept {
        return {buffer.data(), size};
    }
    /// Return true when no error message is stored.
    [[nodiscard]] bool empty() const noexcept {
        return size == 0;
    }
};

/// Decompressed output and the earlier bytes needed to resolve its references.
struct decoded_chunk {
    std::size_t encoded_start_bit_offset = 0;
    std::size_t encoded_end_bit_offset = 0;
    std::size_t decompressed_size_bytes = 0;
    /// Decoded values awaiting resolution. Values at or above 0x8000 refer to
    /// prior_window[value - 0x8000]. Other values contain a literal byte.
    chunk_marker_buffer marker_storage;
    /// Resolved output bytes, ready to copy to the consumer.
    chunk_byte_buffer byte_storage;
    std::array<std::uint8_t, chunk_trailing_window_bytes> trailing_window{};
    std::size_t trailing_window_size = 0;
    /// Saved history before this chunk, up to 32 KiB. A worker uses it to
    /// resolve the prefix after the ordered pass has resolved the tail.
    std::array<std::uint8_t, chunk_trailing_window_bytes> prior_window_snapshot{};
    std::size_t prior_window_snapshot_size = 0;
    bool is_member_final = false;
    /// True while marker_storage still needs to be converted into byte_storage.
    bool uses_marker_storage = false;
    chunk_state state = chunk_state::pending_resolution;
    inline_error_message error_message;
};

// Chunks can be decoded before earlier output is available. Their markers
// keep those references until the ordered pass supplies the missing bytes.

} // namespace bioseq::io::compression
