// DOC0372 — gzip_decode_source round-trip test. Proves the generic
// template<io_source Source> gzip_decode_source actually inflates a gzip stream
// pulled from an arbitrary io_source (the mechanism used to stream a remote
// .fa.gz asset), including the decoded-byte cap. Deterministic + offline.
//
// Catch taxonomy: external API integration over the streaming decoder.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include <catch2/catch_test_macros.hpp>

import bioseq.io.sources;  // io_source, read_result
import bioseq.external.io.gzip_decode_source;

namespace exio = bioseq::external::io;

namespace {

// A minimal in-memory io_source over a byte vector (satisfies bioseq::io::io_source).
class memory_source {
public:
    explicit memory_source(std::vector<std::uint8_t> data)
        : data_(std::move(data)) {}

    bioseq::io::read_result read_into(std::uint8_t* destination,
                                      std::size_t capacity) {
        bioseq::io::read_result result;
        const std::size_t remaining = data_.size() - offset_;
        const std::size_t take = remaining < capacity ? remaining : capacity;
        std::copy(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                  data_.begin() + static_cast<std::ptrdiff_t>(offset_ + take),
                  destination);
        offset_ += take;
        result.bytes_read = take;
        result.is_eof = (offset_ >= data_.size());
        return result;
    }
    std::string last_error_message() const { return {}; }

private:
    std::vector<std::uint8_t> data_;
    std::size_t offset_ = 0;
};

static_assert(bioseq::io::io_source<memory_source>);

// gzip-compress `plain` into a gzip-framed byte vector using zlib.
std::vector<std::uint8_t> gzip_compress(const std::string& plain) {
    z_stream stream{};
    // 15 window bits + 16 => gzip framing.
    REQUIRE(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                         Z_DEFAULT_STRATEGY) == Z_OK);
    stream.next_in =
        reinterpret_cast<Bytef*>(const_cast<char*>(plain.data()));
    stream.avail_in = static_cast<uInt>(plain.size());
    std::vector<std::uint8_t> out(plain.size() + 1024);
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    out.resize(out.size() - stream.avail_out);
    deflateEnd(&stream);
    return out;
}

std::string decode_all(exio::gzip_decode_source<memory_source>& source,
                       bool& error) {
    std::string out;
    std::vector<std::uint8_t> block(4096);
    for (;;) {
        const bioseq::io::read_result result =
            source.read_into(block.data(), block.size());
        if (result.has_error) {
            error = true;
            break;
        }
        out.append(reinterpret_cast<const char*>(block.data()),
                   result.bytes_read);
        if (result.is_eof) {
            break;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("gzip_decode_source inflates a gzip stream from an arbitrary source",
          "[component:external_api][layer:integration][feature:external_source]") {
    std::string plain =
        ">chr_test synthetic\n";
    for (int i = 0; i < 5000; ++i) {
        plain += "ACGTACGTACGTNNNNACGTACGTACGTACGT\n";
    }
    const std::vector<std::uint8_t> compressed = gzip_compress(plain);
    REQUIRE(compressed.size() < plain.size());  // actually compressed

    SECTION("round-trip equals the original") {
        exio::gzip_decode_limits limits;
        limits.maximum_decoded_bytes = plain.size() + 1024;
        exio::gzip_decode_source<memory_source> source(
            memory_source(compressed), limits);
        bool error = false;
        const std::string decoded = decode_all(source, error);
        REQUIRE_FALSE(error);
        REQUIRE(decoded == plain);
    }

    SECTION("decoded-byte cap is enforced (gzip-bomb guard)") {
        exio::gzip_decode_limits limits;
        limits.maximum_decoded_bytes = 1024;  // far below the decoded size
        limits.expansion_numerator = 1000000; // don't trip the ratio first
        exio::gzip_decode_source<memory_source> source(
            memory_source(compressed), limits);
        bool error = false;
        const std::string decoded = decode_all(source, error);
        REQUIRE(error);                 // must fail rather than over-inflate
        REQUIRE(decoded.size() <= 1024 + 4096);
    }
}
