module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <zlib.h>

export module bioseq.external.io.gzip_decode_source;

import bioseq.io.sources;  // io_source concept, read_result

// DOC0372 — arbitrary-source serial gzip inflate. Unlike the file-only
// gzFile-based `gzip_source`, this wraps ANY `io_source` (notably the libcurl
// `remote_byte_source`), so a remote `.fa.gz` asset streams through the shared
// FASTA parser without a temporary file. Decompression is incremental; decoded
// bytes are counted against a hard cap and an expansion-ratio ceiling so a small
// wire body cannot inflate past the admitted decoded budget (gzip-bomb guard).
// zlib's integrated gzip framing validates the header, CRC-32, and ISIZE trailer.
export namespace bioseq::external::io {

struct gzip_decode_limits {
    std::uint64_t maximum_encoded_bytes = 0;   // 0 = unbounded
    std::uint64_t maximum_decoded_bytes = 0;   // 0 = unbounded
    std::uint64_t expansion_numerator = 1000;  // decoded <= grace + enc*num/den
    std::uint64_t expansion_denominator = 1;
    std::uint64_t expansion_grace_bytes = 64 * 1024;
    std::size_t input_chunk_bytes = 128 * 1024;
};

template <bioseq::io::io_source Source>
class gzip_decode_source {
public:
    gzip_decode_source(const gzip_decode_source&) = delete;
    gzip_decode_source& operator=(const gzip_decode_source&) = delete;

    gzip_decode_source(Source source, gzip_decode_limits limits)
        : source_(std::move(source)), limits_(limits) {
        input_.resize(limits_.input_chunk_bytes == 0 ? 128 * 1024
                                                     : limits_.input_chunk_bytes);
        stream_.zalloc = nullptr;
        stream_.zfree = nullptr;
        stream_.opaque = nullptr;
        stream_.next_in = nullptr;
        stream_.avail_in = 0;
        // 15 window bits + 32 => auto-detect gzip or zlib header framing.
        const int rc = ::inflateInit2(&stream_, 15 + 32);
        if (rc != Z_OK) {
            fail("zlib inflateInit2 failed");
        } else {
            inited_ = true;
        }
    }

    gzip_decode_source(gzip_decode_source&& other) noexcept { adopt(other); }
    gzip_decode_source& operator=(gzip_decode_source&& other) noexcept {
        if (this != &other) {
            teardown();
            adopt(other);
        }
        return *this;
    }
    ~gzip_decode_source() { teardown(); }

    [[nodiscard]] bioseq::io::read_result read_into(std::uint8_t* destination,
                                                    std::size_t capacity) {
        bioseq::io::read_result result;
        if (failed_) {
            result.has_error = true;
            return result;
        }
        if (stream_end_) {
            result.is_eof = true;
            return result;
        }
        stream_.next_out = destination;
        stream_.avail_out = static_cast<uInt>(
            capacity > 0xFFFFFFFFu ? 0xFFFFFFFFu : capacity);
        while (stream_.avail_out > 0 && !stream_end_) {
            if (stream_.avail_in == 0 && !source_eof_) {
                const bioseq::io::read_result refill =
                    source_.read_into(input_.data(), input_.size());
                if (refill.has_error) {
                    fail(source_.last_error_message());
                    result.has_error = true;
                    return result;
                }
                stream_.next_in = input_.data();
                stream_.avail_in = static_cast<uInt>(refill.bytes_read);
                encoded_total_ += refill.bytes_read;
                if (limits_.maximum_encoded_bytes != 0 &&
                    encoded_total_ > limits_.maximum_encoded_bytes) {
                    fail("compressed transfer exceeded the encoded byte cap");
                    result.has_error = true;
                    return result;
                }
                if (refill.is_eof) {
                    source_eof_ = true;
                }
            }
            const int flush = source_eof_ ? Z_FINISH : Z_NO_FLUSH;
            const int rc = ::inflate(&stream_, flush);
            if (rc == Z_STREAM_END) {
                stream_end_ = true;
                break;
            }
            if (rc == Z_BUF_ERROR) {
                if (stream_.avail_in == 0 && source_eof_) {
                    fail("gzip stream ended before the trailer");
                    result.has_error = true;
                    return result;
                }
                if (stream_.avail_out == 0) {
                    break;
                }
                continue;  // needs another input refill
            }
            if (rc != Z_OK) {
                fail(stream_.msg != nullptr ? stream_.msg
                                            : "zlib inflate error");
                result.has_error = true;
                return result;
            }
        }
        const std::size_t produced =
            capacity - static_cast<std::size_t>(stream_.avail_out);
        decoded_total_ += produced;
        if (!within_limits()) {
            fail("decompressed size exceeded the decoded/expansion budget");
            result.has_error = true;
            return result;
        }
        result.bytes_read = produced;
        result.is_eof = stream_end_ && produced == 0;
        return result;
    }

    [[nodiscard]] std::string last_error_message() const {
        return error_message_;
    }

    [[nodiscard]] std::uint64_t encoded_bytes() const noexcept {
        return encoded_total_;
    }
    [[nodiscard]] std::uint64_t decoded_bytes() const noexcept {
        return decoded_total_;
    }

private:
    [[nodiscard]] bool within_limits() const noexcept {
        if (limits_.maximum_decoded_bytes != 0 &&
            decoded_total_ > limits_.maximum_decoded_bytes) {
            return false;
        }
        if (limits_.expansion_denominator != 0) {
            // decoded <= grace + encoded * num / den, computed to avoid overflow.
            const std::uint64_t scaled =
                encoded_total_ / limits_.expansion_denominator *
                    limits_.expansion_numerator +
                (encoded_total_ % limits_.expansion_denominator) *
                    limits_.expansion_numerator /
                    limits_.expansion_denominator;
            const std::uint64_t ceiling = limits_.expansion_grace_bytes + scaled;
            if (decoded_total_ > ceiling) {
                return false;
            }
        }
        return true;
    }

    void fail(std::string message) {
        failed_ = true;
        if (error_message_.empty()) {
            error_message_ = std::move(message);
        }
    }

    void adopt(gzip_decode_source& other) noexcept {
        source_ = std::move(other.source_);
        limits_ = other.limits_;
        input_ = std::move(other.input_);
        stream_ = other.stream_;
        inited_ = other.inited_;
        source_eof_ = other.source_eof_;
        stream_end_ = other.stream_end_;
        failed_ = other.failed_;
        error_message_ = std::move(other.error_message_);
        encoded_total_ = other.encoded_total_;
        decoded_total_ = other.decoded_total_;
        other.inited_ = false;  // moved-from stream no longer owns zlib state
    }

    void teardown() noexcept {
        if (inited_) {
            ::inflateEnd(&stream_);
            inited_ = false;
        }
    }

    Source source_;
    gzip_decode_limits limits_;
    std::vector<std::uint8_t> input_;
    z_stream stream_{};
    bool inited_ = false;
    bool source_eof_ = false;
    bool stream_end_ = false;
    bool failed_ = false;
    std::string error_message_;
    std::uint64_t encoded_total_ = 0;
    std::uint64_t decoded_total_ = 0;
};

}  // namespace bioseq::external::io
