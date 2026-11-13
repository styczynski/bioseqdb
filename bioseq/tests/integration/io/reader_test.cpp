/**
 * @file reader_test.cpp
 * @brief Checks FASTA and FASTQ reading through read_sequences_file.
 *
 * Tests multiline records, CRLF, empty files, and small read buffers. Random
 * FASTA input is compared with kseqpp. Other cases cover memory-mapped input,
 * gzip decoding, and dispatching batches to workers.
 */
#include <cstdio>
#include <unistd.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif
#include <zlib.h>
// Canonicalize Highway's internal-linkage templates before importing the batch
// and streaming codec modules (Clang otherwise emits duplicate NEON helpers).
#include <hwy/highway.h>

#include <kseq++/seqio.hpp>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.simd;
import bioseq.io.sources;
import bioseq.io.compression.parallel_gzip_source;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

namespace {

namespace fs = std::filesystem;

/// Writes a temporary file and returns its path. The process ID and atomic
/// counter keep concurrent calls from using the same name.
std::string write_tmp_file(const std::string& content) {
    static std::atomic<std::uint64_t> counter{0};
    fs::path tmp_dir = fs::temp_directory_path();
    fs::path tmp_path = tmp_dir / fmt::format(
        "bioseq_io_reader_{}_{}",
        ::getpid(),
        counter.fetch_add(1, std::memory_order_relaxed));
    std::ofstream out(tmp_path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return tmp_path.string();
}

/// gzip-compress `content` to a temp file for the gzip-path case.
std::string write_tmp_gzip(const std::string& content) {
    static std::atomic<std::uint64_t> counter{0};
    fs::path tmp_dir = fs::temp_directory_path();
    fs::path tmp_path = tmp_dir / fmt::format(
        "bioseq_io_reader_gz_{}_{}.gz",
        ::getpid(),
        counter.fetch_add(1, std::memory_order_relaxed));
    gzFile out = ::gzopen(tmp_path.c_str(), "wb");
    REQUIRE(out != nullptr);
    const int written = ::gzwrite(out, content.data(),
                                  static_cast<unsigned>(content.size()));
    REQUIRE(written == static_cast<int>(content.size()));
    ::gzclose(out);
    return tmp_path.string();
}

/// Decodes packed sequence words to text, leaving out the final padding.
template <typename p_sequence_range>
std::string decode_to_ascii(const p_sequence_range& seq) {
    std::string out;
    using traits = typename p_sequence_range::traits;
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

TEST_CASE("bioseq_io: tiny FASTA dna4 round-trip",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus = ">seq1\nACGT\n"
                               ">seq2 with comment\nAAAACCCCGGGGTTTT\n"
                               ">seq3\nACGTACGTACGTACGTACGTACGTACGTACGT\n";
    const std::string path = write_tmp_file(corpus);

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)}, decode_to_ascii(batch.sequence(i)));
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); });

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

TEST_CASE("bioseq_io: multi-line FASTA dna4",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus = ">seq1\n"
                               "ACGT\n"
                               "ACGT\n"
                               "AC\n"
                               ">seq2\n"
                               "T\n";
    const std::string path = write_tmp_file(corpus);

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)}, decode_to_ascii(batch.sequence(i)));
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(rc.value() == 2);
    REQUIRE(got.size() == 2);
    REQUIRE(got[0].second == "ACGTACGTAC");
    REQUIRE(got[1].second == "T");

    fs::remove(path);
}

TEST_CASE("bioseq_io: CRLF line endings",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus = ">crlf\r\nACGT\r\nACGT\r\n";
    const std::string path = write_tmp_file(corpus);

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)}, decode_to_ascii(batch.sequence(i)));
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].first == "crlf");
    REQUIRE(got[0].second == "ACGTACGT");

    fs::remove(path);
}

TEST_CASE("bioseq_io: empty file",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string path = write_tmp_file("");

    std::vector<std::string> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)});
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(rc.value() == 0);
    REQUIRE(got.empty());

    fs::remove(path);
}

TEST_CASE("bioseq_io: tiny FASTQ dna4 round-trip",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus = "@read1\nACGT\n+\nIIII\n"
                               "@read2 desc\nAAAACCCC\n+ also a comment\n!!!!!!!!\n";
    const std::string path = write_tmp_file(corpus);

    struct record {
        std::string header;
        std::string sequence;
        std::string quality;
    };
    std::vector<record> got;
    std::vector<std::string> errs;

    reader_options opts;
    // Deliver each record separately to exercise retained quality and sequence storage.
    opts.concurrency.batch_target_record_count = 1;

    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fastq>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                auto qspan = batch.quality(i);
                got.push_back(
                    record{std::string{batch.header(i)},
                           decode_to_ascii(batch.sequence(i)),
                           std::string(reinterpret_cast<const char*>(qspan.data()), qspan.size())});
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); },
        opts);

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

// Multiline input and read-buffer boundaries.

TEST_CASE("bioseq_io: multi-line FASTQ qual",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    // Both  the 16-base sequence and its quality string span two lines.
    const std::string corpus = "@multi\n"
                               "ACGTACGT\n"
                               "ACGTACGT\n"
                               "+\n"
                               "IIIIIIII\n"
                               "JJJJJJJJ\n";
    const std::string path = write_tmp_file(corpus);

    struct record {
        std::string header;
        std::string sequence;
        std::string quality;
    };
    std::vector<record> got;
    std::vector<std::string> errs;

    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fastq>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                auto qspan = batch.quality(i);
                got.push_back(
                    record{std::string{batch.header(i)},
                           decode_to_ascii(batch.sequence(i)),
                           std::string(reinterpret_cast<const char*>(qspan.data()), qspan.size())});
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); });

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].header == "multi");
    REQUIRE(got[0].sequence == "ACGTACGTACGTACGT");
    REQUIRE(got[0].quality == "IIIIIIIIJJJJJJJJ");

    fs::remove(path);
}

TEST_CASE("bioseq_io: capacity sweep — varying initial buffer size",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    // Vary header and sequence lengths and line wrapping to cross read-buffer boundaries.
    // Use ACGT sequences for the DNA4 parser.
    std::string corpus;
    corpus.reserve(8192);
    std::vector<std::pair<std::string, std::string>> expected;
    std::mt19937_64 rng(0xCAFEBABEull);
    std::uniform_int_distribution<int> letter_dist(0, 3);
    constexpr std::array<char, 4> dna4{'A', 'C', 'G', 'T'};
    for (int record_index = 0; record_index < 30; ++record_index) {
        const std::size_t header_size =
            5 + (rng() % 17);                      // 5 .. 21 bytes
        const std::size_t body_size =
            5 + (rng() % 257);                     // 5 .. 261 bytes
        std::string header;
        header.reserve(header_size);
        for (std::size_t i = 0; i < header_size; ++i) {
            header.push_back(static_cast<char>('a' + (rng() % 26)));
        }
        std::string body;
        body.reserve(body_size);
        for (std::size_t i = 0; i < body_size; ++i) {
            body.push_back(dna4[letter_dist(rng)]);
        }
        corpus.append(">").append(header).append("\n");
        // Wrap at random columns to exercise multi-line bodies.
        std::size_t emitted = 0;
        const std::size_t wrap_at = 8 + (rng() % 60);  // 8 .. 67
        while (emitted < body_size) {
            const std::size_t this_line =
                std::min(wrap_at, body_size - emitted);
            corpus.append(body, emitted, this_line);
            corpus.push_back('\n');
            emitted += this_line;
        }
        expected.emplace_back(std::move(header), std::move(body));
    }

    const std::string path = write_tmp_file(corpus);

    // Try small buffers that split headers and sequence lines, plus a 1 KiB buffer
    const std::vector<std::size_t> sizes_to_sweep{3, 5, 7, 11, 13, 17, 23, 32, 64, 67, 100, 1024};
    for (const auto initial_size : sizes_to_sweep) {
        reader_options opts;
        opts.io.gzip_auto_detect = false;
        opts.io.prefer_mmap = false;
        opts.io.initial_buffer_size = initial_size;
        opts.concurrency.worker_count = 0;

        std::vector<std::pair<std::string, std::string>> got;
        std::vector<std::string> errs;

        auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
            path,
            [&got](batch_t&& batch) {
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    got.emplace_back(std::string{batch.header(i)},
                                     decode_to_ascii(batch.sequence(i)));
                }
            },
            [&errs](std::string&& msg) { errs.push_back(std::move(msg)); },
            opts);

        REQUIRE(errs.empty());
        REQUIRE(rc.has_value());
        REQUIRE(got.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(got[i].first == expected[i].first);
            REQUIRE(got[i].second == expected[i].second);
        }
    }

    fs::remove(path);
}

TEST_CASE("bioseq_io: differential fuzz vs kseqpp",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    std::mt19937_64 rng(0xDEADBEEFull);
    std::uniform_int_distribution<int> letter_dist(0, 3);
    constexpr std::array<char, 4> dna4{'A', 'C', 'G', 'T'};

    for (int round = 0; round < 50; ++round) {
        const std::size_t record_count = 1 + (rng() % 10);
        std::string corpus;
        corpus.reserve(2048);
        std::vector<std::string> expected_seqs;
        std::vector<std::string> expected_headers;
        const bool use_crlf = (round % 3 == 0);
        for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
            std::string header = fmt::format("seq_{}_{}", round, record_index);
            const std::size_t body_size = 1 + (rng() % 120);
            std::string body;
            body.reserve(body_size);
            for (std::size_t i = 0; i < body_size; ++i) {
                body.push_back(dna4[letter_dist(rng)]);
            }
            corpus.append(">").append(header);
            if (use_crlf)
                corpus.append("\r");
            corpus.push_back('\n');
            corpus.append(body);
            if (use_crlf)
                corpus.append("\r");
            corpus.push_back('\n');
            expected_seqs.push_back(std::move(body));
            expected_headers.push_back(std::move(header));
        }
        const std::string path = write_tmp_file(corpus);

        // Read with Bioseq.
        std::vector<std::pair<std::string, std::string>> ours;
        std::vector<std::string> ours_errs;
        auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
            path,
            [&ours](batch_t&& batch) {
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    ours.emplace_back(std::string{batch.header(i)},
                                      decode_to_ascii(batch.sequence(i)));
                }
            },
            [&ours_errs](std::string&& msg) { ours_errs.push_back(std::move(msg)); });
        REQUIRE(ours_errs.empty());
        REQUIRE(rc.has_value());

        // Read the samj file with kseqpp.
        std::vector<std::pair<std::string, std::string>> theirs;
        {
            klibpp::SeqStreamIn stream(path.c_str());
            klibpp::KSeq record;
            while (stream >> record) {
                std::string joined_header = record.name;
                if (!record.comment.empty()) {
                    joined_header.push_back(' ');
                    joined_header.append(record.comment);
                }
                theirs.emplace_back(std::move(joined_header), record.seq);
            }
        }

        REQUIRE(ours.size() == expected_seqs.size());
        REQUIRE(theirs.size() == expected_seqs.size());
        for (std::size_t i = 0; i < ours.size(); ++i) {
            REQUIRE(ours[i].first == expected_headers[i]);
            REQUIRE(ours[i].second == expected_seqs[i]);
            REQUIRE(theirs[i].first == expected_headers[i]);
            REQUIRE(theirs[i].second == expected_seqs[i]);
        }

        fs::remove(path);
    }
}

TEST_CASE("bioseq_io: mmap path round-trip",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus = ">a\nACGTACGT\n"
                               ">b\nGGGGCCCC\n"
                               ">c\nTTTTTTAA\n";
    const std::string path = write_tmp_file(corpus);

    reader_options opts;
    // Reuse the same reader-owned buffers across completed record callbacks.
    opts.concurrency.batch_target_record_count = 1;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap = true;

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;
    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)}, decode_to_ascii(batch.sequence(i)));
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); },
        opts);

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(got.size() == 3);
    REQUIRE(got[0] == std::pair<std::string, std::string>{"a", "ACGTACGT"});
    REQUIRE(got[1] == std::pair<std::string, std::string>{"b", "GGGGCCCC"});
    REQUIRE(got[2] == std::pair<std::string, std::string>{"c", "TTTTTTAA"});

    fs::remove(path);
}

TEST_CASE("bioseq_io: gzip path round-trip",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    const std::string corpus = ">gz1\nACGT\n"
                               ">gz2\nGGGG\n";
    const std::string path = write_tmp_gzip(corpus);

    reader_options opts;
    // Reuse the same reader-owned buffers across completed record callbacks.
    opts.concurrency.batch_target_record_count = 1;
    opts.io.gzip_auto_detect = true;

    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;
    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        path,
        [&got](batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                got.emplace_back(std::string{batch.header(i)}, decode_to_ascii(batch.sequence(i)));
            }
        },
        [&errs](std::string&& msg) { errs.push_back(std::move(msg)); },
        opts);

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(got.size() == 2);
    REQUIRE(got[0] == std::pair<std::string, std::string>{"gz1", "ACGT"});
    REQUIRE(got[1] == std::pair<std::string, std::string>{"gz2", "GGGG"});

    fs::remove(path);
}

TEST_CASE("bioseq_io: async pipeline (worker_count=2) round-trip",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;

    // Use enough records for several eight-record batches.
    std::string corpus;
    corpus.reserve(64 * 1024);
    std::vector<std::pair<std::string, std::string>> expected;
    std::mt19937_64 rng(0xABCD1234ull);
    std::uniform_int_distribution<int> letter_dist(0, 3);
    constexpr std::array<char, 4> dna4{'A', 'C', 'G', 'T'};
    for (int record_index = 0; record_index < 100; ++record_index) {
        std::string header = fmt::format("async_{}", record_index);
        const std::size_t body_size = 50 + (rng() % 200);
        std::string body;
        body.reserve(body_size);
        for (std::size_t i = 0; i < body_size; ++i) {
            body.push_back(dna4[letter_dist(rng)]);
        }
        corpus.append(">").append(header).append("\n").append(body).append("\n");
        expected.emplace_back(std::move(header), std::move(body));
    }
    const std::string path = write_tmp_file(corpus);

    reader_options opts;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap = false;
    opts.concurrency.worker_count = 2;
    opts.concurrency.batch_target_record_count = 8;
    // Thirteen batches require several joined groups before the final partial batch.
    opts.concurrency.ring_capacity = 2;

    std::mutex got_mutex;
    std::vector<std::pair<std::string, std::string>> got;
    std::vector<std::string> errs;
    auto rc = read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        path,
        [&got, &got_mutex](batch_t&& batch) {
            std::vector<std::pair<std::string, std::string>> local;
            for (std::size_t i = 0; i < batch.size(); ++i) {
                local.emplace_back(std::string{batch.header(i)},
                                   decode_to_ascii(batch.sequence(i)));
            }
            std::lock_guard<std::mutex> guard{got_mutex};
            for (auto& record : local)
                got.push_back(std::move(record));
        },
        [&errs, &got_mutex](std::string&& msg) {
            std::lock_guard<std::mutex> guard{got_mutex};
            errs.push_back(std::move(msg));
        },
        opts);

    REQUIRE(errs.empty());
    REQUIRE(rc.has_value());
    REQUIRE(got.size() == expected.size());
    // Workers may finish batches out of order. Sort both results before comparing.
    std::sort(got.begin(), got.end());
    auto expected_sorted = expected;
    std::sort(expected_sorted.begin(), expected_sorted.end());
    for (std::size_t i = 0; i < expected_sorted.size(); ++i) {
        REQUIRE(got[i] == expected_sorted[i]);
    }

    fs::remove(path);
}

// Compare decompressed bytes from Bioseq and zlib using the human FASTQ fixture.
// Report the first  mismatch. The tests below limit how much of the file is read
// and skip the comparison if the fixture is unavailable.
TEST_CASE("bioseq_io: pgzip vs zlib first divergence",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem][resource:large_memory]") {
    const std::string path =
        std::string{BIOSEQ_IO_TEST_DATA_DIR}
        + "/1000_genomes_human_SRR062634_sample_1_dna5.fastq.gz";
    if (!fs::exists(path)) {
        WARN("Diagnostic comparison skipped — corpus missing: " << path);
        return;
    }

    // Open the zlib reference reader.
    gzFile ref = ::gzopen(path.c_str(), "rb");
    REQUIRE(ref != nullptr);

    // Open the same file through the mapped parallel-gzip reader.
    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect            = true;
    opts.io.prefer_mmap                 = true;
    opts.io.parallel_gzip_enabled       = true;
    opts.io.parallel_gzip_thread_count  = 1;  // use one decoder worker
    auto mmap_or = bioseq::io::mmap_source::try_open_with_magic(path, opts);
    REQUIRE(mmap_or.has_value());
    REQUIRE(mmap_or->is_gzipped);
    auto pgz_or = bioseq::io::parallel_gzip_source::try_open_from_mmap(
        std::move(mmap_or->source), opts);
    REQUIRE(pgz_or.has_value());
    auto& pgzip = *pgz_or;

    constexpr std::size_t k_buffer_bytes = 64 * 1024;
    std::vector<std::uint8_t> ref_buf(k_buffer_bytes);
    std::vector<std::uint8_t> mine_buf(k_buffer_bytes);
    std::size_t total_compared = 0;
    constexpr std::size_t k_maximum_bytes = 100ULL * 1024 * 1024;  // compare at most 100 MiB

    while (total_compared < k_maximum_bytes) {
        const std::size_t want_ref = std::min(k_buffer_bytes, k_maximum_bytes - total_compared);
        const int n_ref = ::gzread(ref, ref_buf.data(),
                                    static_cast<unsigned>(want_ref));
        REQUIRE(n_ref >= 0);
        if (n_ref == 0) break;

        std::size_t got_mine = 0;
        while (got_mine < static_cast<std::size_t>(n_ref)) {
            const auto outcome = pgzip.read_into(
                mine_buf.data() + got_mine,
                static_cast<std::size_t>(n_ref) - got_mine);
            if (outcome.has_error) {
                FAIL("pgzip error at byte " << total_compared + got_mine
                     << ": " << pgzip.last_error_message());
            }
            got_mine += outcome.bytes_read;
            if (outcome.is_eof && got_mine < static_cast<std::size_t>(n_ref)) {
                FAIL("pgzip EOF too early at byte "
                     << total_compared + got_mine);
            }
        }

        for (std::size_t i = 0; i < static_cast<std::size_t>(n_ref); ++i) {
            if (ref_buf[i] != mine_buf[i]) {
                std::fprintf(stderr,
                    "[diag] FIRST DIVERGENCE at byte %zu: zlib=0x%02x mine=0x%02x\n",
                    total_compared + i, static_cast<unsigned>(ref_buf[i]),
                    static_cast<unsigned>(mine_buf[i]));
                std::fprintf(stderr, "[diag] context (zlib):");
                const std::size_t ctx_start = i >= 32 ? i - 32 : 0;
                for (std::size_t k = ctx_start; k < std::min(i + 32,
                        static_cast<std::size_t>(n_ref)); ++k) {
                    std::fprintf(stderr, " %02x", ref_buf[k]);
                }
                std::fprintf(stderr, "\n[diag] context (mine):");
                for (std::size_t k = ctx_start; k < std::min(i + 32,
                        static_cast<std::size_t>(n_ref)); ++k) {
                    std::fprintf(stderr, " %02x", mine_buf[k]);
                }
                std::fprintf(stderr, "\n");
                FAIL("pgzip diverges from zlib at byte "
                     << (total_compared + i));
            }
        }
        total_compared += static_cast<std::size_t>(n_ref);
    }

    ::gzclose(ref);
    INFO("compared " << total_compared << " bytes successfully");
}

TEST_CASE("bioseq_io: pgzip multi-thread vs zlib first divergence",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem][resource:large_memory]") {
    const std::string path =
        std::string{BIOSEQ_IO_TEST_DATA_DIR}
        + "/1000_genomes_human_SRR062634_sample_1_dna5.fastq.gz";
    if (!fs::exists(path)) {
        WARN("Diagnostic comparison skipped — corpus missing: " << path);
        return;
    }

    gzFile ref = ::gzopen(path.c_str(), "rb");
    REQUIRE(ref != nullptr);

    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect            = true;
    opts.io.prefer_mmap                 = true;
    opts.io.parallel_gzip_enabled       = true;
    opts.io.parallel_gzip_thread_count  = 6;  // exercise the parallel pool
    auto mmap_or = bioseq::io::mmap_source::try_open_with_magic(path, opts);
    REQUIRE(mmap_or.has_value());
    REQUIRE(mmap_or->is_gzipped);
    auto pgz_or = bioseq::io::parallel_gzip_source::try_open_from_mmap(
        std::move(mmap_or->source), opts);
    REQUIRE(pgz_or.has_value());
    auto& pgzip = *pgz_or;

    constexpr std::size_t k_buffer_bytes = 64 * 1024;
    std::vector<std::uint8_t> ref_buf(k_buffer_bytes);
    std::vector<std::uint8_t> mine_buf(k_buffer_bytes);
    std::size_t total_compared = 0;
    constexpr std::size_t k_maximum_bytes = 200ULL * 1024 * 1024;

    while (total_compared < k_maximum_bytes) {
        const std::size_t want_ref = std::min(k_buffer_bytes, k_maximum_bytes - total_compared);
        const int n_ref = ::gzread(ref, ref_buf.data(),
                                    static_cast<unsigned>(want_ref));
        REQUIRE(n_ref >= 0);
        if (n_ref == 0) break;

        std::size_t got_mine = 0;
        while (got_mine < static_cast<std::size_t>(n_ref)) {
            const auto outcome = pgzip.read_into(
                mine_buf.data() + got_mine,
                static_cast<std::size_t>(n_ref) - got_mine);
            if (outcome.has_error) {
                FAIL("pgzip(MT) error at byte " << total_compared + got_mine
                     << ": " << pgzip.last_error_message());
            }
            got_mine += outcome.bytes_read;
            if (outcome.is_eof && got_mine < static_cast<std::size_t>(n_ref)) {
                FAIL("pgzip(MT) EOF too early at byte "
                     << total_compared + got_mine
                     << " (expected " << n_ref << ", got " << got_mine << ")");
            }
        }

        for (std::size_t i = 0; i < static_cast<std::size_t>(n_ref); ++i) {
            if (ref_buf[i] != mine_buf[i]) {
                std::fprintf(stderr,
                    "[diag-mt] FIRST DIVERGENCE at byte %zu: zlib=0x%02x mine=0x%02x\n",
                    total_compared + i, static_cast<unsigned>(ref_buf[i]),
                    static_cast<unsigned>(mine_buf[i]));
                std::fprintf(stderr, "[diag-mt] context (zlib):");
                const std::size_t ctx_start = i >= 32 ? i - 32 : 0;
                for (std::size_t k = ctx_start; k < std::min(i + 32,
                        static_cast<std::size_t>(n_ref)); ++k) {
                    std::fprintf(stderr, " %02x", ref_buf[k]);
                }
                std::fprintf(stderr, "\n[diag-mt] context (mine):");
                for (std::size_t k = ctx_start; k < std::min(i + 32,
                        static_cast<std::size_t>(n_ref)); ++k) {
                    std::fprintf(stderr, " %02x", mine_buf[k]);
                }
                std::fprintf(stderr, "\n");
                FAIL("pgzip(MT) diverges from zlib at byte "
                     << (total_compared + i));
            }
        }
        total_compared += static_cast<std::size_t>(n_ref);
    }

    ::gzclose(ref);
    INFO("compared " << total_compared << " bytes successfully");
}

TEST_CASE("FASTQ streams quality spans with strict boundaries", "[component:bioseq][layer:integration][feature:quality][resource:filesystem]") {
    using namespace bioseq::io;
    // Compare every byte/position against scalar predicates. Guard both ends
    // of the readable page so the exact bounded spans also catch overreads.
#if defined(__unix__) || defined(__APPLE__)
    const long page_result = ::sysconf(_SC_PAGESIZE);
    REQUIRE(page_result > 0);
    const auto page_size = static_cast<std::size_t>(page_result);
    REQUIRE(page_size <= std::numeric_limits<std::size_t>::max() / 3u);
#if defined(MAP_ANONYMOUS)
    constexpr int anonymous_map_flag = MAP_ANONYMOUS;
#else
    constexpr int anonymous_map_flag = MAP_ANON;
#endif
    struct guarded_mapping {
        void* address;
        std::size_t bytes;
        ~guarded_mapping() { if (address != MAP_FAILED) static_cast<void>(::munmap(address, bytes)); }
    } mapping{::mmap(nullptr, 3u * page_size, PROT_NONE,
        MAP_PRIVATE | anonymous_map_flag, -1, 0), 3u * page_size};
    REQUIRE(mapping.address != MAP_FAILED);
    auto* const readable = static_cast<std::uint8_t*>(mapping.address) + page_size;
    REQUIRE(::mprotect(readable, page_size, PROT_READ | PROT_WRITE) == 0);
    std::fill_n(readable, page_size, std::uint8_t{'I'});
    const hwy::HWY_NAMESPACE::ScalableTag<std::uint8_t> tag;
    const auto scan_size = 4u * hwy::HWY_NAMESPACE::Lanes(tag) + 3u;
    REQUIRE(scan_size <= page_size);
    auto* const tail = readable + page_size - scan_size;
    for (unsigned value = 0; value < 256u; ++value) {
        const auto byte = static_cast<std::uint8_t>(value);
        for (std::size_t offset = 0; offset < scan_size; ++offset) {
            CAPTURE(value, offset, scan_size);
            tail[offset] = byte;
            REQUIRE(simd::scan_fastq_quality_prefix(tail, scan_size)
                == (value >= 33u && value <= 126u ? scan_size : offset));
            REQUIRE(simd::scan_first_byte_pair('\n', '\r', tail, scan_size)
                == (byte == '\n' || byte == '\r' ? offset : scan_size));
            tail[offset] = 'I';
        }
    }
    for (std::size_t size = 0; size <= scan_size; ++size) {
        CAPTURE(size, scan_size);
        // Vary every starting alignment and tail length through the unrolled
        // scan. Empty input ends exactly at the inaccessible right-hand page.
        auto* const end_aligned = readable + page_size - size;
        REQUIRE(simd::scan_fastq_quality_prefix(end_aligned, size) == size);
        REQUIRE(simd::scan_first_byte_pair('\n', '\r', end_aligned, size) == size);
        REQUIRE(simd::scan_fastq_quality_prefix(readable, size) == size);
        REQUIRE(simd::scan_first_byte_pair('\n', '\r', readable, size) == size);
        if (size != 0) {
            end_aligned[size - 1u] = 0;
            REQUIRE(simd::scan_fastq_quality_prefix(end_aligned, size) == size - 1u);
            end_aligned[size - 1u] = '\r';
            REQUIRE(simd::scan_first_byte_pair('\n', '\r', end_aligned, size) == size - 1u);
            end_aligned[size - 1u] = 'I';
        }
    }
#endif
    struct sink_type {
        std::string quality;
        std::size_t committed{}, chunks{}, largest{}, sequence_symbols{}, largest_sequence_chunk{};
        void begin_record(std::string_view) { quality.clear(); }
        void chunk(packed_sequence_chunk<std::uint64_t> chunk) {
            sequence_symbols += chunk.logical_length;
            largest_sequence_chunk = std::max(largest_sequence_chunk, static_cast<std::size_t>(chunk.logical_length));
        }
        void quality_chunk(std::span<const std::uint8_t> bytes, std::size_t offset) {
            REQUIRE(offset == quality.size());
            quality.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            ++chunks; largest = std::max(largest, bytes.size());
        }
        void end_record() { ++committed; }
    };
    for (std::size_t buffer = 1; buffer <= 33; ++buffer) {
        const auto path = write_tmp_file("@r\r\nACGTACGT\r\n+\r\n!@+;\r\n~I?J\r\n");
        sink_type sink;
        auto options = default_reader_options;
        options.io.initial_buffer_size = buffer;
        options.io.prefer_mmap = false;
        std::vector<std::string> errors;
        const auto result = stream_sequences_file<seq::dna4, std::uint64_t, sequence_file_format::fastq>(
            path.c_str(), sink, [&](std::string&& message) { errors.push_back(std::move(message)); }, options);
        REQUIRE(errors.empty()); REQUIRE(result.has_value());
        REQUIRE(sink.committed == 1); REQUIRE(sink.quality == "!@+;~I?J");
        fs::remove(path);
    }
    for (const auto& bad : std::vector<std::string>{"!!!", "!!!!!", "!! !!", "!!\t!", "!!\r!", "!!\x7f!", "!!\xff!", std::string{"!!\0!", 4}}) {
        const auto path = write_tmp_file("@r\nACGT\n+\n" + bad + "\n");
        sink_type sink;
        auto kind = input_failure_kind::other;
        std::string diagnostic;
        const auto result = stream_sequences_file<seq::dna4, std::uint64_t, sequence_file_format::fastq>(
            path.c_str(), sink, [&](std::string&& message, input_failure_kind failure_kind = input_failure_kind::other) {
                diagnostic = std::move(message); kind = failure_kind;
            });
        REQUIRE_FALSE(result); REQUIRE(sink.committed == 0);
        const bool bad_byte = std::ranges::any_of(bad, [](unsigned char byte) { return byte < 33 || byte > 126; });
        REQUIRE(kind == (bad_byte ? input_failure_kind::invalid_quality_byte : input_failure_kind::other));
        if (bad_byte) { REQUIRE(diagnostic.contains("quality offset 2")); REQUIRE(diagnostic.contains("record 'r'")); }
        fs::remove(path);
    }
    {
        auto& pool = bioseq::parallelism::global_pool::instance();
        struct restore_separator_pool {
            bioseq::parallelism::global_pool& pool;
            bool active{pool.is_active()};
            bioseq::parallelism::pool_config config{pool.maximum_concurrency(), pool.get_mode()};
            ~restore_separator_pool() { if (active) pool.configure(config); else pool.shutdown(); }
        } restore{pool};
        pool.configure({2u, bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel});
        // An ignored separator comment must stream through a fixed-size slab.
        // This length places CR at the last byte of a 16-byte refill for CRLF.
        const std::string long_separator = "@r\nACGT\n+" + std::string(4102u, 'x');
        auto separator_options = default_reader_options;
        separator_options.io.initial_buffer_size = 16u;
        separator_options.io.max_buffer_size = 16u;
        separator_options.io.prefer_mmap = false;
        separator_options.io.gzip_auto_detect = false;
        for (const std::string_view ending : {"\n", "\r\n", ""}) {
            CAPTURE(ending);
            const auto input = long_separator + std::string{ending}
                + (ending.empty() ? "" : "!@+~\n");
            const auto separator_path = write_tmp_file(input);
            sink_type separator_sink;
            std::string diagnostic;
            auto kind = input_failure_kind::other;
            const auto read = stream_sequences_file<seq::dna4, std::uint64_t, sequence_file_format::fastq>(
                separator_path.c_str(), separator_sink,
                [&diagnostic, &kind](std::string&& message, input_failure_kind failure_kind = input_failure_kind::other) {
                    diagnostic = std::move(message); kind = failure_kind;
                }, separator_options);
            if (ending.empty()) {
                REQUIRE_FALSE(read.has_value()); REQUIRE(separator_sink.committed == 0u);
                REQUIRE(kind == input_failure_kind::other);
                REQUIRE(diagnostic == fmt::format("truncated FASTQ record at file position {}", input.size()));
                REQUIRE(read.error() == diagnostic);
            } else {
                REQUIRE(read.has_value()); REQUIRE(*read == 1u); REQUIRE(diagnostic.empty());
                REQUIRE(separator_sink.committed == 1u); REQUIRE(separator_sink.quality == "!@+~");
                REQUIRE(separator_sink.sequence_symbols == 4u);
            }
            for (const std::size_t workers : {0u, 2u}) {
                CAPTURE(workers);
                auto batch_options = separator_options;
                batch_options.concurrency.worker_count = workers;
                batch_options.concurrency.batch_target_record_count = 1u;
                using batch_type = sequence_batch<sequence_custom_alphabet<seq::dna4, std::uint64_t>>;
                std::vector<std::tuple<std::string, std::string, std::string>> records;
                std::string batch_diagnostic;
                // One input record schedules at most one handler; the pipeline joins
                // it before the retained values are checked below.
                const auto batch_read = read_sequences_file<seq::dna4, sequence_file_format::fastq>(
                    separator_path, [&records](batch_type&& batch) {
                        for (std::size_t index = 0; index < batch.size(); ++index) {
                            const auto quality = batch.quality(index);
                            records.emplace_back(batch.header(index), decode_to_ascii(batch.sequence(index)),
                                std::string{reinterpret_cast<const char*>(quality.data()), quality.size()});
                        }
                    }, [&batch_diagnostic](std::string&& message) { batch_diagnostic = std::move(message); }, batch_options);
                if (ending.empty()) {
                    REQUIRE_FALSE(batch_read.has_value()); REQUIRE(records.empty());
                    REQUIRE(batch_diagnostic == diagnostic); REQUIRE(batch_read.error() == diagnostic);
                } else {
                    REQUIRE(batch_read.has_value()); REQUIRE(*batch_read == 1u); REQUIRE(batch_diagnostic.empty());
                    REQUIRE(records.size() == 1u);
                    REQUIRE(std::get<0>(records.front()) == "r");
                    REQUIRE(std::get<1>(records.front()) == "ACGT");
                    REQUIRE(std::get<2>(records.front()) == "!@+~");
                }
            }
            fs::remove(separator_path);
        }
        const auto bad_separator_path = write_tmp_file(long_separator + "\n!! !!\n");
        sink_type bad_separator_sink;
        std::string separator_diagnostic;
        auto separator_kind = input_failure_kind::other;
        const auto bad_separator = stream_sequences_file<seq::dna4, std::uint64_t, sequence_file_format::fastq>(
            bad_separator_path.c_str(), bad_separator_sink,
            [&separator_diagnostic, &separator_kind](std::string&& message, input_failure_kind kind = input_failure_kind::other) {
                separator_diagnostic = std::move(message); separator_kind = kind;
            }, separator_options);
        REQUIRE_FALSE(bad_separator.has_value()); REQUIRE(bad_separator_sink.committed == 0u);
        REQUIRE(separator_kind == input_failure_kind::invalid_quality_byte);
        REQUIRE(separator_diagnostic == fmt::format(
            "invalid FASTQ quality byte 32 at quality offset 2 in record 'r' (file position {})",
            long_separator.size() + 3u));
        REQUIRE(bad_separator.error() == separator_diagnostic);
        fs::remove(bad_separator_path);
    }
    const std::string long_quality(200003, 'I');
    const auto path = write_tmp_file("@r\n" + std::string(long_quality.size(), 'A') + "\n+\n" + long_quality);
    sink_type sink;
    auto options = default_reader_options;
    options.io.initial_buffer_size = 1024;
    options.io.max_buffer_size = 1024;
    const auto result = stream_sequences_file<seq::dna4, std::uint64_t, sequence_file_format::fastq>(
        path.c_str(), sink, [](std::string&&) {}, options);
    REQUIRE(result.has_value()); REQUIRE(sink.quality == long_quality);
    REQUIRE(sink.largest <= 65536); REQUIRE(sink.chunks >= 4); REQUIRE(sink.committed == 1);
    REQUIRE(sink.sequence_symbols == long_quality.size());
    REQUIRE(sink.largest_sequence_chunk <= 1024 + 32);
    fs::remove(path);
}

TEST_CASE("FASTQ gzip readers preserve raw quality bytes across decoder boundaries",
          "[component:bioseq][layer:integration][feature:quality][resource:filesystem]") {
    using namespace bioseq::io;
    using batch_type = sequence_batch<sequence_custom_alphabet<seq::dna4, std::uint64_t>>;
    auto& pool = bioseq::parallelism::global_pool::instance();
    struct restore_pool {
        bioseq::parallelism::global_pool& pool;
        bool active{pool.is_active()};
        bioseq::parallelism::pool_config config{pool.maximum_concurrency(), pool.get_mode()};
        ~restore_pool() { if (active) pool.configure(config); else pool.shutdown(); }
    } restore{pool};
    std::string quality;
    for (std::size_t i = 0; i < 65539; ++i) quality.push_back(static_cast<char>(33 + i % 94));
    quality[0] = '@'; quality[1] = '+';
    const std::string sequence(quality.size(), 'A');
    const auto path = write_tmp_gzip("@first\r\n" + sequence + "\r\n+\r\n" + quality +
        "\r\n@second\n" + sequence + "\n+\n" + quality);
    for (const unsigned concurrency : {0u, 1u, 5u}) {
        if (concurrency == 0) pool.shutdown();
        else pool.configure({concurrency, bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel});
        auto options = default_reader_options;
        options.io.parallel_gzip_enabled = true;
        options.io.parallel_gzip_min_compressed_bytes = 1;
        options.io.parallel_gzip_thread_count = 2;
        options.io.parallel_gzip_chunk_bytes = 32768;
        options.io.initial_buffer_size = 1024;
        options.io.max_buffer_size = 1024;
        options.concurrency.worker_count = 1;
        options.concurrency.parser_count = 1;
        reader_backend backend{};
        std::vector<std::pair<std::string, std::string>> got;
        const auto result = read_sequences_file<seq::dna4, sequence_file_format::fastq>(path,
            [&](batch_type&& batch) {
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    const auto bytes = batch.quality(i);
                    got.emplace_back(decode_to_ascii(batch.sequence(i)),
                        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                }
            }, [](std::string&&) {}, options, &backend);
        REQUIRE(result.has_value()); REQUIRE(*result == 2);
        REQUIRE(backend == (concurrency == 1 ? reader_backend::gzip : reader_backend::parallel_gzip));
        REQUIRE(got.size() == 2);
        for (const auto& value : got) { REQUIRE(value.first == sequence); REQUIRE(value.second == quality); }
    }
    fs::remove(path);
}

TEST_CASE("FASTQ mapped shards follow complete records through marker quality lines",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using batch_type = sequence_batch<sequence_custom_alphabet<seq::dna4, std::uint64_t>>;
    auto& pool = bioseq::parallelism::global_pool::instance();
    struct restore_pool {
        bioseq::parallelism::global_pool& pool;
        bool active{pool.is_active()};
        bioseq::parallelism::pool_config config{pool.maximum_concurrency(), pool.get_mode()};
        ~restore_pool() { if (active) pool.configure(config); else pool.shutdown(); }
    } restore{pool};
    pool.configure({4u, bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel});
    using record = std::tuple<std::string, std::string, std::string>;
    const std::string witness = "@q\n" + std::string(32, 'A') + "\n+\nIIIIII\n@fake\nACGT\n+\n" + std::string(16, 'I') + "\n";
    const std::string witnessed_quality = "IIIIII@fakeACGT+" + std::string(16, 'I');
    const std::string multiline = "@multi\r\nAC\r\nGT\r\n+comment\r\nI@\r\n+!\r\n@empty\n+\n\n@last\nT\n+\n@";
    const std::string lenient = "@first\nAC\n@second\nGT\n+\n!!\n";
    struct fixture_type { std::string input; bool strict; std::vector<record> expected; };
    const std::vector<fixture_type> fixtures{
        {witness, true, {{"q", std::string(32, 'A'), witnessed_quality}}},
        {witness + multiline, true, {{"q", std::string(32, 'A'), witnessed_quality},
            {"multi", "ACGT", "I@+!"}, {"empty", "", ""}, {"last", "T", "@"}}},
        {lenient, false, {{"first", "AC", ""}, {"second", "GT", "!!"}}},
        {"@empty\n+\n", true, {{"empty", "", ""}}},
        {"", true, {}}};
    for (const auto& fixture : fixtures) {
        const auto path = write_tmp_file(fixture.input);
        std::vector<record> sequential;
        for (const std::size_t parsers : {1u, 2u, 4u}) {
            CAPTURE(parsers, fixture.input, fixture.strict);
            auto options = default_reader_options;
            options.concurrency.worker_count = 0;
            options.concurrency.parser_count = parsers;
            options.concurrency.mmap_parallel_min_bytes = 1;
            options.concurrency.batch_target_record_count = 1;
            options.encoder.strict_fastq = fixture.strict;
            std::vector<batch_type> retained;
            std::vector<std::string> errors;
            const auto read = read_sequences_file<seq::dna4, sequence_file_format::fastq>(path,
                [&retained](batch_type&& batch) { retained.push_back(std::move(batch)); },
                [&errors](std::string&& error) { errors.push_back(std::move(error)); }, options);
            REQUIRE(read.has_value()); REQUIRE(errors.empty()); REQUIRE(*read == fixture.expected.size());
            std::vector<record> actual;
            for (const auto& batch : retained)
                for (std::size_t index = 0; index < batch.size(); ++index) {
                    const auto quality = batch.quality(index);
                    actual.emplace_back(batch.header(index), decode_to_ascii(batch.sequence(index)),
                        quality.empty() ? std::string{} : std::string{reinterpret_cast<const char*>(quality.data()), quality.size()});
                }
            // Inter-shard delivery order is intentionally unspecified; names and multiplicities stay exact.
            std::ranges::sort(actual);
            auto expected = fixture.expected; std::ranges::sort(expected);
            REQUIRE(actual == expected);
            if (parsers == 1) sequential = actual;
            else REQUIRE(actual == sequential);
        }
        fs::remove(path);
    }
    for (const auto& invalid : std::vector<std::string>{
            "@bad\nAA\n+\n!\n", "@bad\nAA\n+\n!!!\n", "@bad\nAZ\n+\n!!\n",
            "@bad\nAA\n+\n! \n", "@bad\nA\rA\n+\n!!\n", "@bad\nAA\n+\n!\r!\n",
            "@bad\nAA\n", "@bad", "@bad\nAA\n+", "@bad\nAA\n+\n!!\r"}) {
        const auto path = write_tmp_file(invalid);
        for (const std::size_t parsers : {1u, 2u, 4u}) {
            auto options = default_reader_options;
            options.concurrency.worker_count = 0; options.concurrency.parser_count = parsers;
            options.concurrency.mmap_parallel_min_bytes = 1;
            std::vector<std::string> errors;
            const auto read = read_sequences_file<seq::dna4, sequence_file_format::fastq>(path,
                [](batch_type&&) {}, [&errors](std::string&& error) { errors.push_back(std::move(error)); }, options);
            REQUIRE_FALSE(read.has_value()); REQUIRE(errors.size() == 1u);
        }
        fs::remove(path);
    }
}

TEST_CASE("Text reader slab initialization refuses unavailable storage before delivery",
          "[component:bioseq][layer:integration][feature:io][resource:filesystem]") {
    using namespace bioseq::io;
    using batch_type = sequence_batch<sequence_custom_alphabet<seq::dna4, std::uint64_t>>;
    auto& pool = bioseq::parallelism::global_pool::instance();
    struct restore_pool {
        bioseq::parallelism::global_pool& pool;
        bool active{pool.is_active()};
        bioseq::parallelism::pool_config config{pool.maximum_concurrency(), pool.get_mode()};
        ~restore_pool() { if (active) pool.configure(config); else pool.shutdown(); }
    } restore{pool};
    pool.configure({2u, bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel});
    struct stream_sink {
        std::size_t begun{}, chunks{}, committed{}, symbols{};
        std::string header, sequence, quality;
        void begin_record(std::string_view value) { ++begun; header = value; }
        void chunk(packed_sequence_chunk<std::uint64_t> value) {
            ++chunks; symbols += value.logical_length;
            const auto capacity = value.words.size() * batch_type::sequence_view::traits::letters_per_unit;
            sequence += decode_to_ascii(batch_type::sequence_view{value.words.data(), value.words.size(),
                capacity - static_cast<std::size_t>(value.logical_length)});
        }
        void quality_chunk(std::span<const std::uint8_t> value, std::size_t) {
            quality.append(reinterpret_cast<const char*>(value.data()), value.size());
        }
        void end_record() { ++committed; }
    };
    struct owning_stream_sink : stream_sink {
        std::vector<std::vector<std::uint64_t>> retained;
        void chunk_owned(std::vector<std::uint64_t>&& words, std::uint64_t length, bool final) {
            stream_sink::chunk({std::span<const std::uint64_t>{words}, length, final});
            retained.push_back(std::move(words));
        }
    };
    struct initial_bounds { std::size_t initial{}, maximum{}; bool success{}; };
    const std::array<initial_bounds, 4> cases{{
        {16u, 0u, false},
        {std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(), false},
        {0u, 64u, true},
        {std::numeric_limits<std::size_t>::max(), 64u, true}}};
    for (const auto format : {sequence_file_format::fasta, sequence_file_format::fastq}) {
        struct remove_file {
            std::string path;
            ~remove_file() { std::error_code ignored; fs::remove(path, ignored); }
        } input{write_tmp_file(format == sequence_file_format::fasta ? ">r\nACGT\n" : "@r\nACGT\n+\nIIII\n")};
        for (const bool mapped : {false, true})
            for (const std::size_t workers : {0u, 2u})
                for (const auto bounds : cases) {
                    CAPTURE(format, mapped, workers, bounds.initial, bounds.maximum);
                    auto options = default_reader_options;
                    options.io.prefer_mmap = mapped; options.io.mmap_zero_copy = false;
                    options.io.gzip_auto_detect = false;
                    options.io.initial_buffer_size = bounds.initial; options.io.max_buffer_size = bounds.maximum;
                    options.concurrency.worker_count = workers; options.concurrency.parser_count = 1;
                    std::atomic<std::size_t> batches{}, rows{};
                    std::atomic<bool> content_matches{true};
                    std::vector<std::pair<std::string, input_failure_kind>> errors;
                    const auto failure = [&errors](std::string&& message, input_failure_kind kind = input_failure_kind::other) {
                        errors.emplace_back(std::move(message), kind);
                    };
                    const auto handler = [&batches, &rows, &content_matches](batch_type&& batch) {
                        batches.fetch_add(1u); rows.fetch_add(batch.size());
                        for (std::size_t ordinal{}; ordinal < batch.size(); ++ordinal)
                            if (batch.header(ordinal) != "r" || decode_to_ascii(batch.sequence(ordinal)) != "ACGT")
                                content_matches.store(false);
                    };
                    reader_backend backend{reader_backend::unknown};
                    const auto read = format == sequence_file_format::fasta
                        ? read_sequences_file<seq::dna4, sequence_file_format::fasta>(input.path, handler, failure, options, &backend)
                        : read_sequences_file<seq::dna4, sequence_file_format::fastq>(input.path, handler, failure, options, &backend);
                    REQUIRE(backend == (mapped ? reader_backend::memory_mapped : reader_backend::posix_read));
                    if (bounds.success) {
                        REQUIRE(read.has_value()); REQUIRE(*read == 1u); REQUIRE(errors.empty());
                        REQUIRE(batches.load() == 1u); REQUIRE(rows.load() == 1u); REQUIRE(content_matches.load());
                    } else {
                        REQUIRE_FALSE(read.has_value()); REQUIRE(errors.size() == 1u);
                        REQUIRE(errors.front().first == read.error()); REQUIRE(errors.front().second == input_failure_kind::other);
                        REQUIRE(batches.load() == 0u); REQUIRE(rows.load() == 0u);
                    }
                    if (workers != 0u) continue;
                    const auto check_stream = [&input, &options, &failure, &errors, format, bounds]<class Sink>() {
                        Sink sink; errors.clear();
                        const auto streamed = format == sequence_file_format::fasta
                            ? stream_sequences_file<seq::dna4, std::uint64_t, sequence_file_format::fasta>(input.path.c_str(), sink, failure, options)
                            : stream_sequences_file<seq::dna4, std::uint64_t, sequence_file_format::fastq>(input.path.c_str(), sink, failure, options);
                        if (bounds.success) {
                            REQUIRE(streamed.has_value()); REQUIRE(*streamed == 1u); REQUIRE(errors.empty());
                            REQUIRE(sink.begun == 1u); REQUIRE(sink.committed == 1u); REQUIRE(sink.symbols == 4u);
                            REQUIRE(sink.header == "r"); REQUIRE(sink.sequence == "ACGT");
                            REQUIRE(sink.quality == (format == sequence_file_format::fastq ? "IIII" : ""));
                            if constexpr (requires { sink.retained; }) {
                                REQUIRE(sink.retained.size() == 1u);
                                const auto& words = sink.retained.front();
                                const auto padding = words.size() * batch_type::sequence_view::traits::letters_per_unit - 4u;
                                REQUIRE(decode_to_ascii(batch_type::sequence_view{words.data(), words.size(), padding}) == "ACGT");
                            }
                        } else {
                            REQUIRE_FALSE(streamed.has_value()); REQUIRE(errors.size() == 1u);
                            REQUIRE(errors.front().first == streamed.error()); REQUIRE(errors.front().second == input_failure_kind::other);
                            REQUIRE(sink.begun == 0u); REQUIRE(sink.chunks == 0u); REQUIRE(sink.committed == 0u);
                            REQUIRE(sink.sequence.empty()); REQUIRE(sink.quality.empty());
                            if constexpr (requires { sink.retained; }) REQUIRE(sink.retained.empty());
                        }
                    };
                    check_stream.template operator()<stream_sink>();
                    check_stream.template operator()<owning_stream_sink>();
                }
    }
}
