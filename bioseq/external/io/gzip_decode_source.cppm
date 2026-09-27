/**
 * @file external/io/gzip_decode_source.cppm
 * @brief Incrementally decodes gzip or zlib input while counting encoded and decoded bytes.
 */
module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <zlib.h>

export module bioseq.external.io.gzip_decode_source;

import bioseq.io.sources; // io_source concept, source_read_result

/// Sources that decompress gzip or zlib data as callers request bytes.
export namespace bioseq::external::io {

/**
 * Byte caps, expansion ratio, and input-buffer size for decompression.
 *
 * Zero disables either byte cap. A zero expansion denominator disables the
 * ratio check. Configure limits so counter, multiplication, and addition
 * arithmetic fits uint64_t, and keep input chunks within zlib's uInt range.
 */
struct gzip_decode_limits {
    std::uint64_t maximum_encoded_bytes = 0;  // 0 = unbounded
    std::uint64_t maximum_decoded_bytes = 0;  // 0 = unbounded
    std::uint64_t expansion_numerator = 1000; // decoded <= grace + enc*num/den
    std::uint64_t expansion_denominator = 1;
    std::uint64_t expansion_grace_bytes = 64 * 1024;
    std::size_t input_chunk_bytes = 128 * 1024;
};

/**
 * Decodes one gzip or zlib stream from an owned source.
 *
 * Stops at the first compressed stream end. It does not restart zlib for
 * concatenated gzip members or require the underlying source to be exhausted.
 */
template <bioseq::io::io_source p_source_type> class gzip_decode_source {
  public:
    gzip_decode_source(const gzip_decode_source&) = delete;
    gzip_decode_source& operator=(const gzip_decode_source&) = delete;

    /// Takes ownership of the source and initializes decoding with the supplied limits.
    /// A zero input-chunk size selects the default buffer size.
    gzip_decode_source(p_source_type source, gzip_decode_limits limits)
        : source_(std::move(source)), limits_(limits) {
        input_.resize(limits_.input_chunk_bytes == 0 ? 128 * 1024 : limits_.input_chunk_bytes);
        stream_.zalloc = nullptr;
        stream_.zfree = nullptr;
        stream_.opaque = nullptr;
        stream_.next_in = nullptr;
        stream_.avail_in = 0;
        // 15 window bits + 32 => auto-detect gzip or zlib header framing.
        const int zlib_status = ::inflateInit2(&stream_, 15 + 32);
        if (zlib_status != Z_OK) {
            record_failure("zlib inflateInit2 failed");
        } else {
            is_initialized_ = true;
        }
    }

    /// Transfers the stored source, decoder state, and counters.
    gzip_decode_source(gzip_decode_source&& other) noexcept {
        adopt_state_from(std::move(other));
    }
    /// Releases current state and transfers another decoder.
    gzip_decode_source& operator=(gzip_decode_source&& other) noexcept {
        if (this != &other) {
            release_zlib_state();
            adopt_state_from(std::move(other));
        }
        return *this;
    }
    /// Releases the owned zlib stream.
    ~gzip_decode_source() {
        release_zlib_state();
    }

    /**
     * Reads decompressed bytes into destination.
     *
     * capacity must fit zlib's uInt range. Errors are terminal and reported through
     * has_error and last_error_message(). A failed call can modify destination,
     * so callers must discard that call's bytes. End of stream is reported on the
     * next read after the final nonempty result.
     */
    [[nodiscard]] bioseq::io::source_read_result read_into(std::uint8_t* destination,
                                                           std::size_t capacity) {
        bioseq::io::source_read_result result;
        if (has_failed_) {
            result.has_error = true;
            return result;
        }
        if (is_stream_finished_) {
            result.is_eof = true;
            return result;
        }
        stream_.next_out = destination;
        stream_.avail_out = static_cast<uInt>(capacity > 0xFFFFFFFFu ? 0xFFFFFFFFu : capacity);
        while (stream_.avail_out > 0 && !is_stream_finished_) {
            if (stream_.avail_in == 0 && !is_source_eof_) {
                const bioseq::io::source_read_result refill =
                    source_.read_into(input_.data(), input_.size());
                if (refill.has_error) {
                    record_failure(source_.last_error_message());
                    result.has_error = true;
                    return result;
                }
                stream_.next_in = input_.data();
                stream_.avail_in = static_cast<uInt>(refill.bytes_read);
                encoded_total_ += refill.bytes_read;
                if (limits_.maximum_encoded_bytes != 0 &&
                    encoded_total_ > limits_.maximum_encoded_bytes) {
                    record_failure("compressed transfer exceeded the encoded byte cap");
                    result.has_error = true;
                    return result;
                }
                if (refill.is_eof) {
                    is_source_eof_ = true;
                }
            }
            const int flush = is_source_eof_ ? Z_FINISH : Z_NO_FLUSH;
            const int zlib_status = ::inflate(&stream_, flush);
            if (zlib_status == Z_STREAM_END) {
                is_stream_finished_ = true;
                break;
            }
            if (zlib_status == Z_BUF_ERROR) {
                if (stream_.avail_in == 0 && is_source_eof_) {
                    record_failure("gzip stream ended before the trailer");
                    result.has_error = true;
                    return result;
                }
                if (stream_.avail_out == 0) {
                    break;
                }
                continue; // Continue until more input or output space is needed.
            }
            if (zlib_status != Z_OK) {
                record_failure(stream_.msg != nullptr ? stream_.msg : "zlib inflate error");
                result.has_error = true;
                return result;
            }
        }
        const std::size_t produced = capacity - static_cast<std::size_t>(stream_.avail_out);
        decoded_total_ += produced;
        if (!is_within_limits()) {
            record_failure("decompressed size exceeded the decoded/expansion budget");
            result.has_error = true;
            return result;
        }
        result.bytes_read = produced;
        result.is_eof = is_stream_finished_ && produced == 0;
        return result;
    }

    /// Returns the terminal decoder error, if any.
    [[nodiscard]] std::string last_error_message() const {
        return error_message_;
    }

    /// Returns compressed bytes fetched from the source, including buffered unread input.
    [[nodiscard]] std::uint64_t count_encoded_bytes() const noexcept {
        return encoded_total_;
    }
    /// Returns decoded bytes counted so far, including a block that exceeded a configured limit.
    [[nodiscard]] std::uint64_t count_decoded_bytes() const noexcept {
        return decoded_total_;
    }

  private:
    /// Checks decoded-byte and expansion limits against the current counters.
    [[nodiscard]] bool is_within_limits() const noexcept {
        if (limits_.maximum_decoded_bytes != 0 && decoded_total_ > limits_.maximum_decoded_bytes) {
            return false;
        }
        if (limits_.expansion_denominator != 0) {
            // Split the encoded-byte division before scaling. The configured ratio
            // and counters must still keep these products and sums within uint64_t.
            const std::uint64_t scaled =
                encoded_total_ / limits_.expansion_denominator * limits_.expansion_numerator +
                (encoded_total_ % limits_.expansion_denominator) * limits_.expansion_numerator /
                    limits_.expansion_denominator;
            const std::uint64_t ceiling = limits_.expansion_grace_bytes + scaled;
            if (decoded_total_ > ceiling) {
                return false;
            }
        }
        return true;
    }

    /// Marks the decoder as failed and keeps the first nonempty error message.
    void record_failure(std::string message) {
        has_failed_ = true;
        if (error_message_.empty()) {
            error_message_ = std::move(message);
        }
    }

    /// Moves source storage and copies the zlib state and accounting fields.
    void adopt_state_from(gzip_decode_source&& other) noexcept {
        source_ = std::move(other.source_);
        limits_ = other.limits_;
        input_ = std::move(other.input_);
        stream_ = other.stream_;
        is_initialized_ = other.is_initialized_;
        is_source_eof_ = other.is_source_eof_;
        is_stream_finished_ = other.is_stream_finished_;
        has_failed_ = other.has_failed_;
        error_message_ = std::move(other.error_message_);
        encoded_total_ = other.encoded_total_;
        decoded_total_ = other.decoded_total_;
        other.is_initialized_ = false; // moved-from stream no longer owns zlib state
    }

    /// Releases the owned zlib stream when initialized.
    void release_zlib_state() noexcept {
        if (is_initialized_) {
            ::inflateEnd(&stream_);
            is_initialized_ = false;
        }
    }

    p_source_type source_;
    gzip_decode_limits limits_;
    std::vector<std::uint8_t> input_;
    z_stream stream_{};
    bool is_initialized_ = false;
    bool is_source_eof_ = false;
    bool is_stream_finished_ = false;
    bool has_failed_ = false;
    std::string error_message_;
    std::uint64_t encoded_total_ = 0;
    std::uint64_t decoded_total_ = 0;
};

} // namespace bioseq::external::io
