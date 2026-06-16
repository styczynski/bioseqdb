/**
 * @file chunk.cppm
 * @brief Per-chunk decode driver.
 *
 * Holds a `chunk_uint16_buffer` for SIMD-aligned output, the trailing
 * 32 KiB window for downstream chunk priming, and an inline error
 * message buffer.
 *
 * The initial implementation runs a single zlib `inflate(2)` pass per
 * `decode_chunk` call. The chunk reads as much input as zlib needs to
 * advance to the chunk's target output size; `decompressed_size_bytes`
 * is the produced byte count. The trailing window is captured by
 * memcpy from the tail of the produced bytes for downstream chunks.
 *
 * The output buffer is `uint16_t[]` per the spec. Each lane's high
 * byte is set to zero (no markers in the initial implementation), so
 * the downstream `replace_markers_inplace` becomes a no-op fast path
 * on every vector. This preserves the spec's data shape without
 * paying the marker-resolution cost.
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.io.compression.chunk;

import bioseq.common.mlogging;
import bioseq.io.compression.aligned_byte_buffer;
import bioseq.io.compression.crc32;

export namespace bioseq::io::compression {

inline constexpr std::size_t kChunkTrailingWindowBytes = 32 * 1024;

enum class chunk_state : std::uint8_t {
    PENDING_RESOLUTION = 0,
    RESOLVED           = 1,
    DIRECTLY_DECODED   = 2,
    DECODE_ERROR       = 3,
};

/// Inline error buffer — the chunk arena zero-inits N=16 instances at
/// startup, so per-chunk allocation churn matters. 256-byte fixed
/// array bounds the per-slot storage.
struct inline_error_message {
    static constexpr std::size_t kCapacity = 256;
    std::array<char, kCapacity>  buffer{};
    std::size_t                  size = 0;

    void assign(std::string_view message) noexcept {
        size = std::min(message.size(), kCapacity - 1);
        std::memcpy(buffer.data(), message.data(), size);
        buffer[size] = '\0';
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return {buffer.data(), size};
    }
    [[nodiscard]] bool empty() const noexcept { return size == 0; }
};

struct chunk_data {
    std::size_t         encoded_start_bit_offset = 0;
    std::size_t         encoded_end_bit_offset   = 0;
    std::size_t         decompressed_size_bytes  = 0;
    /// Marker-mode storage. Lanes with `value >= 0x8000` reference
    /// `prior_window[value - 0x8000]`; lanes with `value < 0x8000`
    /// hold the resolved low byte. Used by the marker-mode decode
    /// path (a follow-up; the initial implementation always uses the
    /// `byte_storage` direct-decode field below).
    chunk_uint16_buffer marker_storage;
    /// Direct-decode storage. The initial-implementation chunk_fetcher
    /// runs zlib inflate(2) and writes resolved bytes here; the
    /// orchestrator drains via `std::memcpy` (no narrow needed).
    chunk_uint8_buffer  byte_storage;
    std::array<std::uint8_t, kChunkTrailingWindowBytes> trailing_window{};
    std::size_t         trailing_window_size = 0;
    /// Snapshot of the previous chunk's last 32 KiB (drain_window at
    /// the time this chunk's phase-1 ran). Phase-2 workers use it to
    /// resolve the chunk's REMAINDER independently. Sized to match
    /// `trailing_window` so phase-2 can share the same prev_window
    /// representation as phase-1.
    std::array<std::uint8_t, kChunkTrailingWindowBytes> prior_window_snapshot{};
    std::size_t         prior_window_snapshot_size = 0;
    bool                is_member_final      = false;
    /// `false` = byte_storage holds the resolved bytes. `true` =
    /// marker_storage needs a `replace_markers_inplace` resolution
    /// pass before the bytes can be drained.
    bool                uses_marker_storage  = false;
    chunk_state         state                = chunk_state::PENDING_RESOLUTION;
    inline_error_message error_message;
};

// The chunk producer loop lives in `chunk_fetcher.cppm`. It writes
// resolved bytes directly into `chunk_data::byte_storage` via zlib
// `inflate(2)`, sidestepping the marker mode. A follow-up PR can land
// the marker-mode variant that populates `chunk_data::marker_storage`
// for parallel block-level decode.

}  // namespace bioseq::io::compression
