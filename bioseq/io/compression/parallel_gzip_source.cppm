/**
 * @file parallel_gzip_source.cppm
 * @brief Byte source backed by the parallel gzip decoder.
 *
 * The owned implementation keeps the mapping alive for every compressed view
 * held by the decoder.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

export module bioseq.io.compression.parallel_gzip_source;

import bioseq.io.sources;
import bioseq.io.options;
import bioseq.io.compression.parallel_gzip_decoder;
import bioseq.parallelism.pool;

export namespace bioseq::io {

/// Adapts a memory-mapped gzip stream to the I/O source protocol.
///
/// The source owns the mapping and decoder together so the compressed span
/// remains valid until destruction.
class parallel_gzip_source {
  public:
    /// Construct an empty source that reports an error if read.
    parallel_gzip_source() = default;

    /// Sources exclusively own their mapping and decoder.
    parallel_gzip_source(const parallel_gzip_source&) = delete;

    /// Prevents copying the mapping and active decoder by assignment.
    parallel_gzip_source& operator=(const parallel_gzip_source&) = delete;

    /// Transfer the mapping and decoder into a new source.
    parallel_gzip_source(parallel_gzip_source&&) noexcept = default;

    /// Replace this source with transferred mapping and decoder state.
    parallel_gzip_source& operator=(parallel_gzip_source&&) noexcept = default;

    /// Release the decoder before its backing mapping.
    ~parallel_gzip_source() = default;

    /// Takes ownership of an open mapping and creates its gzip decoder.
    /// @param[in] mmap_in Mapping transferred into the returned source.
    /// @param[in] options Chunk size, worker count, and CRC calculation setting.
    /// @return Source that begins decoding on its first read.
    [[nodiscard]] static std::expected<parallel_gzip_source, std::string>
    try_open_from_mmap(mmap_source mmap_in, const reader_options& options) noexcept {
        // The implementation owns the mapping until after the decoder is destroyed.
        auto direct = mmap_in.direct_view();
        std::span<const std::uint8_t> compressed_view{direct.first, direct.second};

        parallel_gzip_source source;
        source._impl = std::make_unique<impl>();
        source._impl->mapped_source = std::move(mmap_in);
        source._impl->compressed_view = compressed_view;

        // Use  the configured chunk size and worker count, with defaults for zero.
        std::size_t chunk_bytes = options.io.parallel_gzip_chunk_bytes;
        if (chunk_bytes == 0) {
            chunk_bytes = 4 * 1024 * 1024; // default_parallel_gzip_chunk_size
        }
        std::size_t worker_count = options.io.parallel_gzip_thread_count;
        if (worker_count == 0) {
            const unsigned hardware = bioseq::parallelism::default_concurrency_limit();
            worker_count = (hardware > 2) ? std::min<std::size_t>(hardware - 2, 8) : 1;
        }

        source._impl->decoder = std::make_unique<compression::parallel_gzip_decoder>(
            compressed_view, worker_count, chunk_bytes, options.io.parallel_gzip_verify_crc);
        return source;
    }

    /// Copy up to `cap` decompressed bytes into caller-owned storage.
    ///
    /// @param[out] dst Destination containing at least `cap` writable bytes
    /// @param[in] cap Maximum byte count to copy
    /// @return Read count together with EOF and error state
    [[nodiscard]] source_read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        if (_impl == nullptr) {
            _last_error_message = "parallel_gzip_source: not initialised";
            return source_read_result{0, false, true};
        }
        const auto outcome = _impl->decoder->read_into(dst, cap);
        if (outcome.has_error) {
            _last_error_message = _impl->decoder->last_error_message();
        }
        return outcome;
    }

    /// Return the terminal source diagnostic, or an empty string on success.
    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

    /// Forward the decoder's optional borrowed-chunk interface.
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> next_decoded_chunk_view() noexcept {
        if (_impl == nullptr)
            return std::nullopt;
        return _impl->decoder->next_decoded_chunk_view();
    }
    /// Release a borrowed chunk when the decoder exposes one.
    void release_chunk() noexcept {
        if (_impl != nullptr)
            _impl->decoder->release_chunk();
    }

  private:
    /// Owns the mapping before the decoder so borrowed compressed bytes stay valid.
    struct impl {
        mmap_source mapped_source;
        std::span<const std::uint8_t> compressed_view;
        std::unique_ptr<compression::parallel_gzip_decoder> decoder;
    };
    std::unique_ptr<impl> _impl;
    std::string _last_error_message;
};

} // namespace bioseq::io
