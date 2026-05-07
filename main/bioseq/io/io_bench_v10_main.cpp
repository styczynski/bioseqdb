/**
 * @file io_bench_v10_main.cpp
 * @brief IO bench entry point on top of bench_framework (V10).
 *
 * Replaces the legacy Catch2-based harness in `io/io_bench_main.cpp` and
 * its surrounding `io/io_bench/` tree. Three benchmark blocks declared
 * in `io_bench_v10_configuration.json`:
 *
 *   * bench_io_readers_plain — POSIX / mmap / async-pipeline readers + kseqpp + naive_ifstream
 *   * bench_io_readers_gz    — parallel-gzip + zlib paths on the .fastq.gz cell
 *   * bench_io_writers       — sync / parallel4 / parallel8 bioseq writers + naive_ofstream
 *
 * Each block sets `concurrency.bench_lock = true` and
 * `concurrency.concurrency = false` so cells run serially — IO paths
 * spawn many internal worker threads, and overlapping cells stalls the
 * box.
 */
#include "common.h"

#include <kseq++/seqio.hpp>
#include <simdjson.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.public_api;
import bioseq_io.options;
import bioseq_io.writer;
import bench_framework;

#ifndef BIOSEQ_IO_REPORT_DIR
#error "BIOSEQ_IO_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_DATA_DIR
#error "BIOSEQ_IO_DATA_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_BENCH_CONFIG_PATH
#error "BIOSEQ_IO_BENCH_CONFIG_PATH must be defined by the build system"
#endif

namespace fs = std::filesystem;

namespace {

using bioseq::bench_framework::ParamMap;
using bioseq::io::reader_options;
using bioseq::io::writer_options;
using bioseq::io::sequence_batch;
using bioseq::io::sequence_io_file_type;
using bioseq::io::sequence_writer;
using bioseq::io::read_sequences_file;

using BenchAlphabet = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using BenchBatch    = sequence_batch<BenchAlphabet>;

/// Reader Input. Owns the on-disk corpus path. Synthetic corpora are
/// generated in `on_input` and deleted in `on_cleanup`. Real-data
/// corpora are referenced by absolute path; never deleted.
struct ReaderInput {
    std::string  corpus_path;
    std::size_t  bytes_on_disk        = 0;
    std::size_t  decompressed_bytes   = 0;
    std::size_t  expected_record_count = 0;
    bool         is_fastq             = false;
    bool         is_compressed        = false;
    bool         is_synthetic         = false;
};

/// Writer Input. Same shape as ReaderInput, but additionally the source
/// corpus is slurped into memory once so the timed loop only measures
/// the writer.
struct WriterInput {
    std::string  source_corpus_path;
    std::size_t  source_bytes_on_disk        = 0;
    std::size_t  source_record_count         = 0;
    bool         is_fastq                    = false;
    bool         is_synthetic_source         = false;

    /// Per-iteration scratch path. Each `on_run` call writes here, then
    /// `on_evaluate` re-reads to validate roundtrip integrity. Created
    /// once in `on_input`; deleted in `on_cleanup`.
    std::string  output_scratch_path;

    /// In-memory corpus for the writer loop. Populated in `on_input`.
    std::vector<BenchBatch> batches;

    /// XOR-aggregated content hash of `batches`, computed once in
    /// `on_input` for the FASTQ shape (= header || seq ASCII || qual
    /// truncated to encoded length). The `on_evaluate` lambda
    /// recomputes this from the writer's on-disk output and compares
    /// — any mismatch surfaces as `roundtrip_record_match = 0`.
    /// `_fastq` and `_fasta` variants because the FASTQ form folds in
    /// quality bytes; FASTA only header + seq.
    std::uint64_t expected_content_hash_fastq = 0;
    std::uint64_t expected_content_hash_fasta = 0;
};

struct IoOutput {
    std::size_t observed_record_count = 0;
    std::size_t observed_body_bytes   = 0;
    std::size_t bytes_written         = 0;     ///< writer outputs only
    std::size_t roundtrip_record_count = 0;    ///< writer outputs only
};

[[nodiscard]] std::int64_t read_int_field(simdjson::dom::element data,
                                            std::string_view name,
                                            std::int64_t fallback) noexcept {
    std::int64_t value = fallback;
    std::int64_t parsed = 0;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

[[nodiscard]] std::string read_string_field(simdjson::dom::element data,
                                              std::string_view name,
                                              std::string_view fallback) {
    std::string_view raw{};
    if (data[name].get(raw) == simdjson::SUCCESS) {
        return std::string{raw};
    }
    return std::string{fallback};
}

[[nodiscard]] bool read_bool_field(simdjson::dom::element data,
                                    std::string_view name,
                                    bool fallback) noexcept {
    bool value = fallback;
    bool parsed = false;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

/// Allocate a unique scratch path under the system temp directory.
[[nodiscard]] std::string allocate_temp_path(std::string_view tag) {
    static std::atomic<std::uint64_t> counter{0};
    fs::path temp_directory_root = fs::temp_directory_path();
    fs::path full_path = temp_directory_root / fmt::format(
        "bioseq_iobench_{}_{}_{}_{}",
        tag, ::getpid(),
        counter.fetch_add(1, std::memory_order_relaxed),
        std::random_device{}() & 0xFFFFFFu);
    return full_path.string();
}

/// Write a synthetic FASTA / FASTQ corpus to `output_path`. Returns
/// the number of bytes actually written.
[[nodiscard]] std::size_t emit_synthetic_corpus(const std::string& output_path,
                                                  bool is_fastq,
                                                  std::size_t record_count,
                                                  std::size_t mean_record_length_bp,
                                                  std::uint64_t seed) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        loge("[io_bench_v10] failed to open '{}' for writing", output_path);
        return 0;
    }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> base_dist(0, 3);
    std::normal_distribution<double> length_dist(
        static_cast<double>(mean_record_length_bp),
        static_cast<double>(mean_record_length_bp) * 0.1);
    constexpr std::array<char, 4> bases{'A', 'C', 'G', 'T'};
    constexpr std::size_t fasta_wrap = 80;

    std::string buffer;
    buffer.reserve(64 * 1024);
    std::size_t bytes_written = 0;
    auto flush_buffer = [&]() {
        if (buffer.empty()) return;
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes_written += buffer.size();
        buffer.clear();
    };

    for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
        const double sample = length_dist(rng);
        const std::size_t record_length = static_cast<std::size_t>(
            std::max(8.0, sample));
        if (is_fastq) {
            buffer.append(fmt::format("@seq_{}\n", record_index));
        } else {
            buffer.append(fmt::format(">seq_{}\n", record_index));
        }
        if (buffer.size() >= 32 * 1024) flush_buffer();

        if (is_fastq) {
            for (std::size_t letter_index = 0; letter_index < record_length; ++letter_index) {
                buffer.push_back(bases[base_dist(rng)]);
            }
            buffer.push_back('\n');
            buffer.push_back('+');
            buffer.push_back('\n');
            buffer.append(record_length, 'I');
            buffer.push_back('\n');
            if (buffer.size() >= 32 * 1024) flush_buffer();
        } else {
            std::size_t emitted_letters = 0;
            while (emitted_letters < record_length) {
                const std::size_t this_line =
                    std::min(fasta_wrap, record_length - emitted_letters);
                for (std::size_t letter_index = 0; letter_index < this_line; ++letter_index) {
                    buffer.push_back(bases[base_dist(rng)]);
                }
                buffer.push_back('\n');
                emitted_letters += this_line;
                if (buffer.size() >= 32 * 1024) flush_buffer();
            }
        }
    }
    flush_buffer();
    out.flush();
    out.close();
    return bytes_written;
}

/// Common attributes resolved from a test-case `data` blob: where the
/// corpus lives, how big it is, whether it was generated locally (and
/// thus needs `on_cleanup` to delete it). Returned by
/// `prepare_corpus`; reader / writer `on_input` lambdas copy whichever
/// fields they care about into their own `Input` struct.
struct CorpusDescriptor {
    std::string path;
    std::size_t bytes_on_disk      = 0;
    std::size_t record_count       = 0;  ///< 0 for real-data (unknown until parse)
    bool        is_fastq           = false;
    bool        is_compressed      = false;
    bool        is_synthetic       = false;
    bool        valid              = false;
};

/// Resolve a corpus's on-disk path. For real-data cells, look up the
/// path under `data_dir`; for synthetic, generate to a temp path.
[[nodiscard]] CorpusDescriptor prepare_corpus(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data) {
    CorpusDescriptor descriptor;
    const std::string format_name = read_string_field(data, "format", "fasta");
    descriptor.is_fastq = (format_name == "fastq");
    descriptor.is_compressed = read_bool_field(data, "compressed", false);
    const std::string kind = read_string_field(data, "kind", "synthetic");

    if (kind == "real") {
        const std::string relative = read_string_field(data, "real_data_path", "");
        if (relative.empty()) {
            loge("[io_bench_v10] test case '{}' missing real_data_path",
                  test_case_name);
            return descriptor;
        }
        fs::path joined = fs::path{std::string{data_dir}} / relative;
        std::error_code ec;
        if (!fs::exists(joined, ec)) {
            loge("[io_bench_v10] real-data file missing for '{}': {}",
                  test_case_name, joined.string());
            return descriptor;
        }
        const auto observed_size = fs::file_size(joined, ec);
        if (ec) {
            loge("[io_bench_v10] cannot stat '{}': {}",
                  joined.string(), ec.message());
            return descriptor;
        }
        descriptor.path = joined.string();
        descriptor.bytes_on_disk = static_cast<std::size_t>(observed_size);
        descriptor.is_synthetic = false;
        descriptor.valid = true;
        return descriptor;
    }
    if (kind != "synthetic") {
        loge("[io_bench_v10] test case '{}' has unknown kind '{}'",
              test_case_name, kind);
        return descriptor;
    }

    const std::size_t record_count = static_cast<std::size_t>(
        read_int_field(data, "record_count", 1000));
    const std::size_t mean_record_length_bp = static_cast<std::size_t>(
        read_int_field(data, "mean_record_length_bp", 200));
    const std::uint64_t seed = static_cast<std::uint64_t>(
        read_int_field(data, "seed", 42));

    const std::string path_tag =
        std::string{format_name} + "_" + std::string{test_case_name};
    descriptor.path = allocate_temp_path(path_tag);
    const std::size_t bytes = emit_synthetic_corpus(
        descriptor.path, descriptor.is_fastq,
        record_count, mean_record_length_bp, seed);
    if (bytes == 0) {
        loge("[io_bench_v10] synthetic corpus generation failed for '{}'",
              test_case_name);
        descriptor.path.clear();
        return descriptor;
    }
    descriptor.bytes_on_disk = bytes;
    descriptor.record_count  = record_count;
    descriptor.is_synthetic  = true;
    descriptor.valid         = true;
    return descriptor;
}

// =====================================================================
//  Reader runners
// =====================================================================
//
// One `dispatch_reader_*` function per reader algorithm. Each returns
// the IoOutput populated with observed_record_count + observed_body_bytes.

template <bool IsFasta>
[[nodiscard]] IoOutput run_bioseq_io_with_options(const std::string& path,
                                                    const reader_options& options) {
    using alphabet_config = BenchAlphabet;
    using batch_t = sequence_batch<alphabet_config>;
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;

    std::atomic<std::size_t> record_count_atomic{0};
    std::atomic<std::size_t> body_bytes_atomic{0};

    auto handler_fn = [&record_count_atomic, &body_bytes_atomic](batch_t&& batch) {
        const std::size_t local_records = batch.size();
        std::size_t local_body_bytes = 0;
        for (std::size_t record_index = 0; record_index < local_records; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            using traits = typename decltype(seq_view)::traits;
            local_body_bytes += seq_view.size() * traits::letters_per_unit
                                 - seq_view.padding();
        }
        record_count_atomic.fetch_add(local_records, std::memory_order_relaxed);
        body_bytes_atomic.fetch_add(local_body_bytes, std::memory_order_relaxed);
    };
    auto failure_fn = [&path](std::string&& msg) {
        loge("[io_bench_v10] read failed on '{}': {}", path, msg);
    };
    auto outcome = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        loge("[io_bench_v10] read_sequences_file error: {}", outcome.error());
    }
    IoOutput out;
    out.observed_record_count = record_count_atomic.load(std::memory_order_relaxed);
    out.observed_body_bytes   = body_bytes_atomic.load(std::memory_order_relaxed);
    return out;
}

[[nodiscard]] IoOutput run_kseqpp(const std::string& path) {
    IoOutput out;
    klibpp::SeqStreamIn stream(path.c_str());
    klibpp::KSeq record;
    while (stream >> record) {
        out.observed_body_bytes += record.seq.size();
        out.observed_record_count += 1;
    }
    return out;
}

[[nodiscard]] IoOutput run_naive_ifstream_fasta(const std::string& path) {
    IoOutput out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        loge("[io_bench_v10] naive ifstream open failed: '{}'", path);
        return out;
    }
    std::string line;
    bool in_record = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.front() == '>') {
            if (in_record) out.observed_record_count += 1;
            in_record = true;
            continue;
        }
        if (in_record) {
            std::size_t body_size = line.size();
            if (body_size > 0 && line[body_size - 1] == '\r') body_size -= 1;
            out.observed_body_bytes += body_size;
        }
    }
    if (in_record) out.observed_record_count += 1;
    return out;
}

[[nodiscard]] IoOutput run_naive_ifstream_fastq(const std::string& path) {
    IoOutput out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        loge("[io_bench_v10] naive ifstream open failed: '{}'", path);
        return out;
    }
    std::string line;
    enum class FastqStep : std::uint8_t { Header, Seq, Sep, Qual };
    FastqStep step = FastqStep::Header;
    while (std::getline(in, line)) {
        switch (step) {
            case FastqStep::Header:
                if (line.empty() || line.front() != '@') continue;
                step = FastqStep::Seq;
                break;
            case FastqStep::Seq: {
                std::size_t body_size = line.size();
                if (body_size > 0 && line[body_size - 1] == '\r') body_size -= 1;
                out.observed_body_bytes += body_size;
                step = FastqStep::Sep;
                break;
            }
            case FastqStep::Sep:
                step = FastqStep::Qual;
                break;
            case FastqStep::Qual:
                out.observed_record_count += 1;
                step = FastqStep::Header;
                break;
        }
    }
    return out;
}

[[nodiscard]] IoOutput dispatch_reader(std::string_view algo,
                                         const ReaderInput& input) {
    reader_options options{};
    if (algo == "bioseq_io_sync_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_mmap_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = true;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_async4_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 4;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_sync_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_mmap_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = true;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_async4_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 4;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_pgzip_fastq") {
        options.io.gzip_auto_detect = true;
        options.io.prefer_mmap      = true;
        options.io.parallel_gzip_enabled = true;
        options.io.parallel_gzip_thread_count = 0;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_zlib_fastq") {
        options.io.gzip_auto_detect = true;
        options.io.prefer_mmap      = true;
        options.io.parallel_gzip_enabled = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "kseqpp_fasta" || algo == "kseqpp_fastq") {
        return run_kseqpp(input.corpus_path);
    }
    if (algo == "naive_ifstream_fasta") {
        return run_naive_ifstream_fasta(input.corpus_path);
    }
    if (algo == "naive_ifstream_fastq") {
        return run_naive_ifstream_fastq(input.corpus_path);
    }
    loge("[io_bench_v10] unknown reader algorithm '{}'", algo);
    return {};
}

// =====================================================================
//  Reader benchmark lambdas
// =====================================================================

[[nodiscard]] ReaderInput build_reader_input(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data) {
    const CorpusDescriptor descriptor =
        prepare_corpus(data_dir, test_case_name, data);
    ReaderInput input;
    if (!descriptor.valid) return input;
    input.corpus_path           = descriptor.path;
    input.bytes_on_disk         = descriptor.bytes_on_disk;
    input.expected_record_count = descriptor.record_count;
    input.is_fastq              = descriptor.is_fastq;
    input.is_compressed         = descriptor.is_compressed;
    input.is_synthetic          = descriptor.is_synthetic;
    // For gzipped real-data: estimate decompressed size at ~3.2x compressed
    // (matches the SRR FASTQ corpus shape — empirical 6.0 GiB / 1.96 GiB).
    input.decompressed_bytes = input.is_compressed
        ? static_cast<std::size_t>(input.bytes_on_disk * 3.2)
        : input.bytes_on_disk;
    JustLog("[io_bench_v10] reader corpus ready: '{}' (path={}, bytes={})",
            test_case_name, input.corpus_path, input.bytes_on_disk);
    return input;
}

[[nodiscard]] IoOutput run_reader(std::string_view algo,
                                    const ParamMap& /*params*/,
                                    const ReaderInput& input) {
    if (input.corpus_path.empty()) return {};
    return dispatch_reader(algo, input);
}

[[nodiscard]] std::map<std::string, double> evaluate_reader(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const ReaderInput& input,
    const IoOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("bytes_on_disk", static_cast<double>(input.bytes_on_disk));
    metrics.emplace("decompressed_bytes", static_cast<double>(input.decompressed_bytes));
    metrics.emplace("record_count", static_cast<double>(output.observed_record_count));
    // Body bytes observed by the reader. For dna4-on-dna5 corpora this
    // is < raw seq bytes (N's get dropped); for dna4-on-dna4 it equals
    // the raw seq bytes. Surface as a metric so equality rules between
    // bioseq paths can detect a path silently dropping or duplicating
    // bytes that the others see.
    metrics.emplace("observed_body_bytes",
                    static_cast<double>(output.observed_body_bytes));
    // Sanity: the reader must observe at least one record on a
    // non-empty corpus. Surface as a 0/1 metric so the regression
    // layer can pin it to 1 across iterations and test cases.
    metrics.emplace("records_nonzero",
                    output.observed_record_count > 0 ? 1.0 : 0.0);
    return metrics;
}

void cleanup_reader(std::string_view /*data_dir*/,
                    std::string_view /*test_case_name*/,
                    simdjson::dom::element /*data*/,
                    const ReaderInput& input)
{
    if (!input.is_synthetic || input.corpus_path.empty()) return;
    std::error_code ec;
    fs::remove(input.corpus_path, ec);
}

// =====================================================================
//  Writer benchmark lambdas
// =====================================================================

/// XOR-hash a single record's content (header || '\1' || decoded
/// sequence ASCII || '\1' || quality bytes truncated to encoded length).
/// Returns a 64-bit hash. XOR is order-invariant — the parallel writer
/// reorders records across producer threads, so a hash based on a sorted
/// list would be hostile to that path; XOR of per-record hashes does not
/// care about order.
///
/// The truncation `qsize = min(quality.size(), total_letters)` mirrors
/// `sequence_writer::write_batch`: when the input alphabet drops bytes
/// (e.g. dna4 reading dna5 inputs silently drops 'N's), the encoded
/// sequence is shorter than the raw quality span, and the writer
/// truncates qual to total_letters. We hash the same truncated form
/// here so input and output hashes agree under that contract.
///
/// Hash collisions on a single record are statistically unlikely
/// (1/2^64); after XORing N records, the chance of a false-positive
/// match remains far below 1 in 10^9 for realistic N.
template <bool IsFastq>
[[nodiscard]] std::uint64_t hash_record_xor(
    std::string_view header,
    const auto& sequence_view,
    std::span<const std::uint8_t> quality)
{
    using seq_traits = typename std::remove_cvref_t<decltype(sequence_view)>::traits;
    constexpr std::size_t letters_per_word = seq_traits::letters_per_unit;
    const std::size_t total_letters =
        sequence_view.size() * letters_per_word - sequence_view.padding();
    std::string serialized;
    serialized.reserve(header.size() + total_letters
                       + (IsFastq ? quality.size() : 0) + 2);
    serialized.append(header);
    serialized.push_back('\1');
    for (std::size_t letter_index = 0; letter_index < total_letters; ++letter_index) {
        const auto word = sequence_view.data()[letter_index / letters_per_word];
        const std::size_t letter_in_word = letter_index % letters_per_word;
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (seq_traits::bits_per_char * letter_in_word))
            & seq_traits::char_mask);
        serialized.push_back(seq_traits::letter_to_char(rank));
    }
    if constexpr (IsFastq) {
        serialized.push_back('\1');
        const std::size_t qsize = std::min(quality.size(), total_letters);
        serialized.append(reinterpret_cast<const char*>(quality.data()), qsize);
    }
    return std::hash<std::string>{}(serialized);
}

/// Compute the XOR-aggregated content hash of every record across all
/// batches. Order-invariant — safe to use as the canonical content
/// fingerprint for the writer's input AND for the re-read output (the
/// parallel writer may reorder records but cannot change the SET of
/// records). Returns 0 for an empty batch list.
template <bool IsFastq>
[[nodiscard]] std::uint64_t hash_batches_xor(
    const std::vector<BenchBatch>& batches)
{
    std::uint64_t accumulator = 0;
    for (const auto& batch : batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0;
             record_index < record_count_in_batch; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            std::span<const std::uint8_t> qual_view{};
            if constexpr (IsFastq) {
                qual_view = batch.quality(record_index);
            }
            accumulator ^= hash_record_xor<IsFastq>(
                header_view, seq_view, qual_view);
        }
    }
    return accumulator;
}

/// Slurp `path` into an in-memory list of batches via the bioseq reader.
template <bool IsFasta>
[[nodiscard]] bool slurp_corpus(const std::string& path,
                                  std::vector<BenchBatch>& out_batches,
                                  std::size_t& out_record_count) {
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;

    out_batches.clear();
    out_record_count = 0;

    auto handler_fn = [&](BenchBatch&& batch) {
        out_record_count += batch.size();
        out_batches.push_back(std::move(batch));
    };
    auto failure_fn = [&path](std::string&& msg) {
        loge("[io_bench_v10] slurp failed on '{}': {}", path, msg);
    };
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap      = true;
    options.concurrency.worker_count = 0;

    auto outcome = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        loge("[io_bench_v10] slurp_corpus read error: {}", outcome.error());
        return false;
    }
    return !out_batches.empty();
}

[[nodiscard]] WriterInput build_writer_input(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data) {
    WriterInput input;

    // The .fastq.gz cell is reader-only; writers don't have a compress
    // path. Skip silently.
    const bool compressed_hint = read_bool_field(data, "compressed", false);
    if (compressed_hint) {
        JustLog("[io_bench_v10] writer skipping gz cell '{}'", test_case_name);
        return input;
    }
    const CorpusDescriptor descriptor =
        prepare_corpus(data_dir, test_case_name, data);
    if (!descriptor.valid) return input;
    input.source_corpus_path    = descriptor.path;
    input.source_bytes_on_disk  = descriptor.bytes_on_disk;
    input.source_record_count   = descriptor.record_count;
    input.is_fastq              = descriptor.is_fastq;
    input.is_synthetic_source   = descriptor.is_synthetic;

    bool slurp_ok = false;
    if (input.is_fastq) {
        slurp_ok = slurp_corpus<false>(
            input.source_corpus_path, input.batches, input.source_record_count);
    } else {
        slurp_ok = slurp_corpus<true>(
            input.source_corpus_path, input.batches, input.source_record_count);
    }
    if (!slurp_ok) {
        loge("[io_bench_v10] writer slurp failed for '{}' — clearing input",
              test_case_name);
        input.batches.clear();
        return input;
    }
    // Compute the expected content hash once. `on_evaluate` will
    // re-read the writer's output and compute the same hash; mismatches
    // signal silent corruption (truncated qualities, header-without-body
    // chunks, dropped records, etc.).
    if (input.is_fastq) {
        input.expected_content_hash_fastq = hash_batches_xor<true>(input.batches);
    } else {
        input.expected_content_hash_fasta = hash_batches_xor<false>(input.batches);
    }
    input.output_scratch_path = allocate_temp_path(
        std::string{"writer_out_"} + std::string{test_case_name});
    JustLog("[io_bench_v10] writer corpus ready: '{}' (records={}, scratch={})",
            test_case_name, input.source_record_count, input.output_scratch_path);
    return input;
}

template <bool IsFasta>
[[nodiscard]] IoOutput run_bioseq_writer(const WriterInput& input,
                                           std::size_t thread_count,
                                           bool use_mmap_sink = false) {
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;
    writer_options options{};
    if (use_mmap_sink) {
        // Pick a generous upper bound for the output size — has to be
        // large enough that no record write overflows the mmap'd
        // region, but sparse-file metadata makes overestimation cheap.
        // 1.5× the slurped raw bytes leaves headroom for FASTA wrap
        // newlines / FASTQ separator bytes the writer adds back, even
        // after accounting for multi-line bodies in the source corpus
        // collapsing to single-line bodies in the writer's output.
        const std::size_t estimated_output_bytes =
            input.source_bytes_on_disk + input.source_bytes_on_disk / 2;
        options.output_capacity_hint =
            estimated_output_bytes > 4096 ? estimated_output_bytes : 4096;
    }
    auto writer_or_err = sequence_writer<seq::dna4, file_type>::open(
        input.output_scratch_path, options);
    if (!writer_or_err) {
        loge("[io_bench_v10] writer open failed: {}", writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    const std::size_t batches_total = input.batches.size();
    if (thread_count <= 1 || batches_total <= 1) {
        for (const auto& batch : input.batches) {
            if (!writer.write_batch(batch)) {
                loge("[io_bench_v10] writer write_batch failed");
                (void)writer.close();
                return {};
            }
        }
    } else {
        const std::size_t resolved_threads =
            thread_count > batches_total ? batches_total : thread_count;
        std::atomic<bool> any_failure{false};
        auto run_shard = [&](std::size_t shard_index) {
            const std::size_t shard_start =
                (batches_total * shard_index) / resolved_threads;
            const std::size_t shard_end =
                (batches_total * (shard_index + 1)) / resolved_threads;
            for (std::size_t batch_index = shard_start; batch_index < shard_end; ++batch_index) {
                if (!writer.write_batch(input.batches[batch_index])) {
                    any_failure.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        };
        std::vector<std::thread> shard_threads;
        shard_threads.reserve(resolved_threads);
        for (std::size_t shard_index = 0; shard_index < resolved_threads; ++shard_index) {
            shard_threads.emplace_back(run_shard, shard_index);
        }
        for (auto& thread : shard_threads) thread.join();
        if (any_failure.load(std::memory_order_relaxed)) {
            (void)writer.close();
            return {};
        }
    }
    auto close_outcome = writer.close();
    if (!close_outcome) {
        loge("[io_bench_v10] writer close failed: {}", close_outcome.error());
        return {};
    }
    IoOutput out;
    out.observed_record_count = input.source_record_count;
    std::error_code ec;
    out.bytes_written = fs::file_size(input.output_scratch_path, ec);
    return out;
}

template <bool IsFasta>
[[nodiscard]] IoOutput run_naive_ofstream(const WriterInput& input) {
    std::ofstream out(input.output_scratch_path, std::ios::binary);
    if (!out) {
        loge("[io_bench_v10] naive ofstream open failed: '{}'",
              input.output_scratch_path);
        return {};
    }
    std::string scratch;
    scratch.reserve(64 * 1024);
    constexpr std::size_t fasta_wrap = 80;

    for (const auto& batch : input.batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0; record_index < record_count_in_batch; ++record_index) {
            scratch.clear();
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            using seq_traits = typename std::remove_cvref_t<decltype(seq_view)>::traits;
            constexpr std::size_t letters_per_word = seq_traits::letters_per_unit;
            const std::size_t total_letters =
                seq_view.size() * letters_per_word - seq_view.padding();
            scratch.push_back(IsFasta ? '>' : '@');
            scratch.append(header_view);
            scratch.push_back('\n');
            std::size_t in_line_count = 0;
            for (std::size_t letter_index = 0; letter_index < total_letters; ++letter_index) {
                const auto word = seq_view.data()[letter_index / letters_per_word];
                const std::size_t letter_in_word = letter_index % letters_per_word;
                const std::uint8_t rank = static_cast<std::uint8_t>(
                    (word >> (seq_traits::bits_per_char * letter_in_word))
                    & seq_traits::char_mask);
                // Use the alphabet's canonical letter_to_char mapping
                // so we don't bake in the dna4 LUT shape — works for
                // any alphabet supported by `BenchAlphabet`.
                scratch.push_back(seq_traits::letter_to_char(rank));
                if constexpr (IsFasta) {
                    ++in_line_count;
                    if (in_line_count == fasta_wrap &&
                        letter_index + 1 < total_letters) {
                        scratch.push_back('\n');
                        in_line_count = 0;
                    }
                }
            }
            if constexpr (!IsFasta) {
                scratch.push_back('\n');
                scratch.push_back('+');
                scratch.push_back('\n');
                const auto quality_view = batch.quality(record_index);
                // Truncate quality to match the encoded sequence length.
                // Mirrors `sequence_writer::write_batch`: when the input
                // alphabet drops bytes (e.g. dna4 reading dna5 inputs
                // silently drops 'N's), the encoded sequence is shorter
                // than the raw quality span. Writing the full
                // quality_view here would emit qual_len > seq_len,
                // causing the FASTQ FSM to abort on re-read with
                // "FASTQ qual length mismatch (qual=X, seq=Y)".
                const std::size_t qsize = std::min(quality_view.size(),
                                                    total_letters);
                scratch.append(reinterpret_cast<const char*>(quality_view.data()),
                                qsize);
                if (qsize < total_letters) {
                    scratch.append(total_letters - qsize, '!');
                }
            }
            scratch.push_back('\n');
            out.write(scratch.data(), static_cast<std::streamsize>(scratch.size()));
        }
    }
    out.flush();
    out.close();
    IoOutput result;
    result.observed_record_count = input.source_record_count;
    std::error_code ec;
    result.bytes_written = fs::file_size(input.output_scratch_path, ec);
    return result;
}

[[nodiscard]] IoOutput dispatch_writer(std::string_view algo,
                                         const WriterInput& input) {
    if (algo == "bioseq_writer_sync_fasta")      return run_bioseq_writer<true>(input, 1);
    if (algo == "bioseq_writer_parallel4_fasta") return run_bioseq_writer<true>(input, 4);
    if (algo == "bioseq_writer_parallel8_fasta") return run_bioseq_writer<true>(input, 8);
    // mmap_pwrite_sink path: writer.cppm `output_capacity_hint > 0` —
    // bypasses the kernel pwrite memcpy by writing into a mmap'd region
    // that the OS flushes asynchronously. Validates the second sink
    // implementation in sinks.cppm. Uses 4 producer threads to also
    // exercise the per-thread chunk path against the mmap sink.
    if (algo == "bioseq_writer_mmap_fasta")      return run_bioseq_writer<true>(input, 4, /*use_mmap_sink=*/true);
    if (algo == "bioseq_writer_sync_fastq")      return run_bioseq_writer<false>(input, 1);
    if (algo == "bioseq_writer_parallel4_fastq") return run_bioseq_writer<false>(input, 4);
    if (algo == "bioseq_writer_parallel8_fastq") return run_bioseq_writer<false>(input, 8);
    if (algo == "bioseq_writer_mmap_fastq")      return run_bioseq_writer<false>(input, 4, /*use_mmap_sink=*/true);
    if (algo == "naive_ofstream_fasta")          return run_naive_ofstream<true>(input);
    if (algo == "naive_ofstream_fastq")          return run_naive_ofstream<false>(input);
    loge("[io_bench_v10] unknown writer algorithm '{}'", algo);
    return {};
}

[[nodiscard]] IoOutput run_writer(std::string_view algo,
                                    const ParamMap& /*params*/,
                                    const WriterInput& input) {
    if (input.batches.empty() || input.output_scratch_path.empty()) return {};
    return dispatch_writer(algo, input);
}

/// Re-read the writer's output to count records AND verify byte-faithful
/// content. Returns 1.0 only if both:
///   1. record count matches the input slurp;
///   2. the XOR-aggregated content hash matches the input slurp's hash.
/// Either failure returns 0.0 — the bench then surfaces it as a
/// roundtrip violation. This catches silent corruption that the v10
/// "count-only" check missed (e.g. truncated quality in the naive
/// writer, headers split from bodies by mid-record flushes).
template <bool IsFasta>
[[nodiscard]] double roundtrip_match(const std::string& output_path,
                                       std::size_t expected_record_count,
                                       std::uint64_t expected_content_hash) {
    if (output_path.empty()) return 0.0;
    std::error_code ec;
    if (!fs::exists(output_path, ec) || fs::file_size(output_path, ec) == 0) {
        return 0.0;
    }
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;
    std::atomic<std::size_t> record_count{0};
    std::atomic<std::uint64_t> content_hash{0};
    auto handler_fn = [&record_count, &content_hash](BenchBatch&& batch) {
        record_count.fetch_add(batch.size(), std::memory_order_relaxed);
        // Hash this batch's contribution and XOR into the global
        // accumulator. XOR is associative + commutative, so concurrent
        // batch handlers contribute correctly via fetch_xor.
        const std::uint64_t batch_hash = hash_batches_xor<!IsFasta>(
            std::vector<BenchBatch>{std::move(batch)});
        content_hash.fetch_xor(batch_hash, std::memory_order_relaxed);
    };
    auto failure_fn = [&output_path](std::string&& msg) {
        loge("[io_bench_v10] roundtrip read failed on '{}': {}",
              output_path, msg);
    };
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap      = true;
    options.concurrency.worker_count = 0;
    auto outcome = read_sequences_file<seq::dna4, file_type>(
        output_path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) return 0.0;
    const std::size_t observed_records =
        record_count.load(std::memory_order_relaxed);
    const std::uint64_t observed_hash =
        content_hash.load(std::memory_order_relaxed);
    if (observed_records != expected_record_count) {
        loge("[io_bench_v10] roundtrip record count mismatch on '{}': "
             "expected={}, observed={}",
             output_path, expected_record_count, observed_records);
        return 0.0;
    }
    if (observed_hash != expected_content_hash) {
        loge("[io_bench_v10] roundtrip content hash mismatch on '{}': "
             "expected=0x{:016x}, observed=0x{:016x}",
             output_path, expected_content_hash, observed_hash);
        return 0.0;
    }
    return 1.0;
}

[[nodiscard]] std::map<std::string, double> evaluate_writer(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const WriterInput& input,
    const IoOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("bytes_on_disk", static_cast<double>(output.bytes_written));
    metrics.emplace("record_count", static_cast<double>(output.observed_record_count));
    // bytes_written must be > 0 — a zero-byte output is a writer failure
    // even when no FSM error fires on re-read (empty file = 0 records,
    // record-count "matches" if expected_record_count is also 0). We
    // surface this as a separate metric so the regression layer can
    // pin it to a non-zero floor.
    metrics.emplace("output_bytes_nonzero",
                    output.bytes_written > 0 ? 1.0 : 0.0);
    const double match = input.is_fastq
        ? roundtrip_match<false>(input.output_scratch_path,
                                  input.source_record_count,
                                  input.expected_content_hash_fastq)
        : roundtrip_match<true>(input.output_scratch_path,
                                 input.source_record_count,
                                 input.expected_content_hash_fasta);
    metrics.emplace("roundtrip_record_match", match);
    return metrics;
}

void cleanup_writer(std::string_view /*data_dir*/,
                    std::string_view /*test_case_name*/,
                    simdjson::dom::element /*data*/,
                    const WriterInput& input)
{
    std::error_code ec;
    if (input.is_synthetic_source && !input.source_corpus_path.empty()) {
        fs::remove(input.source_corpus_path, ec);
    }
    if (!input.output_scratch_path.empty()) {
        fs::remove(input.output_scratch_path, ec);
    }
}

}  // namespace

int main(int argc, char** argv) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::INF);

    const std::string config_path{BIOSEQ_IO_BENCH_CONFIG_PATH};
    const std::string report_dir{BIOSEQ_IO_REPORT_DIR};
    const std::string data_dir{BIOSEQ_IO_DATA_DIR};

    using bioseq::bench_framework::create_benchmark;

    using ReaderBuild = std::function<ReaderInput(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element)>;
    using ReaderRun = std::function<IoOutput(std::string_view,
                                                const ParamMap&,
                                                const ReaderInput&)>;
    using ReaderEval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const ReaderInput&, const IoOutput&)>;
    using ReaderCleanup = std::function<void(std::string_view,
                                                std::string_view,
                                                simdjson::dom::element,
                                                const ReaderInput&)>;

    using WriterBuild = std::function<WriterInput(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element)>;
    using WriterRun = std::function<IoOutput(std::string_view,
                                                const ParamMap&,
                                                const WriterInput&)>;
    using WriterEval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const WriterInput&, const IoOutput&)>;
    using WriterCleanup = std::function<void(std::string_view,
                                                std::string_view,
                                                simdjson::dom::element,
                                                const WriterInput&)>;

    return bioseq::bench_framework::run(
        argc, argv, report_dir, data_dir,
        create_benchmark<ReaderInput, IoOutput>(
            "IO readers — FASTA (plain / async / mmap)",
            "bench_io_readers_fasta", config_path,
            ReaderBuild{build_reader_input},
            ReaderRun{run_reader},
            ReaderEval{evaluate_reader},
            ReaderCleanup{cleanup_reader}),
        create_benchmark<ReaderInput, IoOutput>(
            "IO readers — FASTQ (plain / async / mmap)",
            "bench_io_readers_fastq", config_path,
            ReaderBuild{build_reader_input},
            ReaderRun{run_reader},
            ReaderEval{evaluate_reader},
            ReaderCleanup{cleanup_reader}),
        create_benchmark<ReaderInput, IoOutput>(
            "IO readers — gzipped paths",
            "bench_io_readers_gz", config_path,
            ReaderBuild{build_reader_input},
            ReaderRun{run_reader},
            ReaderEval{evaluate_reader},
            ReaderCleanup{cleanup_reader}),
        create_benchmark<WriterInput, IoOutput>(
            "IO writers — FASTA (sync / parallel)",
            "bench_io_writers_fasta", config_path,
            WriterBuild{build_writer_input},
            WriterRun{run_writer},
            WriterEval{evaluate_writer},
            WriterCleanup{cleanup_writer}),
        create_benchmark<WriterInput, IoOutput>(
            "IO writers — FASTQ (sync / parallel)",
            "bench_io_writers_fastq", config_path,
            WriterBuild{build_writer_input},
            WriterRun{run_writer},
            WriterEval{evaluate_writer},
            WriterCleanup{cleanup_writer}));
}
