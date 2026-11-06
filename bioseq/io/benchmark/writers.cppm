/**
 * @file writers.cppm
 * @brief Writer workloads and round-trip validation.
 */
module;

// Canonicalize Highway's internal-linkage templates before importing the batch
// and streaming codecs, as in the IO reader tests, to avoid duplicate NEON helpers.
#include <hwy/highway.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <limits>
#include <simdjson.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.io.benchmark:writers;

import :common;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.io.writer;
import bioseq.parallelism.pool;

namespace bioseq::io::benchmark::detail {

/// Hash a record's header and decoded sequence. FASTQ also includes quality
/// bytes up to the sequence length, matching the writer's truncation.
/// The caller combines record hashes with XOR to, ignore output order.
template <bool p_is_fastq>
[[nodiscard]] std::uint64_t hash_record_xor(std::string_view header,
                                            const auto& sequence_view,
                                            std::span<const std::uint8_t> quality) {
    using seq_traits = typename std::remove_cvref_t<decltype(sequence_view)>::traits;
    constexpr std::size_t letters_per_word = seq_traits::letters_per_unit;
    const std::size_t total_letters =
        sequence_view.size() * letters_per_word - sequence_view.padding();
    std::string serialized;
    serialized.reserve(header.size() + total_letters + (p_is_fastq ? quality.size() : 0) + 2);
    serialized.append(header);
    serialized.push_back('\1');
    for (std::size_t letter_index = 0; letter_index < total_letters; ++letter_index) {
        const auto word = sequence_view.data()[letter_index / letters_per_word];
        const std::size_t letter_in_word = letter_index % letters_per_word;
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (seq_traits::bits_per_char * letter_in_word)) & seq_traits::char_mask);
        serialized.push_back(seq_traits::letter_to_char(rank));
    }
    if constexpr (p_is_fastq) {
        serialized.push_back('\1');
        const std::size_t qsize = std::min(quality.size(), total_letters);
        serialized.append(reinterpret_cast<const char*>(quality.data()), qsize);
    }
    return std::hash<std::string>{}(serialized);
}

/// Combine all record hashes with XOR so parallel output can reorder records.
/// Return zero  for an empty input. Matching hashes are a check on content,
/// but repeated records can cancel each other in the XOR.
template <bool p_is_fastq>
[[nodiscard]] std::uint64_t hash_batches_xor(const std::vector<benchmark_batch>& batches) {
    std::uint64_t accumulator = 0;
    for (const auto& batch : batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0; record_index < record_count_in_batch; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            std::span<const std::uint8_t> qual_view{};
            if constexpr (p_is_fastq) {
                qual_view = batch.quality(record_index);
            }
            accumulator ^= hash_record_xor<p_is_fastq>(header_view, seq_view, qual_view);
        }
    }
    return accumulator;
}

/// Read the corpus into batches before timing the writer.
template <bool p_is_fasta>
[[nodiscard]] bool slurp_corpus(const std::string& path,
                                std::vector<benchmark_batch>& out_batches,
                                std::size_t& out_record_count) {
    constexpr auto file_type =
        p_is_fasta ? sequence_file_format::fasta : sequence_file_format::fastq;

    out_batches.clear();
    out_record_count = 0;

    // Keep each decoded batch for the subsequent wriner measurement
    auto handler_fn = [&out_batches, &out_record_count](benchmark_batch&& batch) {
        out_record_count += batch.size();
        out_batches.push_back(std::move(batch));
    };
    // Report fixture-read failures with their source path.
    auto failure_fn = [&path](std::string&& msg) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] slurp failed on '{}': {}", path, msg);
    };
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap = true;
    options.concurrency.worker_count = 0;

    auto outcome = read_sequences_file<seq::dna4, file_type>(path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] slurp_corpus read error: {}",
                                         outcome.error());
        return false;
    }
    return !out_batches.empty();
}

/// Load an uncompressed corpus and prepare its expected record count and hash.
[[nodiscard]] writer_input build_writer_input(std::string_view data_dir,
                                              std::string_view test_case_name,
                                              simdjson::dom::element data,
                                              const run_constraints& /*constraints*/) {
    writer_input input;

    // Compressed inputs are reserved for the reader benchmarks.
    const bool compressed_hint = read_bool_field(data, "compressed", false);
    if (compressed_hint) {
        bioseq_log<logging_level::debug>("[bioseq.io.benchmark] writer skipping gz cell '{}'",
                                         test_case_name);
        return input;
    }
    const corpus_descriptor descriptor = prepare_corpus(data_dir, test_case_name, data);
    if (!descriptor.valid)
        return input;
    input.source_corpus_path = descriptor.path;
    input.source_bytes_on_disk = descriptor.bytes_on_disk;
    input.source_record_count = descriptor.record_count;
    input.is_fastq = descriptor.is_fastq;
    input.is_synthetic_source = descriptor.is_synthetic;

    bool slurp_ok = false;
    if (input.is_fastq) {
        slurp_ok =
            slurp_corpus<false>(input.source_corpus_path, input.batches, input.source_record_count);
    } else {
        slurp_ok =
            slurp_corpus<true>(input.source_corpus_path, input.batches, input.source_record_count);
    }
    if (!slurp_ok) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] writer slurp failed for '{}' — clearing input", test_case_name);
        input.batches.clear();
        return input;
    }
    // Compute the expected hash before timing. Evaluation compares it  with
    // the hash of the file written by the benchmark.
    if (input.is_fastq) {
        input.expected_content_hash_fastq = hash_batches_xor<true>(input.batches);
    } else {
        input.expected_content_hash_fasta = hash_batches_xor<false>(input.batches);
    }
    input.output_scratch_path =
        allocate_temp_path(std::string{"writer_out_"} + std::string{test_case_name});
    bioseq_log<logging_level::debug>(
        "[bioseq.io.benchmark] writer corpus ready: '{}' (records={}, scratch={})",
        test_case_name,
        input.source_record_count,
        input.output_scratch_path);
    return input;
}

/// Write prepared batches within the current shared-arena participant budget.
template <bool p_is_fasta>
[[nodiscard]] io_measurement
run_bioseq_writer(const writer_input& input, std::size_t thread_count, bool use_mmap_sink = false) {
    constexpr auto file_type =
        p_is_fasta ? sequence_file_format::fasta : sequence_file_format::fastq;
    writer_options options{};
    options.overwrite = true;
    concurrency_options concurrency{};
    concurrency.worker_count = thread_count;
    if (use_mmap_sink) {
        // Leave room for record formatting when reserving the mapped output file.
        // The capacity hint must be large enough for the complete result.
        const std::size_t estimated_output_bytes =
            input.source_bytes_on_disk + input.source_bytes_on_disk / 2;
        options.output_capacity_hint =
            estimated_output_bytes > 4096 ? estimated_output_bytes : 4096;
    }
    auto writer_or_err =
        sequence_writer<seq::dna4, file_type>::open(input.output_scratch_path, options, concurrency);
    if (!writer_or_err) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer open failed: {}",
                                         writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    for (const auto& batch : input.batches) {
        if (!writer.write_batch(batch)) {
            bioseq_log<logging_level::error>("[bioseq.io.benchmark] ordered writer failed");
            return {};
        }
    }
    auto close_outcome = writer.close();
    if (!close_outcome) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer close failed: {}",
                                         close_outcome.error());
        return {};
    }
    io_measurement out;
    out.observed_record_count = input.source_record_count;
    std::error_code ec;
    out.bytes_written = filesystem::file_size(input.output_scratch_path, ec);
    return out;
}

/// Serialize prepared batches through a line-oriented ofstream baseline.
template <bool p_is_fasta>
[[nodiscard]] io_measurement run_naive_ofstream(const writer_input& input) {
    std::ofstream out(input.output_scratch_path, std::ios::binary);
    if (!out) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] naive ofstream open failed: '{}'",
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
            scratch.push_back(p_is_fasta ? '>' : '@');
            scratch.append(header_view);
            scratch.push_back('\n');
            std::size_t in_line_count = 0;
            for (std::size_t letter_index = 0; letter_index < total_letters; ++letter_index) {
                const auto word = seq_view.data()[letter_index / letters_per_word];
                const std::size_t letter_in_word = letter_index % letters_per_word;
                const std::uint8_t rank = static_cast<std::uint8_t>(
                    (word >> (seq_traits::bits_per_char * letter_in_word)) & seq_traits::char_mask);
                // Decode packed ranks through the alphabet's character mapping.
                scratch.push_back(seq_traits::letter_to_char(rank));
                if constexpr (p_is_fasta) {
                    ++in_line_count;
                    if (in_line_count == fasta_wrap && letter_index + 1 < total_letters) {
                        scratch.push_back('\n');
                        in_line_count = 0;
                    }
                }
            }
            if constexpr (!p_is_fasta) {
                scratch.push_back('\n');
                scratch.push_back('+');
                scratch.push_back('\n');
                const auto quality_view = batch.quality(record_index);
                // Match the writer's quality handling: trim extra bytes and fill any
                // missing quality positions with Phred-0 scores.
                const std::size_t qsize = std::min(quality_view.size(), total_letters);
                scratch.append(reinterpret_cast<const char*>(quality_view.data()), qsize);
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
    io_measurement result;
    result.observed_record_count = input.source_record_count;
    std::error_code ec;
    result.bytes_written = filesystem::file_size(input.output_scratch_path, ec);
    return result;
}

/** Resolve the existing algorithm's requested total budget; zero rejects an unknown name. */
[[nodiscard]] std::size_t writer_worker_budget(std::string_view algorithm) {
    if (algorithm == "bioseq_writer_sync_fasta" || algorithm == "bioseq_writer_sync_fastq"
        || algorithm == "naive_ofstream_fasta" || algorithm == "naive_ofstream_fastq") return 1u;
    if (algorithm == "bioseq_writer_parallel4_fasta" || algorithm == "bioseq_writer_parallel4_fastq"
        || algorithm == "bioseq_writer_mmap_fasta" || algorithm == "bioseq_writer_mmap_fastq") return 4u;
    if (algorithm == "bioseq_writer_parallel8_fasta" || algorithm == "bioseq_writer_parallel8_fastq") return 8u;
    return 0u;
}

/** Prepare the existing writer variant outside timing, including single-caller native baselines. */
[[nodiscard]] io_pool_scope prepare_writer_iteration(std::string_view algorithm, std::size_t repeat) {
    const auto requested = writer_worker_budget(algorithm);
    io_pool_scope preparation{static_cast<std::int64_t>(requested)};
    if (!preparation.error.empty()) return preparation;
    const auto& pool = parallelism::global_pool::instance();
    bioseq_log<logging_level::info>(
        "writer budget: algorithm={} repeat={} requested_workers={} pool_limit={} "
        "framework_limit={} active_workers=unobserved",
        algorithm, repeat, requested, pool.maximum_concurrency(), preparation.framework_limit);
    return preparation;
}

/// Select the writer implementation using the same budget resolver as untimed preparation.
[[nodiscard]] io_measurement dispatch_writer(std::string_view algo, const writer_input& input) {
    const auto workers = writer_worker_budget(algo);
    if (workers == 0u) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] unknown writer algorithm '{}'", algo);
        return {};
    }
    if (algo == "naive_ofstream_fasta") return run_naive_ofstream<true>(input);
    if (algo == "naive_ofstream_fastq") return run_naive_ofstream<false>(input);
    const bool use_mmap_sink = algo == "bioseq_writer_mmap_fasta" || algo == "bioseq_writer_mmap_fastq";
    if (algo.ends_with("_fasta")) return run_bioseq_writer<true>(input, workers, use_mmap_sink);
    return run_bioseq_writer<false>(input, workers, use_mmap_sink);
}

/// Run the selected writer after input batches and an output path are ready.
[[nodiscard]] io_measurement
run_writer(std::string_view algo, const parameter_map& /*params*/, const writer_input& input) {
    if (input.batches.empty() || input.output_scratch_path.empty())
        return {};
    return dispatch_writer(algo, input);
}

/// Read the output back and compare its record count and combined hash.
/// Return 1.0 when both match, or 0.0 for a mismatch, empty file, or read error.
/// The hash check allows records to apear in a different order.
template <bool p_is_fasta>
[[nodiscard]] double roundtrip_match(const std::string& output_path,
                                     std::size_t expected_record_count,
                                     std::uint64_t expected_content_hash) {
    if (output_path.empty())
        return 0.0;
    std::error_code ec;
    if (!filesystem::exists(output_path, ec) || filesystem::file_size(output_path, ec) == 0) {
        return 0.0;
    }
    constexpr auto file_type =
        p_is_fasta ? sequence_file_format::fasta : sequence_file_format::fastq;
    std::atomic<std::size_t> record_count{0};
    std::atomic<std::uint64_t> content_hash{0};
    // Fold each decoded batch into the round-trip count and content hash.
    auto handler_fn = [&record_count, &content_hash](benchmark_batch&& batch) {
        record_count.fetch_add(batch.size(), std::memory_order_relaxed);
        // XOR lets each batch contribute regardless of callback order
        const std::uint64_t batch_hash =
            hash_batches_xor<!p_is_fasta>(std::vector<benchmark_batch>{std::move(batch)});
        content_hash.fetch_xor(batch_hash, std::memory_order_relaxed);
    };
    // Report round-trip read failures with their output path.
    auto failure_fn = [&output_path](std::string&& msg) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] roundtrip read failed on '{}': {}", output_path, msg);
    };
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap = true;
    options.concurrency.worker_count = 0;
    auto outcome =
        read_sequences_file<seq::dna4, file_type>(output_path, handler_fn, failure_fn, options);
    if (!outcome.has_value())
        return 0.0;
    const std::size_t observed_records = record_count.load(std::memory_order_relaxed);
    const std::uint64_t observed_hash = content_hash.load(std::memory_order_relaxed);
    if (observed_records != expected_record_count) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] roundtrip record count mismatch on '{}': "
            "expected={}, observed={}",
            output_path,
            expected_record_count,
            observed_records);
        return 0.0;
    }
    if (observed_hash != expected_content_hash) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] roundtrip content hash mismatch on '{}': "
            "expected=0x{:016x}, observed=0x{:016x}",
            output_path,
            expected_content_hash,
            observed_hash);
        return 0.0;
    }
    return 1.0;
}

/// Report  output size and the result, of checking record count and content hash.
[[nodiscard]] std::map<std::string, double> evaluate_writer(std::string_view /*algo*/,
                                                            const parameter_map& /*params*/,
                                                            const writer_input& input,
                                                            const io_measurement& output) {
    std::map<std::string, double> metrics;
    metrics.emplace("bytes_on_disk", static_cast<double>(output.bytes_written));
    metrics.emplace("record_count", static_cast<double>(output.observed_record_count));
    // Reject empty output even if a record-count comparison could match zero.
    metrics.emplace("output_bytes_nonzero", output.bytes_written > 0 ? 1.0 : 0.0);
    // A rejected budget or failed write must not validate a prior variant's file.
    const bool measured_output = output.bytes_written > 0u
        && output.observed_record_count == input.source_record_count;
    const double match = !measured_output ? std::numeric_limits<double>::quiet_NaN()
        : input.is_fastq ? roundtrip_match<false>(input.output_scratch_path,
                                                input.source_record_count,
                                                input.expected_content_hash_fastq)
                        : roundtrip_match<true>(input.output_scratch_path,
                                               input.source_record_count,
                                               input.expected_content_hash_fasta);
    metrics.emplace("roundtrip_record_match", match);
    return metrics;
}

/// Delete generated input and the writer's output file.
void cleanup_writer(std::string_view /*data_dir*/,
                    std::string_view /*test_case_name*/,
                    simdjson::dom::element /*data*/,
                    const writer_input& input) {
    std::error_code ec;
    if (input.is_synthetic_source && !input.source_corpus_path.empty()) {
        filesystem::remove(input.source_corpus_path, ec);
    }
    if (!input.output_scratch_path.empty()) {
        filesystem::remove(input.output_scratch_path, ec);
    }
}

} // namespace bioseq::io::benchmark::detail
