/**
 * @file parallel_gzip_source.cppm
 * @brief `io_source`-conforming adapter on top of `parallel_gzip_decoder`.
 *
 * PIMPL via `std::unique_ptr<impl>` (the one place CODE_STYLE.md §1
 * explicitly allows). The PIMPL holds the mmap source (which keeps the
 * fd alive for the duration of the parse), the decoder, and the
 * trailing error message. Constructed via
 * `try_open_from_mmap(mmap_source, options)` so the dispatcher can
 * hand off the already-opened mmap source.
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

export module bioseq_io.compression.parallel_gzip_source;

import mlogging;
import bioseq_io.sources;
import bioseq_io.options;
import bioseq_io.compression.parallel_gzip_decoder;

export namespace bioseq::io {

class parallel_gzip_source {
public:
    parallel_gzip_source() = default;
    parallel_gzip_source(const parallel_gzip_source&) = delete;
    parallel_gzip_source& operator=(const parallel_gzip_source&) = delete;
    parallel_gzip_source(parallel_gzip_source&&) noexcept = default;
    parallel_gzip_source& operator=(parallel_gzip_source&&) noexcept = default;
    ~parallel_gzip_source() = default;

    /// Open from an already-opened mmap source. The `options` carry
    /// chunk size / worker count knobs from `io_options`.
    [[nodiscard]] static std::expected<parallel_gzip_source, std::string>
    try_open_from_mmap(mmap_source mmap_in,
                       const reader_options& options) noexcept {
        // Take a direct view of the mmap for the decoder. The mmap
        // outlives the decoder — both live in the impl.
        auto direct = mmap_in.direct_view();
        std::span<const std::uint8_t> compressed_view{
            direct.first, direct.second};

        parallel_gzip_source source;
        source._impl = std::make_unique<impl>();
        source._impl->mapped_source = std::move(mmap_in);
        source._impl->compressed_view = compressed_view;

        // Compute chunk target / worker count from options.
        std::size_t chunk_bytes = options.io.parallel_gzip_chunk_bytes;
        if (chunk_bytes == 0) {
            chunk_bytes = 4 * 1024 * 1024;  // kDefaultParallelGzipChunkSize
        }
        std::size_t worker_count = options.io.parallel_gzip_thread_count;
        if (worker_count == 0) {
            const unsigned hardware = std::thread::hardware_concurrency();
            worker_count = (hardware > 2) ? std::min<std::size_t>(hardware - 2, 8) : 1;
        }

        source._impl->decoder = std::make_unique<
            compression::parallel_gzip_decoder>(
            compressed_view,
            worker_count,
            chunk_bytes,
            options.io.parallel_gzip_verify_crc);
        return source;
    }

    [[nodiscard]] read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        if (_impl == nullptr) {
            _last_error_message = "parallel_gzip_source: not initialised";
            return read_result{0, false, true};
        }
        const auto outcome = _impl->decoder->read_into(dst, cap);
        if (outcome.has_error) {
            _last_error_message = _impl->decoder->last_error_message();
        }
        return outcome;
    }

    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

    [[nodiscard]] std::optional<std::span<const std::uint8_t>>
    next_decoded_chunk_view() noexcept {
        if (_impl == nullptr) return std::nullopt;
        return _impl->decoder->next_decoded_chunk_view();
    }
    void release_chunk() noexcept {
        if (_impl != nullptr) _impl->decoder->release_chunk();
    }

private:
    struct impl {
        mmap_source                                       mapped_source;
        std::span<const std::uint8_t>                     compressed_view;
        std::unique_ptr<compression::parallel_gzip_decoder> decoder;
    };
    std::unique_ptr<impl> _impl;
    std::string           _last_error_message;
};

}  // namespace bioseq::io
