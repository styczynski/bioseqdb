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
import bioseq.io.simd;
import bioseq.io.file_kind;
import bioseq.io.alignment.record_codec;
import bioseq.io.alignment;
import bioseq.io.record_schema;
import bioseq.io.reader;
import bioseq.io.sinks;
import bioseq.io.write_pipeline;
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
            REQUIRE(codec::retain_original_native_auxiliary(differences, false, sam_text) == input.nonnegative);
            REQUIRE(codec::retain_original_native_auxiliary(score, false, sam_text));
            REQUIRE(codec::retain_original_native_auxiliary(score, true, sam_text));
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
            REQUIRE(codec::retain_original_native_auxiliary(field, false, sam_text) == input.valid);
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
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(*parsed, false, sam_text));
            REQUIRE(codec::retain_original_native_auxiliary(custom, false, sam_text));
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
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(field, false, false));
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(field, false, true));
            auto custom = field;
            custom.tag = {'Z', 'F'};
            REQUIRE(codec::retain_original_native_auxiliary(custom, false, false));
            REQUIRE_FALSE(codec::retain_original_native_auxiliary(custom, false, true));
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
    const std::vector<bioseq::io::record_header_context>& contexts = {}) {
    namespace codec = bioseq::io::alignment;
    bioseq::io::record_source_metadata metadata;
    metadata.references = references;
    metadata.header_contexts = contexts;
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
          "[component:bioseq][layer:unit][feature:io]") {
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
    known.mate_reference_length = 42;
    REQUIRE_FALSE(codec::prepare_native_alignment(alignment, 65, 2, known, false).has_value());
}
