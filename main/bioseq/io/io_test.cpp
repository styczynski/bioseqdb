/**
 * @file io_test.cpp
 * @brief Correctness tests for `bioseq::io::read_sequences_file`.
 *
 * v1 covers: tiny FASTA round-trip, FASTQ round-trip, empty file,
 * mixed CRLF / LF, multi-line FASTA. The exhaustive capacity sweep and
 * differential-fuzz vs. kseqpp tests in spec §19 are phase-5 follow-ups.
 */
#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include <kseq++/seqio.hpp>

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.public_api;
import bioseq_io.options;

namespace {

namespace fs = std::filesystem;

/// Write `content` to a temp file and return the path. The path is
/// unique-per-test via Catch2's `getCurrentTestName`.
std::string write_tmp_file(const std::string& content) {
    static std::atomic<std::uint64_t> counter{0};
    fs::path tmp_dir = fs::temp_directory_path();
    fs::path tmp_path = tmp_dir / fmt::format(
        "bioseq_io_test_{}_{}",
        ::getpid(),
        counter.fetch_add(1, std::memory_order_relaxed));
    std::ofstream out(tmp_path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return tmp_path.string();
}

/// Decode a `compact_sequence_range<sequence_custom_alphabet<dna4, uint64_t>>`
/// back to ASCII via the alphabet's symbol iterator. Used to compare
/// against the original input.
template <typename SequenceRange>
std::string decode_to_ascii(const SequenceRange& seq) {
    std::string out;
    using traits = typename SequenceRange::traits;
    const std::size_t total_letters =
        seq.size() * traits::letters_per_unit - seq.padding();
    out.reserve(total_letters);
    std::size_t emitted = 0;
    for (auto unit : seq) {
        for (std::size_t lane = 0; lane < traits::letters_per_unit; ++lane) {
            if (emitted == total_letters) break;
            const auto rank = static_cast<std::uint8_t>(
                (unit >> (traits::bits_per_char * lane)) & traits::char_mask);
            out.push_back(traits::letter_to_char(rank));
            ++emitted;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("bioseq_io: tiny FASTA dna4 round-trip", "[io-test-correctness]") {

    klibpp::KSeq record;

    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus =
        ">seq1\nACGT\n"
        ">seq2 with comment\nAAAACCCCGGGGTTTT\n"
        ">seq3\nACGTACGTACGTACGTACGTACGTACGTACGT\n";
    const std::string path = write_tmp_file(corpus);

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_io_file_type::FASTA>(
        path,
        [&](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)},
                                 decode_to_ascii(batch.sequence(i)));
            }
        },
        [&](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(rc.value() == 3);
    REQUIRE(got.size() == 3);
    REQUIRE(got[0].first == "seq1");
    REQUIRE(got[0].second == "ACGT");
    REQUIRE(got[1].first == "seq2 with comment");
    REQUIRE(got[1].second == "AAAACCCCGGGGTTTT");
    REQUIRE(got[2].first == "seq3");
    REQUIRE(got[2].second == "ACGTACGTACGTACGTACGTACGTACGTACGT");

    fs::remove(path);
}

TEST_CASE("bioseq_io: multi-line FASTA dna4", "[io-test-correctness]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus =
        ">seq1\n"
        "ACGT\n"
        "ACGT\n"
        "AC\n"
        ">seq2\n"
        "T\n";
    const std::string path = write_tmp_file(corpus);

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_io_file_type::FASTA>(
        path,
        [&](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)},
                                 decode_to_ascii(batch.sequence(i)));
            }
        },
        [&](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(rc.value() == 2);
    REQUIRE(got.size() == 2);
    REQUIRE(got[0].second == "ACGTACGTAC");
    REQUIRE(got[1].second == "T");

    fs::remove(path);
}

TEST_CASE("bioseq_io: CRLF line endings", "[io-test-correctness]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus = ">crlf\r\nACGT\r\nACGT\r\n";
    const std::string path = write_tmp_file(corpus);

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_io_file_type::FASTA>(
        path,
        [&](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)},
                                 decode_to_ascii(batch.sequence(i)));
            }
        },
        [&](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].first == "crlf");
    REQUIRE(got[0].second == "ACGTACGT");

    fs::remove(path);
}

TEST_CASE("bioseq_io: empty file", "[io-test-correctness]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string path = write_tmp_file("");

    std::vector<std::string> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_io_file_type::FASTA>(
        path,
        [&](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)});
            }
        },
        [&](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(rc.value() == 0);
    REQUIRE(got.empty());

    fs::remove(path);
}

TEST_CASE("bioseq_io: tiny FASTQ dna4 round-trip", "[io-test-correctness]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus =
        "@read1\nACGT\n+\nIIII\n"
        "@read2 desc\nAAAACCCC\n+ also a comment\n!!!!!!!!\n";
    const std::string path = write_tmp_file(corpus);

    struct record {
        std::string header;
        std::string sequence;
        std::string quality;
    };
    std::vector<record> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_io_file_type::FASTQ>(
        path,
        [&](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                auto qspan = batch.quality(i);
                got.push_back(record{
                    std::string{batch.header(i)},
                    decode_to_ascii(batch.sequence(i)),
                    std::string(reinterpret_cast<const char*>(qspan.data()),
                                qspan.size())});
            }
        },
        [&](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(rc.value() == 2);
    REQUIRE(got.size() == 2);
    REQUIRE(got[0].header == "read1");
    REQUIRE(got[0].sequence == "ACGT");
    REQUIRE(got[0].quality == "IIII");
    REQUIRE(got[1].header == "read2 desc");
    REQUIRE(got[1].sequence == "AAAACCCC");
    REQUIRE(got[1].quality == "!!!!!!!!");

    fs::remove(path);
}
