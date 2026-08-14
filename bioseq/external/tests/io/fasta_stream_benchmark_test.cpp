// Compare bounded FASTA streaming with a full-body baseline over the same local
// payload. The fixture requires filesystem access and a deliberately large
// allocation.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

import bioseq.io.sources;  // posix_read_source, source_read_result
import bioseq.external.provider.errors;
import bioseq.external.provider.model;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.io.sequence_stream;

namespace prov = bioseq::external::provider;
namespace exio = bioseq::external::io;

namespace {

std::string write_fixture(std::uint64_t bp) {
    const std::filesystem::path directory = "/tmp/bioseq_external_api";
    std::error_code directory_error;
    std::filesystem::create_directories(directory, directory_error);
    REQUIRE_FALSE(directory_error);
    const std::string path = (directory / "fasta_stream_fixture.fa").string();
    std::FILE* file = std::fopen(path.c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fputs(">bench_synthetic_chromosome\n", file);
    static const char alphabet[4] = {'A', 'C', 'G', 'T'};
    std::string line;
    std::uint64_t written = 0;
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    while (written < bp) {
        line.clear();
        for (int i = 0; i < 70 && written < bp; ++i, ++written) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            line.push_back(alphabet[(seed >> 33) & 3u]);
        }
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), file);
    }
    std::fclose(file);
    return path;
}

// A sink that counts the chunk payloads the production emitter pushes.
struct chunk_counter {
    std::uint64_t letters = 0;
    std::uint64_t chunks = 0;
    static std::expected<void, prov::external_error> push(
        void* self, prov::owning_provider_message&& message) {
        auto* counter = static_cast<chunk_counter*>(self);
        if (auto* payload =
                std::get_if<prov::owning_sequence_payload>(&message)) {
            counter->letters += payload->bases.size();
            ++counter->chunks;
        }
        return {};
    }
    prov::provider_message_sink_ref ref() {
        return prov::provider_message_sink_ref{this, &chunk_counter::push};
    }
};

struct run_metrics {
    std::uint64_t letters = 0;
    std::size_t peak_live_bytes = 0;
    double seconds = 0.0;
    double decoded_mib_per_s = 0.0;
};

double mib_per_s(std::uint64_t letters, double seconds) {
    return seconds > 0.0
               ? (static_cast<double>(letters) / (1024.0 * 1024.0)) / seconds
               : 0.0;
}

// direct: stream the file through the PRODUCTION fasta_chunk_emitter. Its peak
// rechunk buffer is the bounded live working set (never the whole record).
run_metrics run_direct(const std::string& path, std::uint64_t chunk_bases) {
    run_metrics metrics;
    const auto start = std::chrono::steady_clock::now();
    auto source = bioseq::io::posix_read_source::try_open(path);
    REQUIRE(source.has_value());
    chunk_counter counter;
    exio::fasta_chunk_emitter emitter(prov::query_target::sequence_chunk,
                                      chunk_bases, "bench_fixture",
                                      counter.ref());
    std::vector<std::uint8_t> block(65536);
    bool failed = false;
    for (;;) {
        const bioseq::io::source_read_result result =
            source->read_into(block.data(), block.size());
        if (result.has_error) {
            failed = true;
            break;
        }
        if (!emitter.feed(block.data(), result.bytes_read).has_value()) {
            failed = true;
            break;
        }
        if (result.is_eof) {
            break;
        }
    }
    REQUIRE_FALSE(failed);
    REQUIRE(emitter.finish().has_value());
    const auto end = std::chrono::steady_clock::now();
    metrics.letters = counter.letters;
    metrics.peak_live_bytes = static_cast<std::size_t>(emitter.peak_rechunk_bytes());
    metrics.seconds = std::chrono::duration<double>(end - start).count();
    metrics.decoded_mib_per_s = mib_per_s(metrics.letters, metrics.seconds);
    return metrics;
}

// full-body: read the whole file into memory, then extract. Peak live buffer is
// the entire body — the allocation the streaming path avoids.
run_metrics run_full_body(const std::string& path) {
    run_metrics metrics;
    const auto start = std::chrono::steady_clock::now();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    std::string body;
    std::vector<char> block(65536);
    for (;;) {
        const std::size_t got = std::fread(block.data(), 1, block.size(), file);
        if (got == 0) {
            break;
        }
        body.append(block.data(), got);
    }
    std::fclose(file);
    metrics.peak_live_bytes = body.capacity();
    bool in_header = false;
    for (const char symbol : body) {
        if (symbol == '>') { in_header = true; continue; }
        if (symbol == '\n') { in_header = false; continue; }
        if (in_header || symbol == '\r') { continue; }
        ++metrics.letters;
    }
    const auto end = std::chrono::steady_clock::now();
    metrics.seconds = std::chrono::duration<double>(end - start).count();
    metrics.decoded_mib_per_s = mib_per_s(metrics.letters, metrics.seconds);
    return metrics;
}

}  // namespace

TEST_CASE("external_fasta_stream: production emitter stays bounded vs full-body",
          "[component:external_api][layer:integration][feature:external_source][resource:filesystem]") {
    const std::uint64_t bp = 16u * 1024u * 1024u;  // 16 Mbp fixture
    const std::uint64_t chunk_bases = 1048572;
    const std::string path = write_fixture(bp);

    const run_metrics direct = run_direct(path, chunk_bases);
    const run_metrics spooled = run_full_body(path);

    std::printf(
        "\n[external_fasta_stream] payload=%llu bp, chunk_bases=%llu\n"
        "  direct (production fasta_chunk_emitter): letters=%llu  "
        "peak_rechunk=%.2f MiB  %.1f MiB/s\n"
        "  full-body (whole record in memory):      letters=%llu  "
        "peak_live=%.2f MiB  %.1f MiB/s\n"
        "  peak live-buffer reduction (full-body/direct) = %.1fx\n",
        static_cast<unsigned long long>(bp),
        static_cast<unsigned long long>(chunk_bases),
        static_cast<unsigned long long>(direct.letters),
        static_cast<double>(direct.peak_live_bytes) / (1024.0 * 1024.0),
        direct.decoded_mib_per_s,
        static_cast<unsigned long long>(spooled.letters),
        static_cast<double>(spooled.peak_live_bytes) / (1024.0 * 1024.0),
        spooled.decoded_mib_per_s,
        static_cast<double>(spooled.peak_live_bytes) /
            static_cast<double>(direct.peak_live_bytes));

    REQUIRE(direct.letters == bp);
    REQUIRE(spooled.letters == bp);
    // The production emitter's peak rechunk buffer is bounded by chunk_bases plus
    // one 64 KiB read block, regardless of the 16 MiB body — no full-body alloc.
    REQUIRE(direct.peak_live_bytes <= chunk_bases + 128u * 1024u);
    REQUIRE(spooled.peak_live_bytes >= bp);
    REQUIRE(direct.peak_live_bytes * 4 < spooled.peak_live_bytes);
}
