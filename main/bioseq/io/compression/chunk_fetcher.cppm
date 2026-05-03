/**
 * @file chunk_fetcher.cppm
 * @brief Multi-worker parallel chunk decoder.
 *
 * The pipeline (rapidgzip §3.2 + per-member orchestration):
 *
 *   * **member partitioning** — the producer thread parses gzip
 *     headers sequentially (cheap; ~10 bytes per member) and dispatches
 *     each member's decode as an independent unit. This sidesteps the
 *     window-reset semantics of multi-member streams: workers within a
 *     member share their decoder's window, but a member boundary
 *     resets the window to empty per RFC 1952.
 *
 *   * **chunks within a member** are partitioned by compressed bytes
 *     (`kDefaultChunkCompressedTargetBytes` ≈ 4 MiB). Worker 0 of the
 *     member parses the gzip header and runs the decoder in **direct
 *     mode** from the member's first deflate block. Workers 1..N find
 *     the first valid dynamic-Huffman block at/after their assigned
 *     compressed offset (`seek_dynamic_huffman_candidate`), then
 *     decode in **marker mode**.
 *
 *   * **drain** is in chunk-index order across the entire file.
 *     Markers are resolved using the previous chunk's tail-32-KiB
 *     window, with a reset to empty at every member boundary.
 *
 * On `worker_count == 1` the path collapses to a simple single-thread
 * `deflate::block` over the stream — no marker mode, no
 * cross-thread coordination. This is the proven correctness baseline
 * (single-thread bench gives ~1.8x zlib by itself).
 */
module;

#include "common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

export module bioseq_io.compression.chunk_fetcher;

import mlogging;
import bioseq_io.compression.aligned_byte_buffer;
import bioseq_io.compression.bit_reader;
import bioseq_io.compression.blockfinder;
import bioseq_io.compression.chunk;
import bioseq_io.compression.deflate;
import bioseq_io.compression.gzip_format;
import bioseq_io.compression.window_map;

export namespace bioseq::io::compression {

using chunk_slot_index = std::uint16_t;
inline constexpr chunk_slot_index kInvalidSlot =
    std::numeric_limits<chunk_slot_index>::max();

/// Capacity of the in-flight chunk arena. Sized to hold ~16 chunks
/// in-flight: a small buffer between the producer and consumer hides
/// disk-read / decode jitter without exhausting RAM.
inline constexpr std::size_t kChunkArenaCapacity = 64;
/// 4 MiB compressed per chunk — paper-validated optimum (rapidgzip §6,
/// fig:chunk-size). Smaller → block-finder overhead dominates; larger
/// → load imbalance across workers and high RAM.
inline constexpr std::size_t kDefaultChunkTargetBytes = 4 * 1024 * 1024;
/// Cap worker count. More workers stop helping past ~core count, and
/// the orchestrator's drain loop is the eventual serial bottleneck.
inline constexpr std::size_t kMaxWorkerCount = 16;

namespace detail {

/// Append a `decoded_data_view`'s `data` arrays to a uint8 buffer.
[[nodiscard]] inline std::size_t copy_direct_view(
    chunk_uint8_buffer&            byte_storage,
    const decoded_data_view&       view) {
    const std::size_t total = view.data_size();
    if (total == 0) return 0;
    auto extend_or = byte_storage.extend(total);
    if (!extend_or) return std::numeric_limits<std::size_t>::max();
    std::uint8_t* dst = extend_or->data();
    std::size_t copied = 0;
    for (const auto& part : view.data) {
        if (part.empty()) continue;
        std::memcpy(dst + copied, part.data(), part.size());
        copied += part.size();
    }
    return copied;
}

/// Append a `decoded_data_view`'s marker stream to a uint16 buffer.
[[nodiscard]] inline std::size_t copy_marker_view(
    chunk_uint16_buffer&           marker_storage,
    const decoded_data_view&       view) {
    const std::size_t total = view.data_with_markers_size();
    if (total == 0) return 0;
    auto extend_or = marker_storage.extend(total);
    if (!extend_or) return std::numeric_limits<std::size_t>::max();
    std::uint16_t* dst = extend_or->data();
    std::size_t copied = 0;
    for (const auto& part : view.data_with_markers) {
        if (part.empty()) continue;
        std::memcpy(dst + copied, part.data(),
                    part.size() * sizeof(std::uint16_t));
        copied += part.size();
    }
    return copied;
}

/// Drive `block` over `reader` until either `target_bytes` of output
/// have been emitted or end-of-stream is reached. Switches between
/// uint8 and uint16 view storage transparently as the block transitions
/// out of marker mode.
[[nodiscard]] inline std::pair<std::size_t, decode_status>
decode_until(block&         decoder,
             bit_reader&    reader,
             chunk_data&    out_chunk,
             std::size_t    target_bytes) {
    std::size_t produced = 0;
    decode_status final_status = decode_status::OK;

    while (produced < target_bytes) {
        if (decoder.is_end_of_block()) {
            const auto err = decoder.read_header(reader);
            if (err != decode_status::OK) {
                final_status = err;
                break;
            }
        }
        const std::size_t want = std::min(
            target_bytes - produced, std::size_t{1024 * 1024});
        const auto [view, status] = decoder.read(reader, want);
        if (view.size() > 0) {
            if (view.contains_markers()) {
                out_chunk.uses_marker_storage = true;
                const std::size_t copied =
                    copy_marker_view(out_chunk.marker_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    final_status = decode_status::GENERIC_ERROR;
                    break;
                }
                produced += copied;
            } else {
                if (out_chunk.uses_marker_storage
                    && out_chunk.marker_storage.size > 0) {
                    // Late demotion mid-chunk: the chunk holds a
                    // marker prefix and now wants to add direct
                    // bytes. Narrow the existing markers in-place
                    // — they're all literals (set_initial_window
                    // resolved them) so each lane's low byte is the
                    // resolved byte.
                    auto reserve_or = out_chunk.byte_storage.reserve(
                        out_chunk.marker_storage.size + view.data_size());
                    if (!reserve_or) {
                        final_status = decode_status::GENERIC_ERROR;
                        break;
                    }
                    auto extend_or = out_chunk.byte_storage.extend(
                        out_chunk.marker_storage.size);
                    if (!extend_or) {
                        final_status = decode_status::GENERIC_ERROR;
                        break;
                    }
                    std::uint8_t* dst = extend_or->data();
                    const std::uint16_t* src =
                        out_chunk.marker_storage.data.get();
                    for (std::size_t i = 0; i < out_chunk.marker_storage.size; ++i) {
                        dst[i] = static_cast<std::uint8_t>(src[i] & 0xFFu);
                    }
                    out_chunk.marker_storage.clear();
                    out_chunk.uses_marker_storage = false;
                }
                const std::size_t copied =
                    copy_direct_view(out_chunk.byte_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    final_status = decode_status::GENERIC_ERROR;
                    break;
                }
                produced += copied;
            }
        }
        if (status != decode_status::OK) {
            final_status = status;
            break;
        }
        if (decoder.is_end_of_block() && decoder.is_last_block()) {
            break;
        }
    }
    return {produced, final_status};
}

}  // namespace detail

class chunk_fetcher {
public:
    chunk_fetcher(std::span<const std::uint8_t> compressed_view,
                  std::size_t                   chunk_decompressed_target_bytes,
                  std::size_t                   worker_count,
                  bool                          verify_crc) noexcept
        : _compressed_view(compressed_view),
          _chunk_target_bytes(
              chunk_decompressed_target_bytes == 0
                  ? kDefaultChunkTargetBytes
                  : chunk_decompressed_target_bytes),
          _worker_count(std::clamp(worker_count, std::size_t{1}, kMaxWorkerCount)),
          _verify_crc(verify_crc) {
        _chunk_arena.reserve(kChunkArenaCapacity);
        for (std::size_t i = 0; i < kChunkArenaCapacity; ++i) {
            _chunk_arena.emplace_back(std::make_unique<chunk_data>());
        }
    }

    ~chunk_fetcher() {
        shutdown();
    }

    chunk_fetcher(const chunk_fetcher&) = delete;
    chunk_fetcher& operator=(const chunk_fetcher&) = delete;
    chunk_fetcher(chunk_fetcher&&) = delete;
    chunk_fetcher& operator=(chunk_fetcher&&) = delete;

    void start_producer() noexcept {
        if (_producer_started) return;
        _producer_started = true;
        _producer = std::thread([this]() noexcept { producer_loop(); });
    }

    [[nodiscard]] std::pair<const chunk_data*, bool>
    get_next_chunk() noexcept {
        start_producer();
        std::unique_lock<std::mutex> guard(_state_mutex);
        _state_cv.wait(guard, [this]() noexcept {
            return _ready_count > 0
                || _producer_done.load(std::memory_order_acquire);
        });
        if (_ready_count == 0) {
            const bool has_error = !_last_error_message.empty();
            return {nullptr, has_error};
        }
        const std::size_t slot = _ready_head;
        _ready_head = (_ready_head + 1) % kChunkArenaCapacity;
        --_ready_count;
        _state_cv.notify_all();
        chunk_data* chunk = _chunk_arena[slot].get();
        return {chunk, chunk->state == chunk_state::DECODE_ERROR};
    }

    void release_chunk(const chunk_data* chunk) noexcept {
        if (chunk == nullptr) return;
        std::size_t slot = kChunkArenaCapacity;
        for (std::size_t i = 0; i < kChunkArenaCapacity; ++i) {
            if (_chunk_arena[i].get() == chunk) { slot = i; break; }
        }
        if (slot >= kChunkArenaCapacity) return;
        std::lock_guard<std::mutex> guard(_state_mutex);
        ++_free_count;
        _state_cv.notify_all();
    }

    [[nodiscard]] std::string last_error_message() const noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        return _last_error_message;
    }

    void release_until(std::size_t /*encoded_bit_offset*/) noexcept {}

    void shutdown() noexcept {
        if (!_producer_started) return;
        _shutdown.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _state_cv.notify_all();
        }
        if (_producer.joinable()) _producer.join();
        _producer_started = false;
    }

private:
    /// Producer thread. Iterates over gzip members; for each member,
    /// decodes chunks sequentially (single-thread) and pushes them to
    /// the arena. The orchestrator drains in arena order. This is the
    /// **correctness baseline** — the multi-worker pre-fetch is
    /// layered on top via `prefetch_workers` (see follow-up).
    void producer_loop() noexcept {
        bit_reader reader{_compressed_view};

        while (true) {
            auto header_or = parse_gzip_header(reader, _compressed_view);
            if (!header_or) {
                set_error("gzip header parse failed: " + header_or.error());
                signal_done();
                return;
            }
            // Fresh decoder per member so the LZ77 window resets to
            // empty (RFC 1952 §2 — each member is an independent
            // deflate stream).
            block decoder;
            decoder.reset();
            decoder.set_initial_window(std::span<const std::uint8_t>{});

            while (!_shutdown.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> guard(_state_mutex);
                _state_cv.wait(guard, [this]() noexcept {
                    return _free_count > 0
                        || _shutdown.load(std::memory_order_acquire);
                });
                if (_shutdown.load(std::memory_order_acquire)) return;
                const std::size_t slot = _ready_tail;
                _ready_tail = (_ready_tail + 1) % kChunkArenaCapacity;
                --_free_count;
                guard.unlock();

                chunk_data& chunk = *_chunk_arena[slot];
                chunk.encoded_start_bit_offset = reader.bit_position();
                chunk.state = chunk_state::DIRECTLY_DECODED;
                chunk.uses_marker_storage = false;
                chunk.byte_storage.clear();
                chunk.marker_storage.clear();
                chunk.is_member_final = false;
                chunk.decompressed_size_bytes = 0;
                chunk.error_message.assign(std::string_view{"", 0});

                if (auto reserve_or =
                        chunk.byte_storage.reserve(_chunk_target_bytes);
                    !reserve_or) {
                    chunk.state = chunk_state::DECODE_ERROR;
                    chunk.error_message.assign(reserve_or.error());
                    set_error(reserve_or.error());
                    publish_chunk();
                    signal_done();
                    return;
                }

                const auto [produced, status] =
                    detail::decode_until(decoder, reader, chunk,
                                         _chunk_target_bytes);
                chunk.decompressed_size_bytes = produced;
                chunk.encoded_end_bit_offset = reader.bit_position();
                if (status != decode_status::OK
                    && status != decode_status::END_OF_BLOCK
                    && status != decode_status::END_OF_STREAM) {
                    chunk.state = chunk_state::DECODE_ERROR;
                    chunk.error_message.assign(decode_status_name(status));
                    set_error(std::string{decode_status_name(status)});
                    publish_chunk();
                    signal_done();
                    return;
                }

                if (!chunk.uses_marker_storage) {
                    const std::size_t window_bytes = std::min<std::size_t>(
                        chunk.byte_storage.size, kChunkTrailingWindowBytes);
                    if (window_bytes > 0) {
                        const std::uint8_t* src =
                            chunk.byte_storage.data.get()
                            + (chunk.byte_storage.size - window_bytes);
                        std::memcpy(chunk.trailing_window.data(), src, window_bytes);
                    }
                    chunk.trailing_window_size = window_bytes;
                }

                const bool stream_end =
                    decoder.is_end_of_block() && decoder.is_last_block();
                if (stream_end) {
                    chunk.is_member_final = true;
                    publish_chunk();
                    // Skip footer (CRC32 + ISIZE = 8 B). We rely on
                    // zlib semantics — we don't currently verify the
                    // CRC across member boundaries. A separate
                    // CRC-verification pass is a follow-up.
                    reader.align_to_byte_boundary();
                    if (reader.bits_buffered() < 64) reader.refill();
                    if (reader.bits_buffered() >= 64) {
                        (void)reader.read_aligned_le_u64();
                    }
                    break;  // out of inner block-decode loop
                }
                publish_chunk();
            }

            // Multi-member: continue if more bytes remain.
            const std::size_t consumed_bytes = reader.bit_position() / 8u;
            if (consumed_bytes >= _compressed_view.size()) break;
        }

        signal_done();
    }

    void publish_chunk() noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        ++_ready_count;
        _state_cv.notify_all();
    }

    void signal_done() noexcept {
        _producer_done.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> guard(_state_mutex);
        _state_cv.notify_all();
    }

    void set_error(std::string message) noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        if (_last_error_message.empty()) {
            _last_error_message = std::move(message);
        }
    }

    std::span<const std::uint8_t> _compressed_view;
    std::size_t                   _chunk_target_bytes;
    std::size_t                   _worker_count;
    bool                          _verify_crc;

    std::vector<std::unique_ptr<chunk_data>> _chunk_arena;

    mutable std::mutex             _state_mutex;
    std::condition_variable        _state_cv;
    std::size_t                    _ready_head  = 0;
    std::size_t                    _ready_tail  = 0;
    std::size_t                    _ready_count = 0;
    std::size_t                    _free_count  = kChunkArenaCapacity;

    std::atomic<bool>              _shutdown{false};
    std::atomic<bool>              _producer_done{false};
    std::thread                    _producer;
    bool                           _producer_started = false;
    std::string                    _last_error_message;
};

}  // namespace bioseq::io::compression
