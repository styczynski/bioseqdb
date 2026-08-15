/**
 * @file writers.cppm
 * @brief Writer workloads, content hashing, and round-trip validation.
 */
module;

#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

namespace bioseq::io::benchmark::detail {

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
template <bool p_is_fastq>
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
                       + (p_is_fastq ? quality.size() : 0) + 2);
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
    if constexpr (p_is_fastq) {
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
template <bool p_is_fastq>
[[nodiscard]] std::uint64_t hash_batches_xor(
    const std::vector<benchmark_batch>& batches)
{
    std::uint64_t accumulator = 0;
    for (const auto& batch : batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0;
             record_index < record_count_in_batch; ++record_index) {
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

/// Slurp `path` into an in-memory list of batches via the bioseq reader.
template <bool p_is_fasta>
[[nodiscard]] bool slurp_corpus(const std::string& path,
                                std::vector<benchmark_batch>& out_batches,
                                std::size_t& out_record_count) {
    constexpr auto file_type =
        p_is_fasta ? sequence_file_format::fasta : sequence_file_format::fastq;

    out_batches.clear();
    out_record_count = 0;

    auto handler_fn = [&out_batches, &out_record_count](benchmark_batch&& batch) {
        out_record_count += batch.size();
        out_batches.push_back(std::move(batch));
    };
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

/// Slurp one uncompressed corpus and compute its round-trip content identity.
[[nodiscard]] writer_input build_writer_input(std::string_view data_dir,
                                              std::string_view test_case_name,
                                              simdjson::dom::element data,
                                              const run_constraints& /*constraints*/) {
    writer_input input;

    // The .fastq.gz cell is reader-only; writers don't have a compress
    // path. Skip silently.
    const bool compressed_hint = read_bool_field(data, "compressed", false);
    if (compressed_hint) {
        bioseq_log<logging_level::debug>("[bioseq.io.benchmark] writer skipping gz cell '{}'", test_case_name);
        return input;
    }
    const corpus_descriptor descriptor =
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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer slurp failed for '{}' — clearing input",
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
    bioseq_log<logging_level::debug>("[bioseq.io.benchmark] writer corpus ready: '{}' (records={}, scratch={})",
            test_case_name, input.source_record_count, input.output_scratch_path);
    return input;
}

/// Write all prepared batches through the Bioseq sink with the requested producers.
template <bool p_is_fasta>
[[nodiscard]] io_measurement run_bioseq_writer(const writer_input& input,
                                           std::size_t thread_count,
                                           bool use_mmap_sink = false) {
    constexpr auto file_type = p_is_fasta
        ? sequence_file_format::fasta
        : sequence_file_format::fastq;
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
    auto writer_or_err =
        sequence_writer<seq::dna4, file_type>::open(input.output_scratch_path, options);
    if (!writer_or_err) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer open failed: {}",
                                         writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    const std::size_t batches_total = input.batches.size();
    if (thread_count <= 1 || batches_total <= 1) {
        for (const auto& batch : input.batches) {
            if (!writer.write_batch(batch)) {
                bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer write_batch failed");
                (void)writer.close();
                return {};
            }
        }
    } else {
        const std::size_t resolved_threads =
            thread_count > batches_total ? batches_total : thread_count;
        std::atomic<bool> any_failure{false};
        auto run_shard = [&any_failure, &batches_total, &input, &resolved_threads, &writer](
                             std::size_t shard_index) {
            const std::size_t shard_start = (batches_total * shard_index) / resolved_threads;
            const std::size_t shard_end = (batches_total * (shard_index + 1)) / resolved_threads;
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
        for (auto& thread : shard_threads)
            thread.join();
        if (any_failure.load(std::memory_order_relaxed)) {
            (void)writer.close();
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
                    (word >> (seq_traits::bits_per_char * letter_in_word))
                    & seq_traits::char_mask);
                // Use the alphabet's canonical letter_to_char mapping
                // so we don't bake in the dna4 LUT shape — works for
                // any alphabet supported by `benchmark_alphabet`.
                scratch.push_back(seq_traits::letter_to_char(rank));
                if constexpr (p_is_fasta) {
                    ++in_line_count;
                    if (in_line_count == fasta_wrap &&
                        letter_index + 1 < total_letters) {
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
    io_measurement result;
    result.observed_record_count = input.source_record_count;
    std::error_code ec;
    result.bytes_written = filesystem::file_size(input.output_scratch_path, ec);
    return result;
}

/// Resolve a configured writer algorithm to its concrete workload.
[[nodiscard]] io_measurement dispatch_writer(std::string_view algo,
                                         const writer_input& input) {
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
    bioseq_log<logging_level::error>("[bioseq.io.benchmark] unknown writer algorithm '{}'", algo);
    return {};
}

/// Run one writer workload when slurping and scratch-path setup succeeded.
[[nodiscard]] io_measurement run_writer(std::string_view algo,
                                    const parameter_map& /*params*/,
                                    const writer_input& input) {
    if (input.batches.empty() || input.output_scratch_path.empty()) return {};
    return dispatch_writer(algo, input);
}

/// Re-read the writer's output to count records AND verify byte-faithful
/// content. Returns 1.0 only if both:
///   1. record count matches the input slurp;
///   2. the XOR-aggregated content hash matches the input slurp's hash.
/// Either failure returns 0.0 — the bench then surfaces it as a
/// roundtrip violation. This catches silent corruption that a
/// count-only check misses (e.g. truncated quality in the naive
/// writer, headers split from bodies by mid-record flushes).
template <bool p_is_fasta>
[[nodiscard]] double roundtrip_match(const std::string& output_path,
                                       std::size_t expected_record_count,
                                       std::uint64_t expected_content_hash) {
    if (output_path.empty()) return 0.0;
    std::error_code ec;
    if (!filesystem::exists(output_path, ec) || filesystem::file_size(output_path, ec) == 0) {
        return 0.0;
    }
    constexpr auto file_type = p_is_fasta
        ? sequence_file_format::fasta
        : sequence_file_format::fastq;
    std::atomic<std::size_t> record_count{0};
    std::atomic<std::uint64_t> content_hash{0};
    auto handler_fn = [&record_count, &content_hash](benchmark_batch&& batch) {
        record_count.fetch_add(batch.size(), std::memory_order_relaxed);
        // Hash this batch's contribution and XOR into the global
        // accumulator. XOR is associative + commutative, so concurrent
        // batch handlers contribute correctly via fetch_xor.
        const std::uint64_t batch_hash = hash_batches_xor<!p_is_fasta>(
            std::vector<benchmark_batch>{std::move(batch)});
        content_hash.fetch_xor(batch_hash, std::memory_order_relaxed);
    };
    auto failure_fn = [&output_path](std::string&& msg) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] roundtrip read failed on '{}': {}",
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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] roundtrip record count mismatch on '{}': "
             "expected={}, observed={}",
             output_path, expected_record_count, observed_records);
        return 0.0;
    }
    if (observed_hash != expected_content_hash) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] roundtrip content hash mismatch on '{}': "
             "expected=0x{:016x}, observed=0x{:016x}",
             output_path, expected_content_hash, observed_hash);
        return 0.0;
    }
    return 1.0;
}

/// Validate nonempty output and exact order-independent round-trip content.
[[nodiscard]] std::map<std::string, double> evaluate_writer(
    std::string_view /*algo*/,
    const parameter_map& /*params*/,
    const writer_input& input,
    const io_measurement& output)
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

/// Remove the synthetic input and per-iteration writer output when present.
void cleanup_writer(std::string_view /*data_dir*/,
                    std::string_view /*test_case_name*/,
                    simdjson::dom::element /*data*/,
                    const writer_input& input)
{
    std::error_code ec;
    if (input.is_synthetic_source && !input.source_corpus_path.empty()) {
        filesystem::remove(input.source_corpus_path, ec);
    }
    if (!input.output_scratch_path.empty()) {
        filesystem::remove(input.output_scratch_path, ec);
    }
}


}  // namespace bioseq::io::benchmark::detail
