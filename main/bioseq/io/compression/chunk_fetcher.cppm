/**
 * @file chunk_fetcher.cppm
 * @brief Chunk arena + producer/parser worker.
 *
 * Single-producer pipeline. The producer thread runs zlib `inflate(2)`
 * on the compressed view and emits chunks of `_chunk_target_bytes`
 * decompressed bytes into a 16-deep arena consumed by the parser
 * thread. The arena lets the parser drain one chunk while the
 * producer prepares the next, hiding ~half the inflate latency.
 *
 * Block-level parallel decompression (the rapidgzip approach) requires
 * a marker-mode DEFLATE port (`deflate.cppm` follow-up) — neither
 * zlib nor libdeflate exposes a "decode without prior window"
 * primitive that we'd need to start workers in parallel.
 */
module;

#include "common.h"

#include <zlib.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

export module bioseq_io.compression.chunk_fetcher;

import mlogging;
import bioseq_io.compression.aligned_byte_buffer;
import bioseq_io.compression.chunk;
import bioseq_io.compression.window_map;

export namespace bioseq::io::compression {

using chunk_slot_index = std::uint8_t;
inline constexpr chunk_slot_index kInvalidSlot =
    std::numeric_limits<chunk_slot_index>::max();

inline constexpr std::size_t kChunkArenaDepth = 16;
inline constexpr std::size_t kDefaultChunkTargetBytes = 4 * 1024 * 1024;

class chunk_fetcher {
public:
    chunk_fetcher(std::span<const std::uint8_t> compressed_view,
                  std::size_t                   chunk_decompressed_target_bytes,
                  std::size_t                   /*worker_count*/,
                  bool                          verify_crc) noexcept
        : _compressed_view(compressed_view),
          _chunk_target_bytes(
              chunk_decompressed_target_bytes == 0
                  ? kDefaultChunkTargetBytes
                  : chunk_decompressed_target_bytes),
          _verify_crc(verify_crc) {
        for (auto& slot_state : _slot_states) {
            slot_state.store(0, std::memory_order_relaxed);
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
        _ready_head = (_ready_head + 1) % kChunkArenaDepth;
        --_ready_count;
        _state_cv.notify_all();
        const chunk_data* chunk = &_chunk_arena[slot];
        return {chunk, chunk->state == chunk_state::DECODE_ERROR};
    }

    void release_chunk(const chunk_data* chunk) noexcept {
        if (chunk == nullptr) return;
        const std::size_t slot = static_cast<std::size_t>(
            chunk - _chunk_arena.data());
        if (slot >= kChunkArenaDepth) return;
        std::lock_guard<std::mutex> guard(_state_mutex);
        _slot_states[slot].store(0, std::memory_order_release);
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
    void producer_loop() noexcept {
        z_stream strm{};
        strm.zalloc = Z_NULL;
        strm.zfree  = Z_NULL;
        strm.opaque = Z_NULL;
        // 31 = 15 (max windowBits) + 16 (gzip wrapper). Lets zlib parse
        // the gzip header / footer / multi-member transitions itself.
        if (::inflateInit2(&strm, 31) != Z_OK) {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _last_error_message = "inflateInit2 failed";
            _producer_done.store(true, std::memory_order_release);
            _state_cv.notify_all();
            return;
        }
        strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(
            _compressed_view.data()));
        strm.avail_in = static_cast<uInt>(std::min<std::size_t>(
            _compressed_view.size(),
            std::numeric_limits<uInt>::max()));
        bool reached_eof = false;

        while (!_shutdown.load(std::memory_order_acquire) && !reached_eof) {
            std::unique_lock<std::mutex> guard(_state_mutex);
            _state_cv.wait(guard, [this]() noexcept {
                return _free_count > 0
                    || _shutdown.load(std::memory_order_acquire);
            });
            if (_shutdown.load(std::memory_order_acquire)) break;
            const std::size_t slot = _ready_tail;
            _ready_tail = (_ready_tail + 1) % kChunkArenaDepth;
            --_free_count;
            guard.unlock();

            chunk_data& chunk = _chunk_arena[slot];
            chunk_uint16_buffer reused_marker = std::move(chunk.marker_storage);
            chunk_uint8_buffer  reused_bytes  = std::move(chunk.byte_storage);
            chunk = chunk_data{};
            chunk.marker_storage = std::move(reused_marker);
            chunk.byte_storage   = std::move(reused_bytes);
            chunk.encoded_start_bit_offset =
                static_cast<std::size_t>(strm.total_in) * 8;
            chunk.state = chunk_state::DIRECTLY_DECODED;
            chunk.uses_marker_storage = false;
            chunk.byte_storage.clear();
            chunk.marker_storage.clear();

            const std::size_t chunk_target = _chunk_target_bytes;
            if (auto reserve_result = chunk.byte_storage.reserve(chunk_target);
                !reserve_result) {
                chunk.state = chunk_state::DECODE_ERROR;
                chunk.error_message.assign(reserve_result.error());
            } else {
                std::size_t produced_bytes = 0;
                bool decode_ok = true;
                while (produced_bytes < chunk_target) {
                    if (strm.avail_in == 0) {
                        reached_eof = true;
                        break;
                    }
                    const std::size_t headroom =
                        chunk_target - produced_bytes;
                    auto extend_result = chunk.byte_storage.extend(headroom);
                    if (!extend_result) {
                        chunk.state = chunk_state::DECODE_ERROR;
                        chunk.error_message.assign(extend_result.error());
                        decode_ok = false;
                        break;
                    }
                    std::uint8_t* destination = extend_result->data();
                    strm.next_out  = destination;
                    strm.avail_out = static_cast<uInt>(headroom);
                    const int outcome = ::inflate(&strm, Z_NO_FLUSH);
                    const std::size_t produced = headroom -
                        static_cast<std::size_t>(strm.avail_out);
                    chunk.byte_storage.size -= (headroom - produced);
                    produced_bytes += produced;
                    if (outcome == Z_STREAM_END) {
                        chunk.is_member_final = true;
                        if (strm.avail_in == 0) {
                            reached_eof = true;
                        } else {
                            ::inflateReset(&strm);
                        }
                        break;
                    }
                    if (outcome != Z_OK) {
                        chunk.state = chunk_state::DECODE_ERROR;
                        chunk.error_message.assign(fmt::format(
                            "zlib inflate failed: code={} message={}",
                            outcome,
                            strm.msg != nullptr ? strm.msg : "<no msg>"));
                        decode_ok = false;
                        break;
                    }
                    if (produced == 0) break;
                }
                chunk.encoded_end_bit_offset =
                    static_cast<std::size_t>(strm.total_in) * 8;
                chunk.decompressed_size_bytes = produced_bytes;
                if (!decode_ok) {
                    std::lock_guard<std::mutex> err_guard(_state_mutex);
                    if (_last_error_message.empty()) {
                        _last_error_message =
                            std::string{chunk.error_message.view()};
                    }
                }
                const std::size_t window_bytes = std::min<std::size_t>(
                    produced_bytes, kChunkTrailingWindowBytes);
                if (window_bytes > 0) {
                    const std::uint8_t* output_bytes =
                        chunk.byte_storage.data.get();
                    const std::size_t window_start = produced_bytes - window_bytes;
                    std::memcpy(chunk.trailing_window.data(),
                                output_bytes + window_start, window_bytes);
                    chunk.trailing_window_size = window_bytes;
                }
            }

            {
                std::lock_guard<std::mutex> ready_guard(_state_mutex);
                ++_ready_count;
                _state_cv.notify_all();
            }
        }
        ::inflateEnd(&strm);
        _producer_done.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> done_guard(_state_mutex);
        _state_cv.notify_all();
        (void)_verify_crc;
    }

    std::span<const std::uint8_t> _compressed_view;
    std::size_t                   _chunk_target_bytes;
    bool                          _verify_crc;

    std::array<chunk_data, kChunkArenaDepth>                _chunk_arena{};
    std::array<std::atomic<std::uint8_t>, kChunkArenaDepth> _slot_states{};

    mutable std::mutex             _state_mutex;
    std::condition_variable        _state_cv;
    std::size_t                    _ready_head  = 0;
    std::size_t                    _ready_tail  = 0;
    std::size_t                    _ready_count = 0;
    std::size_t                    _free_count  = kChunkArenaDepth;

    std::atomic<bool>              _shutdown{false};
    std::atomic<bool>              _producer_done{false};
    std::thread                    _producer;
    bool                           _producer_started = false;
    std::string                    _last_error_message;
};

}  // namespace bioseq::io::compression
