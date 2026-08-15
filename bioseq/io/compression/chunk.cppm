/**
 * @file chunk.cppm
 * @brief Decoded chunk storage and ordered-drain state.
 *
 * A chunk owns either resolved bytes or marker lanes, its trailing 32 KiB
 * window, and one bounded diagnostic.
 */
module;

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

/// Readiness of a decoded chunk in the ordered consumer pipeline.
enum class chunk_state : std::uint8_t {
    pending_resolution = 0,
    resolved = 1,
    directly_decoded = 2,
    decode_error = 3,
};

/// Inline error buffer — the chunk arena zero-inits N=16 instances at
/// startup, so per-chunk allocation churn matters. 256-byte fixed
/// array bounds the per-slot storage.
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
    /// Return the live message bytes without the trailing null byte.
    [[nodiscard]] std::string_view view() const noexcept {
        return {buffer.data(), size};
    }
    /// Return whether no diagnostic bytes are stored.
    [[nodiscard]] bool empty() const noexcept {
        return size == 0;
    }
};

/// One bounded decompression result and its dependency window.
struct decoded_chunk {
    std::size_t encoded_start_bit_offset = 0;
    std::size_t encoded_end_bit_offset = 0;
    std::size_t decompressed_size_bytes = 0;
    /// Marker-mode storage. Lanes with `value >= 0x8000` reference
    /// `prior_window[value - 0x8000]`. Lanes with `value < 0x8000`
    /// hold the resolved low byte. Used by the marker-mode decode
    /// path.
    chunk_marker_buffer marker_storage;
    /// Direct-decode storage. The chunk fetcher writes resolved bytes here. The
    /// orchestrator drains via `std::memcpy` (no narrow needed).
    chunk_byte_buffer byte_storage;
    std::array<std::uint8_t, chunk_trailing_window_bytes> trailing_window{};
    std::size_t trailing_window_size = 0;
    /// Snapshot of the previous chunk's last 32 KiB when this chunk's
    /// trailing window was resolved. Workers use it to resolve the
    /// remaining prefix independently. Sized to match `trailing_window`.
    std::array<std::uint8_t, chunk_trailing_window_bytes> prior_window_snapshot{};
    std::size_t prior_window_snapshot_size = 0;
    bool is_member_final = false;
    /// `false` = byte_storage holds the resolved bytes. `true` =
    /// marker_storage needs a `replace_markers_inplace` resolution
    /// pass before the bytes can be drained.
    bool uses_marker_storage = false;
    chunk_state state = chunk_state::pending_resolution;
    inline_error_message error_message;
};

// The chunk producer loop writes resolved output into byte storage and keeps
// marker storage for blocks that still depend on the preceding window.

} // namespace bioseq::io::compression
