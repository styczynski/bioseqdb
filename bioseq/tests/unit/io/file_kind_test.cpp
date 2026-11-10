/**
 * @file file_kind_test.cpp
 * @brief Checks file detection, atomic output, capacity and alignment codec boundaries.
 */
// Canonicalize Highway helpers before importing the streaming codecs.
#include <hwy/highway.h>
#include <cstdlib>
#include <unistd.h>
#include <zlib.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.io.options;
import bioseq.io.buffer;
import bioseq.io.simd;
import bioseq.io.file_kind;
import bioseq.io.alignment.record_codec;
import bioseq.io.alignment;
import bioseq.io.record_schema;
import bioseq.io.reader;
import bioseq.io.sinks;
import bioseq.io.write_pipeline;
import bioseq.highway.codec_scalar_probe;
import bioseq.simd.utils;
import bioseq.sequences.translation;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.storage.lazy_sequence_view;
import bioseq.storage.size;

namespace {

using bioseq::io::detect_sequence_file_kind;
using bioseq::io::sequence_file_format;

} // namespace

TEST_CASE("IO CRC32 matches zlib across SIMD blocks, tails and continuation",
          "[component:bioseq][layer:unit][feature:io]") {
    constexpr std::string_view check_value{"123456789"};
    REQUIRE(bioseq::io::simd::crc32_iso_hdlc_simd(0,
        reinterpret_cast<const std::uint8_t*>(check_value.data()), check_value.size()) == 0xCBF43926u);

    // Cover BGZF's maximum uncompressed member and unaligned borrowed spans.
    std::array<std::uint8_t, 65536u + 128u + 16u> storage{};
    constexpr std::array<std::uint32_t, 4> initial_values{0u, 0xFFFFFFFFu, 0x12345678u, 0xA5A55A5Au};
    constexpr std::array<std::size_t, 4> alignments{0u, 1u, 7u, 15u};
    constexpr std::array<std::size_t, 4> full_prefixes{0u, 128u, 256u, 1024u};
    constexpr std::array<std::size_t, 4> member_lengths{147u, 1219u, 65535u, 65536u};
    for (std::size_t pattern = 0u; pattern < 3u; ++pattern) {
        for (std::size_t index = 0u; index < storage.size(); ++index) {
            storage[index] = pattern == 0u ? std::uint8_t{0}
                : pattern == 1u ? std::uint8_t{255}
                : static_cast<std::uint8_t>((index * 73u) ^ (index >> 3u) ^ ((index * index) >> 5u));
        }
        for (const auto initial : initial_values) {
            for (const auto alignment : alignments) {
                const auto input = std::span<const std::uint8_t>{storage}.subspan(alignment);
                CAPTURE(pattern, initial, alignment);
                const auto check_length = [initial, input](std::size_t length) {
                    CAPTURE(length);
                    const auto expected = static_cast<std::uint32_t>(::crc32_z(
                        static_cast<uLong>(initial), input.data(), static_cast<z_size_t>(length)));
                    REQUIRE(bioseq::io::simd::crc32_iso_hdlc_simd(initial, input.data(), length) == expected);
                };
                for (const auto prefix : full_prefixes)
                    for (std::size_t tail = 0u; tail < 128u; ++tail) check_length(prefix + tail);
                for (const auto length : member_lengths) check_length(length);

                constexpr std::size_t stream_length = 1219u;
                const auto expected = static_cast<std::uint32_t>(::crc32_z(
                    static_cast<uLong>(initial), input.data(), static_cast<z_size_t>(stream_length)));
                const auto check_split = [initial, input, expected](std::size_t split) {
                    CAPTURE(split);
                    const auto prefix = bioseq::io::simd::crc32_iso_hdlc_simd(initial, input.data(), split);
                    REQUIRE(prefix == static_cast<std::uint32_t>(::crc32_z(
                        static_cast<uLong>(initial), input.data(), static_cast<z_size_t>(split))));
                    REQUIRE(bioseq::io::simd::crc32_iso_hdlc_simd(
                        prefix, input.data() + split, stream_length - split) == expected);
                };
                // Both scalar/SIMD handoff directions, every tail, and two SIMD updates.
                for (std::size_t split = 0u; split <= 256u; ++split) check_split(split);
                for (std::size_t tail = 0u; tail < 128u; ++tail) check_split(stream_length - tail);
                for (const std::size_t chunk_size : {1u, 127u, 128u, 129u, 257u}) {
                    CAPTURE(chunk_size);
                    auto running = initial;
                    for (std::size_t offset = 0u; offset < stream_length;) {
                        const auto count = std::min(chunk_size, stream_length - offset);
                        running = bioseq::io::simd::crc32_iso_hdlc_simd(running, input.data() + offset, count);
                        offset += count;
                    }
                    REQUIRE(running == expected);
                }
            }
        }
    }
}

TEST_CASE("file_utils: FASTA extensions map to FASTA",
          "[component:bioseq][layer:unit][feature:io]") {
    for (const std::string_view path :
         {"a.fasta", "a.fa", "a.fna", "/tmp/dir.with.dots/genome.fasta"}) {
        const auto kind = detect_sequence_file_kind(path);
        REQUIRE(kind.has_value());
        REQUIRE(kind->format == sequence_file_format::fasta);
        REQUIRE(kind->compression == bioseq::io::sequence_compression::none);
    }
}

TEST_CASE("file_utils: FASTQ extensions map to FASTQ",
          "[component:bioseq][layer:unit][feature:io]") {
    for (const std::string_view path : {"reads.fastq", "reads.fq", "reads.fnq"}) {
        const auto kind = detect_sequence_file_kind(path);
        REQUIRE(kind.has_value());
        REQUIRE(kind->format == sequence_file_format::fastq);
        REQUIRE(kind->compression == bioseq::io::sequence_compression::none);
    }
}

TEST_CASE("file_utils: case-insensitive extension match",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto kind = detect_sequence_file_kind("a.FASTA");
    REQUIRE(kind.has_value());
    REQUIRE(kind->format == sequence_file_format::fasta);
    REQUIRE(kind->compression == bioseq::io::sequence_compression::none);

    const auto kind_q = detect_sequence_file_kind("READS.FQ");
    REQUIRE(kind_q.has_value());
    REQUIRE(kind_q->format == sequence_file_format::fastq);
}

TEST_CASE("file_utils: gzip suffixes are stripped and flagged",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto fastq_gz = detect_sequence_file_kind("a.fastq.gz");
    REQUIRE(fastq_gz.has_value());
    REQUIRE(fastq_gz->format == sequence_file_format::fastq);
    REQUIRE(fastq_gz->compression == bioseq::io::sequence_compression::gzip);

    const auto fasta_bgz = detect_sequence_file_kind("x.fasta.bgz");
    REQUIRE(fasta_bgz.has_value());
    REQUIRE(fasta_bgz->format == sequence_file_format::fasta);
    REQUIRE(fasta_bgz->compression == bioseq::io::sequence_compression::bgzf);

    const auto fa_bgzf = detect_sequence_file_kind("genome.fa.bgzf");
    REQUIRE(fa_bgzf.has_value());
    REQUIRE(fa_bgzf->format == sequence_file_format::fasta);
    REQUIRE(fa_bgzf->compression == bioseq::io::sequence_compression::bgzf);
}

TEST_CASE("file_utils: unrecognised extension is an error",
          "[component:bioseq][layer:unit][feature:io]") {

    REQUIRE_FALSE(detect_sequence_file_kind("a.txt").has_value());

    REQUIRE_FALSE(detect_sequence_file_kind("a").has_value());
    // A filename needs at least one character  before the extension.
    REQUIRE_FALSE(detect_sequence_file_kind(".fasta").has_value());
    // Compression does not make an unknown sequence extension valid.
    REQUIRE_FALSE(detect_sequence_file_kind("a.txt.gz").has_value());
    // Include the path in the error so PostgreSQL can report the bad input.
    const auto err = detect_sequence_file_kind("weird.dat");
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error().find("weird.dat") != std::string::npos);
}

namespace {

/** Own a private test directory; production publication remains under its file owner. */
struct publication_fixture {
    std::filesystem::path directory;
    const bool pool_was_active{bioseq::parallelism::global_pool::instance().is_active()};
    /** Create a collision-safe, mode-0700 temporary directory. */
    publication_fixture() {
        std::string pattern = (std::filesystem::temp_directory_path() / "bioseq_publication_XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        directory = pattern;
    }
    /** Retire first-use scheduler state after writer owners unwind, then remove private files. */
    ~publication_fixture() {
        if (!pool_was_active) bioseq::parallelism::global_pool::instance().shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    /** Read complete small expected fixture bytes without depending on the writer. */
    [[nodiscard]] static std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }
    /** Create an independent existing destination for overwrite/no-overwrite checks. */
    static void seed(const std::filesystem::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }
    /** Verify temporary staging names were removed after unwinding their owner. */
    [[nodiscard]] bool has_private_staging() const {
        for (const auto& entry : std::filesystem::directory_iterator{directory})
            if (entry.path().filename().string().starts_with(".bioseq-")) return true;
        return false;
    }
};

/** Test-only exception representing a caller-thread cancellation checkpoint. */
struct publication_cancelled {};

/** Count the final callback and unwind before any destination publication. */
void cancel_publication(void* opaque) {
    ++*static_cast<std::size_t*>(opaque);
    throw publication_cancelled{};
}

/** Stage one small output through the public reservation/write API. */
void stage_publication(bioseq::io::posix_pwrite_sink& sink, std::string_view bytes) {
    const auto offset = sink.reserve_offset(bytes.size());
    REQUIRE(offset.has_value());
    const auto written = sink.write_at(std::as_bytes(std::span{bytes.data(), bytes.size()}), *offset);
    REQUIRE(written.has_value());
    REQUIRE(*written == bytes.size());
}


/** A batch permit proves when the writer retires its retained source owner. */
struct pressure_retry_batch {
    bioseq::parallelism::memory_permit permit;
    const bioseq::io::export_record_view* record{};
    std::size_t* released_batches{};

    /** Adopt the already evaluated row and its pending admission permit. */
    pressure_retry_batch(bioseq::parallelism::memory_permit admitted,
            const bioseq::io::export_record_view& view, std::size_t& released)
        : permit{std::move(admitted)}, record{&view}, released_batches{&released} {}
    pressure_retry_batch(const pressure_retry_batch&) = delete;
    pressure_retry_batch& operator=(const pressure_retry_batch&) = delete;
    pressure_retry_batch(pressure_retry_batch&&) noexcept = default;
    /** Retire an overwritten owner before adopting another live batch. */
    pressure_retry_batch& operator=(pressure_retry_batch&& other) noexcept {
        if (this != &other) {
            if (permit) ++*released_batches;
            permit = std::move(other.permit);
            record = other.record;
            released_batches = other.released_batches;
        }
        return *this;
    }
    /** Moved-from batches have no permit and cannot double-count retirement. */
    ~pressure_retry_batch() { if (permit) ++*released_batches; }
    /** Each next() delivery contains exactly one immutable row. */
    [[nodiscard]] std::size_t size() const { return 1u; }
    /** The source owns fixed backing until the complete export returns. */
    [[nodiscard]] bioseq::io::export_record_view row(std::size_t) const { return *record; }
    /** Report the actual retained permit, without inventing a separate budget. */
    [[nodiscard]] std::size_t accounted_capacity_bytes() const {
        return static_cast<std::size_t>(permit.bytes());
    }
};

/** Two-row contract fixture injects pressure at a retained pending row, never by timing. */
struct pressure_retry_source {
    using batch_type = pressure_retry_batch;
    enum class pressure_policy { none, once, repeated };
    static constexpr auto alphabet = ::supported_storage_alphabets::index_of<seq::dna4>();
    const pressure_policy policy;
    const std::array<std::array<std::uint8_t, 4>, 2> ranks{{{0u, 1u, 2u, 3u}, {3u, 2u, 1u, 0u}}};
    std::array<bioseq::io::export_record_view, 2> records;
    bioseq::io::record_source_metadata source_metadata{.declared_alphabet_codes = {alphabet}};
    bioseq::parallelism::memory_permit pending_permit;
    std::array<std::size_t, 2> evaluations{};
    std::size_t next_calls{}, delivered_batches{}, pressure_returns{}, released_batches{};
    std::size_t released_before_pressure{}, released_before_retry{}, cancel_calls{};
    bool cancel_released_pending{};

    /** Borrow only the requested tile from the fixed test sequence. */
    [[nodiscard]] static const std::uint8_t* read(const void* context,
            bioseq::storage::sequence_size_t from, std::size_t count, std::span<std::uint8_t> scratch) {
        const auto& symbols = *static_cast<const std::array<std::uint8_t, 4>*>(context);
        if (from > symbols.size() || count > symbols.size() - from || count > scratch.size()) return nullptr;
        return symbols.data() + from;
    }

    /** An incomplete dictionary forces SAM rows through the retained/replay path. */
    explicit pressure_retry_source(pressure_policy selected) : policy{selected} {
        constexpr std::array<std::string_view, 2> names{"first", "second"};
        for (std::size_t index = 0; index < records.size(); ++index) {
            auto& record = records[index];
            record.ordinal = index + 1u;
            record.header = names[index];
            record.qname = names[index];
            record.sequence = bioseq::storage::borrowed_rank_window_source{
                &ranks[index], &read, ranks[index].size(), alphabet,
                bioseq::storage::sequence_transform::identity, true};
            record.alignment.emplace();
            record.alignment->flags = 4u;
            record.alignment->mapping_quality = 0u;
            record.identity.classification = bioseq::io::alignment_classification_source::explicit_flags;
        }
    }
    pressure_retry_source(const pressure_retry_source&) = delete;
    pressure_retry_source& operator=(const pressure_retry_source&) = delete;
    /** The manifest supplies the only sequence type needed by these native rows. */
    [[nodiscard]] std::span<const bioseq::io::record_column> columns() const { return {}; }
    /** Keep the dictionary incomplete so no row may stream before source exhaustion. */
    [[nodiscard]] const bioseq::io::record_source_metadata& metadata() const { return source_metadata; }

    /** Evaluate once, retain on pressure, and transfer that same row on retry. */
    [[nodiscard]] std::expected<std::optional<batch_type>, bioseq::io::record_error> next() {
        ++next_calls;
        if (delivered_batches == records.size()) return std::optional<batch_type>{};
        if (!pending_permit) {
            auto admitted = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                sizeof(bioseq::io::export_record_view));
            if (!admitted) return std::unexpected(bioseq::io::record_error{
                delivered_batches + 1u, std::nullopt, "test source could not reserve one tiny row"});
            pending_permit = std::move(*admitted);
            ++evaluations[delivered_batches];
        }
        if (delivered_batches == 1u && policy != pressure_policy::none) {
            if (pressure_returns == 0u) released_before_pressure = released_batches;
            else released_before_retry = released_batches;
            if (pressure_returns == 0u || policy == pressure_policy::repeated) {
                ++pressure_returns;
                return std::unexpected(bioseq::io::record_error{2u, std::nullopt,
                    "injected pending-row memory pressure", bioseq::io::record_error_code::memory_pressure});
            }
        }
        const auto index = delivered_batches++;
        return std::optional<batch_type>{std::in_place,
            std::move(pending_permit), records[index], released_batches};
    }

    /** An unrecoverable retry must cancel and release the still-pending source permit. */
    void cancel() {
        ++cancel_calls;
        cancel_released_pending = static_cast<bool>(pending_permit);
        pending_permit.release();
    }
};
static_assert(bioseq::io::export_record_source<pressure_retry_source>);
} // namespace

TEST_CASE("file_utils: cancellation at the final publication checkpoint preserves destinations",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    for (const bool existing : {false, true}) {
        CAPTURE(existing);
        publication_fixture fixture;
        const auto destination = fixture.directory / "output.fasta";
        if (existing) publication_fixture::seed(destination, "original bytes\n");
        std::size_t callbacks{};
        {
            auto output = bioseq::io::posix_pwrite_sink::try_open(destination.string(), true);
            REQUIRE(output.has_value());
            stage_publication(*output, ">complete staged read\nACGT\n");
            REQUIRE_THROWS_AS(output->publish({&callbacks, cancel_publication}), publication_cancelled);
            REQUIRE(callbacks == 1u);
            REQUIRE(std::filesystem::exists(destination) == existing);
            if (existing) REQUIRE(publication_fixture::read(destination) == "original bytes\n");
        }
        REQUIRE_FALSE(fixture.has_private_staging());
        // A cancelled owner cannot poison a later independent writer.
        {
            auto output = bioseq::io::posix_pwrite_sink::try_open(destination.string(), true);
            REQUIRE(output.has_value());
            stage_publication(*output, ">replacement\nTGCA\n");
            REQUIRE(output->publish().has_value());
        }
        REQUIRE(publication_fixture::read(destination) == ">replacement\nTGCA\n");
        REQUIRE_FALSE(fixture.has_private_staging());
    }
}

TEST_CASE("file_utils: no-overwrite publication resolves competing staged writers atomically",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    const auto destination = fixture.directory / "winner.fasta";
    {
        auto first = bioseq::io::posix_pwrite_sink::try_open(destination.string());
        auto second = bioseq::io::posix_pwrite_sink::try_open(destination.string());
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        stage_publication(*first, ">first\nACGT\n");
        stage_publication(*second, ">second\nTGCA\n");
        REQUIRE(first->publish().has_value());
        const auto rejected = second->publish();
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(publication_fixture::read(destination) == ">first\nACGT\n");
    }
    REQUIRE_FALSE(fixture.has_private_staging());
}

TEST_CASE("IO sink rejects excess capacity before advancing the reservation",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    const auto path = (fixture.directory / "bounded").string();
    auto opened = bioseq::io::posix_pwrite_sink::try_open(path, false, 4u);
    REQUIRE(opened.has_value());
    auto sink = std::move(*opened);

    const auto first = sink.reserve_offset(2u);
    REQUIRE(first.has_value());
    REQUIRE(*first == 0u);
    const auto rejected = sink.reserve_offset(3u);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().find("maximum_output_bytes") != std::string::npos);
    REQUIRE(sink.size_bytes() == 2u);

    const auto second = sink.reserve_offset(2u);
    REQUIRE(second.has_value());
    REQUIRE(*second == 2u);
    REQUIRE(sink.size_bytes() == 4u);

    // Both ownership transfer forms must preserve the bound.
    bioseq::io::posix_pwrite_sink transferred;
    transferred = std::move(sink);
    REQUIRE_FALSE(transferred.reserve_offset(1u).has_value());
    REQUIRE(transferred.size_bytes() == 4u);
    const auto empty = transferred.reserve_offset(0u);
    REQUIRE(empty.has_value());
    REQUIRE(*empty == 4u);

    const std::string_view oversized = "ACGTN";
    REQUIRE_FALSE(transferred.write_at(
        std::as_bytes(std::span{oversized.data(), oversized.size()}), 0u).has_value());
    const std::string_view payload = "ACGT";
    const auto written = transferred.write_at(
        std::as_bytes(std::span{payload.data(), payload.size()}), 0u);
    REQUIRE(written.has_value());
    REQUIRE(*written == 4u);
    REQUIRE(transferred.publish().has_value());
    REQUIRE(publication_fixture::read(fixture.directory / "bounded") == payload);
    REQUIRE(std::filesystem::file_size(path) == 4u);
}

TEST_CASE("IO output capacity distinguishes zero from an absent limit",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    SECTION("explicit zero permits an empty file") {
        const auto path = (fixture.directory / "empty").string();
        auto opened = bioseq::io::posix_pwrite_sink::try_open(path, false, 0u);
        REQUIRE(opened.has_value());
        REQUIRE(opened->reserve_offset(0u).has_value());
        REQUIRE_FALSE(opened->reserve_offset(1u).has_value());
        REQUIRE(opened->size_bytes() == 0u);
        REQUIRE(opened->publish().has_value());
        REQUIRE(std::filesystem::file_size(path) == 0u);
    }
    SECTION("default is unlimited and does not inherit a small mapping capacity") {
        REQUIRE_FALSE(bioseq::io::writer_options{}.maximum_output_bytes.has_value());
        const auto path = (fixture.directory / "unlimited").string();
        auto opened = bioseq::io::posix_pwrite_sink::try_open(path);
        REQUIRE(opened.has_value());
        const std::string payload(4097u, 'A');
        REQUIRE(opened->append_bytes(
            std::as_bytes(std::span{payload.data(), payload.size()})).has_value());
        REQUIRE(opened->publish().has_value());
        REQUIRE(publication_fixture::read(fixture.directory / "unlimited") == payload);
    }
    SECTION("an unrepresentable explicit limit creates no staging file") {
        const auto path = (fixture.directory / "invalid").string();
        const auto opened = bioseq::io::posix_pwrite_sink::try_open(
            path, false, std::numeric_limits<std::uint64_t>::max());
        REQUIRE_FALSE(opened.has_value());
        REQUIRE(std::filesystem::is_empty(fixture.directory));
    }
}

TEST_CASE("ordered writer propagates exact bounds and abandons oversized output",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    bioseq::io::writer_options options;
    options.chunk_capacity_bytes = 2u;
    options.maximum_output_bytes = 4u;
    bioseq::io::concurrency_options concurrency;
    concurrency.worker_count = 1u;
    concurrency.io_buffer_pool_size = 1u;

    SECTION("exact output size publishes") {
        auto opened = bioseq::io::ordered_output_sink::open(
            (fixture.directory / "exact").string(), options,
            bioseq::io::sequence_compression::none, concurrency);
        REQUIRE(opened.has_value());
        REQUIRE((*opened)->append("ACGT").has_value());
        REQUIRE((*opened)->finish().has_value());
        REQUIRE((*opened)->bytes_written() == 4u);
        REQUIRE(publication_fixture::read(fixture.directory / "exact") == "ACGT");
    }
    SECTION("one byte over the bound does not replace the previous file") {
        const auto path = (fixture.directory / "protected").string();
        { std::ofstream previous{path, std::ios::binary}; previous << "old"; }
        options.overwrite = true;
        options.maximum_output_bytes = 3u;
        {
            auto opened = bioseq::io::ordered_output_sink::open(
                path, options, bioseq::io::sequence_compression::none, concurrency);
            REQUIRE(opened.has_value());
            const auto appended = (*opened)->append("ACGT");
            REQUIRE_FALSE(appended.has_value());
            REQUIRE(appended.error().find("maximum_output_bytes") != std::string::npos);
            REQUIRE((*opened)->bytes_written() == 2u);
            REQUIRE_FALSE((*opened)->finish().has_value());
            REQUIRE_FALSE((*opened)->finish().has_value());
        }
        REQUIRE(publication_fixture::read(fixture.directory / "protected") == "old");
        REQUIRE(std::distance(std::filesystem::directory_iterator{fixture.directory},
                              std::filesystem::directory_iterator{}) == 1);
    }
}

TEST_CASE("ordered writer applies its bound to final BGZF framing",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    bioseq::io::writer_options options;
    bioseq::io::concurrency_options concurrency;
    concurrency.worker_count = 1u;
    concurrency.io_buffer_pool_size = 1u;
    // The standard BGZF EOF member occupies 28 bytes even with no records.
    for (const std::uint64_t capacity : {27u, 28u}) {
        options.maximum_output_bytes = capacity;
        const auto path = (fixture.directory / std::to_string(capacity)).string();
        {
            auto opened = bioseq::io::ordered_output_sink::open(
                path, options, bioseq::io::sequence_compression::bgzf, concurrency);
            REQUIRE(opened.has_value());
            const auto finished = (*opened)->finish();
            REQUIRE(finished.has_value() == (capacity == 28u));
            REQUIRE((*opened)->bytes_written() == (capacity == 28u ? 28u : 0u));
        }
        REQUIRE(std::filesystem::exists(path) == (capacity == 28u));
        if (capacity == 28u) REQUIRE(std::filesystem::file_size(path) == 28u);
    }
}

TEST_CASE("IO alignment geometry retains asymmetric hard and soft query clips",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    codec::sam_cigar_source text{"2H3S4M1I2M5S7H"};
    const auto measured = codec::measure_wire_cigar({text.borrow(), 0, 0, false});
    REQUIRE(measured.has_value());
    REQUIRE(measured->geometry.query_span == 15u);
    REQUIRE(measured->geometry.reference_span == 6u);
    REQUIRE(measured->geometry.query_before_hard_clipping == 24u);
    REQUIRE(measured->geometry.leading_query_clip == 5u);
    REQUIRE(measured->geometry.trailing_query_clip == 12u);
    const auto [forward_start, forward_end] = measured->geometry.original_query_interval(false);
    const auto [reverse_start, reverse_end] = measured->geometry.original_query_interval(true);
    REQUIRE(forward_start == 5u);
    REQUIRE(forward_end == 12u);
    REQUIRE(reverse_start == 12u);
    REQUIRE(reverse_end == 19u);

    // Independent BAM word spellings exercise the native decoder's measurement pass.
    constexpr std::array words{std::uint32_t{(2u << 4u) | 5u}, std::uint32_t{(3u << 4u) | 4u},
        std::uint32_t{4u << 4u}, std::uint32_t{(1u << 4u) | 1u}, std::uint32_t{2u << 4u},
        std::uint32_t{(5u << 4u) | 4u}, std::uint32_t{(7u << 4u) | 5u}};
    std::array<std::uint8_t, words.size() * 4u> bytes{};
    for (std::size_t index = 0; index < words.size(); ++index)
        codec::store_wire_integer(std::span{bytes}.subspan(index * 4u, 4u), words[index]);
    const auto native = codec::measure_native_cigar(bytes);
    REQUIRE(native.has_value());
    REQUIRE(native->query_span == 15u);
    REQUIRE(native->query_before_hard_clipping == 24u);
    REQUIRE(native->leading_query_clip == 5u);
    REQUIRE(native->trailing_query_clip == 12u);
    REQUIRE(native->original_query_interval(true) == measured->geometry.original_query_interval(true));
}

TEST_CASE("IO alignment validates complete CIGAR query coordinates in the original orientation",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    codec::sam_cigar_source text{"2H3S4M1I2M5S7H"};
    codec::native_reference_binding references;
    references.reference_id = 0;
    references.reference_length = 100;
    for (const bool protein : {false, true}) {
        for (const bool reverse : {false, true}) {
            CAPTURE(protein, reverse);
            const auto flags = static_cast<std::uint16_t>(reverse ? 16u : 0u);
            const std::uint64_t scale = protein ? 3u : 1u;
            bioseq::io::alignment_record_view alignment;
            alignment.cigar = text.borrow();
            alignment.target_start = 10;
            alignment.target_end = 16;
            alignment.query_length = 24;
            alignment.query_start = reverse ? 12u : 5u;
            alignment.query_end = reverse ? 19u : 12u;
            const auto accepted = codec::prepare_native_alignment(alignment, flags, 15, references, protein);
            REQUIRE(accepted.has_value());
            REQUIRE(accepted->core.sequence_length == static_cast<std::int32_t>(15u * scale));
            REQUIRE(accepted->cigar_size.geometry.leading_query_clip == 5u * scale);
            REQUIRE(accepted->cigar_size.geometry.trailing_query_clip == 12u * scale);

            auto invalid = alignment;
            ++*invalid.query_start;
            REQUIRE_FALSE(codec::prepare_native_alignment(invalid, flags, 15, references, protein).has_value());
            invalid = alignment;
            ++*invalid.query_end;
            REQUIRE_FALSE(codec::prepare_native_alignment(invalid, flags, 15, references, protein).has_value());
            invalid = alignment;
            invalid.query_end = 25;
            REQUIRE_FALSE(codec::prepare_native_alignment(invalid, flags, 15, references, protein).has_value());
            invalid = alignment;
            invalid.query_start = 20;
            invalid.query_end = 19;
            REQUIRE_FALSE(codec::prepare_native_alignment(invalid, flags, 15, references, protein).has_value());
        }
    }
}

TEST_CASE("IO alignment core clipping agrees with complete-CIGAR coordinate validation",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    codec::sam_cigar_source text{"2M"};
    codec::native_reference_binding references;
    references.reference_id = 0;
    references.reference_length = 100;
    bioseq::io::alignment_record_view alignment;
    alignment.cigar = text.borrow();
    alignment.form = bioseq::io::cigar_form::alignment_core;
    alignment.query_length = 5;
    alignment.query_start = 1;
    alignment.query_end = 3;
    alignment.target_start = 10;
    alignment.target_end = 12;
    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const auto flags = static_cast<std::uint16_t>(reverse ? 16u : 0u);
        const auto accepted = codec::prepare_native_alignment(alignment, flags, 5, references, false);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->cigar_size.geometry.leading_query_clip == (reverse ? 2u : 1u));
        REQUIRE(accepted->cigar_size.geometry.trailing_query_clip == (reverse ? 1u : 2u));
        const auto [query_start, query_end] = accepted->cigar_size.geometry.original_query_interval(reverse);
        REQUIRE(query_start == 1u);
        REQUIRE(query_end == 3u);
    }
}

TEST_CASE("IO alignment mate reference bounds use original coordinates before projection",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    codec::sam_cigar_source text{"2M"};
    codec::native_reference_binding references;
    references.reference_id = 0;
    references.reference_length = 100;
    references.mate_reference_id = 1;
    references.mate_reference_length = 20;
    references.mate_same_projection_domain = true;
    bioseq::io::alignment_record_view alignment;
    alignment.cigar = text.borrow();
    alignment.target_start = 10;
    alignment.target_end = 12;
    alignment.query_length = 2;
    alignment.query_start = 0;
    alignment.query_end = 2;
    for (const bool protein : {false, true}) {
        for (const std::uint64_t mate_start : {std::uint64_t{0}, std::uint64_t{19}}) {
            CAPTURE(protein, mate_start);
            alignment.mate_start = mate_start;
            const auto accepted = codec::prepare_native_alignment(alignment, 65, 2, references, protein);
            REQUIRE(accepted.has_value());
            REQUIRE(accepted->core.mate_position == static_cast<std::int32_t>(mate_start * (protein ? 3u : 1u)));
        }
        alignment.mate_start = 20;
        REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 65, 2, references, protein).has_value());
    }

    // Both cross-domain directions suppress coordinates, even for unpaired rows.
    references.mate_same_projection_domain = false;
    alignment.mate_start = 19;
    alignment.template_length = 17;
    for (const bool protein : {false, true}) {
        for (const std::uint16_t flags : {std::uint16_t{0}, std::uint16_t{99}}) {
            CAPTURE(protein, flags);
            const auto projected = codec::prepare_native_alignment(alignment, flags, 2, references, protein);
            REQUIRE(projected.has_value()); REQUIRE(projected->mate_projection_suppressed);
            REQUIRE(projected->core.flags == (flags == 99u ? 73u : 0u));
            REQUIRE(projected->core.mate_reference_id == -1);
            REQUIRE(projected->core.mate_position == -1); REQUIRE(projected->core.template_length == 0);
            REQUIRE(alignment.mate_start == 19u); REQUIRE(alignment.template_length == 17);
        }
    }

    // An unknown protein mate stays solely in preservation; native pairing is truthful.
    references.mate_reference_id = -1;
    references.mate_reference_length.reset();
    references.mate_same_projection_domain = false;
    alignment.mate_start = 777;
    alignment.template_length = 17;
    const auto unprojected = codec::prepare_native_alignment(alignment, 99, 2, references, true);
    REQUIRE(unprojected.has_value());
    REQUIRE(unprojected->core.flags == 73u);
    REQUIRE(unprojected->core.mate_reference_id == -1);
    REQUIRE(unprojected->core.mate_position == -1);
    REQUIRE(unprojected->core.template_length == 0);
    REQUIRE(unprojected->mate_projection_suppressed);
    const auto absent_nucleotide = codec::prepare_native_alignment(alignment, 99, 2, references, false);
    REQUIRE(absent_nucleotide.has_value()); REQUIRE_FALSE(absent_nucleotide->mate_projection_suppressed);
    REQUIRE(absent_nucleotide->core.flags == 99u); REQUIRE(absent_nucleotide->core.mate_reference_id == -1);
    REQUIRE(absent_nucleotide->core.mate_position == -1); REQUIRE(absent_nucleotide->core.template_length == 17);
}

TEST_CASE("IO alignment TLEN uses SAM signed boundaries including protein scale",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    codec::sam_cigar_source text{"1M"};
    constexpr auto wire_limit = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    for (const bool protein : {false, true}) {
        const std::int64_t scale = protein ? 3 : 1;
        const auto original_limit = wire_limit / scale;
        codec::native_reference_binding references;
        references.reference_id = 0;
        references.reference_length = static_cast<std::uint64_t>(original_limit);
        references.mate_reference_id = 0;
        references.mate_reference_length = references.reference_length;
        references.mate_same_projection_domain = true;
        bioseq::io::alignment_record_view alignment;
        alignment.cigar = text.borrow();
        alignment.query_length = 1;
        alignment.query_start = 0;
        alignment.query_end = 1;
        for (const bool negative : {false, true}) {
            CAPTURE(protein, negative);
            alignment.target_start = negative ? static_cast<std::uint64_t>(original_limit - 1) : 0u;
            alignment.target_end = *alignment.target_start + 1;
            alignment.mate_start = negative ? 0u : static_cast<std::uint64_t>(original_limit - 1);
            alignment.template_length = negative ? -original_limit : original_limit;
            const auto accepted = codec::prepare_native_alignment(alignment, 65, 1, references, protein);
            REQUIRE(accepted.has_value());
            REQUIRE(accepted->core.template_length == alignment.template_length * scale);
            alignment.template_length += negative ? -1 : 1;
            REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 65, 1, references, protein).has_value());
        }
    }
}

TEST_CASE("Native alignment summaries preserve all legal integer widths and signed scores",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    struct integer_case { char type; std::string_view text; bool nonnegative; };
    constexpr auto cases = std::to_array<integer_case>({
        {'c', "0", true}, {'c', "127", true}, {'c', "-128", false},
        {'C', "255", true}, {'s', "32767", true}, {'s', "-32768", false},
        {'S', "65535", true}, {'i', "2147483647", true}, {'i', "-2147483648", false},
        {'I', "4294967295", true},
    });
    for (const auto& input : cases) {
        CAPTURE(input.type, input.text);
        std::array<std::uint8_t, 4> storage{};
        REQUIRE(codec::parse_sam_auxiliary_number(input.type, input.text, storage).has_value());
        const auto bytes = std::as_bytes(std::span{storage}.first(codec::auxiliary_element_bytes(input.type)));
        const bioseq::io::auxiliary_field_view differences{{'N', 'M'}, input.type, 0, bytes};
        const bioseq::io::auxiliary_field_view score{{'A', 'S'}, input.type, 0, bytes};
        REQUIRE(codec::validate_auxiliary_field(differences).has_value());
        REQUIRE(codec::valid_native_alignment_summary(differences) == input.nonnegative);
        REQUIRE(codec::valid_native_alignment_summary(score));
        for (const bool sam_text : {false, true}) {
            REQUIRE(codec::retain_original_native_auxiliary(differences, false, sam_text, false) == input.nonnegative);
            REQUIRE(codec::retain_original_native_auxiliary(score, false, sam_text, false));
            REQUIRE(codec::retain_original_native_auxiliary(score, true, sam_text, false));
        }
    }
}

TEST_CASE("Native MD summaries follow the complete SAMtags grammar without numeric overflow",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    struct md_case { std::string_view text; bool valid; };
    constexpr auto cases = std::to_array<md_case>({
        {"0", true}, {"00", true}, {"01", true}, {"0A0", true}, {"0Z0", true},
        {"0^ABC0", true}, {"1A0^BC0D2", true}, {"0A0^BC0Z0", true},
        {"999999999999999999999999999999999999999999999999999999999999999999", true},
        {"", false}, {"A0", false}, {"0A", false}, {"0AA0", false},
        {"0^0", false}, {"0^A", false}, {"0^^A0", false}, {"0^A^C0", false},
        {"0a0", false}, {"0^Aa0", false}, {"1 ", false}, {" 1", false},
        {"+1", false}, {"-1", false}, {"1.0", false}, {"0A0junk", false},
    });
    for (const auto& input : cases) {
        CAPTURE(input.text);
        const bioseq::io::auxiliary_field_view field{{'M', 'D'}, 'Z', 0,
            std::as_bytes(std::span<const char>{input.text.data(), input.text.size()})};
        REQUIRE(codec::validate_auxiliary_field(field).has_value());
        REQUIRE(codec::valid_native_alignment_summary(field) == input.valid);
        for (const bool sam_text : {false, true})
            REQUIRE(codec::retain_original_native_auxiliary(field, false, sam_text, false) == input.valid);
    }
}

TEST_CASE("Malformed standard summaries remain payload-only without restricting custom tags",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    for (const std::string_view text : {
            "NM:Z:bad", "NM:f:0", "MD:i:1", "MD:f:1", "AS:Z:bad", "AS:f:-1"}) {
        CAPTURE(text);
        std::array<std::uint8_t, 4> storage{};
        const auto parsed = codec::parse_sam_auxiliary_scalar(text, storage);
        REQUIRE(parsed.has_value());
        REQUIRE(codec::validate_auxiliary_field(*parsed).has_value());
        REQUIRE_FALSE(codec::valid_native_alignment_summary(*parsed));
        auto custom = *parsed;
        custom.tag = {'z', 'z'};
        REQUIRE(codec::valid_native_alignment_summary(custom));
        for (const bool sam_text : {false, true}) {
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(*parsed, false, sam_text, false));
            REQUIRE(codec::retain_original_native_auxiliary(custom, false, sam_text, false));
        }
    }
    const std::array<std::uint8_t, 4> empty_array{};
    const bioseq::io::auxiliary_field_view array_summary{{'A', 'S'}, 'B', 'C', std::as_bytes(std::span{empty_array})};
    REQUIRE(codec::validate_auxiliary_field(array_summary).has_value());
    REQUIRE_FALSE(codec::valid_native_alignment_summary(array_summary));
    for (const auto tag : {std::array{'N', 'M'}, std::array{'M', 'D'}, std::array{'A', 'S'}}) {
        for (const std::uint32_t bits : {0x7fc00001u, 0x7f800000u, 0xff800000u}) {
            CAPTURE(tag, bits);
            std::array<std::uint8_t, 4> storage{};
            codec::store_wire_integer(storage, bits);
            const bioseq::io::auxiliary_field_view field{tag, 'f', 0, std::as_bytes(std::span{storage})};
            REQUIRE(codec::validate_auxiliary_field(field).has_value());
            REQUIRE_FALSE(codec::valid_native_alignment_summary(field));
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(field, false, false, false));
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(field, false, true, false));
            auto custom = field;
            custom.tag = {'Z', 'F'};
            REQUIRE(codec::retain_original_native_auxiliary(custom, false, false, false));
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(custom, false, true, false));
        }
    }
}

TEST_CASE("SAM numeric auxiliaries round directly to binary32",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    struct number_case { std::string_view text; std::uint32_t bits; };
    constexpr auto cases = std::to_array<number_case>({
        {"0", 0x00000000u}, {"-0", 0x80000000u}, {"+0", 0x00000000u},
        {"-0.0e+100", 0x80000000u}, {"0e-99999", 0x00000000u},
        {"-0e-99999", 0x80000000u}, {"+1.25", 0x3fa00000u},
        {".5", 0x3f000000u}, {"1e+2", 0x42c80000u}, {"0.1", 0x3dcccccdu},
        // Exact ties choose the even significand; the nearby decimal catches double rounding.
        {"1.000000059604644775390625", 0x3f800000u},
        {"1.0000000596046448", 0x3f800001u},
        {"1.0000000596046447", 0x3f800000u},
        {"1.000000178813934326171875", 0x3f800002u},
        {"-1.0000000596046448", 0xbf800001u},
        {"16777217", 0x4b800000u}, {"16777219", 0x4b800002u},
        // Largest finite binary32 and the last integer below its overflow midpoint.
        {"340282346638528859811704183484516925440", 0x7f7fffffu},
        {"340282356779733661637539395458142568447", 0x7f7fffffu},
        {"1.1754943508222875e-38", 0x00800000u},
        // Exact powers of two exercise gradual underflow and both subnormal tie directions.
        {"1.40129846432481707092372958328991613128026194187651577175706828388979108268586060148663818836212158203125e-45", 0x00000001u},
        {"-1.40129846432481707092372958328991613128026194187651577175706828388979108268586060148663818836212158203125e-45", 0x80000001u},
        {"7.006492321624086e-46", 0x00000001u},
        {"2.101947696487225606385594374934874196920392912814773657635602425834686624028790902229957282543182373046875e-45", 0x00000002u},
        {"1.175494210692441075487029444849287348827052428745893333857174530571588870475618904265502351336181163787841796875e-38", 0x007fffffu},
        {"1.1754942807573642917278829910357665133228589927589904276829631184250030649651730385585324256680905818939208984375e-38", 0x00800000u},
    });
    for (const auto& input : cases) {
        CAPTURE(input.text, input.bits);
        std::array<std::uint8_t, 4> output{};
        const auto parsed = codec::parse_sam_auxiliary_number('f', input.text, output);
        REQUIRE(parsed.has_value());
        REQUIRE(codec::read_wire_unsigned<std::uint32_t>(output) == input.bits);
    }
}

TEST_CASE("SAM float auxiliaries reject invalid and nonrepresentable values without writing",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    constexpr auto invalid = std::to_array<std::string_view>({
        "", "+", "-", ".", "-.", "+-1", "+-0", "++1", "--1", "-+1",
        " 1", "1 ", "\t1", "1\n", "1,5", "1.2.3", "1e", "1e+", "1e-",
        "0x1p0", "1f", "inf", "-inf", "+inf", "Infinity", "NaN", "nan(123)", "-nan",
        "1e99999", "-1e99999", "1e-99999", "-1e-99999",
        "340282356779733661637539395458142568448",
        "-340282356779733661637539395458142568448",
        "7.006492321624085e-46",
        // Half the smallest subnormal rounds to zero and must report underflow.
        "7.00649232162408535461864791644958065640130970938257885878534141944895541342930300743319094181060791015625e-46",
        std::string_view{"1\0x", 3},
    });
    for (const auto input : invalid) {
        CAPTURE(input);
        std::array<std::uint8_t, 4> output{0xa5, 0xa5, 0xa5, 0xa5};
        const auto original = output;
        REQUIRE_FALSE(codec::parse_sam_auxiliary_number('f', input, output).has_value());
        REQUIRE(output == original);
    }
    std::array<std::uint8_t, 3> short_output{0xa5, 0xa5, 0xa5};
    const auto original = short_output;
    REQUIRE_FALSE(codec::parse_sam_auxiliary_number('f', "1", short_output).has_value());
    REQUIRE(short_output == original);
}

TEST_CASE("SAM numeric auxiliary subtypes enforce ranges and single signs",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    struct number_case { char type; std::string_view text; std::uint32_t bits; };
    constexpr auto valid = std::to_array<number_case>({
        {'c', "-128", 0x80u}, {'c', "+127", 0x7fu}, {'C', "255", 0xffu},
        {'s', "-32768", 0x8000u}, {'s', "+32767", 0x7fffu}, {'S', "65535", 0xffffu},
        {'i', "-2147483648", 0x80000000u}, {'i', "+2147483647", 0x7fffffffu},
        {'I', "+4294967295", 0xffffffffu},
    });
    for (const auto& input : valid) {
        CAPTURE(input.type, input.text);
        std::array<std::uint8_t, 4> output{};
        REQUIRE(codec::parse_sam_auxiliary_number(input.type, input.text, output).has_value());
        REQUIRE(codec::read_wire_unsigned<std::uint32_t>(output) == input.bits);
    }
    constexpr auto overflow = std::to_array<number_case>({
        {'c', "-129", 0}, {'c', "128", 0}, {'C', "-1", 0}, {'C', "256", 0},
        {'s', "-32769", 0}, {'s', "32768", 0}, {'S', "-1", 0}, {'S', "65536", 0},
        {'i', "-2147483649", 0}, {'i', "2147483648", 0},
        {'I', "-1", 0}, {'I', "4294967296", 0},
    });
    for (const auto& input : overflow) {
        CAPTURE(input.type, input.text);
        std::array<std::uint8_t, 4> output{0xa5, 0xa5, 0xa5, 0xa5};
        const auto original = output;
        REQUIRE_FALSE(codec::parse_sam_auxiliary_number(input.type, input.text, output).has_value());
        REQUIRE(output == original);
    }
    for (const char type : {'c', 'C', 's', 'S', 'i', 'I', 'f'}) {
        for (const std::string_view text : {"+-1", "+-0", "++1", "--1", "-+1", "+", "-"}) {
            CAPTURE(type, text);
            std::array<std::uint8_t, 4> output{0xa5, 0xa5, 0xa5, 0xa5};
            const auto original = output;
            REQUIRE_FALSE(codec::parse_sam_auxiliary_number(type, text, output).has_value());
            REQUIRE(output == original);
        }
    }
}

TEST_CASE("SAM scalar integer auxiliaries preserve signed and unsigned boundaries",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    struct number_case { std::string_view text; char type; std::uint32_t bits; };
    constexpr auto valid = std::to_array<number_case>({
        {"XI:i:-2147483648", 'i', 0x80000000u},
        {"XI:i:+2147483647", 'i', 0x7fffffffu},
        {"XI:i:2147483648", 'I', 0x80000000u},
        {"XI:i:+4294967295", 'I', 0xffffffffu},
        {"XI:i:-0", 'i', 0u}, {"XI:i:+0", 'i', 0u},
    });
    for (const auto& input : valid) {
        CAPTURE(input.text);
        std::array<std::uint8_t, 4> scratch{};
        const auto field = codec::parse_sam_auxiliary_scalar(input.text, scratch);
        REQUIRE(field.has_value());
        REQUIRE(field->type == input.type);
        REQUIRE(field->payload.size() == 4u);
        REQUIRE(codec::read_wire_unsigned<std::uint32_t>(scratch) == input.bits);
    }
    for (const std::string_view text : {
            "XI:i:", "XI:i:+", "XI:i:-", "XI:i:+-1", "XI:i:+-0", "XI:i:++1",
            "XI:i:--1", "XI:i:-+1", "XI:i:-2147483649", "XI:i:4294967296",
            "XI:i:1.0", "XI:i:1e0", "XI:i: 1", "XI:i:1 "}) {
        CAPTURE(text);
        std::array<std::uint8_t, 4> scratch{0xa5, 0xa5, 0xa5, 0xa5};
        const auto original = scratch;
        REQUIRE_FALSE(codec::parse_sam_auxiliary_scalar(text, scratch).has_value());
        REQUIRE(scratch == original);
    }
}

TEST_CASE("write_records preserves source-bound interpretation through private replay",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    struct option_case {
        std::string_view label;
        std::optional<bioseq::io::record_orientation> orientation;
        std::optional<bioseq::io::cigar_form> form;
        bool accepted;
    };
    const std::array cases{
        option_case{"absent", {}, {}, true},
        option_case{"matching", bioseq::io::record_orientation::reference_forward,
            bioseq::io::cigar_form::sam_complete, true},
        option_case{"orientation", bioseq::io::record_orientation::original_query, {}, false},
        option_case{"form", {}, bioseq::io::cigar_form::alignment_core, false},
    };
    for (const auto& input : cases) {
        CAPTURE(input.label);
        publication_fixture fixture;
        pressure_retry_source source{pressure_retry_source::pressure_policy::once};
        // Only the first, spilled row is trusted. No identity/context fields can stand in for this fact.
        source.records[0].alignment->source_bound_interpretation = true;
        source.records[0].alignment->sequence_orientation = bioseq::io::record_orientation::reference_forward;
        bioseq::io::writer_options options;
        options.format = bioseq::io::sequence_file_format::sam;
        options.chunk_capacity_bytes = 128u;
        options.sequence_orientation = input.orientation;
        options.alignment_cigar_form = input.form;
        bioseq::io::concurrency_options concurrency;
        concurrency.worker_count = 1u;
        concurrency.io_buffer_pool_size = 1u;
        const auto path = fixture.directory / (std::string{input.label} + ".sam");
        const auto written = bioseq::io::write_records(source, path.string(), options, concurrency);
        REQUIRE(source.pressure_returns == 1u);
        REQUIRE(source.released_before_retry == 1u); // The trusted first row crossed compact replay.
        REQUIRE(written.has_value() == input.accepted);
        if (input.accepted) {
            REQUIRE(*written == 2u);
            const auto bytes = publication_fixture::read(path);
            REQUIRE(bytes.find("\"source_orientation\":\"reference_forward\"") != std::string::npos);
            REQUIRE(bytes.find("\"cigar_form\":\"sam_complete\"") != std::string::npos);
        } else {
            REQUIRE(written.error().message.find("disagrees with the source alignment") != std::string::npos);
            REQUIRE_FALSE(std::filesystem::exists(path));
        }
        // Options only change a borrowed copy, never the source owner's interpretation.
        REQUIRE(source.records[1].alignment->sequence_orientation == bioseq::io::record_orientation::original_query);
        REQUIRE(source.records[1].alignment->form == bioseq::io::cigar_form::sam_complete);
    }
}

TEST_CASE("write_records spills retained owners then retries one evaluated pending row",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto reserved_before = admission.reserved_bytes();
    bioseq::io::writer_options options;
    options.format = bioseq::io::sequence_file_format::sam;
    options.chunk_capacity_bytes = 128u;
    bioseq::io::concurrency_options concurrency;
    concurrency.worker_count = 1u; // No formatting jobs: the retained-batch retry is the only possible branch.
    concurrency.io_buffer_pool_size = 1u;
    const auto baseline_path = fixture.directory / "baseline.sam";
    {
        pressure_retry_source baseline{pressure_retry_source::pressure_policy::none};
        const auto written = bioseq::io::write_records(baseline, baseline_path.string(), options, concurrency);
        REQUIRE(written.has_value());
        REQUIRE(*written == 2u);
        REQUIRE(baseline.next_calls == 3u);
        REQUIRE((baseline.evaluations == std::array<std::size_t, 2>{1u, 1u}));
        REQUIRE(baseline.released_batches == 2u);
        REQUIRE(baseline.cancel_calls == 0u);
        REQUIRE(admission.reserved_bytes() == reserved_before);
    }
    const auto retry_path = fixture.directory / "retry.sam";
    pressure_retry_source source{pressure_retry_source::pressure_policy::once};
    const auto written = bioseq::io::write_records(source, retry_path.string(), options, concurrency);
    REQUIRE(written.has_value());
    REQUIRE(*written == 2u);
    REQUIRE(source.next_calls == 4u); // Two deliveries, one forced retry, one EOF.
    REQUIRE((source.evaluations == std::array<std::size_t, 2>{1u, 1u}));
    REQUIRE(source.pressure_returns == 1u);
    REQUIRE(source.released_before_pressure == 0u);
    REQUIRE(source.released_before_retry == 1u); // spill_retained retired the first source owner before next().
    REQUIRE(source.released_batches == 2u);
    REQUIRE(source.cancel_calls == 0u);
    REQUIRE_FALSE(static_cast<bool>(source.pending_permit));
    REQUIRE(admission.reserved_bytes() == reserved_before);

    const auto output = publication_fixture::read(retry_path);
    REQUIRE(output == publication_fixture::read(baseline_path)); // Exact header, payload and record-order equality.
    std::istringstream lines{output};
    std::vector<std::string> records;
    for (std::string line; std::getline(lines, line);)
        if (!line.starts_with('@')) records.push_back(std::move(line));
    REQUIRE(records.size() == 2u);
    REQUIRE(records[0].starts_with("first\t4\t*\t0\t0\t*\t*\t0\t0\tACGT\t*\tbq:Z:"));
    REQUIRE(records[1].starts_with("second\t4\t*\t0\t0\t*\t*\t0\t0\tTGCA\t*\tbq:Z:"));
    REQUIRE_FALSE(fixture.has_private_staging());
}

TEST_CASE("write_records cancels unrecoverable pending-row pressure without publishing",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    for (const bool existing : {false, true}) {
        CAPTURE(existing);
        publication_fixture fixture;
        const auto path = fixture.directory / "protected.sam";
        if (existing) publication_fixture::seed(path, "previous complete output\n");
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        const auto reserved_before = admission.reserved_bytes();
        bioseq::io::writer_options options;
        options.format = bioseq::io::sequence_file_format::sam;
        options.chunk_capacity_bytes = 128u;
        options.overwrite = existing;
        bioseq::io::concurrency_options concurrency;
        concurrency.worker_count = 1u;
        concurrency.io_buffer_pool_size = 1u;
        pressure_retry_source source{pressure_retry_source::pressure_policy::repeated};
        const auto written = bioseq::io::write_records(source, path.string(), options, concurrency);
        REQUIRE_FALSE(written.has_value());
        REQUIRE(written.error().code == bioseq::io::record_error_code::memory_pressure);
        REQUIRE(written.error().row_ordinal == 2u);
        REQUIRE(written.error().message == "injected pending-row memory pressure");
        REQUIRE(source.next_calls == 3u); // One delivery and two failed attempts at the same pending row.
        REQUIRE(source.delivered_batches == 1u);
        REQUIRE((source.evaluations == std::array<std::size_t, 2>{1u, 1u}));
        REQUIRE(source.pressure_returns == 2u);
        REQUIRE(source.released_before_pressure == 0u);
        REQUIRE(source.released_before_retry == 1u);
        REQUIRE(source.released_batches == 1u);
        REQUIRE(source.cancel_calls == 1u);
        REQUIRE(source.cancel_released_pending);
        REQUIRE_FALSE(static_cast<bool>(source.pending_permit));
        REQUIRE(admission.reserved_bytes() == reserved_before); // Source remains alive: cancel(), not its destructor, released it.
        REQUIRE(std::filesystem::exists(path) == existing);
        if (existing) REQUIRE(publication_fixture::read(path) == "previous complete output\n");
        REQUIRE_FALSE(fixture.has_private_staging());
    }
}


namespace {

/** Exercise checked native headers with a bounded sink and tiny test-owned metadata. */
[[nodiscard]] std::expected<std::string, std::string> capture_sam_header(
    const std::vector<std::string>& lines,
    const std::vector<bioseq::io::reference_descriptor>& references = {},
    const std::vector<bioseq::io::record_header_context>& contexts = {},
    const std::optional<std::vector<std::string>>& replay_header = std::nullopt) {
    namespace codec = bioseq::io::alignment;
    bioseq::io::record_source_metadata metadata;
    metadata.references = references;
    metadata.header_contexts = contexts;
    metadata.replay_header_lines = replay_header;
    bioseq::io::writer_options options;
    options.sam_header = lines;
    codec::alignment_export_context context;
    // Syntax-only fixtures are small; production admission remains owned by the writer.
    const codec::alignment_context_admission admission{
        nullptr, [](void*, std::size_t, bioseq::io::io_memory_credit*) -> std::expected<void, std::string> { return {}; }};
    if (const auto initialized = context.initialize(metadata, options, admission); !initialized)
        return std::unexpected(initialized.error());
    if (const auto finalized = context.finalize(); !finalized) return std::unexpected(finalized.error());
    std::array<std::uint8_t, 4096> bytes{};
    codec::alignment_window_sink sink{bytes};
    if (const auto emitted = context.emit_header(sink, sequence_file_format::sam); !emitted)
        return std::unexpected(emitted.error());
    return std::string{reinterpret_cast<const char*>(bytes.data()), sink.written};
}

} // namespace

TEST_CASE("SAM reference aliases reject empty and conflicting names before emission",
          "[component:bioseq][layer:unit][feature:io]") {
    for (const std::string_view aliases : {
            "", ",alias", "alias,", "alias,,other", "alias,alias", "ref", "*alias", "=alias", "bad name"}) {
        CAPTURE(aliases);
        REQUIRE_FALSE(capture_sam_header({"@SQ\tSN:ref\tLN:10\tAN:" + std::string{aliases}}).has_value());
        const bioseq::io::reference_descriptor reference{
            .identity = 0, .name = "ref", .length = 10,
            .attributes = {{{'A', 'N'}, std::string{aliases}}}};
        REQUIRE_FALSE(capture_sam_header({}, {reference}).has_value());
    }
    REQUIRE_FALSE(capture_sam_header({"@SQ\tSN:one\tLN:10\tAN:two", "@SQ\tSN:two\tLN:10"}).has_value());
    REQUIRE_FALSE(capture_sam_header({"@SQ\tSN:two\tLN:10", "@SQ\tSN:one\tLN:10\tAN:two"}).has_value());
    REQUIRE_FALSE(capture_sam_header({"@SQ\tSN:one\tLN:10\tAN:alias", "@SQ\tSN:two\tLN:10\tAN:alias"}).has_value());
    const auto valid = capture_sam_header({"@SQ\tSN:ref\tLN:10\tAN:one,two"});
    REQUIRE(valid.has_value());
    REQUIRE(valid->find("\tAN:one,two\n") != std::string::npos);
}

TEST_CASE("SAM header attributes reject ASCII controls in native and typed declarations",
          "[component:bioseq][layer:unit][feature:io]") {
    for (const std::string_view prefix : {
            "@HD\tVN:1.6\tzz:", "@SQ\tSN:ref\tLN:10\tSP:", "@SQ\tSN:ref\tLN:10\tDS:",
            "@RG\tID:rg\tSM:", "@RG\tID:rg\tDS:", "@PG\tID:pg\tPN:", "@PG\tID:pg\tDS:", "@PG\tID:pg\tCL:"}) {
        for (unsigned character = 0; character <= 127u; ++character) {
            if (character >= 32u && character < 127u) continue;
            CAPTURE(prefix, character);
            const std::string value = "left" + std::string(1, static_cast<char>(character)) + "right";
            REQUIRE_FALSE(capture_sam_header({std::string{prefix} + value}).has_value());
            const bioseq::io::reference_descriptor reference{
                .identity = 0, .name = "ref", .length = 10, .attributes = {{{'D', 'S'}, value}}};
            REQUIRE_FALSE(capture_sam_header({}, {reference}).has_value());
        }
    }
    for (const std::string_view line : {"@HD\t", "@RG\tID:rg\t", "@PG\tID:pg\t1X:value", "@CO"})
        REQUIRE_FALSE(capture_sam_header({std::string{line}}).has_value());
}

TEST_CASE("SAM header UTF8 exceptions preserve descriptions commands and comment tabs",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::string unicode = std::string(96, 'a') + " caf\xc3\xa9 \xe6\xb0\xb4 \xf0\x9f\xa7\xac";
    const std::vector<std::string> lines{
        "@SQ\tSN:ref\tLN:10\tDS:" + unicode,
        "@RG\tID:rg\tDS:" + unicode,
        "@PG\tID:pg\tDS:" + unicode + "\tCL:tool " + unicode,
        "@CO\tfirst\t" + unicode + "\tlast", "@CO\t"};
    const auto emitted = capture_sam_header(lines);
    REQUIRE(emitted.has_value());
    for (const auto& line : lines) REQUIRE(emitted->find(line + '\n') != std::string::npos);
    REQUIRE(bioseq::io::alignment::parse_sam_reference_header(lines.front()).has_value());
    const bioseq::io::reference_descriptor reference{
        .identity = 0, .name = "ref", .length = 10, .attributes = {{{'D', 'S'}, unicode}}};
    const auto typed = capture_sam_header({}, {reference});
    REQUIRE(typed.has_value());
    REQUIRE(typed->find("\tDS:" + unicode + '\n') != std::string::npos);
    for (const std::string_view prefix : {
            "@HD\tVN:1.6\tDS:", "@SQ\tSN:ref\tLN:10\tSP:", "@SQ\tSN:ref\tLN:10\tCL:",
            "@RG\tID:rg\tSM:", "@RG\tID:rg\tCL:", "@PG\tID:pg\tPN:", "@PG\tID:pg\tzz:"}) {
        CAPTURE(prefix);
        REQUIRE_FALSE(capture_sam_header({std::string{prefix} + unicode}).has_value());
    }
}

TEST_CASE("SAM permitted UTF8 fields reject malformed sequences before emission",
          "[component:bioseq][layer:unit][feature:io]") {
    for (const std::string_view prefix : {
            "@SQ\tSN:ref\tLN:10\tDS:", "@RG\tID:rg\tDS:", "@PG\tID:pg\tDS:", "@PG\tID:pg\tCL:", "@CO\t"}) {
        for (const std::string_view invalid : {
                "\x80", "\xc0\xaf", "\xc2", "\xe2\x82", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2" "x" "\xa1", "\xff"}) {
            CAPTURE(prefix, invalid);
            REQUIRE_FALSE(capture_sam_header({std::string{prefix} + invalid}).has_value());
        }
    }
    for (const char separator : {'\0', '\r', '\n'}) {
        const std::string line = "@CO\tleft" + std::string(1, separator) + "right";
        REQUIRE_FALSE(capture_sam_header({line}).has_value());
    }
}


TEST_CASE("SAM required header fields and HD ordering are checked per original context",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    for (const std::string_view line : {
            "@HDFOO", "@SQFOO", "@HD\tSO:unknown", "@HD\tVN:1", "@HD\tVN:.6", "@HD\tVN:1.",
            "@HD\tVN:1.6.0", "@HD\tVN:v1.6", "@HD\tVN:1.6\tVN:1.6", "@SQ\tSN:ref", "@SQ\tLN:10"}) {
        CAPTURE(line);
        REQUIRE_FALSE(capture_sam_header({std::string{line}}).has_value());
        REQUIRE_FALSE(capture_sam_header({}, {}, {{0, {std::string{line}}}}).has_value());
    }
    REQUIRE_FALSE(capture_sam_header({"@HD\tVN:1.6", "@HD\tVN:1.6"}).has_value());
    REQUIRE_FALSE(capture_sam_header({"@CO\tcomment", "@HD\tVN:1.6"}).has_value());
    REQUIRE(capture_sam_header({}).has_value());
    REQUIRE(capture_sam_header({"@SQ\tSN:ref\tLN:10"}).has_value());
    const auto merged = capture_sam_header({}, {}, {
        {0, {"@HD\tVN:1.6", "@RG\tID:first"}},
        {1, {"@HD\tVN:1.5", "@PG\tID:second"}},
        {2, {"@CO\theader without HD"}}});
    REQUIRE(merged.has_value());
    REQUIRE(merged->find("@RG\tID:first\n") != std::string::npos);
    REQUIRE(merged->find("@PG\tID:second\n") != std::string::npos);
    REQUIRE(merged->find("@CO\theader without HD\n") != std::string::npos);

    // Replay defaults belong to the actual native file, independently of its
    // original contexts and the original-symbol geometry of protein references.
    struct replay_consumer {
        bioseq::io::record_source_metadata metadata;
        std::size_t records{};
        /** Own this tiny fixture snapshot after the reader's admitted header dies. */
        std::expected<void, std::string> header(const bioseq::io::record_source_metadata& source) {
            metadata = source;
            return {};
        }
        /** Header-only fixtures must not produce records. */
        std::expected<void, std::string> record(bioseq::io::decoded_record&) {
            ++records;
            return {};
        }
    };
    const std::vector<bioseq::io::record_header_context> originals{
        {41u, {"@HD\tVN:1.6", "@RG\tID:shared\tSM:original", "@CO\toriginal comment"}}};
    bioseq::io::reference_descriptor protein_reference;
    protein_reference.identity = 7u;
    protein_reference.name = "protein";
    protein_reference.length = 11u;
    protein_reference.alphabet_code = ::supported_storage_alphabets::index_of<seq::aa27>();
    protein_reference.context_id = 41u;
    const std::vector<std::string> selected{
        "@RG\tID:shared\tSM:chosen", "@PG\tID:chosen\tPN:fixture", "@CO\tchosen comment"};
    const auto selected_file = capture_sam_header(selected, {protein_reference}, originals);
    REQUIRE(selected_file.has_value());
    publication_fixture fixture;
    const auto path = fixture.directory / "replay_header.sam";
    std::string current = *selected_file;
    for (const bool downstream_edits : {false, true}) {
        auto expected_lines = selected;
        if (downstream_edits) {
            const std::string old_group = "@RG\tID:shared\tSM:chosen\n";
            const auto group_position = current.find(old_group);
            REQUIRE(group_position != std::string::npos);
            current.replace(group_position, old_group.size(), "@RG\tID:shared\tSM:edited\n");
            const auto manifest_position = current.find("@CO\tbioseq.io:1:");
            REQUIRE(manifest_position != std::string::npos);
            current.insert(manifest_position,
                "@PG\tID:downstream\tPP:chosen\tPN:fixture\n@CO\tdownstream comment\n");
            expected_lines[0] = "@RG\tID:shared\tSM:edited";
            expected_lines.push_back("@PG\tID:downstream\tPP:chosen\tPN:fixture");
            expected_lines.push_back("@CO\tdownstream comment");
        }
        for (unsigned cycle = 0; cycle != 3u; ++cycle) {
            CAPTURE(downstream_edits, cycle);
            publication_fixture::seed(path, current);
            replay_consumer consumer;
            const auto loaded = bioseq::io::read_alignment_records(path.string(), sequence_file_format::sam, consumer);
            REQUIRE(loaded.has_value());
            REQUIRE(*loaded == 0u);
            REQUIRE(consumer.records == 0u);
            REQUIRE(consumer.metadata.header_contexts.size() == 1u);
            REQUIRE(consumer.metadata.header_contexts.front().id == 41u);
            REQUIRE(consumer.metadata.header_contexts.front().original_header_lines == originals.front().original_header_lines);
            REQUIRE(consumer.metadata.replay_header_lines.has_value());
            REQUIRE(*consumer.metadata.replay_header_lines == expected_lines);
            REQUIRE(consumer.metadata.references.size() == 1u);
            REQUIRE(consumer.metadata.references.front().length == 11u);
            REQUIRE(consumer.metadata.references.front().alphabet_code == protein_reference.alphabet_code);
            REQUIRE(consumer.metadata.references.front().context_id == protein_reference.context_id);
            const auto emitted = capture_sam_header({}, consumer.metadata.references,
                consumer.metadata.header_contexts, consumer.metadata.replay_header_lines);
            REQUIRE(emitted.has_value());
            REQUIRE(emitted->find("\tLN:33\t") != std::string::npos);
            REQUIRE(emitted->find("@CO\tbioseq.io:1:") != std::string::npos);
            REQUIRE(emitted->find("@CO\tbioseq.io:1:") == emitted->rfind("@CO\tbioseq.io:1:"));
            if (!downstream_edits || cycle != 0u) REQUIRE(*emitted == current);
            current = *emitted;
        }
    }
    // An intentionally empty replacement remains empty on replay, even when
    // the preserved original context contains RG and CO declarations.
    const auto empty_file = capture_sam_header({"@HD\tVN:1.6"}, {protein_reference}, originals);
    REQUIRE(empty_file.has_value());
    publication_fixture::seed(path, *empty_file);
    replay_consumer empty_consumer;
    REQUIRE(bioseq::io::read_alignment_records(path.string(), sequence_file_format::sam, empty_consumer).has_value());
    REQUIRE(empty_consumer.metadata.replay_header_lines.has_value());
    REQUIRE(empty_consumer.metadata.replay_header_lines->empty());
    const auto empty_replay = capture_sam_header({}, empty_consumer.metadata.references,
        empty_consumer.metadata.header_contexts, empty_consumer.metadata.replay_header_lines);
    REQUIRE(empty_replay.has_value());
    REQUIRE(*empty_replay == *empty_file);
    for (const std::string_view forbidden : {
            "@HD\tVN:1.6", "@SQ\tSN:protein\tLN:33", "@CO\tbioseq.io:1:{}"}) {
        REQUIRE_FALSE(capture_sam_header({}, {}, {}, std::vector<std::string>{std::string{forbidden}}).has_value());
    }
}

TEST_CASE("SAM and BAM retain original FLAG bits while native semantics ignore reserved bits",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    struct byte_sink {
        std::vector<std::uint8_t> bytes;
        /** Retain this small native prefix so its wire fields can be checked independently. */
        std::expected<void, std::string> append(std::span<const std::uint8_t> input) {
            bytes.insert(bytes.end(), input.begin(), input.end());
            return {};
        }
    };
    codec::sam_cigar_source cigar{"2M"};
    codec::native_reference_binding references;
    references.reference_id = 0;
    references.reference_length = 100;
    bioseq::io::alignment_record_view alignment;
    alignment.cigar = cigar.borrow();
    alignment.mapping_quality = 37;
    alignment.target_start = 0;
    for (const std::uint16_t flags : {std::uint16_t{0x1000}, std::uint16_t{0x8000},
            std::uint16_t{0xf000}, std::uint16_t{0xf010}, std::uint16_t{0xffff}}) {
        CAPTURE(flags);
        const auto known_flags = static_cast<std::uint16_t>(flags & 0x0fffu);
        const auto sam = codec::parse_sam_record("q\t" + std::to_string(flags)
            + "\tchr\t1\t37\t2M\t*\t0\t0\tAC\t!~");
        REQUIRE(sam.has_value());
        REQUIRE(sam->flags == flags);

        // Construct a complete two-base native record independently of the emitter.
        std::array<std::uint8_t, 45> bytes{};
        codec::store_wire_integer(std::span{bytes}.first(4), std::uint32_t{41});
        const auto core = std::span{bytes}.subspan(4);
        core[8] = 2; core[9] = 37;
        codec::store_wire_integer(core.subspan(12, 2), std::uint16_t{1});
        codec::store_wire_integer(core.subspan(14, 2), flags);
        codec::store_wire_integer(core.subspan(16, 4), std::int32_t{2});
        codec::store_wire_integer(core.subspan(20, 4), std::int32_t{-1});
        codec::store_wire_integer(core.subspan(24, 4), std::int32_t{-1});
        core[32] = 'q';
        codec::store_wire_integer(core.subspan(34, 4), std::uint32_t{2u << 4u});
        core[38] = 0x12; core[39] = 0; core[40] = 93;
        const auto bam = codec::parse_bam_record(bytes, 1);
        REQUIRE(bam.has_value());
        REQUIRE(bam->core.flags == flags);

        alignment.flags = flags;
        const auto projected = codec::prepare_native_alignment(alignment, flags, 2, references, false);
        REQUIRE(projected.has_value());
        REQUIRE(projected->core.flags == known_flags);
        REQUIRE(alignment.flags == flags);
        REQUIRE(codec::validate_bam_core_projection(*projected, *bam).has_value());
        auto changed = *bam;
        changed.core.flags ^= 0x10u;
        REQUIRE_FALSE(codec::validate_bam_core_projection(*projected, changed).has_value());

        // Both low-level emitters clear reserved bits even when handed the raw parsed core.
        const auto measured = codec::measure_bam_record("q", bam->core, projected->cigar_size, 0);
        REQUIRE(measured.has_value());
        byte_sink binary;
        REQUIRE(codec::emit_bam_record_prefix(binary, "q", bam->core, *measured).has_value());
        REQUIRE(codec::read_wire_unsigned<std::uint16_t>(std::span{binary.bytes}.subspan(18, 2)) == known_flags);
        byte_sink text;
        REQUIRE(codec::emit_sam_record_prefix(text, "q", "chr", "*", bam->core,
            projected->cigar, projected->cigar_size).has_value());
        const std::string prefix{text.bytes.begin(), text.bytes.end()};
        REQUIRE(prefix == "q\t" + std::to_string(known_flags) + "\tchr\t1\t37\t2M\t*\t0\t0\t");
    }
}


TEST_CASE("BAM header text accepts only trailing NUL padding without shifting the dictionary",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    struct header_consumer {
        std::size_t header_calls{}, record_calls{};
        std::vector<bioseq::io::reference_descriptor> references;
        std::vector<bioseq::io::record_header_context> contexts;
        /** Retain the small header so dictionary framing and padding removal are observable. */
        std::expected<void, std::string> header(const bioseq::io::record_source_metadata& metadata) {
            ++header_calls;
            references = metadata.references;
            contexts = metadata.header_contexts;
            return {};
        }
        /** A header-only fixture must not manufacture an alignment record. */
        std::expected<void, std::string> record(bioseq::io::decoded_record&) {
            ++record_calls;
            return {};
        }
    };
    publication_fixture fixture;
    const auto path = fixture.directory / "header.bam";
    const auto write_header = [&path](std::string_view text) {
        std::string bytes{"BAM\1", 4};
        const auto append_integer = [&bytes](std::uint32_t value) {
            std::array<std::uint8_t, 4> encoded{};
            bioseq::io::alignment::store_wire_integer(encoded, value);
            bytes.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        };
        append_integer(static_cast<std::uint32_t>(text.size()));
        bytes.append(text);
        append_integer(1); append_integer(4);
        bytes.append("chr\0", 4);
        append_integer(100);
        publication_fixture::seed(path, bytes);
    };
    constexpr std::string_view header = "@HD\tVN:1.6\n@SQ\tSN:chr\tLN:100\n";
    for (const auto visible : {header, header.substr(0, header.size() - 1), std::string_view{}}) {
        for (const std::size_t padding : {0u, 1u, 7u, 257u}) {
            CAPTURE(visible, padding);
            write_header(std::string{visible} + std::string(padding, '\0'));
            header_consumer consumer;
            const auto read = bioseq::io::read_alignment_records(path.string(), sequence_file_format::bam, consumer);
            REQUIRE(read.has_value());
            REQUIRE(*read == 0u);
            REQUIRE(consumer.header_calls == 1u);
            REQUIRE(consumer.record_calls == 0u);
            REQUIRE(consumer.references.size() == 1u);
            REQUIRE(consumer.references.front().name == "chr");
            REQUIRE(consumer.references.front().length == 100u);
            for (const auto& context : consumer.contexts)
                for (const auto& line : context.original_header_lines)
                    REQUIRE(line.find('\0') == std::string::npos);
        }
    }
    for (const auto text : {std::string{"\0", 1} + std::string{header},
            std::string{"@HD\tVN:1.6"} + std::string(1, '\0') + "\n@SQ\tSN:chr\tLN:100\n",
            std::string{header} + std::string{"\0\0\t", 3}}) {
        write_header(text);
        header_consumer consumer;
        const auto read = bioseq::io::read_alignment_records(path.string(), sequence_file_format::bam, consumer);
        REQUIRE_FALSE(read.has_value());
        REQUIRE(read.error().find("embedded NUL") != std::string::npos);
        REQUIRE(consumer.header_calls == 0u);
        REQUIRE(consumer.record_calls == 0u);
    }
}

TEST_CASE("SAM sequence LUT accepts legal letters and dot while rejecting other bytes",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    constexpr std::string_view iupac = "ACMGRSVTWYHKDBN";
    for (std::size_t byte = 0; byte < 256u; ++byte) {
        CAPTURE(byte);
        const std::array<std::uint8_t, 1> input{static_cast<std::uint8_t>(byte)};
        std::array<std::uint8_t, 1> output{};
        const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        const auto decoded = codec::decode_sam_rank_tile<seqan3::dna15>(input, output);
        REQUIRE(decoded.has_value() == (letter || byte == '.'));
        if (decoded) {
            auto expected = static_cast<char>(byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte);
            if (expected == 'U') expected = 'T';
            if (iupac.find(expected) == std::string_view::npos) expected = 'N';
            REQUIRE(seqan3::to_char(seqan3::assign_rank_to(output.front(), seqan3::dna15{})) == expected);
        }
    }
    // A nontrivial tail traverses the same gather used by bounded production windows.
    constexpr std::string_view letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.";
    std::vector<std::uint8_t> input;
    for (std::size_t repeat = 0; repeat < 23u; ++repeat) input.insert(input.end(), letters.begin(), letters.end());
    std::vector<std::uint8_t> output(input.size());
    REQUIRE(codec::decode_sam_rank_tile<seqan3::dna15>(input, output).has_value());
    for (std::size_t index = 0; index < input.size(); ++index) {
        auto expected = static_cast<char>(input[index]);
        if (expected >= 'a' && expected <= 'z') expected -= 'a' - 'A';
        if (expected == 'U') expected = 'T';
        if (iupac.find(expected) == std::string_view::npos) expected = 'N';
        REQUIRE(seqan3::to_char(seqan3::assign_rank_to(output[index], seqan3::dna15{})) == expected);
    }
}

TEST_CASE("SAM unknown letters require a representable N and never widen quality alphabets",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    for (const char character : std::string_view{"EeFfIiJjLlOoPpQqXxZz."}) {
        CAPTURE(character);
        const std::array<std::uint8_t, 1> input{static_cast<std::uint8_t>(character)};
        std::array<std::uint8_t, 1> output{};
        REQUIRE(codec::decode_sam_rank_tile<seqan3::dna5>(input, output).has_value());
        REQUIRE(seqan3::to_char(seqan3::assign_rank_to(output.front(), seqan3::dna5{})) == 'N');
        REQUIRE_FALSE(codec::decode_sam_rank_tile<seqan3::dna4>(input, output).has_value());
    }
    // Valid IUPAC ambiguity stays exact; a narrower declared source is not silently reduced.
    const std::array<std::uint8_t, 1> ambiguity{static_cast<std::uint8_t>('R')};
    std::array<std::uint8_t, 1> output{};
    REQUIRE_FALSE(codec::decode_sam_rank_tile<seqan3::dna5>(ambiguity, output).has_value());
    const auto check_quality = []<class Quality>(std::size_t cardinality) {
        for (std::size_t byte = 0; byte < 256u; ++byte) {
            CAPTURE(byte, cardinality);
            const std::array<std::uint8_t, 1> input{static_cast<std::uint8_t>(byte)};
            std::array<std::uint8_t, 1> ranks{};
            const auto decoded = bioseq::io::alignment::decode_sam_rank_tile<Quality>(input, ranks);
            const bool valid = byte >= 33u && byte < 33u + cardinality;
            REQUIRE(decoded.has_value() == valid);
            if (valid) REQUIRE(ranks.front() == byte - 33u);
        }
    };
    check_quality.template operator()<seqan3::phred42>(42u);
    check_quality.template operator()<seqan3::phred94>(94u);
}

TEST_CASE("BAM sequence decode ignores every value of the unused odd final nibble",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    for (const std::size_t count : {1u, 3u, 65u, 129u}) {
        for (std::uint8_t padding = 0; padding < 16u; ++padding) {
            CAPTURE(count, padding);
            std::vector<std::uint8_t> packed((count + 1u) / 2u, 0x11u);
            packed.back() = static_cast<std::uint8_t>(0x10u | padding);
            std::vector<std::uint8_t> decoded(count);
            REQUIRE(codec::decode_bam_rank_tile<seqan3::dna15>(packed, decoded).has_value());
            for (const auto rank : decoded)
                REQUIRE(seqan3::to_char(seqan3::assign_rank_to(rank, seqan3::dna15{})) == 'A');
        }
    }
}

TEST_CASE("Unavailable SAM references preserve uninterpreted source coordinates",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    const auto sam = codec::parse_sam_record("q\t13\t*\t37\t37\t2M\t*\t43\t0\tAC\t!~");
    REQUIRE(sam.has_value());
    REQUIRE(sam->position == 36u);
    REQUIRE(sam->mate_position == 42u);
    REQUIRE(sam->reference_name == "*");
    REQUIRE(sam->mate_reference_name == "*");
    REQUIRE_FALSE(codec::parse_sam_record("q\t0\t*\t37\t37\t2M\t*\t0\t0\tAC\t!~").has_value());

    std::array<std::uint8_t, 45> bytes{};
    codec::store_wire_integer(std::span{bytes}.first(4), std::uint32_t{41});
    const auto core = std::span{bytes}.subspan(4);
    codec::store_wire_integer(core.first(4), std::int32_t{-1});
    codec::store_wire_integer(core.subspan(4, 4), std::int32_t{36});
    core[8] = 2; core[9] = 37;
    codec::store_wire_integer(core.subspan(10, 2), std::uint16_t{4681});
    codec::store_wire_integer(core.subspan(12, 2), std::uint16_t{1});
    codec::store_wire_integer(core.subspan(14, 2), std::uint16_t{13});
    codec::store_wire_integer(core.subspan(16, 4), std::int32_t{2});
    codec::store_wire_integer(core.subspan(20, 4), std::int32_t{-1});
    codec::store_wire_integer(core.subspan(24, 4), std::int32_t{42});
    core[32] = 'q';
    codec::store_wire_integer(core.subspan(34, 4), std::uint32_t{2u << 4u});
    core[38] = 0x12; core[39] = 0; core[40] = 93;
    const auto bam = codec::parse_bam_record(bytes, 1);
    REQUIRE(bam.has_value());
    REQUIRE(bam->core.position == 36);
    REQUIRE(bam->core.mate_position == 42);
    codec::store_wire_integer(core.subspan(14, 2), std::uint16_t{0});
    REQUIRE_FALSE(codec::parse_bam_record(bytes, 1).has_value());

    codec::sam_cigar_source cigar{"2M"};
    bioseq::io::alignment_record_view alignment;
    alignment.flags = 13;
    alignment.cigar = cigar.borrow();
    alignment.reference_name = "*";
    alignment.target_start = 36;
    alignment.mate_reference = "*";
    alignment.mate_start = 42;
    for (const bool protein : {false, true}) {
        CAPTURE(protein);
        codec::native_reference_binding absent;
        const auto projected = codec::prepare_native_alignment(alignment, 13, 2, absent, protein);
        REQUIRE(projected.has_value());
        REQUIRE(projected->core.reference_id == -1);
        REQUIRE(projected->core.position == -1);
        REQUIRE(projected->core.mate_reference_id == -1);
        REQUIRE(projected->core.mate_position == -1);
        REQUIRE(projected->core.bin == 4680u);
        REQUIRE(alignment.target_start == 36u);
        REQUIRE(alignment.mate_start == 42u);
    }
    // Unbound protein coordinates/lengths are retained, never scaled into native fields.
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    auto large_original = alignment;
    large_original.flags = 4;
    large_original.target_start = limit - 1;
    large_original.mate_start = limit - 1;
    large_original.reference_length = limit;
    codec::native_reference_binding absent;
    const auto unscaled = codec::prepare_native_alignment(large_original, 4, 2, absent, true);
    REQUIRE(unscaled.has_value());
    REQUIRE(unscaled->core.position == -1);
    REQUIRE(unscaled->core.mate_position == -1);
    REQUIRE(large_original.target_start == limit - 1);
    REQUIRE(large_original.mate_start == limit - 1);
    REQUIRE(large_original.reference_length == limit);
    absent.reference_id = 0;
    REQUIRE_FALSE(codec::prepare_native_alignment(large_original, 4, 2, absent, true).has_value());
    absent.reference_id = -1;
    absent.mate_reference_id = 0;
    absent.mate_same_projection_domain = true; // This control supplies a known protein mate descriptor.
    REQUIRE_FALSE(codec::prepare_native_alignment(large_original, 4, 2, absent, true).has_value());
    absent.mate_reference_id = -1;
    large_original.reference_length = 0;
    REQUIRE_FALSE(codec::prepare_native_alignment(large_original, 4, 2, absent, true).has_value());

    alignment.flags = 65;
    codec::native_reference_binding known;
    known.reference_id = 0;
    known.reference_length = 100;
    const auto target_only = codec::prepare_native_alignment(alignment, 65, 2, known, false);
    REQUIRE(target_only.has_value());
    REQUIRE(target_only->core.position == 36);
    REQUIRE(target_only->core.mate_position == -1);
    known.mate_reference_id = 0;
    known.mate_same_projection_domain = true;
    known.mate_reference_length = 42;
    REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 65, 2, known, false).has_value());
}

TEST_CASE("IO auxiliary vector growth owns both allocations and preserves values on denial",
          "[component:bioseq][layer:unit][feature:io]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous;
        ~restore_cap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, admission.index_memory_cap_bytes()};
    admission.set_index_memory_cap_bytes(baseline + 16u * 1024u * 1024u);
    struct moving_value {
        std::uint64_t value{};
        std::uint64_t* minimum_move_reservation{};
        /** A move observes the reservation while old and new vectors coexist. */
        moving_value(std::uint64_t input, std::uint64_t& observed) noexcept
            : value(input), minimum_move_reservation(&observed) {}
        moving_value(const moving_value&) = delete;
        moving_value(moving_value&& other) noexcept
            : value(other.value), minimum_move_reservation(other.minimum_move_reservation) {
            auto& controller = bioseq::parallelism::memory_admission_controller::instance();
            *minimum_move_reservation = std::min(*minimum_move_reservation, controller.reserved_bytes());
        }
    };
    {
        bioseq::parallelism::memory_permit permit;
        std::vector<moving_value> values;
        std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
        bioseq::io::io_memory_credit credit;
        auto spare = admission.try_reserve(64u);
        REQUIRE(spare.has_value());
        credit.reset(std::move(*spare));
        REQUIRE(bioseq::io::reserve_io_vector_capacity(values, permit, 1024u, &credit).has_value());
        REQUIRE(admission.reserved_bytes() == baseline + permit.bytes());
        const auto old_capacity = values.capacity();
        for (std::size_t index = 0; index < old_capacity; ++index) values.emplace_back(index, minimum);
        const auto owned_before = admission.reserved_bytes();
        admission.set_index_memory_cap_bytes(owned_before + sizeof(moving_value));
        const auto rejected = bioseq::io::reserve_io_vector_capacity(values, permit, old_capacity + 1u, &credit);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(values.capacity() == old_capacity);
        REQUIRE(values.size() == old_capacity);
        REQUIRE(admission.reserved_bytes() == owned_before);
        REQUIRE(minimum == std::numeric_limits<std::uint64_t>::max());

        admission.set_index_memory_cap_bytes(baseline + 16u * 1024u * 1024u);
        REQUIRE(bioseq::io::reserve_io_vector_capacity(values, permit, old_capacity + 1u).has_value());
        REQUIRE(minimum >= baseline + (old_capacity + values.capacity()) * sizeof(moving_value));
        REQUIRE(permit.bytes() == values.capacity() * sizeof(moving_value));
        for (std::size_t index = 0; index < values.size(); ++index) REQUIRE(values[index].value == index);
    }
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("SAM many tiny auxiliaries retain ordered values through native and restored vectors",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    constexpr std::string_view first_letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    constexpr std::string_view second_letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::vector<std::array<char, 2>> tags;
    for (const char first : first_letters) {
        for (const char second : second_letters) {
            const std::array<char, 2> tag{first, second};
            if (tag == std::array<char, 2>{'C', 'G'} || tag == std::array<char, 2>{'b', 'q'}
                || tag == std::array<char, 2>{'R', 'G'} || tag == std::array<char, 2>{'P', 'G'}
                || tag == std::array<char, 2>{'N', 'M'} || tag == std::array<char, 2>{'M', 'D'}) continue;
            tags.push_back(tag);
        }
    }
    struct tag_consumer {
        std::span<const std::array<char, 2>> expected;
        std::size_t callbacks{};
        std::expected<void, std::string> header(const bioseq::io::record_source_metadata&) { return {}; }
        /** Check every value while both native and canonical owners are still alive. */
        std::expected<void, std::string> record(bioseq::io::decoded_record& decoded) {
            ++callbacks;
            const auto& row = decoded.record;
            if (!row.alignment || row.alignment->auxiliary.size() != expected.size())
                return std::unexpected("auxiliary count changed");
            for (std::size_t index = 0; index < expected.size(); ++index) {
                const auto& field = row.alignment->auxiliary[index];
                if (field.tag != expected[index] || field.type != 'Z' || !field.payload.empty())
                    return std::unexpected("auxiliary value/order changed");
            }
            return {};
        }
    };
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto baseline = admission.reserved_bytes();
    for (const auto count : {std::size_t{1024u}, std::size_t{1025u}, std::size_t{2049u}, tags.size()}) {
        CAPTURE(count);
        std::string sam{"@HD\tVN:1.6\nq\t4\t*\t0\t0\t*\t*\t0\t0\tA\t!"};
        for (std::size_t index = 0; index < count; ++index) {
            sam.push_back('\t');
            sam.append(tags[index].data(), tags[index].size());
            sam += ":Z:";
        }
        sam.push_back('\n');
        const auto path = fixture.directory / ("many_tags_" + std::to_string(count) + ".sam");
        publication_fixture::seed(path, sam);
        tag_consumer consumer{std::span{tags}.first(count)};
        const auto read = bioseq::io::read_alignment_records(path.string(), bioseq::io::sequence_file_format::sam, consumer);
        REQUIRE(read.has_value());
        REQUIRE(*read == 1u);
        REQUIRE(consumer.callbacks == 1u);
        REQUIRE(admission.reserved_bytes() == baseline);
        {
            bioseq::io::decoded_record preserved;
            REQUIRE(preserved.field("flag", std::string_view{"4"}).has_value());
            for (std::size_t index = 0; index < count; ++index) {
                for (const bool projected : {false, true}) {
                    REQUIRE(preserved.begin_auxiliary(projected, tags[index], 'Z', '\0').has_value());
                    REQUIRE(preserved.end_auxiliary(1u).has_value());
                }
            }
            const auto frozen = preserved.freeze();
            REQUIRE(frozen.has_value());
            REQUIRE(frozen->alignment.has_value());
            REQUIRE(frozen->alignment->auxiliary.size() == count);
            REQUIRE(preserved.projected_auxiliary().size() == count);
            for (std::size_t index = 0; index < count; ++index) {
                REQUIRE(frozen->alignment->auxiliary[index].tag == tags[index]);
                REQUIRE(preserved.projected_auxiliary()[index].tag == tags[index]);
                REQUIRE(preserved.projected_auxiliary()[index].payload.empty());
            }
        }
        REQUIRE(admission.reserved_bytes() == baseline);
    }
}

namespace {

/** Keep worker-count variants independent of an earlier first-use serial arena. */
struct fixture_dispatch_pool {
    bioseq::parallelism::global_pool& pool{bioseq::parallelism::global_pool::instance()};
    const bool was_active{pool.is_active()};
    const bioseq::parallelism::pool_config previous{pool.maximum_concurrency(), pool.get_mode()};
    fixture_dispatch_pool() {
        pool.configure({4u, bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel});
    }
    ~fixture_dispatch_pool() { if (was_active) pool.configure(previous); else pool.shutdown(); }
};

/** Build an independent tiny gzip/BGZF member with zlib, checked framing and CRC. */
[[nodiscard]] std::string fixture_gzip_member(std::string_view payload, bool bgzf) {
    REQUIRE(payload.size() <= 32768u);
    z_stream stream{};
    REQUIRE(::deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::vector<std::uint8_t> compressed(::compressBound(static_cast<uLong>(payload.size())));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(payload.data()));
    stream.avail_in = static_cast<uInt>(payload.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());
    const auto status = ::deflate(&stream, Z_FINISH);
    const auto output_size = static_cast<std::size_t>(stream.total_out);
    REQUIRE(::deflateEnd(&stream) == Z_OK);
    REQUIRE(status == Z_STREAM_END);
    std::string bytes{"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10};
    if (bgzf) {
        bytes[3] = 4;
        bytes.append("\x06\x00\x42\x43\x02\x00\x00\x00", 8);
    }
    bytes.append(reinterpret_cast<const char*>(compressed.data()), output_size);
    const auto append_u32 = [&bytes](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
    };
    append_u32(static_cast<std::uint32_t>(::crc32_z(0,
        reinterpret_cast<const Bytef*>(payload.data()), payload.size())));
    append_u32(static_cast<std::uint32_t>(payload.size()));
    if (bgzf) {
        REQUIRE(bytes.size() <= 65536u);
        const auto size = bytes.size() - 1u;
        bytes[16] = static_cast<char>(size & 0xffu);
        bytes[17] = static_cast<char>(size >> 8u);
    }
    return bytes;
}

/** Independently decompress every member of a small writer output through zlib. */
[[nodiscard]] std::string fixture_gunzip(const std::filesystem::path& path) {
    gzFile input = ::gzopen(path.c_str(), "rb");
    REQUIRE(input != nullptr);
    std::array<char, 4096> bytes{};
    std::string result;
    int read{};
    while ((read = ::gzread(input, bytes.data(), static_cast<unsigned>(bytes.size()))) > 0)
        result.append(bytes.data(), static_cast<std::size_t>(read));
    REQUIRE(read == 0);
    REQUIRE(::gzclose(input) == Z_OK);
    return result;
}

/** Frame small raw BAM bytes into independent BGZF members and the standard EOF. */
[[nodiscard]] std::string fixture_bgzf(std::string_view raw) {
    std::string bytes;
    for (std::size_t offset = 0; offset < raw.size();) {
        const auto count = std::min<std::size_t>(32768u, raw.size() - offset);
        bytes += fixture_gzip_member(raw.substr(offset, count), true);
        offset += count;
    }
    bytes += fixture_gzip_member({}, true);
    return bytes;
}

/** Locate SEQ/QUAL using independently checked BAM lengths, without the reader codec. */
struct fixture_bam_sequence { std::size_t offset{}, symbols{}, quality_offset{}, core_offset{}, record_end{}; };
[[nodiscard]] std::vector<fixture_bam_sequence> fixture_bam_sequences(std::string_view bytes) {
    REQUIRE(bytes.starts_with(std::string_view{"BAM\1", 4}));
    const auto u32 = [bytes](std::size_t offset) {
        REQUIRE(offset <= bytes.size()); REQUIRE(bytes.size() - offset >= 4u);
        std::uint32_t result{};
        for (unsigned index = 0; index < 4; ++index)
            result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8u);
        return result;
    };
    std::size_t cursor = 8u + u32(4);
    const auto references = u32(cursor); cursor += 4u;
    for (std::size_t index = 0; index < references; ++index) {
        const auto name_size = u32(cursor);
        REQUIRE(name_size <= bytes.size() - cursor - 4u);
        cursor += 8u + name_size;
    }
    std::vector<fixture_bam_sequence> fields;
    while (cursor < bytes.size()) {
        const auto block_size = u32(cursor);
        REQUIRE(block_size >= 32u); REQUIRE(block_size <= bytes.size() - cursor - 4u);
        const auto core = cursor + 4u;
        const auto name_size = static_cast<unsigned char>(bytes[core + 8u]);
        const auto cigar_count = static_cast<std::size_t>(u32(core + 12u) & 0xffffu);
        const auto symbols = static_cast<std::size_t>(u32(core + 16u));
        const auto sequence_offset = core + 32u + name_size + cigar_count * 4u;
        const auto quality_offset = sequence_offset + symbols / 2u + symbols % 2u;
        REQUIRE(quality_offset <= core + block_size);
        REQUIRE(symbols <= core + block_size - quality_offset);
        fields.push_back({sequence_offset, symbols, quality_offset, core, core + block_size});
        cursor = core + block_size;
    }
    REQUIRE(cursor == bytes.size());
    return fields;
}

/** Stable test-owned ranks; all formats borrow the same alphabet-erased reader. */
struct fixture_rank_owner {
    std::vector<std::uint8_t> ranks;
    [[nodiscard]] static const std::uint8_t* read(const void* context,
        bioseq::storage::sequence_size_t offset, std::size_t count, std::span<std::uint8_t> scratch) {
        const auto& owner = *static_cast<const fixture_rank_owner*>(context);
        if (offset > owner.ranks.size() || count > owner.ranks.size() - offset || count > scratch.size()) return nullptr;
        return owner.ranks.data() + offset;
    }
    [[nodiscard]] bioseq::storage::borrowed_rank_window_source borrow(std::uint8_t alphabet) const & {
        return {this, &read, ranks.size(), alphabet, bioseq::storage::sequence_transform::identity, true};
    }
};

} // namespace

TEST_CASE("Canonical comparison ignores only the final unused BAM nibble",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    for (const std::size_t size : {1u, 2u, 513u}) {
        std::vector<std::uint8_t> original(size, 0x11u);
        original.back() = 0x10u;
        for (std::uint8_t padding = 0; padding < 16u; ++padding) {
            auto expected = original;
            expected.back() |= padding;
            for (const std::size_t chunk : {1u, 2u, 512u}) {
                codec::native_projection_comparison_sink sink{expected, 0xf0u};
                REQUIRE(sink.append({}).has_value());
                for (std::size_t offset = 0; offset < size;) {
                    const auto count = std::min(chunk, size - offset);
                    REQUIRE(sink.append(std::span<const std::uint8_t>{original}.subspan(offset, count)).has_value());
                    offset += count;
                }
                REQUIRE(sink.append({}).has_value()); REQUIRE(sink.finish().has_value());
            }
        }
        auto changed = original;
        changed.back() ^= 0x10u;
        codec::native_projection_comparison_sink high_nibble{original, 0xf0u};
        REQUIRE_FALSE(high_nibble.append(changed).has_value());
        changed = original; changed.back() ^= 1u;
        codec::native_projection_comparison_sink even_or_quality{original};
        REQUIRE_FALSE(even_or_quality.append(changed).has_value());
        if (size > 1) {
            changed = original; changed.front() ^= 1u;
            codec::native_projection_comparison_sink earlier_byte{original, 0xf0u};
            REQUIRE_FALSE(earlier_byte.append(changed).has_value());
        }
    }
    codec::native_projection_comparison_sink empty{{}, 0xf0u};
    REQUIRE(empty.append({}).has_value()); REQUIRE(empty.finish().has_value());
    constexpr std::array<std::uint8_t, 1> unexpected{0};
    REQUIRE_FALSE(empty.append(unexpected).has_value());
}

TEST_CASE("Canonical BAM odd SEQ padding survives restoration and rewrites as zero",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    fixture_dispatch_pool scheduler;
    constexpr auto dna = ::supported_storage_alphabets::index_of<seq::dna4>();
    constexpr auto protein = ::supported_storage_alphabets::index_of<seqan3::aa20>();
    constexpr auto quality = ::supported_storage_alphabets::index_of<seqan3::phred42>();
    bioseq::io::writer_options writing;
    writing.format = sequence_file_format::bam;
    writing.overwrite = true;
    writing.chunk_capacity_bytes = 4096u;
    bioseq::io::concurrency_options serial; serial.worker_count = 1u;
    struct consumer_type {
        std::uint8_t alphabet{};
        std::size_t length{}, headers{};
        std::vector<std::vector<std::uint8_t>> sequences, qualities;
        std::expected<void, std::string> header(const bioseq::io::record_source_metadata&) { ++headers; return {}; }
        std::expected<void, std::string> record(bioseq::io::decoded_record& decoded) {
            const auto& row = decoded.record;
            REQUIRE(row.sequence.has_value()); REQUIRE(row.quality.has_value());
            REQUIRE(row.sequence->alphabet_code == alphabet);
            REQUIRE(row.sequence->symbol_count == length); REQUIRE(row.quality->symbol_count == length);
            std::vector<std::uint8_t> scratch(length);
            auto sequence = row.sequence->read_checked(0, length, scratch); REQUIRE(sequence.has_value());
            sequences.emplace_back(sequence->begin(), sequence->end());
            auto scores = row.quality->read_checked(0, length, scratch); REQUIRE(scores.has_value());
            qualities.emplace_back(scores->begin(), scores->end());
            return {};
        }
    };
    for (const auto alphabet : {dna, protein}) {
        for (const std::size_t length : {1u, 3u, 1025u}) {
            if (alphabet == protein && length == 1025u) continue;
            for (const bool reverse : {false, true}) {
                CAPTURE(alphabet, length, reverse);
                fixture_rank_owner sequence{std::vector<std::uint8_t>(length, 0u)};
                fixture_rank_owner scores{std::vector<std::uint8_t>(length, 13u)};
                const auto bind = [&sequence, &scores, alphabet, reverse, quality](pressure_retry_source& source) {
                    source.source_metadata.declared_alphabet_codes = {alphabet};
                    source.source_metadata.declared_quality_alphabet_codes = {quality};
                    for (auto& row : source.records) {
                        row.sequence = sequence.borrow(alphabet); row.quality = scores.borrow(quality);
                        row.alignment->flags = static_cast<std::uint16_t>(reverse ? 20u : 4u);
                    }
                };
                const auto original_path = fixture.directory / "odd-original.bam";
                pressure_retry_source original{pressure_retry_source::pressure_policy::none}; bind(original);
                const auto written = bioseq::io::write_records(original, original_path.string(), writing, serial);
                REQUIRE(written.has_value()); REQUIRE(*written == 2u);
                const auto raw = fixture_gunzip(original_path);
                const auto fields = fixture_bam_sequences(raw); REQUIRE(fields.size() == 2u);
                for (const auto field : fields) {
                    REQUIRE(field.symbols == length * (alphabet == protein ? 3u : 1u));
                    REQUIRE((static_cast<unsigned char>(raw[field.offset + field.symbols / 2u]) & 15u) == 0u);
                }
                const auto altered_path = fixture.directory / "odd-altered.bam";
                for (std::uint8_t padding = 0; padding < 16u; ++padding) {
                    auto altered = raw;
                    for (const auto field : fields)
                        altered[field.offset + field.symbols / 2u] = static_cast<char>(
                            static_cast<unsigned char>(altered[field.offset + field.symbols / 2u]) | padding);
                    publication_fixture::seed(altered_path, fixture_bgzf(altered));
                    for (const std::size_t workers : {1u, 4u}) {
                        auto options = bioseq::io::default_reader_options;
                        options.concurrency.worker_count = workers;
                        consumer_type consumer{alphabet, length};
                        const auto read = bioseq::io::read_alignment_records(altered_path.string(),
                            sequence_file_format::bam, consumer, options);
                        REQUIRE(read.has_value()); REQUIRE(*read == 2u); REQUIRE(consumer.headers == 1u);
                        REQUIRE(consumer.sequences.size() == 2u); REQUIRE(consumer.qualities.size() == 2u);
                        for (std::size_t index = 0; index < 2; ++index) {
                            REQUIRE(consumer.sequences[index] == sequence.ranks);
                            REQUIRE(consumer.qualities[index] == scores.ranks);
                        }
                        if (padding == 15u && workers == 1u) {
                            sequence.ranks = consumer.sequences.front(); scores.ranks = consumer.qualities.front();
                            pressure_retry_source restored{pressure_retry_source::pressure_policy::none}; bind(restored);
                            const auto restored_path = fixture.directory / "odd-restored.bam";
                            REQUIRE(bioseq::io::write_records(restored, restored_path.string(), writing, serial).has_value());
                            const auto rewritten = fixture_gunzip(restored_path);
                            for (const auto field : fixture_bam_sequences(rewritten))
                                REQUIRE((static_cast<unsigned char>(rewritten[field.offset + field.symbols / 2u]) & 15u) == 0u);
                        }
                    }
                }
                // Independent preserved protein ranks, or an unrepresentable DNA4 N, make a real mismatch.
                auto corrupted = raw;
                corrupted[fields.front().offset] = static_cast<char>(alphabet == protein
                    ? static_cast<unsigned char>(corrupted[fields.front().offset]) ^ 0x10u
                    : (static_cast<unsigned char>(corrupted[fields.front().offset]) & 15u) | 0xf0u);
                publication_fixture::seed(altered_path, fixture_bgzf(corrupted));
                consumer_type rejected{alphabet, length};
                REQUIRE_FALSE(bioseq::io::read_alignment_records(altered_path.string(), sequence_file_format::bam, rejected).has_value());
                if (alphabet == protein) {
                    corrupted = raw; ++corrupted[fields.front().quality_offset];
                    publication_fixture::seed(altered_path, fixture_bgzf(corrupted));
                    consumer_type unequal_triplet{alphabet, length};
                    REQUIRE_FALSE(bioseq::io::read_alignment_records(altered_path.string(), sequence_file_format::bam, unequal_triplet).has_value());
                }
            }
        }
    }
}

TEST_CASE("Placed unmapped geometry preserves native fields without mapped reference bounds",
          "[component:bioseq][layer:unit][feature:io]") {
    namespace codec = bioseq::io::alignment;
    codec::sam_cigar_source cigar{"2M"};
    codec::native_reference_binding references;
    references.reference_id = 0; references.reference_length = 1;
    references.mate_reference_id = 0; references.mate_reference_length = 1;
    references.mate_same_projection_domain = true;
    bioseq::io::alignment_record_view alignment;
    alignment.cigar = cigar.borrow(); alignment.target_start = 0; alignment.target_end = 2;
    alignment.mate_start = 7;
    for (const bool protein : {false, true}) {
        for (const std::uint16_t flags : {std::uint16_t{4}, std::uint16_t{13}}) {
            const auto plan = codec::prepare_native_alignment(alignment, flags, 2, references, protein);
            REQUIRE(plan.has_value()); REQUIRE(plan->core.position == 0);
            REQUIRE(plan->cigar_size.geometry.reference_span == (protein ? 6u : 2u));
            REQUIRE(plan->core.mate_position == (protein ? 21 : 7));
            REQUIRE(plan->core.bin == codec::bam_region_bin(0, 1));
            alignment.target_start = 9; alignment.target_end = 11;
            REQUIRE(codec::prepare_native_alignment(alignment, flags, 2, references, protein).has_value());
            alignment.target_start = 0; alignment.target_end = 2;
        }
        REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 0, 2, references, protein).has_value());
        REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 5, 2, references, protein).has_value());
        auto circular = references; circular.reference_is_circular = true;
        REQUIRE(codec::prepare_native_alignment(alignment, 0, 2, circular, protein).has_value());
        REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 4, 3, references, protein).has_value());
        alignment.target_start = std::numeric_limits<std::uint32_t>::max(); alignment.target_end.reset();
        REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 4, 2, references, protein).has_value());
        alignment.target_start = 0; alignment.target_end = 2;
    }
}

TEST_CASE("Writer validates RG and PG before publication for native and protein rows",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    fixture_dispatch_pool scheduler;
    constexpr auto dna = ::supported_storage_alphabets::index_of<seq::dna4>();
    constexpr auto protein = ::supported_storage_alphabets::index_of<seqan3::aa20>();
    for (const auto format : {sequence_file_format::sam, sequence_file_format::bam})
        for (const auto alphabet : {dna, protein})
            for (const std::size_t workers : {1u, 4u})
                for (const auto tag : {std::array<char, 2>{'R', 'G'}, std::array<char, 2>{'P', 'G'}})
                    for (const bool existing : {false, true})
                        for (const int scenario : {0, 1, 2}) {
                            CAPTURE(format, alphabet, workers, tag, existing, scenario);
                            const std::string declared{"declared"}, missing{"missing"};
                            const auto payload = scenario == 1 ? std::string_view{missing}
                                : scenario == 2 ? std::string_view{declared}.substr(0, 1) : std::string_view{declared};
                            const bioseq::io::auxiliary_field_view auxiliary{tag, scenario == 2 ? 'A' : 'Z', 0,
                                std::as_bytes(std::span{payload.data(), payload.size()})};
                            pressure_retry_source source{pressure_retry_source::pressure_policy::none};
                            source.source_metadata.declared_alphabet_codes = {alphabet};
                            for (auto& row : source.records) row.sequence->alphabet_code = alphabet;
                            source.records.back().alignment->auxiliary = std::span{&auxiliary, 1};
                            bioseq::io::writer_options options;
                            options.format = format; options.overwrite = existing; options.chunk_capacity_bytes = 4096;
                            options.sam_header = {"@RG\tID:declared", "@PG\tID:declared\tPN:fixture"};
                            bioseq::io::concurrency_options concurrency; concurrency.worker_count = workers;
                            const auto path = fixture.directory / (format == sequence_file_format::bam ? "links.bam" : "links.sam");
                            std::filesystem::remove(path);
                            if (existing) publication_fixture::seed(path, "prior complete file\n");
                            const auto written = bioseq::io::write_records(source, path.string(), options, concurrency);
                            if (scenario == 0) {
                                REQUIRE(written.has_value()); REQUIRE(*written == 2u);
                            } else {
                                REQUIRE_FALSE(written.has_value());
                                REQUIRE(written.error().message.find("RG/PG") != std::string::npos);
                                REQUIRE(std::filesystem::exists(path) == existing);
                                if (existing) REQUIRE(publication_fixture::read(path) == "prior complete file\n");
                            }
                            REQUIRE_FALSE(fixture.has_private_staging());
                        }
}

TEST_CASE("SAM gzip dispatch accepts mixed members exactly once and keeps BAM strict",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    fixture_dispatch_pool scheduler;
    const std::string first{"@HD\tVN:1.6\nfirst\t4\t*\t0\t0\t*\t*\t0\t0\tAC\t!!\n"};
    const std::string second{"second\t4\t*\t0\t0\t*\t*\t0\t0\tGT\t##\n"};
    struct consumer_type {
        std::size_t headers{};
        std::vector<std::string> names;
        std::expected<void, std::string> header(const bioseq::io::record_source_metadata&) { ++headers; return {}; }
        std::expected<void, std::string> record(bioseq::io::decoded_record& row) {
            REQUIRE(row.record.qname.has_value()); names.emplace_back(*row.record.qname); return {};
        }
    };
    const auto path = fixture.directory / "members.sam.gz";
    for (const bool first_bgzf : {false, true})
        for (const bool second_bgzf : {false, true})
            for (const bool mapped : {false, true})
                for (const std::size_t workers : {1u, 4u}) {
                    CAPTURE(first_bgzf, second_bgzf, mapped, workers);
                    const auto compressed = fixture_gzip_member(first, first_bgzf)
                        + fixture_gzip_member({}, first_bgzf) + fixture_gzip_member(second, second_bgzf);
                    publication_fixture::seed(path, compressed);
                    auto options = bioseq::io::default_reader_options;
                    options.io.max_mmap_bytes = mapped ? 1u << 20u : 0u;
                    options.io.parallel_gzip_enabled = workers > 1;
                    options.io.parallel_gzip_min_compressed_bytes = 1;
                    options.io.parallel_gzip_thread_count = 2;
                    options.io.parallel_gzip_chunk_bytes = 32768;
                    options.concurrency.worker_count = workers;
                    consumer_type consumer;
                    const auto read = bioseq::io::read_alignment_records(path.string(), sequence_file_format::sam, consumer, options);
                    REQUIRE(read.has_value()); REQUIRE(*read == 2u); REQUIRE(consumer.headers == 1u);
                    REQUIRE((consumer.names == std::vector<std::string>{"first", "second"}));
                    auto truncated = compressed; truncated.pop_back(); publication_fixture::seed(path, truncated);
                    consumer_type incomplete;
                    REQUIRE_FALSE(bioseq::io::read_alignment_records(path.string(), sequence_file_format::sam, incomplete, options).has_value());
                    auto corrupt = compressed; corrupt[corrupt.size() - 8u] ^= 1; publication_fixture::seed(path, corrupt);
                    consumer_type bad_crc;
                    REQUIRE_FALSE(bioseq::io::read_alignment_records(path.string(), sequence_file_format::sam, bad_crc, options).has_value());
                }
    // A proper BAM header followed by an ordinary gzip member must not take the SAM fallback.
    const std::string empty_bam{"BAM\1\0\0\0\0\0\0\0\0", 12};
    publication_fixture::seed(path, fixture_gzip_member(empty_bam, true) + fixture_gzip_member({}, false));
    for (const bool mapped : {false, true}) {
        auto options = bioseq::io::default_reader_options; options.io.max_mmap_bytes = mapped ? 1u << 20u : 0u;
        consumer_type consumer;
        REQUIRE_FALSE(bioseq::io::read_alignment_records(path.string(), sequence_file_format::bam, consumer, options).has_value());
    }
}

TEST_CASE("Reader slab refused growth preserves its live bytes and positions",
          "[component:bioseq][layer:unit][feature:io]") {
    struct allocation_state {
        bool refuse{};
        std::size_t calls{}, releases{};
        static void* allocate(void* context, std::size_t bytes) noexcept {
            auto& state = *static_cast<allocation_state*>(context);
            ++state.calls;
            return state.refuse ? nullptr : std::malloc(bytes);
        }
        static void release(void* context, void* memory) noexcept {
            ++static_cast<allocation_state*>(context)->releases;
            std::free(memory);
        }
    };
    SECTION("Configured capacity refusal preserves the original owner") {
        bioseq::io::buffer_slab slab{16}; REQUIRE(slab.raw() != nullptr);
        for (std::size_t index = 0; index < 16; ++index) slab.raw()[index] = static_cast<std::uint8_t>(index);
        slab.fill_size = 16; slab.parse_cursor = 3; slab.is_eof = true;
        const auto* previous = slab.raw();
        auto options = bioseq::io::default_reader_options; options.io.max_buffer_size = 16;
        REQUIRE_FALSE(slab.grow(options)); REQUIRE(slab.raw() == previous); REQUIRE(slab.capacity == 16);
        REQUIRE(slab.fill_size == 16); REQUIRE(slab.parse_cursor == 3); REQUIRE(slab.is_eof);
        for (std::size_t index = 0; index < 16; ++index) REQUIRE(slab.raw()[index] == index);
    }
    SECTION("A true allocator null preserves valid live storage and permits retry") {
        // Both callback contexts outlive every successful allocation they own.
        allocation_state original, replacement{.refuse = true};
        {
            bioseq::io::buffer_slab slab{16, &allocation_state::allocate, &allocation_state::release, &original};
            REQUIRE(slab.raw() != nullptr); REQUIRE(original.calls == 1u);
            for (std::size_t index = 0; index < 16; ++index) slab.raw()[index] = static_cast<std::uint8_t>(index);
            slab.fill_size = 16; slab.parse_cursor = 3; slab.is_eof = true;
            const auto* previous = slab.raw();
            auto options = bioseq::io::default_reader_options; options.io.max_buffer_size = 64;
            REQUIRE_FALSE(slab.grow(options, &allocation_state::allocate, &allocation_state::release, &replacement));
            REQUIRE(replacement.calls == 1u); REQUIRE(replacement.releases == 0u); REQUIRE(original.releases == 0u);
            REQUIRE(slab.raw() == previous); REQUIRE(slab.capacity == 16u);
            REQUIRE(slab.fill_size == 16u); REQUIRE(slab.parse_cursor == 3u); REQUIRE(slab.is_eof);
            for (std::size_t index = 0; index < 16; ++index) REQUIRE(slab.raw()[index] == index);
            replacement.refuse = false;
            REQUIRE(slab.grow(options, &allocation_state::allocate, &allocation_state::release, &replacement));
            REQUIRE(replacement.calls == 2u); REQUIRE(original.releases == 1u); REQUIRE(replacement.releases == 0u);
            REQUIRE(slab.raw() != nullptr); REQUIRE(slab.capacity == 32u);
            REQUIRE(slab.fill_size == 16u); REQUIRE(slab.parse_cursor == 3u); REQUIRE(slab.is_eof);
            for (std::size_t index = 0; index < 16; ++index) REQUIRE(slab.raw()[index] == index);
        }
        REQUIRE(original.releases == 1u); REQUIRE(replacement.releases == 1u);
    }
    SECTION("A true initial allocator null never advertises unavailable capacity") {
        allocation_state allocator{.refuse = true};
        auto options = bioseq::io::default_reader_options;
        options.io.initial_buffer_size = 16; options.io.max_buffer_size = 64;
        {
            bioseq::io::buffer_slab slab{options, &allocation_state::allocate, &allocation_state::release, &allocator};
            REQUIRE(allocator.calls == 1u); REQUIRE(slab.raw() == nullptr); REQUIRE(slab.capacity == 0u);
            REQUIRE(slab.fill_size == 0u); REQUIRE(slab.parse_cursor == 0u); REQUIRE_FALSE(slab.is_eof);
        }
        REQUIRE(allocator.releases == 0u);
    }
    SECTION("Zero capacity avoids Highway assertions and grows with positive progress") {
        allocation_state allocator;
        {
            bioseq::io::buffer_slab slab{0, &allocation_state::allocate, &allocation_state::release, &allocator};
            REQUIRE(slab.raw() == nullptr); REQUIRE(slab.capacity == 0u); REQUIRE(allocator.calls == 0u);
            auto options = bioseq::io::default_reader_options; options.io.max_buffer_size = 0;
            REQUIRE_FALSE(slab.grow(options, &allocation_state::allocate, &allocation_state::release, &allocator));
            REQUIRE(allocator.calls == 0u);
            options.io.max_buffer_size = 1;
            REQUIRE(slab.grow(options, &allocation_state::allocate, &allocation_state::release, &allocator));
            REQUIRE(slab.capacity == 1u); REQUIRE(slab.raw() != nullptr); REQUIRE(allocator.calls == 1u);
            REQUIRE(slab.fill_size == 0u); REQUIRE(slab.parse_cursor == 0u); REQUIRE_FALSE(slab.is_eof);
            REQUIRE_FALSE(slab.grow(options, &allocation_state::allocate, &allocation_state::release, &allocator));
            REQUIRE(allocator.calls == 1u);
        }
        REQUIRE(allocator.releases == 1u);
    }
    SECTION("Unsupported Highway sizes are rejected before the allocator") {
        for (const auto bytes : {std::numeric_limits<std::size_t>::max() / 2u,
                                 std::numeric_limits<std::size_t>::max()}) {
            allocation_state allocator;
            bioseq::io::buffer_slab slab{bytes, &allocation_state::allocate, &allocation_state::release, &allocator};
            REQUIRE(slab.raw() == nullptr); REQUIRE(slab.capacity == 0u);
            REQUIRE(allocator.calls == 0u); REQUIRE(allocator.releases == 0u);
        }
    }
    SECTION("Reader options clamp initial storage to a nonzero configured maximum") {
        for (const auto initial : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
            allocation_state allocator;
            auto options = bioseq::io::default_reader_options;
            options.io.initial_buffer_size = initial; options.io.max_buffer_size = 64;
            {
                bioseq::io::buffer_slab slab{options, &allocation_state::allocate, &allocation_state::release, &allocator};
                REQUIRE(slab.raw() != nullptr); REQUIRE(slab.capacity == (initial == 0u ? 1u : 64u));
                REQUIRE(allocator.calls == 1u);
            }
            REQUIRE(allocator.releases == 1u);
        }
    }
}

TEST_CASE("Placed unmapped records pass native SAM loading and SAM BAM publication",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    publication_fixture fixture;
    fixture_dispatch_pool scheduler;
    struct consumer_type {
        std::size_t count{};
        std::expected<void, std::string> header(const bioseq::io::record_source_metadata& metadata) {
            REQUIRE(metadata.references.size() == 1u); REQUIRE(metadata.references.front().length == 1u);
            return {};
        }
        std::expected<void, std::string> record(bioseq::io::decoded_record& decoded) {
            const auto& row = decoded.record;
            REQUIRE(row.alignment.has_value()); REQUIRE(row.alignment->flags == 4u);
            REQUIRE(row.alignment->target_start == 0u); REQUIRE(row.alignment->cigar.has_value());
            REQUIRE(row.sequence.has_value()); REQUIRE(row.sequence->symbol_count == 4u);
            ++count; return {};
        }
    };
    const auto native = fixture.directory / "native-placed.sam";
    publication_fixture::seed(native, "@SQ\tSN:r\tLN:1\nq\t4\tr\t1\t0\t4M\t*\t0\t0\tACGT\t*\n");
    consumer_type native_consumer;
    const auto loaded = bioseq::io::read_alignment_records(native.string(), sequence_file_format::sam, native_consumer);
    REQUIRE(loaded.has_value()); REQUIRE(*loaded == 1u); REQUIRE(native_consumer.count == 1u);
    bioseq::io::alignment::sam_cigar_source cigar{"4M"};
    for (const auto format : {sequence_file_format::sam, sequence_file_format::bam}) {
        pressure_retry_source source{pressure_retry_source::pressure_policy::none};
        source.source_metadata.references = {{1, "r", 1, pressure_retry_source::alphabet}};
        for (auto& row : source.records) {
            row.alignment->reference_name = "r"; row.alignment->target_start = 0; row.alignment->target_end = 4;
            row.alignment->cigar = cigar.borrow();
        }
        bioseq::io::writer_options options; options.format = format; options.chunk_capacity_bytes = 4096;
        bioseq::io::concurrency_options concurrency; concurrency.worker_count = 4;
        const auto path = fixture.directory / (format == sequence_file_format::sam ? "placed.sam" : "placed.bam");
        const auto written = bioseq::io::write_records(source, path.string(), options, concurrency);
        REQUIRE(written.has_value()); REQUIRE(*written == 2u);
        consumer_type restored;
        const auto read = bioseq::io::read_alignment_records(path.string(), format, restored);
        REQUIRE(read.has_value()); REQUIRE(*read == 2u); REQUIRE(restored.count == 2u);
    }
}

namespace {

/** The neutral ABI reports facts from the separately compiled HWY_SCALAR owner. */
[[nodiscard]] bioseq::highway::codec_scalar::codec_binding scalar_codec_binding() {
    const auto binding = bioseq::highway::codec_scalar::codec_kernels();
    REQUIRE(binding.target == static_cast<std::int64_t>(HWY_SCALAR));
    REQUIRE(binding.byte_lanes == 1u); REQUIRE(binding.rank_lanes == 1u);
    REQUIRE(binding.transform != nullptr);
    REQUIRE(binding.crc_implementation == bioseq::highway::codec_scalar::crc_path::existing_zlib_fallback);
    return binding;
}

/** Small independent output oracle; no production sink ownership policy is replaced. */
struct scalar_test_bytes {
    std::vector<std::uint8_t> bytes;
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> input) {
        bytes.insert(bytes.end(), input.begin(), input.end()); return {};
    }
    [[nodiscard]] std::expected<void, std::string> append(std::string_view input) {
        return append({reinterpret_cast<const std::uint8_t*>(input.data()), input.size()});
    }
};

constexpr std::array<std::size_t, 16> scalar_boundary_lengths{
    0u, 1u, 2u, 3u, 15u, 16u, 17u, 31u, 32u, 33u, 127u, 128u, 129u, 1023u, 1024u, 1025u};

/** Independent native spelling uses only alphabet/codon policy, not codec transforms. */
template<sequence_alphabet Alphabet>
[[nodiscard]] std::string scalar_expected_letters(std::span<const std::uint8_t> ranks, bool reverse) {
    std::string expected;
    for (const auto rank : ranks) {
        if constexpr (seqan3::aminoacid_alphabet<Alphabet>) {
            const auto codon = bioseq::sequences::select_representative_codon<Alphabet, seqan3::dna5>(rank,
                bioseq::sequences::missing_codon_policy::preserve_with_ambiguous_triplet);
            REQUIRE(codon.has_value());
            for (const auto base : *codon) expected += seqan3::to_char(seqan3::dna5{}.assign_rank(base));
        } else expected += seqan3::to_char(Alphabet{}.assign_rank(rank));
    }
    if (reverse) {
        std::ranges::reverse(expected);
        for (auto& letter : expected) letter = seqan3::to_char(seqan3::complement(seqan3::dna15{}.assign_char(letter)));
    }
    return expected;
}

/** SAM nt16 table is format-defined; compare packed bytes independently of codec loops. */
[[nodiscard]] std::vector<std::uint8_t> scalar_expected_packed(std::string_view letters) {
    constexpr std::string_view nt16{"=ACMGRSVTWYHKDBN"};
    std::vector<std::uint8_t> packed((letters.size() + 1u) / 2u, 0u);
    for (std::size_t index{}; index < letters.size(); ++index) {
        const auto rank = nt16.find(letters[index]); REQUIRE(rank != std::string_view::npos);
        packed[index / 2u] |= static_cast<std::uint8_t>(rank << (index % 2u == 0u ? 4u : 0u));
    }
    return packed;
}

/** Instantiate the existing biological codec with the scalar function pointer, never its native fast path. */
template<sequence_alphabet Alphabet, bool Reverse, bool Packed>
void check_scalar_biological(const fixture_rank_owner& owner,
        const bioseq::highway::codec_scalar::codec_binding& binding) {
    namespace codec = bioseq::io::alignment;
    constexpr auto code = supported_storage_alphabets::index_of<Alphabet>();
    const auto source = owner.borrow(code); REQUIRE_FALSE(source.resident.has_value());
    scalar_test_bytes sink;
    const auto written = [&sink, &source, &binding] {
        if constexpr (seqan3::aminoacid_alphabet<Alphabet>)
            return codec::emit_protein_sequence<Alphabet, Reverse, Packed>(sink, source, binding.transform);
        else return codec::emit_native_sequence<Alphabet, Reverse, Packed>(sink, source, binding.transform);
    }();
    REQUIRE(written.has_value()); REQUIRE(*written == sink.bytes.size());
    const auto letters = scalar_expected_letters<Alphabet>(owner.ranks, Reverse);
    if constexpr (Packed) REQUIRE(sink.bytes == scalar_expected_packed(letters));
    else REQUIRE(std::string(sink.bytes.begin(), sink.bytes.end()) == letters);
    if constexpr (seqan3::nucleotide_alphabet<Alphabet>) {
        std::vector<std::uint8_t> decoded(letters.size());
        const auto read = [&sink, &decoded, &binding] {
            if constexpr (Packed) return codec::decode_bam_rank_tile<Alphabet>(sink.bytes, decoded, binding.transform);
            else return codec::decode_sam_rank_tile<Alphabet>(sink.bytes, decoded, binding.transform);
        }();
        REQUIRE(read.has_value());
        for (std::size_t index{}; index < decoded.size(); ++index)
            REQUIRE(decoded[index] == seqan3::to_rank(Alphabet{}.assign_char(letters[index])));
    }
}

/** Quality expectations independently apply the published integer/Solexa conversion. */
template<bioseq::sequences::quality_alphabet Alphabet, bool Reverse, bool Text>
void check_scalar_quality(const fixture_rank_owner& owner, bool triplets,
        const bioseq::highway::codec_scalar::codec_binding& binding) {
    namespace codec = bioseq::io::alignment;
    using traits = bioseq::sequences::quality_alphabet_traits<Alphabet>;
    scalar_test_bytes sink;
    const auto written = codec::emit_quality<Alphabet, Reverse, Text>(sink,
        owner.borrow(supported_storage_alphabets::index_of<Alphabet>()), triplets, true, binding.transform);
    REQUIRE(written.has_value());
    std::vector<std::uint8_t> expected;
    for (std::size_t index{}; index < owner.ranks.size(); ++index) {
        const auto rank = owner.ranks[Reverse ? owner.ranks.size() - index - 1u : index];
        const auto original_score = static_cast<int>(rank) + traits::score_offset;
        auto score = traits::legacy
            ? static_cast<int>(std::lround(10.0 * std::log10(1.0 + std::pow(10.0, original_score / 10.0))))
            : original_score;
        if constexpr (Text) {
            if (!triplets && owner.ranks.size() == 1u && score == 9) score = 10;
            score += 33;
        }
        for (std::size_t copy{}; copy < (triplets ? 3u : 1u); ++copy)
            expected.push_back(static_cast<std::uint8_t>(score));
    }
    REQUIRE(sink.bytes == expected); REQUIRE(*written == expected.size());
    if (triplets) {
        std::vector<std::uint8_t> collapsed(owner.ranks.size());
        REQUIRE(codec::decode_quality_triplet_tile(sink.bytes, collapsed, binding.transform).has_value());
        for (std::size_t index{}; index < collapsed.size(); ++index) REQUIRE(collapsed[index] == expected[3u * index]);
        if (!sink.bytes.empty()) {
            sink.bytes[1] ^= 1u;
            REQUIRE_FALSE(codec::decode_quality_triplet_tile(sink.bytes, collapsed, binding.transform).has_value());
        }
    } else if constexpr (!Text) {
        std::vector<std::uint8_t> decoded(owner.ranks.size());
        const auto restored = codec::decode_bam_quality_tile<Alphabet>(sink.bytes, decoded, binding.transform);
        if constexpr (traits::legacy) REQUIRE_FALSE(restored.has_value());
        else {
            REQUIRE(restored.has_value());
            auto expected_ranks = owner.ranks;
            if constexpr (Reverse) std::ranges::reverse(expected_ranks);
            REQUIRE(decoded == expected_ranks);
        }
    }
}

/** Reflected polynomial oracle is independent of the selected zlib scalar fallback. */
[[nodiscard]] std::uint32_t scalar_crc_oracle(std::uint32_t initial, std::span<const std::uint8_t> bytes) {
    auto value = ~initial;
    for (const auto byte : bytes) {
        value ^= byte;
        for (unsigned bit{}; bit < 8u; ++bit)
            value = (value >> 1u) ^ ((value & 1u) != 0u ? 0xedb88320u : 0u);
    }
    return ~value;
}

/** No allocation/assertion/throw is allowed from the scanner's noexcept callback. */
struct scalar_scan_offsets {
    std::span<std::size_t> slots;
    std::size_t count{};
    bool overflow{};
    static void collect(void* context, std::size_t offset) noexcept {
        auto& result = *static_cast<scalar_scan_offsets*>(context);
        if (result.count == result.slots.size()) { result.overflow = true; return; }
        result.slots[result.count++] = offset;
    }
};

/** Emit the exact completed file before publication_fixture retires its private directory. */
void log_scalar_codec_file(const bioseq::highway::codec_scalar::codec_binding& binding,
        std::string_view alphabet, std::string_view format, std::string_view bytes) {
    const auto case_name = std::string{alphabet} + '.' + std::string{format};
    const auto log = [](std::string_view json) {
        bioseq_log_progress<logging_level::info>("BIOSEQ_SCALAR_CODEC_V1 {}", json);
    };
    log("{\"event\":\"begin\",\"case\":\"" + case_name + "\",\"alphabet\":\"" + std::string{alphabet}
        + "\",\"format\":\"" + std::string{format} + "\",\"target\":" + std::to_string(binding.target)
        + ",\"byte_lanes\":" + std::to_string(binding.byte_lanes)
        + ",\"rank_lanes\":" + std::to_string(binding.rank_lanes)
        + ",\"bytes\":" + std::to_string(bytes.size()) + '}');
    constexpr std::string_view hexadecimal{"0123456789ABCDEF"};
    for (std::size_t offset{}; offset < bytes.size();) {
        const auto count = std::min<std::size_t>(512u, bytes.size() - offset);
        std::string hex; hex.reserve(2u * count);
        for (const auto character : bytes.substr(offset, count)) {
            const auto value = static_cast<unsigned char>(character);
            hex += hexadecimal[value >> 4u]; hex += hexadecimal[value & 15u];
        }
        log("{\"event\":\"chunk\",\"case\":\"" + case_name + "\",\"offset\":" + std::to_string(offset)
            + ",\"hex\":\"" + hex + "\"}");
        offset += count;
    }
    log("{\"event\":\"end\",\"case\":\"" + case_name + "\",\"bytes\":" + std::to_string(bytes.size()) + '}');
}

} // namespace

TEST_CASE("Literal Highway scalar codec scanners and transforms retain exact boundaries",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto binding = scalar_codec_binding();
    using operation = bioseq::simd::sequence_rank_transform;
    constexpr std::array<std::uint8_t, 4> needles{0u, 17u, 128u, 255u};
    for (const auto size : scalar_boundary_lengths) {
        CAPTURE(size);
        std::vector<std::uint8_t> storage(size + 2u, 0xa5u);
        auto bytes = std::span{storage}.subspan(1u, size);
        for (std::size_t index{}; index < size; ++index) bytes[index] = static_cast<std::uint8_t>(index * 73u);
        const auto first = [bytes](std::span<const std::uint8_t> selected) {
            for (std::size_t index{}; index < bytes.size(); ++index)
                if (std::ranges::find(selected, bytes[index]) != selected.end()) return index;
            return bytes.size();
        };
        REQUIRE(binding.first_byte(needles[0], bytes.data(), size) == first(std::span{needles}.first(1u)));
        REQUIRE(binding.first_pair(needles[0], needles[1], bytes.data(), size) == first(std::span{needles}.first(2u)));
        REQUIRE(binding.first_triple(needles[0], needles[1], needles[2], bytes.data(), size) == first(std::span{needles}.first(3u)));
        REQUIRE(binding.first_triple(17u, 17u, 17u, bytes.data(), size) == binding.first_byte(17u, bytes.data(), size));
        // Search past the first vector: absent, middle/tail, and later-needle-only hits.
        std::vector<std::uint8_t> search(size, 42u);
        REQUIRE(binding.first_byte(17u, search.data(), size) == size);
        REQUIRE(binding.first_pair(17u, 18u, search.data(), size) == size);
        REQUIRE(binding.first_triple(17u, 18u, 19u, search.data(), size) == size);
        if (size != 0u) {
            for (const auto position : {std::size_t{0u}, size / 2u, size - 1u}) {
                for (const std::uint8_t selected : {std::uint8_t{17u}, std::uint8_t{18u}, std::uint8_t{19u}}) {
                    search[position] = selected;
                    REQUIRE(binding.first_byte(17u, search.data(), size) == (selected == 17u ? position : size));
                    REQUIRE(binding.first_pair(17u, 18u, search.data(), size) == (selected != 19u ? position : size));
                    REQUIRE(binding.first_triple(17u, 18u, 19u, search.data(), size) == position);
                    search[position] = 42u;
                }
            }
        }
        std::vector<std::size_t> observed(size);
        scalar_scan_offsets offsets{observed};
        binding.each_delim4(needles[0], needles[1], needles[2], needles[3], bytes.data(), size,
            &offsets, &scalar_scan_offsets::collect);
        REQUIRE_FALSE(offsets.overflow);
        std::vector<std::size_t> expected;
        for (std::size_t index{}; index < size; ++index)
            if (std::ranges::find(needles, bytes[index]) != needles.end()) expected.push_back(index);
        observed.resize(offsets.count); REQUIRE(observed == expected);

        std::vector<std::uint8_t> output(size + 2u, 0x5au);
        auto destination = std::span{output}.subspan(1u, size);
        REQUIRE(binding.transform(operation::reverse_bytes, bytes, 0u, {}, destination));
        REQUIRE(std::ranges::equal(destination, bytes | std::views::reverse));
        std::array<std::uint32_t, 256> table{};
        for (std::size_t index{}; index < table.size(); ++index) table[index] = static_cast<std::uint32_t>(255u - index);
        REQUIRE(binding.transform(operation::map_bytes, bytes, table.size(), table, destination));
        for (std::size_t index{}; index < size; ++index) REQUIRE(destination[index] == 255u - bytes[index]);
        REQUIRE(output.front() == 0x5au); REQUIRE(output.back() == 0x5au);
        REQUIRE(storage.front() == 0xa5u); REQUIRE(storage.back() == 0xa5u);
        REQUIRE_FALSE(binding.transform(operation::map_bytes, bytes, 0u, table, destination));
        REQUIRE_FALSE(binding.transform(operation::reverse_bytes, bytes, 0u, {}, output));
        std::vector<std::uint8_t> hex(2u * size), decoded(size);
        REQUIRE(binding.encode_hex(bytes, hex, 256u)); REQUIRE(binding.decode_hex(hex, decoded, 256u));
        REQUIRE(std::ranges::equal(decoded, bytes));
        if (!hex.empty()) {
            hex[0] = 'a'; REQUIRE_FALSE(binding.decode_hex(hex, decoded, 256u));
            REQUIRE_FALSE(binding.decode_hex(std::span{hex}.first(hex.size() - 1u), decoded, 256u));
        }
    }
    for (unsigned value{}; value < 256u; ++value) {
        const auto byte = static_cast<std::uint8_t>(value);
        const char character = static_cast<char>(byte);
        const bool json = value >= 32u && value < 127u && character != '"' && character != '\\';
        const bool hex = (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F');
        const bool header = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9') || std::string_view{"._~-"}.find(character) != std::string_view::npos;
        REQUIRE(binding.json_prefix(&character, 1u) == static_cast<std::size_t>(json));
        REQUIRE(binding.sam_aux_prefix(&byte, 1u, false) == static_cast<std::size_t>(value >= 32u && value <= 126u));
        REQUIRE(binding.sam_aux_prefix(&byte, 1u, true) == static_cast<std::size_t>(hex));
        REQUIRE(binding.header_prefix(&character, 1u) == static_cast<std::size_t>(header));
        REQUIRE(binding.transform(operation::validate_phred, {&byte, 1u}, 0u, {}, {}) == (value <= 93u));
        REQUIRE(binding.transform(operation::validate_fastq, {&byte, 1u}, 0u, {}, {}) == (value >= 33u && value <= 126u));
        REQUIRE(binding.transform(operation::validate_missing_quality, {&byte, 1u}, 0u, {}, {}) == (value == 255u));
    }
    std::array<std::uint32_t, 64> translation{};
    for (std::size_t index{}; index < translation.size(); ++index) translation[index] = static_cast<std::uint32_t>(index);
    std::array<std::uint8_t, 192> triplets{};
    for (std::size_t index{}; index < 64u; ++index) {
        triplets[3u * index] = static_cast<std::uint8_t>(index / 16u);
        triplets[3u * index + 1u] = static_cast<std::uint8_t>(index / 4u % 4u);
        triplets[3u * index + 2u] = static_cast<std::uint8_t>(index % 4u);
    }
    std::array<std::uint8_t, 64> translated{};
    REQUIRE(binding.transform(operation::translate_triplets, triplets, 4u, translation, translated));
    for (std::size_t index{}; index < translated.size(); ++index) REQUIRE(translated[index] == index);
    triplets[31] = 4u;
    REQUIRE_FALSE(binding.transform(operation::translate_triplets, triplets, 4u, translation, translated));
    REQUIRE_FALSE(binding.transform(operation::translate_triplets, std::span{triplets}.first(191u), 4u, translation, translated));
    std::array<std::uint32_t, 16> identity{};
    for (std::size_t index{}; index < identity.size(); ++index) identity[index] = static_cast<std::uint32_t>(index);
    const std::array<std::uint8_t, 6> odd_source{1u, 2u, 4u, 8u, 15u, 1u};
    std::array<std::uint8_t, 4> packed{};
    auto first = bioseq::io::alignment::pack_bam_rank_tile(std::span{odd_source}.first(3u), 16u, identity,
        {}, false, packed, binding.transform);
    REQUIRE(first.has_value()); REQUIRE(first->bytes_written == 1u); REQUIRE(first->next.pending);
    auto second = bioseq::io::alignment::pack_bam_rank_tile(std::span{odd_source}.subspan(3u), 16u, identity,
        first->next, true, std::span{packed}.subspan(1u), binding.transform);
    REQUIRE(second.has_value()); REQUIRE_FALSE(second->next.pending);
    REQUIRE(std::ranges::equal(std::span{packed}.first(3u), std::array<std::uint8_t, 3>{0x12u, 0x48u, 0xf1u}));
    REQUIRE_FALSE(bioseq::io::alignment::pack_bam_rank_tile(std::span{odd_source}.first(2u), 16u, identity,
        {}, true, {}, binding.transform).has_value());
    REQUIRE_FALSE(bioseq::io::alignment::pack_bam_rank_tile(std::span{odd_source}.first(1u), 16u, identity,
        {16u, true}, true, packed, binding.transform).has_value());
    const std::array<std::uint8_t, 1> reference_dependent{0u};
    std::array<std::uint8_t, 1> decoded_reference{};
    REQUIRE(bioseq::io::alignment::decode_bam_wire_tile(reference_dependent, decoded_reference, binding.transform).has_value());
    REQUIRE(decoded_reference[0] == 0u);
    REQUIRE_FALSE(bioseq::io::alignment::decode_bam_rank_tile<seq::dna4>(reference_dependent, decoded_reference, binding.transform).has_value());
    for (const auto bits : {0u, 0x80000000u, 0x3fc00000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00001u}) {
        std::array<std::uint8_t, 4> encoded{};
        for (unsigned index{}; index < 4u; ++index) encoded[index] = static_cast<std::uint8_t>(bits >> (8u * index));
        REQUIRE(binding.finite_f32(encoded) == ((bits & 0x7f800000u) != 0x7f800000u));
        REQUIRE_FALSE(binding.finite_f32(std::span{encoded}.first(3u)));
    }
    std::vector<std::uint8_t> crc_input(65537u);
    for (std::size_t index{}; index < crc_input.size(); ++index) crc_input[index] = static_cast<std::uint8_t>(index * 37u);
    for (const std::size_t size : {0u, 1u, 127u, 128u, 129u, 65536u}) {
        const auto bytes = std::span<const std::uint8_t>{crc_input}.subspan(1u, size);
        for (const std::uint32_t initial : {0u, 0xffffffffu, 0x12345678u}) {
            const auto expected = scalar_crc_oracle(initial, bytes);
            REQUIRE(binding.crc32(initial, bytes.data(), bytes.size()) == expected);
            const auto split = size / 2u;
            const auto prefix = binding.crc32(initial, bytes.data(), split);
            REQUIRE(binding.crc32(prefix, bytes.data() + split, size - split) == expected);
        }
    }
}

TEST_CASE("Literal Highway scalar biological and quality codecs cover every registry alphabet",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto binding = scalar_codec_binding();
    for (std::uint8_t code{}; code < supported_biological_alphabets::alphabet_count; ++code) {
        supported_biological_alphabets::dispatch_alphabet_by_code(code, [&binding, code]<class Alphabet>() {
            for (const auto size : scalar_boundary_lengths) {
                CAPTURE(code, size);
                fixture_rank_owner owner{std::vector<std::uint8_t>(size)};
                for (std::size_t index{}; index < size; ++index) owner.ranks[index] = static_cast<std::uint8_t>(index % seqan3::alphabet_size<Alphabet>);
                check_scalar_biological<Alphabet, false, false>(owner, binding);
                check_scalar_biological<Alphabet, false, true>(owner, binding);
                check_scalar_biological<Alphabet, true, false>(owner, binding);
                check_scalar_biological<Alphabet, true, true>(owner, binding);
            }
            fixture_rank_owner malformed{{static_cast<std::uint8_t>(seqan3::alphabet_size<Alphabet>)}};
            scalar_test_bytes sink;
            const auto rejected = [&sink, &malformed, &binding, code] {
                if constexpr (seqan3::aminoacid_alphabet<Alphabet>) return bioseq::io::alignment::emit_protein_sequence<Alphabet, false, true>(sink, malformed.borrow(code), binding.transform);
                else return bioseq::io::alignment::emit_native_sequence<Alphabet, false, true>(sink, malformed.borrow(code), binding.transform);
            }();
            REQUIRE_FALSE(rejected.has_value());
        });
    }
    for (std::uint8_t code{}; code < supported_storage_alphabets::alphabet_count; ++code) {
        supported_storage_alphabets::dispatch_alphabet_by_code(code, [&binding, code]<class Alphabet>() {
            if constexpr (bioseq::sequences::quality_alphabet<Alphabet>) {
                for (const auto size : scalar_boundary_lengths) {
                    fixture_rank_owner owner{std::vector<std::uint8_t>(size)};
                    for (std::size_t index{}; index < size; ++index) owner.ranks[index] = static_cast<std::uint8_t>(index % seqan3::alphabet_size<Alphabet>);
                    for (const bool triplets : {false, true}) {
                        CAPTURE(code, size, triplets);
                        check_scalar_quality<Alphabet, false, false>(owner, triplets, binding);
                        check_scalar_quality<Alphabet, false, true>(owner, triplets, binding);
                        check_scalar_quality<Alphabet, true, false>(owner, triplets, binding);
                        check_scalar_quality<Alphabet, true, true>(owner, triplets, binding);
                    }
                }
                fixture_rank_owner malformed{{static_cast<std::uint8_t>(seqan3::alphabet_size<Alphabet>)}};
                scalar_test_bytes sink;
                REQUIRE_FALSE((bioseq::io::alignment::emit_quality<Alphabet, false, false>(sink,
                    malformed.borrow(code), false, false, binding.transform)).has_value());
            }
        });
    }
    struct refusing_sink {
        std::expected<void, std::string> append(std::span<const std::uint8_t>) {
            return std::unexpected(std::string{"scalar sink refusal"});
        }
    } refusal;
    fixture_rank_owner one{{0u}};
    constexpr auto dna_code = supported_storage_alphabets::index_of<seq::dna4>();
    constexpr auto protein_code = supported_storage_alphabets::index_of<seq::aa20>();
    const auto nucleotide_refused = bioseq::io::alignment::emit_native_sequence<seq::dna4, false, false>(
        refusal, one.borrow(dna_code), binding.transform);
    REQUIRE_FALSE(nucleotide_refused.has_value()); REQUIRE(nucleotide_refused.error() == "scalar sink refusal");
    const auto protein_refused = bioseq::io::alignment::emit_protein_sequence<seq::aa20, false, true>(
        refusal, one.borrow(protein_code), binding.transform);
    REQUIRE_FALSE(protein_refused.has_value()); REQUIRE(protein_refused.error() == "scalar sink refusal");
    auto unavailable = one.borrow(dna_code); unavailable.symbol_count = 2u;
    scalar_test_bytes destination;
    REQUIRE_FALSE((bioseq::io::alignment::emit_native_sequence<seq::dna4, false, true>(destination, unavailable, binding.transform)).has_value());
    unavailable.alphabet_code = protein_code;
    REQUIRE_FALSE((bioseq::io::alignment::emit_protein_sequence<seq::aa20, false, false>(destination, unavailable, binding.transform)).has_value());
    REQUIRE(destination.bytes.empty());
    fixture_rank_owner singleton{{9u}};
    check_scalar_quality<seq::phred42, false, true>(singleton, false, binding);
    check_scalar_quality<seq::phred42, false, false>(singleton, false, binding);
}

TEST_CASE("Literal Highway scalar kernels produce canonical SAM BAM witnesses for all alphabets",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    namespace codec = bioseq::io::alignment;
    const auto binding = scalar_codec_binding();
    publication_fixture fixture;
    constexpr auto phred = supported_storage_alphabets::index_of<seq::phred42>();
    constexpr std::array<std::size_t, 4> lengths{11u, 11u, 1u, 3u};
    std::size_t total_bytes{}, files{};
    for (std::uint8_t code{}; code < supported_biological_alphabets::alphabet_count; ++code) {
        std::array<fixture_rank_owner, 4> sequences, qualities;
        std::array<std::string, 4> expected_letters;
        bool protein{};
        supported_biological_alphabets::dispatch_alphabet_by_code(code,
            [&sequences, &expected_letters, &protein, &lengths]<class Alphabet>() {
                protein = seqan3::aminoacid_alphabet<Alphabet>;
                for (std::size_t index{}; index < sequences.size(); ++index) {
                    sequences[index].ranks.resize(lengths[index]);
                    for (std::size_t position{}; position < lengths[index]; ++position)
                        sequences[index].ranks[position] = static_cast<std::uint8_t>(position % seqan3::alphabet_size<Alphabet>);
                    expected_letters[index] = scalar_expected_letters<Alphabet>(sequences[index].ranks, index == 1u);
                }
            });
        for (std::size_t index{}; index < qualities.size(); ++index) {
            qualities[index].ranks.resize(lengths[index]);
            for (std::size_t position{}; position < lengths[index]; ++position)
                qualities[index].ranks[position] = static_cast<std::uint8_t>(index == 2u ? 9u : position);
        }
        const auto alphabet = supported_biological_alphabets::name_of_alphabet_code(code);
        for (const auto format : {sequence_file_format::sam, sequence_file_format::bam}) {
            CAPTURE(code, format);
            const bool binary = format == sequence_file_format::bam;
            const std::string_view format_name = binary ? "bam" : "sam";
            const auto path = fixture.directory / (std::string{alphabet} + '.' + std::string{format_name});
            bioseq::io::record_source_metadata metadata;
            metadata.declared_alphabet_codes = {code}; metadata.declared_quality_alphabet_codes = {phred};
            metadata.reference_dictionary_complete = true;
            metadata.references.push_back({.identity = 7u, .name = "scalar_ref", .length = 64u, .alphabet_code = code});
            bioseq::io::writer_options options;
            options.format = format; options.chunk_capacity_bytes = 4096u;
            options.sam_header = {"@PG\tID:scalar_codec\tPN:bioseq-scalar-fixture", "@CO\tscalar byte witness"};
            bioseq::io::concurrency_options serial; serial.worker_count = 1u; serial.io_buffer_pool_size = 1u;
            std::array<std::string, 4> names{"scalar_0", "scalar_1", "scalar_2", "scalar_3"};
            std::array<std::string, 4> headers{"scalar record 0", "scalar record 1", "singleton present quality", "absent quality"};
            const std::array<codec::sam_cigar_source, 4> cigars{
                codec::sam_cigar_source{"11M"}, codec::sam_cigar_source{"11M"},
                codec::sam_cigar_source{"1M"}, codec::sam_cigar_source{"3M"}};
            const std::array<bioseq::io::scalar_field_view, 1> fields{{
                {.name = "note", .kind = bioseq::io::column_value_kind::scalar_text,
                 .qualified_type = "pg_catalog.text", .bytes = "tab\tquote\"slash\\"}}};
            std::array<bioseq::io::export_record_view, 4> rows;
            for (std::size_t index{}; index < rows.size(); ++index) {
                auto& row = rows[index]; row.ordinal = index;
                row.identity.template_id = index; row.identity.segment_id = index; row.identity.reference_id = 7u;
                row.header = headers[index]; row.qname = names[index]; row.sequence = sequences[index].borrow(code);
                if (index != 3u) row.quality = qualities[index].borrow(phred);
                row.metadata = fields;
                row.alignment.emplace(); auto& alignment = *row.alignment;
                alignment.flags = index == 1u ? 16u : 0u; alignment.mapping_quality = 37u;
                alignment.reference_name = "scalar_ref"; alignment.reference_length = 64u;
                alignment.target_start = 2u + 12u * index; alignment.target_end = *alignment.target_start + lengths[index];
                alignment.query_start = 0u; alignment.query_end = lengths[index]; alignment.query_length = lengths[index];
                alignment.cigar = cigars[index].borrow();
            }
            {
                bioseq::io::export_context_memory context_memory;
                codec::alignment_export_context context;
                REQUIRE(context.initialize(metadata, options,
                    {&context_memory, &bioseq::io::export_context_memory::acquire}).has_value());
                for (const auto& row : rows) REQUIRE(context.observe(row).has_value());
                REQUIRE(context.finalize().has_value());
                const auto output = bioseq::io::ordered_output_sink::open(path.string(), options,
                    binary ? bioseq::io::sequence_compression::bgzf : bioseq::io::sequence_compression::none, serial);
                REQUIRE(output.has_value());
                REQUIRE(context.emit_header(**output, format).has_value());
                for (const auto& row : rows) {
                    const auto resolved = context.resolve(row); REQUIRE(resolved.has_value());
                    REQUIRE(bioseq::io::emit_alignment_record(**output, row, *resolved, format, binding.transform).has_value());
                }
                REQUIRE((*output)->finish().has_value());
            }
            const auto actual = publication_fixture::read(path);
            const auto raw = binary ? fixture_gunzip(path) : actual;
            const auto reference_name = protein ? "bioseq_aa_0" : "scalar_ref";
            const auto reference_line = "@SQ\tSN:" + std::string{reference_name} + "\tLN:" + std::to_string(protein ? 192u : 64u);
            REQUIRE(raw.find(reference_line) != std::string::npos);
            std::array<std::vector<std::uint8_t>, 4> native_qualities;
            for (std::size_t index{}; index < rows.size(); ++index) {
                if (index == 3u) {
                    native_qualities[index].assign(expected_letters[index].size(), 255u);
                    continue;
                }
                auto ranks = qualities[index].ranks;
                if (index == 1u) std::ranges::reverse(ranks);
                for (auto rank : ranks) {
                    if (!binary && !protein && ranks.size() == 1u && rank == 9u) rank = 10u;
                    for (std::size_t copy{}; copy < (protein ? 3u : 1u); ++copy)
                        native_qualities[index].push_back(static_cast<std::uint8_t>(rank + (binary ? 0u : 33u)));
                }
            }
            if (binary) {
                const auto body = fixture_bam_sequences(raw); REQUIRE(body.size() == rows.size());
                for (std::size_t index{}; index < rows.size(); ++index) {
                    const auto packed = scalar_expected_packed(expected_letters[index]);
                    REQUIRE(body[index].symbols == expected_letters[index].size());
                    const auto sequence = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(raw.data()) + body[index].offset, packed.size()};
                    REQUIRE(std::ranges::equal(sequence, packed));
                    const auto quality = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(raw.data()) + body[index].quality_offset, body[index].symbols};
                    REQUIRE(std::ranges::equal(quality, native_qualities[index]));
                }
            } else {
                std::size_t cursor{}, row_index{};
                while (cursor < raw.size()) {
                    const auto newline = raw.find('\n', cursor); REQUIRE(newline != std::string::npos);
                    const auto line = std::string_view{raw}.substr(cursor, newline - cursor); cursor = newline + 1u;
                    if (line.starts_with('@')) continue;
                    REQUIRE(row_index < rows.size());
                    std::vector<std::string_view> columns;
                    for (std::size_t offset{};;) {
                        const auto tab = line.find('\t', offset);
                        columns.push_back(line.substr(offset, tab == std::string_view::npos ? tab : tab - offset));
                        if (tab == std::string_view::npos) break;
                        offset = tab + 1u;
                    }
                    REQUIRE(columns.size() >= 12u); REQUIRE(columns[0] == names[row_index]);
                    REQUIRE(columns[1] == (row_index == 1u ? "16" : "0")); REQUIRE(columns[2] == reference_name);
                    REQUIRE(columns[3] == std::to_string((2u + 12u * row_index) * (protein ? 3u : 1u) + 1u));
                    REQUIRE(columns[4] == "37"); REQUIRE(columns[5] == std::to_string(lengths[row_index] * (protein ? 3u : 1u)) + 'M');
                    REQUIRE(columns[9] == expected_letters[row_index]);
                    REQUIRE(columns[10] == (row_index == 3u ? std::string{"*"}
                        : std::string(native_qualities[row_index].begin(), native_qualities[row_index].end())));
                    ++row_index;
                }
                REQUIRE(row_index == rows.size());
            }
            struct original_consumer {
                std::span<const bioseq::io::export_record_view> original;
                const std::array<fixture_rank_owner, 4>& sequences;
                const std::array<fixture_rank_owner, 4>& qualities;
                std::uint8_t code;
                std::size_t headers{}, count{};
                std::expected<void, std::string> header(const bioseq::io::record_source_metadata& source) {
                    ++headers; REQUIRE(source.references.size() == 1u);
                    REQUIRE(source.references[0].length == 64u); REQUIRE(source.references[0].alphabet_code == code);
                    return {};
                }
                std::expected<void, std::string> record(bioseq::io::decoded_record& decoded) {
                    REQUIRE(count < original.size()); const auto& row = decoded.record; const auto& expected = original[count];
                    REQUIRE(row.header == expected.header); REQUIRE(row.qname == expected.qname);
                    REQUIRE(row.sequence.has_value()); REQUIRE(row.sequence->alphabet_code == code);
                    std::array<std::uint8_t, 32> scratch{};
                    const auto sequence = row.sequence->read_checked(0u, sequences[count].ranks.size(), scratch);
                    REQUIRE(sequence.has_value()); REQUIRE(std::ranges::equal(*sequence, sequences[count].ranks));
                    REQUIRE(row.quality.has_value() == expected.quality.has_value());
                    if (row.quality) {
                        REQUIRE(row.quality->alphabet_code == expected.quality->alphabet_code);
                        const auto quality = row.quality->read_checked(0u, qualities[count].ranks.size(), scratch);
                        REQUIRE(quality.has_value()); REQUIRE(std::ranges::equal(*quality, qualities[count].ranks));
                    }
                    REQUIRE(row.alignment.has_value()); REQUIRE(row.alignment->flags == expected.alignment->flags);
                    REQUIRE(row.alignment->target_start == expected.alignment->target_start);
                    REQUIRE(row.alignment->reference_length == 64u);
                    REQUIRE(row.metadata.size() == 1u); REQUIRE(row.metadata[0].bytes == expected.metadata[0].bytes);
                    ++count; return {};
                }
            } consumer{rows, sequences, qualities, code};
            auto reading = bioseq::io::default_reader_options;
            reading.concurrency.worker_count = 1u; reading.concurrency.parser_count = 1u;
            const auto restored = bioseq::io::read_alignment_records(path.string(), format, consumer, reading);
            REQUIRE(restored.has_value()); REQUIRE(*restored == rows.size());
            REQUIRE(consumer.headers == 1u); REQUIRE(consumer.count == rows.size());
            REQUIRE(actual.size() <= 65536u - total_bytes); total_bytes += actual.size(); ++files;
            log_scalar_codec_file(binding, alphabet, format_name, actual);
        }
    }
    REQUIRE(files == 14u);
    bioseq_log_progress<logging_level::info>("BIOSEQ_SCALAR_CODEC_V1 {{\"event\":\"set_end\",\"files\":14,\"bytes\":{}}}", total_bytes);
}


TEST_CASE("Cross-domain mates have unknown native geometry and lossless original fields",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    namespace codec = bioseq::io::alignment;
    publication_fixture fixture;
    fixture_dispatch_pool scheduler;
    constexpr auto dna = supported_storage_alphabets::index_of<seq::dna4>();
    constexpr auto protein = supported_storage_alphabets::index_of<seq::aa20>();
    constexpr auto phred = supported_storage_alphabets::index_of<seq::phred42>();
    fixture_rank_owner sequence{{0u, 0u}}, quality{{13u, 13u}};
    codec::sam_cigar_source cigar{"2M"};
    constexpr std::array<std::string_view, 4> names{"dna_pair", "protein_pair", "dna_unpaired", "protein_unpaired"};
    constexpr std::string_view mate_cigar{"2M"}, mate_sequence{"AA"}, mate_quality{".."}, md{"2"};
    const std::array<std::uint8_t, 4> mq{37u, 0u, 0u, 0u}, nm{};
    const std::array<bioseq::io::auxiliary_field_view, 6> auxiliary{{
        {{'M', 'C'}, 'Z', 0, std::as_bytes(std::span{mate_cigar.data(), mate_cigar.size()})},
        {{'R', '2'}, 'Z', 0, std::as_bytes(std::span{mate_sequence.data(), mate_sequence.size()})},
        {{'Q', '2'}, 'Z', 0, std::as_bytes(std::span{mate_quality.data(), mate_quality.size()})},
        {{'M', 'Q'}, 'i', 0, std::as_bytes(std::span{mq})},
        {{'N', 'M'}, 'i', 0, std::as_bytes(std::span{nm})},
        {{'M', 'D'}, 'Z', 0, std::as_bytes(std::span{md.data(), md.size()})}}};
    for (const auto& field : auxiliary) {
        REQUIRE(codec::validate_auxiliary_field(field).has_value());
        for (const bool sam_text : {false, true}) {
            REQUIRE(codec::retain_original_native_auxiliary(field, false, sam_text, false));
            const bool mate_symbols = field.tag == std::array{'M', 'C'}
                || field.tag == std::array{'R', '2'} || field.tag == std::array{'Q', '2'};
            REQUIRE(codec::retain_original_native_auxiliary(field, false, sam_text, true) == !mate_symbols);
        }
    }
    struct source_type {
        using batch_type = pressure_retry_batch;
        bioseq::io::record_source_metadata source_metadata;
        std::array<bioseq::io::export_record_view, 4> records;
        std::size_t cursor{}, released{};
        /** Reuse the existing admitted one-row fixture while retaining four stable rows. */
        std::expected<std::optional<batch_type>, bioseq::io::record_error> next() {
            if (cursor == records.size()) return std::optional<batch_type>{};
            auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                sizeof(bioseq::io::export_record_view));
            if (!permit) return std::unexpected(bioseq::io::record_error{
                cursor + 1u, std::nullopt, "mate fixture cannot admit its next row"});
            return std::optional<batch_type>{std::in_place, std::move(*permit), records[cursor++], released};
        }
        /** The typed views and dictionary already describe this fixed fixture. */
        std::span<const bioseq::io::record_column> columns() const { return {}; }
        /** Borrow call-lifetime metadata, as the production writer requires. */
        const bioseq::io::record_source_metadata& metadata() const { return source_metadata; }
        /** No pending source owner exists outside the delivered batch. */
        void cancel() {}
    };
    static_assert(bioseq::io::export_record_source<source_type>);
    for (const auto format : {sequence_file_format::sam, sequence_file_format::bam}) {
        for (const auto [workers, boolean_flags] : std::array{
                std::pair{std::size_t{1u}, false}, std::pair{std::size_t{4u}, false},
                std::pair{std::size_t{1u}, true}, std::pair{std::size_t{4u}, true}}) {
            CAPTURE(format, workers, boolean_flags);
            source_type source;
            source.source_metadata.declared_alphabet_codes = {dna, protein};
            source.source_metadata.declared_quality_alphabet_codes = {phred};
            source.source_metadata.reference_dictionary_complete = true;
            source.source_metadata.references = {
                {.identity = 0u, .name = "nucleotide", .length = 100u, .alphabet_code = dna},
                {.identity = 1u, .name = "protein", .length = 20u, .alphabet_code = protein}};
            for (std::size_t index{}; index < source.records.size(); ++index) {
                const bool synthetic = index % 2u != 0;
                auto& row = source.records[index];
                row.ordinal = index + 1u; row.header = names[index]; row.qname = names[index];
                row.identity.template_id = index; row.identity.segment_id = index;
                row.identity.reference_id = synthetic ? 1u : 0u;
                // Boolean-only PG mappings retain the default classification and no numeric FLAG.
                row.identity.classification = boolean_flags
                    ? bioseq::io::alignment_classification_source::no_alignment
                    : bioseq::io::alignment_classification_source::explicit_flags;
                row.sequence = sequence.borrow(synthetic ? protein : dna); row.quality = quality.borrow(phred);
                row.alignment.emplace(); auto& alignment = *row.alignment;
                const std::uint16_t original_flags = index < 2u ? 99u : 0u;
                if (boolean_flags) {
                    for (std::size_t bit{}; bit < alignment.flag_constraints.size(); ++bit)
                        alignment.flag_constraints[bit] = (original_flags & (std::uint16_t{1u} << bit)) != 0u;
                    REQUIRE_FALSE(alignment.flags.has_value());
                } else alignment.flags = original_flags;
                alignment.mapping_quality = 37u;
                alignment.reference_name = synthetic ? "protein" : "nucleotide";
                alignment.reference_length = synthetic ? 20u : 100u;
                alignment.target_start = 10u; alignment.target_end = 12u;
                alignment.query_start = 0u; alignment.query_end = 2u; alignment.query_length = 2u;
                alignment.mate_reference = synthetic ? "nucleotide" : "protein";
                alignment.mate_start = 19u; alignment.template_length = 17;
                alignment.cigar = cigar.borrow(); alignment.auxiliary = auxiliary;
            }
            bioseq::io::writer_options writing;
            writing.format = format; writing.chunk_capacity_bytes = 4096u;
            bioseq::io::concurrency_options concurrency; concurrency.worker_count = workers;
            const bool binary = format == sequence_file_format::bam;
            const auto path = fixture.directory / ("mates_" + std::to_string(workers)
                + (boolean_flags ? "_boolean" : "_numeric") + (binary ? ".bam" : ".sam"));
            const auto written = bioseq::io::write_records(source, path.string(), writing, concurrency);
            REQUIRE(written.has_value()); REQUIRE(*written == 4u); REQUIRE(source.released == 4u);
            REQUIRE_FALSE(fixture.has_private_staging());
            const auto raw = binary ? fixture_gunzip(path) : publication_fixture::read(path);
            REQUIRE(raw.find("@SQ\tSN:nucleotide\tLN:100") != std::string::npos);
            REQUIRE(raw.find("@SQ\tSN:bioseq_aa_1\tLN:60") != std::string::npos);
            const auto check_tags = [](const std::set<std::array<char, 2>>& tags, bool synthetic) {
                REQUIRE_FALSE(tags.contains({'M', 'C'})); REQUIRE_FALSE(tags.contains({'R', '2'}));
                REQUIRE_FALSE(tags.contains({'Q', '2'})); REQUIRE(tags.contains({'b', 'q'}));
                REQUIRE(tags.contains({'M', 'Q'}) == !synthetic);
                REQUIRE(tags.contains({'N', 'M'}) == !synthetic);
                REQUIRE(tags.contains({'M', 'D'}) == !synthetic);
            };
            if (binary) {
                const auto body = fixture_bam_sequences(raw); REQUIRE(body.size() == 4u);
                const auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()};
                const auto u32 = [bytes](std::size_t offset) {
                    REQUIRE(offset <= bytes.size()); REQUIRE(bytes.size() - offset >= 4u);
                    std::uint32_t value{};
                    for (unsigned index{}; index < 4u; ++index)
                        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8u * index);
                    return value;
                };
                for (std::size_t index{}; index < body.size(); ++index) {
                    const bool synthetic = index % 2u != 0;
                    const auto core = body[index].core_offset;
                    REQUIRE((u32(core + 12u) >> 16u) == (index < 2u ? 73u : 0u));
                    REQUIRE(u32(core) == (synthetic ? 1u : 0u)); REQUIRE(u32(core + 4u) == (synthetic ? 30u : 10u));
                    REQUIRE(u32(core + 20u) == std::numeric_limits<std::uint32_t>::max());
                    REQUIRE(u32(core + 24u) == std::numeric_limits<std::uint32_t>::max()); REQUIRE(u32(core + 28u) == 0u);
                    REQUIRE((u32(core + 12u) & 65535u) == 1u);
                    REQUIRE(u32(body[index].offset - 4u) == ((synthetic ? 6u : 2u) << 4u));
                    std::set<std::array<char, 2>> tags;
                    auto offset = body[index].quality_offset + body[index].symbols;
                    while (offset < body[index].record_end) {
                        const auto field = codec::parse_bam_auxiliary(bytes.subspan(offset, body[index].record_end - offset));
                        REQUIRE(field.has_value()); REQUIRE(field->consumed_bytes > 0u);
                        REQUIRE(tags.insert(field->field.tag).second); offset += field->consumed_bytes;
                    }
                    REQUIRE(offset == body[index].record_end); check_tags(tags, synthetic);
                }
            } else {
                std::size_t cursor{}, index{};
                while (cursor < raw.size()) {
                    const auto newline = raw.find('\n', cursor); REQUIRE(newline != std::string::npos);
                    const auto line = std::string_view{raw}.substr(cursor, newline - cursor); cursor = newline + 1u;
                    if (line.starts_with('@')) continue;
                    REQUIRE(index < 4u); const bool synthetic = index % 2u != 0;
                    const auto row = codec::parse_sam_record(line); REQUIRE(row.has_value());
                    REQUIRE(row->qname == names[index]); REQUIRE(row->flags == (index < 2u ? 73u : 0u));
                    REQUIRE(row->reference_name == (synthetic ? "bioseq_aa_1" : "nucleotide"));
                    REQUIRE(row->position == (synthetic ? 30u : 10u)); REQUIRE(row->cigar == (synthetic ? "6M" : "2M"));
                    REQUIRE(row->mate_reference_name == "*"); REQUIRE_FALSE(row->mate_position.has_value());
                    REQUIRE(row->template_length == 0);
                    std::set<std::array<char, 2>> tags;
                    for (std::size_t offset{}; offset < row->auxiliary.size();) {
                        const auto tab = row->auxiliary.find('\t', offset);
                        const auto field = row->auxiliary.substr(offset, tab == std::string_view::npos ? tab : tab - offset);
                        REQUIRE(field.size() >= 5u); REQUIRE(field[2] == ':');
                        REQUIRE(tags.insert({field[0], field[1]}).second);
                        if (tab == std::string_view::npos) break;
                        offset = tab + 1u;
                    }
                    check_tags(tags, synthetic); ++index;
                }
                REQUIRE(index == 4u);
            }
            struct original_consumer {
                const source_type& original;
                const fixture_rank_owner& sequence;
                const fixture_rank_owner& quality;
                std::size_t count{};
                /** The canonical dictionary retains original, not projected, lengths. */
                std::expected<void, std::string> header(const bioseq::io::record_source_metadata& metadata) {
                    REQUIRE(metadata.references.size() == 2u);
                    for (std::size_t index{}; index < metadata.references.size(); ++index) {
                        REQUIRE(metadata.references[index].length == original.source_metadata.references[index].length);
                        REQUIRE(metadata.references[index].alphabet_code == original.source_metadata.references[index].alphabet_code);
                    }
                    return {};
                }
                /** Check every suppressed source fact after ordered public delivery. */
                std::expected<void, std::string> record(bioseq::io::decoded_record& decoded) {
                    REQUIRE(count < original.records.size()); const auto& expected = original.records[count];
                    const auto& row = decoded.record;
                    REQUIRE(row.header == expected.header); REQUIRE(row.qname == expected.qname);
                    REQUIRE(row.identity.template_id == expected.identity.template_id);
                    REQUIRE(row.identity.segment_id == expected.identity.segment_id);
                    REQUIRE(row.identity.reference_id == expected.identity.reference_id);
                    REQUIRE(row.sequence.has_value()); REQUIRE(row.quality.has_value());
                    REQUIRE(row.sequence->alphabet_code == expected.sequence->alphabet_code);
                    REQUIRE(row.quality->alphabet_code == expected.quality->alphabet_code);
                    std::array<std::uint8_t, 2> scratch{};
                    const auto symbols = row.sequence->read_checked(0u, 2u, scratch);
                    REQUIRE(symbols.has_value()); REQUIRE(std::ranges::equal(*symbols, sequence.ranks));
                    const auto scores = row.quality->read_checked(0u, 2u, scratch);
                    REQUIRE(scores.has_value()); REQUIRE(std::ranges::equal(*scores, quality.ranks));
                    REQUIRE(row.alignment.has_value()); const auto& alignment = *row.alignment;
                    // Both input forms must restore original 99, even though native paired FLAG is 73.
                    REQUIRE(alignment.flags.has_value());
                    REQUIRE(*alignment.flags == (count < 2u ? 99u : 0u));
                    REQUIRE(alignment.reference_name == expected.alignment->reference_name);
                    REQUIRE(alignment.reference_length == expected.alignment->reference_length);
                    REQUIRE(alignment.target_start == 10u); REQUIRE(alignment.target_end == 12u);
                    REQUIRE(alignment.query_start == 0u); REQUIRE(alignment.query_end == 2u); REQUIRE(alignment.query_length == 2u);
                    const auto recipe = codec::make_cigar_wire_recipe(alignment, false, *alignment.flags);
                    REQUIRE(recipe.has_value()); const auto measured = codec::measure_wire_cigar(*recipe);
                    REQUIRE(measured.has_value()); std::array<std::uint8_t, 8> text{};
                    codec::alignment_window_sink cigar_text{text};
                    REQUIRE(codec::emit_sam_cigar(cigar_text, *recipe, *measured).has_value());
                    const std::string_view original_cigar{reinterpret_cast<const char*>(text.data()), cigar_text.written};
                    REQUIRE(original_cigar == "2M");
                    REQUIRE(alignment.mate_reference == expected.alignment->mate_reference);
                    REQUIRE(alignment.mate_start == 19u); REQUIRE(alignment.template_length == 17);
                    REQUIRE(alignment.auxiliary.size() == expected.alignment->auxiliary.size());
                    for (std::size_t index{}; index < alignment.auxiliary.size(); ++index)
                        REQUIRE(codec::auxiliary_logically_equal(alignment.auxiliary[index], expected.alignment->auxiliary[index]));
                    ++count; return {};
                }
            } consumer{source, sequence, quality};
            auto reading = bioseq::io::default_reader_options;
            reading.concurrency.worker_count = workers; reading.concurrency.parser_count = 1u;
            const auto restored = bioseq::io::read_alignment_records(path.string(), format, consumer, reading);
            REQUIRE(restored.has_value()); REQUIRE(*restored == 4u); REQUIRE(consumer.count == 4u);
            const auto reject_projection = [&fixture, &source, &sequence, &quality, &reading, format, binary](
                    const std::string& altered, std::string_view diagnostic) {
                const auto corrupted = fixture.directory / (binary ? "mate_corrupt.bam" : "mate_corrupt.sam");
                publication_fixture::seed(corrupted, binary ? fixture_bgzf(altered) : altered);
                original_consumer rejected{source, sequence, quality};
                const auto loaded = bioseq::io::read_alignment_records(corrupted.string(), format, rejected, reading);
                REQUIRE_FALSE(loaded.has_value()); REQUIRE(loaded.error().find(diagnostic) != std::string::npos);
                REQUIRE(rejected.count == 0u);
            };
            // Recreate the old cross-domain core AND its redundant FLAG snapshot.
            // The rejection must come from rederived geometry, not snapshot inequality.
            auto wrong_core = raw;
            std::size_t first_record{};
            if (binary) {
                const auto body = fixture_bam_sequences(wrong_core);
                first_record = body.front().core_offset;
                const auto bytes = std::span<std::uint8_t>{reinterpret_cast<std::uint8_t*>(wrong_core.data()), wrong_core.size()};
                codec::store_wire_integer(bytes.subspan(first_record + 14u, 2u), std::uint16_t{99u});
                codec::store_wire_integer(bytes.subspan(first_record + 20u, 4u), std::int32_t{1});
                codec::store_wire_integer(bytes.subspan(first_record + 24u, 4u), std::int32_t{19});
                codec::store_wire_integer(bytes.subspan(first_record + 28u, 4u), std::int32_t{17});
            } else {
                constexpr std::string_view old_core{"dna_pair\t73\tnucleotide\t11\t37\t2M\t*\t0\t0\t"};
                first_record = wrong_core.find(old_core); REQUIRE(first_record != std::string::npos);
                wrong_core.replace(first_record, old_core.size(),
                    "dna_pair\t99\tnucleotide\t11\t37\t2M\tbioseq_aa_1\t20\t17\t");
            }
            constexpr std::string_view native_flag{"\"projected_flag\":\"73\""};
            const auto snapshot = wrong_core.find(native_flag, first_record); REQUIRE(snapshot != std::string::npos);
            wrong_core.replace(snapshot, native_flag.size(), "\"projected_flag\":\"99\"");
            reject_projection(wrong_core, "Original alignment projection differs from native core");
            if (!binary) {
                // Add all three original mate tags to BOTH native text and projected_aux.
                // Snapshot equality alone cannot reject this semantically invalid projection.
                auto wrong_auxiliary = raw;
                constexpr std::string_view native_auxiliary{"\tMQ:i:37\tNM:i:0\tMD:Z:2\t"};
                const auto native = wrong_auxiliary.find(native_auxiliary); REQUIRE(native != std::string::npos);
                wrong_auxiliary.insert(native, "\tMC:Z:2M\tR2:Z:AA\tQ2:Z:..");
                constexpr std::string_view marker{"\"projected_aux\":["};
                const auto projected = wrong_auxiliary.find(marker, native); REQUIRE(projected != std::string::npos);
                wrong_auxiliary.insert(projected + marker.size(),
                    "{\"tag\":\"MC\",\"type\":\"Z\",\"value\":\"2M\"},"
                    "{\"tag\":\"R2\",\"type\":\"Z\",\"value\":\"AA\"},"
                    "{\"tag\":\"Q2\",\"type\":\"Z\",\"value\":\"..\"},");
                reject_projection(wrong_auxiliary, "Canonical projected tag has no original value or supported derivation");
            }
        }
    }
}
