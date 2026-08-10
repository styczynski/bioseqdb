/**
 * @file chunk.cppm
 * @brief Per-chunk decode driver.
 *
 * Holds a `chunk_marker_buffer` for SIMD-aligned output, the trailing
 * 32 KiB window for downstream chunk priming, and an inline error
 * message buffer.
 *
 * The direct decoder runs a single zlib `inflate(2)` pass per chunk.
 * It reads as much input as zlib needs to
 * advance to the chunk's target output size; `decompressed_size_bytes`
 * is the produced byte count. The trailing window is captured by
 * memcpy from the tail of the produced bytes for downstream chunks.
 *
 * The marker output buffer is `uint16_t[]`. Each literal lane's high
 * byte is zero, so
 * the downstream `replace_markers_inplace` becomes a no-op fast path
 * on every vector.
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
    std::array<char, capacity>  buffer{};
    std::size_t                  size = 0;

    void assign(std::string_view message) noexcept {
        size = std::min(message.size(), capacity - 1);
        std::memcpy(buffer.data(), message.data(), size);
        buffer[size] = '\0';
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return {buffer.data(), size};
    }
    [[nodiscard]] bool empty() const noexcept { return size == 0; }
};

/// One bounded decompression result and its dependency window.
struct decoded_chunk {
    std::size_t         encoded_start_bit_offset = 0;
    std::size_t         encoded_end_bit_offset   = 0;
    std::size_t         decompressed_size_bytes  = 0;
    /// Marker-mode storage. Lanes with `value >= 0x8000` reference
    /// `prior_window[value - 0x8000]`; lanes with `value < 0x8000`
    /// hold the resolved low byte. Used by the marker-mode decode
    /// path.
    chunk_marker_buffer marker_storage;
    /// Direct-decode storage. The chunk fetcher writes resolved bytes here; the
    /// orchestrator drains via `std::memcpy` (no narrow needed).
    chunk_byte_buffer  byte_storage;
    std::array<std::uint8_t, chunk_trailing_window_bytes> trailing_window{};
    std::size_t         trailing_window_size = 0;
    /// Snapshot of the previous chunk's last 32 KiB when this chunk's
    /// trailing window was resolved. Workers use it to resolve the
    /// remaining prefix independently. Sized to match `trailing_window`.
    std::array<std::uint8_t, chunk_trailing_window_bytes> prior_window_snapshot{};
    std::size_t         prior_window_snapshot_size = 0;
    bool                is_member_final      = false;
    /// `false` = byte_storage holds the resolved bytes. `true` =
    /// marker_storage needs a `replace_markers_inplace` resolution
    /// pass before the bytes can be drained.
    bool                uses_marker_storage  = false;
    chunk_state         state                = chunk_state::pending_resolution;
    inline_error_message error_message;
};

// The chunk producer loop lives in `chunk_fetcher.cppm`. It writes
// resolved bytes directly into `decoded_chunk::byte_storage` via zlib
// `inflate(2)`, while independently decoded blocks use marker storage.

}  // namespace bioseq::io::compression
