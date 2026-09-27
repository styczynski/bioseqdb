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
#include <zlib.h>

#include <kseq++/seqio.hpp>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.io.sources;
import bioseq.io.compression.parallel_gzip_source;

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

    // Both the 16-base sequence and its quality string span two lines.
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

    // Try small buffers that split headers and sequence lines, plus a 1 KiB buffer.
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

        // Read the same file with kseqpp.
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
// Report the first mismatch. The tests below limit how much of the file is read
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
