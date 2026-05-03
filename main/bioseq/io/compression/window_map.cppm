/**
 * @file window_map.cppm
 * @brief Fixed-depth ring of `(encoded_end_bit_offset, 32 KiB window)`
 *        entries. Sized at `kCacheDepth = 16`.
 *
 * Single-writer / single-reader semantics: the chunk_fetcher's per-chunk
 * worker writes the resolved tail-window into slot[chunk.next_slot]
 * after marker resolution; the orchestrator reads it from
 * slot[next_chunk.prior_window_slot]. Relaxed atomics on the slot
 * generation counters keep the overhead at one inc / one load per
 * window swap.
 */
module;

#include "common.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

export module bioseq_io.compression.window_map;

import mlogging;

export namespace bioseq::io::compression {

inline constexpr std::size_t kWindowSizeBytes = 32 * 1024;

/// One entry in the window ring. `bytes` holds the trailing 32 KiB of
/// the chunk's decoded output; `size` < 32 KiB only at end-of-stream.
/// `encoded_end_bit_offset` is the bit-offset just past the chunk's
/// last DEFLATE block. `generation` is bumped on each write so the
/// reader can spot stale entries.
struct window_entry {
    std::array<std::uint8_t, kWindowSizeBytes> bytes{};
    std::size_t   size                       = 0;
    std::size_t   encoded_end_bit_offset     = 0;
    std::uint64_t generation                 = 0;
};

}  // namespace bioseq::io::compression
