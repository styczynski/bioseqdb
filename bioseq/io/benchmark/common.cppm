/**
 * @file common.cppm
 * @brief Shared I/O benchmark data and corpus preparation.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

module bioseq.io.benchmark:common;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.io.writer;
import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser;
import bioseq.bench.framework;

namespace bioseq::io::benchmark::detail {

namespace filesystem = std::filesystem;

using parameter_map = bioseq::bench::framework::parameter_map;
using run_constraints = bioseq::bench::framework::run_constraints;
using bioseq::io::read_sequences_file;
using bioseq::io::reader_options;
using bioseq::io::sequence_batch;
using bioseq::io::sequence_file_format;
using bioseq::io::sequence_writer;
using bioseq::io::writer_options;

using benchmark_alphabet = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using benchmark_batch = sequence_batch<benchmark_alphabet>;

/// Corpus path and size estimates for a reader run. Cleanup deletes synthetic
/// files created for the benchmark and keeps supplied data files.
struct reader_input {
    std::string corpus_path;
    std::size_t bytes_on_disk = 0;
    std::size_t decompressed_bytes = 0;
    std::size_t expected_record_count = 0;
    bool is_fastq = false;
    bool is_compressed = false;
    bool is_synthetic = false;
};

/// Corpus loaded before timing so a writer run measures writing the batches.
struct writer_input {
    std::string source_corpus_path;
    std::size_t source_bytes_on_disk = 0;
    std::size_t source_record_count = 0;
    bool is_fastq = false;
    bool is_synthetic_source = false;

    /// Output path reused by writer runs. Evaluation reads the result back, and
    /// cleanup removes the file.
    std::string output_scratch_path;

    /// In-memory corpus for the writer loop. Populated in `on_input`.
    std::vector<benchmark_batch> batches;

    /// Expected XOR of record hashes for checking the written output. FASTA hashes
    /// headers and sequences. FASTQ also hashes quality bytes up to sequence length.
    std::uint64_t expected_content_hash_fastq = 0;
    std::uint64_t expected_content_hash_fasta = 0;
};

/// Measurements shared by reader and writer workloads.
struct io_measurement {
    std::size_t observed_record_count = 0;
    std::size_t observed_body_bytes = 0;
    std::size_t bytes_written = 0;          ///< writer outputs only
    std::size_t roundtrip_record_count = 0; ///< Reserved for a writer round-trip record count.
};

/// Reader input and field mapping for comparing UniProt header parsing with
/// copying the whole header. Mapping setup happens before timing.
struct identifier_benchmark_input {
    reader_input reader;
    bioseq::io::identifier_parser::identifier_mapping_result mapping;
    bioseq::io::identifier_parser::header_format effective_format =
        bioseq::io::identifier_parser::header_format::uniprotkb;
};

/// Identifier extraction counters emitted by one workload execution.
struct identifier_benchmark_measurement {
    std::size_t observed_record_count = 0;
    std::size_t observed_body_bytes = 0;
    std::size_t header_bytes = 0;
    std::size_t parsed_field_count = 0;
    std::size_t parsed_field_bytes = 0;
};

/// Sequence types for comparing immediate and delayed copying. Packed results
/// use the same memory budget and materializer as the storage code.
using lazy_expression_range = compact_sequence_range<benchmark_alphabet>;
using lazy_expression_facade = bioseq::storage::lazy_sequence_view<seq::dna4>;
using lazy_expression_packed = bioseq::storage::admitted_packed_sequence<seq::dna4>;
using lazy_expression_size = bioseq::storage::sequence_size_t;

inline constexpr std::uint64_t lazy_expression_hash_seed = 0x0377d1a5e5e0c0deULL;
inline constexpr std::uint8_t lazy_expression_storage_width = 8u;

/// Source sequence, expected output length, and hash for an expression run.
struct lazy_expression_input {
    lazy_expression_range source;
    lazy_expression_size source_logical_length = 0;
    lazy_expression_size expected_output_logical_length = 0;
    std::uint64_t expected_hash = 0;
};

/// Bytes allocated and reserved from the memory budget while copying results.
struct lazy_expression_allocation_statistics {
    std::uint64_t payload_materializations = 0;
    std::uint64_t intermediate_payload_materializations = 0;
    std::uint64_t boundary_materializations = 0;
    std::uint64_t payload_bytes_materialized = 0;
    std::uint64_t live_payload_bytes = 0;
    std::uint64_t peak_live_payload_bytes = 0;
    std::uint64_t admitted_bytes_total = 0;
    std::uint64_t live_admitted_bytes = 0;
    std::uint64_t peak_live_admitted_bytes = 0;
};

/// Output length, content hash, and allocation counts from one expression run.
struct lazy_expression_measurement {
    lazy_expression_size output_logical_length = 0;
    std::uint64_t logical_hash = 0;
    lazy_expression_allocation_statistics allocations{};
};

/// Read an integer property or use the fallback for an absent or ill-typed value.
[[nodiscard]] std::int64_t
read_int_field(simdjson::dom::element data, std::string_view name, std::int64_t fallback) noexcept {
    std::int64_t value = fallback;
    std::int64_t parsed = 0;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

/// Read a string property or use the supplied fallback.
[[nodiscard]] std::string
read_string_field(simdjson::dom::element data, std::string_view name, std::string_view fallback) {
    std::string_view raw{};
    if (data[name].get(raw) == simdjson::SUCCESS) {
        return std::string{raw};
    }
    return std::string{fallback};
}

/// Read a Boolean property or use the supplied fallback.
[[nodiscard]] bool
read_bool_field(simdjson::dom::element data, std::string_view name, bool fallback) noexcept {
    bool value = fallback;
    bool parsed = false;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

/// Choose a scratch filename in the system temporary directory.
/// The name includes a process ID, counter, and random suffix.
[[nodiscard]] std::string allocate_temp_path(std::string_view tag) {
    static std::atomic<std::uint64_t> counter{0};
    filesystem::path temp_directory_root = filesystem::temp_directory_path();
    filesystem::path full_path =
        temp_directory_root / fmt::format("bioseq_iobench_{}_{}_{}_{}",
                                          tag,
                                          ::getpid(),
                                          counter.fetch_add(1, std::memory_order_relaxed),
                                          std::random_device{}() & 0xFFFFFFu);
    return full_path.string();
}

/// Write generated DNA records as FASTA or FASTQ. Return the number of bytes
/// submitted to the output stream, or zero if the file cannot be opened.
[[nodiscard]] std::size_t emit_synthetic_corpus(const std::string& output_path,
                                                bool is_fastq,
                                                std::size_t record_count,
                                                std::size_t mean_record_logical_length,
                                                std::uint64_t seed,
                                                std::string_view header_shape = "plain") {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] failed to open '{}' for writing",
                                         output_path);
        return 0;
    }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> base_dist(0, 3);
    std::normal_distribution<double> length_dist(static_cast<double>(mean_record_logical_length),
                                                 static_cast<double>(mean_record_logical_length) *
                                                     0.1);
    constexpr std::array<char, 4> bases{'A', 'C', 'G', 'T'};
    constexpr std::size_t fasta_wrap = 80;

    std::string buffer;
    buffer.reserve(64 * 1024);
    std::size_t bytes_written = 0;
    // Write the accumulated fixture bytes and reuse the allocation.
    auto flush_buffer = [&buffer, &bytes_written, &out]() {
        if (buffer.empty())
            return;
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes_written += buffer.size();
        buffer.clear();
    };

    for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
        const double sample = length_dist(rng);
        const std::size_t record_length = static_cast<std::size_t>(std::max(8.0, sample));
        // UniProt-shaped FASTA headers exercise positional fields and metadata tags.
        // Plain headers and FASTQ records use the sequence number.
        if (header_shape == "uniprot_metadata" && !is_fastq) {
            buffer.append(fmt::format(">tr|P{:06}|P{:06}_BENCH Synthetic parser benchmark protein "
                                      "OS=Homo sapiens OX=9606 PE=1 SV={}\n",
                                      record_index,
                                      record_index,
                                      1 + (record_index % 9)));
        } else if (is_fastq) {
            buffer.append(fmt::format("@seq_{}\n", record_index));
        } else {
            buffer.append(fmt::format(">seq_{}\n", record_index));
        }
        if (buffer.size() >= 32 * 1024)
            flush_buffer();

        if (is_fastq) {
            for (std::size_t letter_index = 0; letter_index < record_length; ++letter_index) {
                buffer.push_back(bases[base_dist(rng)]);
            }
            buffer.push_back('\n');
            buffer.push_back('+');
            buffer.push_back('\n');
            buffer.append(record_length, 'I');
            buffer.push_back('\n');
            if (buffer.size() >= 32 * 1024)
                flush_buffer();
        } else {
            std::size_t emitted_letters = 0;
            while (emitted_letters < record_length) {
                const std::size_t this_line = std::min(fasta_wrap, record_length - emitted_letters);
                for (std::size_t letter_index = 0; letter_index < this_line; ++letter_index) {
                    buffer.push_back(bases[base_dist(rng)]);
                }
                buffer.push_back('\n');
                emitted_letters += this_line;
                if (buffer.size() >= 32 * 1024)
                    flush_buffer();
            }
        }
    }
    flush_buffer();
    out.flush();
    out.close();
    return bytes_written;
}

/// File details from prepare_corpus. The synthetic flag tells cleanup whether
/// this benchmark created the file.
struct corpus_descriptor {
    std::string path;
    std::size_t bytes_on_disk = 0;
    std::size_t record_count = 0; ///< 0 for real-data (unknown until parse)
    bool is_fastq = false;
    bool is_compressed = false;
    bool is_synthetic = false;
    bool valid = false;
};

/// Resolve a corpus's on-disk path. For real-data cells, look up the
/// path under `data_dir`. Synthetic corpora are generated at a temporary path.
[[nodiscard]] corpus_descriptor prepare_corpus(std::string_view data_dir,
                                               std::string_view test_case_name,
                                               simdjson::dom::element data) {
    corpus_descriptor descriptor;
    const std::string format_name = read_string_field(data, "format", "fasta");
    descriptor.is_fastq = (format_name == "fastq");
    descriptor.is_compressed = read_bool_field(data, "compressed", false);
    const std::string kind = read_string_field(data, "kind", "synthetic");

    if (kind == "real") {
        const std::string relative = read_string_field(data, "real_data_path", "");
        if (relative.empty()) {
            bioseq_log<logging_level::error>(
                "[bioseq.io.benchmark] test case '{}' missing real_data_path", test_case_name);
            return descriptor;
        }
        filesystem::path joined = filesystem::path{std::string{data_dir}} / relative;
        std::error_code ec;
        if (!filesystem::exists(joined, ec)) {
            bioseq_log<logging_level::error>(
                "[bioseq.io.benchmark] real-data file missing for '{}': {}",
                test_case_name,
                joined.string());
            return descriptor;
        }
        const auto observed_size = filesystem::file_size(joined, ec);
        if (ec) {
            bioseq_log<logging_level::error>(
                "[bioseq.io.benchmark] cannot stat '{}': {}", joined.string(), ec.message());
            return descriptor;
        }
        descriptor.path = joined.string();
        descriptor.bytes_on_disk = static_cast<std::size_t>(observed_size);
        descriptor.is_synthetic = false;
        descriptor.valid = true;
        return descriptor;
    }
    if (kind != "synthetic") {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] test case '{}' has unknown kind '{}'", test_case_name, kind);
        return descriptor;
    }

    const std::size_t record_count =
        static_cast<std::size_t>(read_int_field(data, "record_count", 1000));
    const std::size_t mean_record_logical_length =
        static_cast<std::size_t>(read_int_field(data, "mean_record_length_bp", 200));
    const std::uint64_t seed = static_cast<std::uint64_t>(read_int_field(data, "seed", 42));
    const std::string header_shape = read_string_field(data, "header_shape", "plain");

    const std::string path_tag = std::string{format_name} + "_" + std::string{test_case_name};
    descriptor.path = allocate_temp_path(path_tag);
    const std::size_t bytes = emit_synthetic_corpus(descriptor.path,
                                                    descriptor.is_fastq,
                                                    record_count,
                                                    mean_record_logical_length,
                                                    seed,
                                                    header_shape);
    if (bytes == 0) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] synthetic corpus generation failed for '{}'", test_case_name);
        descriptor.path.clear();
        return descriptor;
    }
    descriptor.bytes_on_disk = bytes;
    descriptor.record_count = record_count;
    descriptor.is_synthetic = true;
    descriptor.valid = true;
    return descriptor;
}

/// Estimate corpus memory from the configured record count and mean length,
/// or from a supplied file's size. Compressed files use a rough expansion factor.
/// Return zero when the size cannot be determined.
[[nodiscard]] std::size_t estimate_corpus_bytes(std::string_view data_dir,
                                                simdjson::dom::element data) noexcept {
    const std::string kind = read_string_field(data, "kind", "synthetic");
    if (kind == "real") {
        const std::string relative = read_string_field(data, "real_data_path", "");
        if (relative.empty())
            return 0;
        filesystem::path joined = filesystem::path{std::string{data_dir}} / relative;
        std::error_code ec;
        const auto observed_size = filesystem::file_size(joined, ec);
        if (ec)
            return 0;
        std::size_t bytes = static_cast<std::size_t>(observed_size);
        if (read_bool_field(data, "compressed", false)) {
            // Same factor `build_reader_input` uses for `decompressed_bytes`.
            bytes = static_cast<std::size_t>(static_cast<double>(bytes) * 3.2);
        }
        return bytes;
    }
    if (kind != "synthetic")
        return 0;
    const auto record_count = static_cast<std::size_t>(read_int_field(data, "record_count", 1000));
    const auto mean_record_logical_length =
        static_cast<std::size_t>(read_int_field(data, "mean_record_length_bp", 200));
    return record_count * mean_record_logical_length;
}

/// Estimate memory for reader buffers and decoded batches so the benchmark
/// framework can limit concurrent runs. The estimate is based on corpus size.
[[nodiscard]] run_constraints estimate_reader_resources(std::string_view data_dir,
                                                        std::string_view /*test_case_name*/,
                                                        simdjson::dom::element data) noexcept {
    const std::size_t bytes = estimate_corpus_bytes(data_dir, data);
    return {
        .base_memory = bytes * 2,
        .extra_memory = bytes * 2,
    };
}

/// Estimate memory for the batches kept by a writer run and its output
/// buffers. The framework uses this to limit concurrent runs.
[[nodiscard]] run_constraints estimate_writer_resources(std::string_view data_dir,
                                                        std::string_view /*test_case_name*/,
                                                        simdjson::dom::element data) noexcept {
    const std::size_t bytes = estimate_corpus_bytes(data_dir, data);
    return {
        .base_memory = bytes * 2,
        .extra_memory = bytes * 2,
    };
}

} // namespace bioseq::io::benchmark::detail
