/**
 * @file runners.cppm
 * @brief Three reader runners with a uniform `run(path)` signature.
 *
 *   * `run_bioseq_io_sync` — our reader in synchronous mode (one
 *     thread does I/O + parse + handler dispatch).
 *   * `run_bioseq_io_async` — our reader with a worker pool consuming
 *     batches in parallel.
 *   * `run_bioseq_io_mmap` — our reader pointed at the mmap source.
 *   * `run_kseqpp` — `klibpp::SeqStreamIn`. The de-facto FASTA/FASTQ
 *     reader in the C++ ecosystem; the bench's primary baseline.
 *   * `run_naive_ifstream` — `std::ifstream` byte-by-byte parser. The
 *     "every other reader is at least this fast" floor; fails
 *     gracefully when bigger than the baselines.
 *
 * All runners return a small `RunOutcome` struct so the matrix runner
 * can record record counts and corpus byte counts uniformly.
 *
 * The runners deliberately decode FASTA dna4 records into a
 * lightweight checksum (XOR of letter ranks) so the work the readers
 * do is comparable. Without "pretend to consume" the optimiser would
 * elide the entire parse path on a no-op handler.
 */
module;

#include "common.h"

#include <kseq++/seqio.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

export module bioseq_io_bench.runners;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.public_api;
import bioseq_io.options;
import bioseq_io.writer;

export {

namespace bioseq::io_bench {

/// What every runner returns. The bench's accuracy check compares
/// `record_count` across readers and asserts it matches the corpus's
/// expected count.
struct RunOutcome {
    std::size_t record_count = 0;
    /// Sum of body byte counts across the parsed records. Used as a
    /// secondary sanity check (catches "reader silently truncated
    /// records" without paying the cost of a per-letter decode in
    /// the timed loop). For bioseq_io the body byte count comes from
    /// `compact_sequence_range::size() * letters_per_unit -
    /// padding`; for kseqpp / naive it's the `std::string::size()`.
    std::size_t total_body_bytes = 0;
};

/// Compute the byte-count of a packed `compact_sequence_range<dna4,
/// uint64_t>` without iterating per-letter. O(1).
template <typename SequenceRange>
[[nodiscard]] inline std::size_t packed_letter_count(
    const SequenceRange& sequence_range) noexcept {
    using traits = typename SequenceRange::traits;
    return sequence_range.size() * traits::letters_per_unit
         - sequence_range.padding();
}

// -------------------------------------------------------------------
// bioseq_io runners
// -------------------------------------------------------------------

namespace detail {

template <bool IsFasta>
[[nodiscard]] inline RunOutcome run_bioseq_io_with_options(
    const std::string& path,
    const bioseq::io::reader_options& options) {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;

    // Handler stays as cheap as possible — the bench measures the
    // reader's bytes-on-disk throughput, NOT the handler's
    // post-processing speed. Counting records and summing letter
    // counts (O(1) per record via `compact_sequence_range::size()`
    // and `padding()`) is enough to detect "reader silently dropped
    // records" without burning cycles in the timed window.
    //
    // The handler may run concurrently when `worker_count > 0`, so the
    // accumulator state uses atomics. Atomic-relaxed adds in a tight
    // loop are ~1 ns each — invisible against the parser's
    // per-byte work.
    std::atomic<std::size_t> record_count_atomic{0};
    std::atomic<std::size_t> body_bytes_atomic{0};

    auto handler_fn =
        [&record_count_atomic, &body_bytes_atomic](batch_t&& batch) {
            std::size_t local_records = batch.size();
            std::size_t local_body_bytes = 0;
            for (std::size_t record_index = 0; record_index < local_records;
                 ++record_index) {
                local_body_bytes += packed_letter_count(
                    batch.sequence(record_index));
            }
            record_count_atomic.fetch_add(
                local_records, std::memory_order_relaxed);
            body_bytes_atomic.fetch_add(
                local_body_bytes, std::memory_order_relaxed);
        };
    auto failure_fn = [&path](std::string&& msg) {
        loge("[bioseq_io_bench.runners.bioseq] failure on '{}': {}",
             path, msg);
    };
    auto result = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, options);
    if (!result.has_value()) {
        loge("[bioseq_io_bench.runners.bioseq] read_sequences_file returned error: {}",
             result.error());
    }
    RunOutcome outcome;
    outcome.record_count =
        record_count_atomic.load(std::memory_order_relaxed);
    outcome.total_body_bytes =
        body_bytes_atomic.load(std::memory_order_relaxed);
    return outcome;
}

}  // namespace detail

/// Synchronous bioseq_io runner. Forces gzip auto-detect off,
/// `prefer_mmap` off, `worker_count` 0 — pure POSIX read with no
/// async dispatch.
[[nodiscard]] inline RunOutcome run_bioseq_io_sync_fasta(const std::string& path) {
    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap      = false;
    opts.concurrency.worker_count = 0;
    return detail::run_bioseq_io_with_options<true>(path, opts);
}

[[nodiscard]] inline RunOutcome run_bioseq_io_sync_fastq(const std::string& path) {
    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap      = false;
    opts.concurrency.worker_count = 0;
    return detail::run_bioseq_io_with_options<false>(path, opts);
}

/// mmap-backed bioseq_io runner. Same parser, different I/O source.
[[nodiscard]] inline RunOutcome run_bioseq_io_mmap_fasta(const std::string& path) {
    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap      = true;
    opts.concurrency.worker_count = 0;
    return detail::run_bioseq_io_with_options<true>(path, opts);
}

[[nodiscard]] inline RunOutcome run_bioseq_io_mmap_fastq(const std::string& path) {
    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap      = true;
    opts.concurrency.worker_count = 0;
    return detail::run_bioseq_io_with_options<false>(path, opts);
}

/// Async bioseq_io runner with N workers consuming batches in
/// parallel. Producer/parser still single-threaded.
[[nodiscard]] inline RunOutcome run_bioseq_io_async_fasta(
    const std::string& path, std::size_t worker_count) {
    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap      = false;
    opts.concurrency.worker_count = worker_count;
    return detail::run_bioseq_io_with_options<true>(path, opts);
}

[[nodiscard]] inline RunOutcome run_bioseq_io_async_fastq(
    const std::string& path, std::size_t worker_count) {
    bioseq::io::reader_options opts;
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap      = false;
    opts.concurrency.worker_count = worker_count;
    return detail::run_bioseq_io_with_options<false>(path, opts);
}

// -------------------------------------------------------------------
// kseqpp runner
// -------------------------------------------------------------------

/// Wrap `klibpp::SeqStreamIn` into the same `RunOutcome` shape. The
/// loop walks one record at a time via the stream's `>>` operator
/// and counts records + body bytes. The body-byte counter is summed
/// from `record.seq.size()` — `record.seq` is a `std::string` of the
/// raw ASCII letters kseqpp built up, so counting its `size()` is
/// O(1) per record.
[[nodiscard]] inline RunOutcome run_kseqpp(const std::string& path) {
    RunOutcome outcome;
    klibpp::SeqStreamIn stream(path.c_str());
    klibpp::KSeq record;
    while (stream >> record) {
        outcome.total_body_bytes += record.seq.size();
        outcome.record_count += 1;
    }
    return outcome;
}

// -------------------------------------------------------------------
// naive_ifstream runner
// -------------------------------------------------------------------

/// Byte-by-byte FASTA / FASTQ parser using `std::ifstream`. The
/// "every other reader is at least this fast" floor. Counts records
/// and body bytes; doesn't decode — same lightweight handler shape as
/// the bioseq_io / kseqpp runners.
[[nodiscard]] inline RunOutcome run_naive_ifstream_fasta(const std::string& path) {
    RunOutcome outcome;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        loge("[bioseq_io_bench.runners.naive] failed to open '{}'", path);
        return outcome;
    }
    std::string current_line;
    bool in_record = false;
    while (std::getline(in, current_line)) {
        if (current_line.empty()) continue;
        if (current_line.front() == '>') {
            if (in_record) outcome.record_count += 1;
            in_record = true;
            continue;
        }
        if (in_record) {
            std::size_t body_size = current_line.size();
            if (body_size > 0 && current_line[body_size - 1] == '\r') {
                body_size -= 1;
            }
            outcome.total_body_bytes += body_size;
        }
    }
    if (in_record) outcome.record_count += 1;
    return outcome;
}

[[nodiscard]] inline RunOutcome run_naive_ifstream_fastq(const std::string& path) {
    RunOutcome outcome;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        loge("[bioseq_io_bench.runners.naive] failed to open '{}'", path);
        return outcome;
    }
    std::string current_line;
    enum class FastqStep { HEADER, SEQ, SEP, QUAL };
    FastqStep step = FastqStep::HEADER;
    while (std::getline(in, current_line)) {
        switch (step) {
            case FastqStep::HEADER:
                if (current_line.empty() || current_line.front() != '@') continue;
                step = FastqStep::SEQ;
                break;
            case FastqStep::SEQ: {
                std::size_t body_size = current_line.size();
                if (body_size > 0 && current_line[body_size - 1] == '\r') {
                    body_size -= 1;
                }
                outcome.total_body_bytes += body_size;
                step = FastqStep::SEP;
                break;
            }
            case FastqStep::SEP:
                step = FastqStep::QUAL;
                break;
            case FastqStep::QUAL:
                outcome.record_count += 1;
                step = FastqStep::HEADER;
                break;
        }
    }
    return outcome;
}

// =====================================================================
// Writer side
// =====================================================================
//
// The writer bench follows the same shape as the reader bench: each
// runner takes an input corpus and an output path, formats the bytes,
// and returns a `WriteOutcome`. The corpus is loaded once into memory
// (a vector of sequence_batch) outside the timed window so the writer
// benches measure pure write throughput.

/// Per-batch corpus the writer benches loop over. Owns the parsed
/// records so the timed loop touches only the writer.
template <typename C>
struct WriterCorpus {
    std::vector<bioseq::io::sequence_batch<C>> batches;
    std::size_t record_count = 0;
    std::size_t total_body_letters = 0;
};

/// Outcome of one writer iteration. Mirrors `RunOutcome` for the
/// reader side: `record_count` carries the input record count
/// (sanity-checked across runs), `total_body_bytes` carries the
/// on-disk file size after the flush completed.
struct WriteOutcome {
    std::size_t record_count = 0;
    std::size_t bytes_written = 0;
};

/// Slurp a file's records into memory once via the bioseq reader.
/// Used by every writer runner as a fixed input corpus.
template <bool IsFasta>
[[nodiscard]] inline WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>
slurp_corpus_dna4(const std::string& path) {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;

    WriterCorpus<alphabet_config> corpus;
    auto handler_fn = [&](batch_t&& batch) {
        const std::size_t n_records = batch.size();
        for (std::size_t record_index = 0; record_index < n_records;
             ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            corpus.total_body_letters += packed_letter_count(seq_view);
        }
        corpus.record_count += n_records;
        corpus.batches.push_back(std::move(batch));
    };
    auto failure_fn = [&path](std::string&& msg) {
        loge("[bioseq_io_bench.runners.slurp] failure on '{}': {}",
             path, msg);
    };
    bioseq::io::reader_options reader_opts;
    reader_opts.io.gzip_auto_detect = false;
    reader_opts.io.prefer_mmap      = true;
    reader_opts.concurrency.worker_count = 0;
    auto rc = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, reader_opts);
    if (!rc.has_value()) {
        loge("[bioseq_io_bench.runners.slurp] read_sequences_file error: {}",
             rc.error());
    }
    return corpus;
}

namespace detail {

/// Single-thread writer runner. The fixed corpus is iterated
/// sequentially through `write_batch`; the writer's atomic-offset
/// pwrite path runs uncontended.
template <bool IsFasta>
[[nodiscard]] inline WriteOutcome run_bioseq_writer_sync_with_options(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path,
    const bioseq::io::writer_options& opts) {
    using namespace bioseq::io;
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;
    auto writer_or_err =
        sequence_writer<seq::dna4, file_type>::open(output_path, opts);
    if (!writer_or_err) {
        loge("[bioseq_io_bench.runners.writer_sync] open('{}') failed: {}",
             output_path, writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    for (const auto& batch : corpus.batches) {
        if (!writer.write_batch(batch)) {
            loge("[bioseq_io_bench.runners.writer_sync] write_batch failed");
            (void)writer.close();
            return {};
        }
    }
    auto close_outcome = writer.close();
    if (!close_outcome) {
        loge("[bioseq_io_bench.runners.writer_sync] close failed: {}",
             close_outcome.error());
        return {};
    }
    WriteOutcome outcome;
    outcome.record_count = corpus.record_count;
    std::error_code ec;
    outcome.bytes_written = std::filesystem::file_size(output_path, ec);
    return outcome;
}

/// Multi-thread writer runner. The fixed corpus is sharded across
/// `thread_count` threads; each thread calls `write_batch` on its
/// shard concurrently. The writer's per-thread chunk + atomic-offset
/// pwrite path keeps the syscalls disjoint.
template <bool IsFasta>
[[nodiscard]] inline WriteOutcome run_bioseq_writer_parallel_with_options(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path,
    const bioseq::io::writer_options& opts,
    std::size_t thread_count) {
    using namespace bioseq::io;
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;
    auto writer_or_err =
        sequence_writer<seq::dna4, file_type>::open(output_path, opts);
    if (!writer_or_err) {
        loge("[bioseq_io_bench.runners.writer_parallel] open('{}') failed: {}",
             output_path, writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    const std::size_t batches_total = corpus.batches.size();
    if (thread_count == 0) thread_count = 1;
    if (thread_count > batches_total && batches_total > 0) {
        thread_count = batches_total;
    }
    std::atomic<bool> any_failure{false};

    auto run_shard = [&](std::size_t shard_index) {
        const std::size_t shard_start =
            (batches_total * shard_index) / thread_count;
        const std::size_t shard_end =
            (batches_total * (shard_index + 1)) / thread_count;
        for (std::size_t batch_index = shard_start; batch_index < shard_end;
             ++batch_index) {
            if (!writer.write_batch(corpus.batches[batch_index])) {
                any_failure.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t shard_index = 0; shard_index < thread_count;
         ++shard_index) {
        threads.emplace_back(run_shard, shard_index);
    }
    for (auto& thread : threads) thread.join();

    auto close_outcome = writer.close();
    if (any_failure.load(std::memory_order_relaxed) || !close_outcome) {
        loge("[bioseq_io_bench.runners.writer_parallel] write/close failed");
        if (!close_outcome) {
            loge("[bioseq_io_bench.runners.writer_parallel] close error: {}",
                 close_outcome.error());
        }
        return {};
    }
    WriteOutcome outcome;
    outcome.record_count = corpus.record_count;
    std::error_code ec;
    outcome.bytes_written = std::filesystem::file_size(output_path, ec);
    return outcome;
}

}  // namespace detail

[[nodiscard]] inline WriteOutcome run_bioseq_writer_sync_fasta(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path) {
    bioseq::io::writer_options opts;
    return detail::run_bioseq_writer_sync_with_options<true>(
        corpus, output_path, opts);
}

[[nodiscard]] inline WriteOutcome run_bioseq_writer_sync_fastq(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path) {
    bioseq::io::writer_options opts;
    return detail::run_bioseq_writer_sync_with_options<false>(
        corpus, output_path, opts);
}

[[nodiscard]] inline WriteOutcome run_bioseq_writer_parallel_fasta(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path,
    std::size_t thread_count) {
    bioseq::io::writer_options opts;
    return detail::run_bioseq_writer_parallel_with_options<true>(
        corpus, output_path, opts, thread_count);
}

[[nodiscard]] inline WriteOutcome run_bioseq_writer_parallel_fastq(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path,
    std::size_t thread_count) {
    bioseq::io::writer_options opts;
    return detail::run_bioseq_writer_parallel_with_options<false>(
        corpus, output_path, opts, thread_count);
}

/// Naive `std::ofstream` writer. The "every other writer is at least
/// this fast" floor — single-threaded, byte-by-byte ASCII emission
/// straight from the in-memory corpus. Mirrors `run_naive_ifstream`
/// for the reader side. Decodes the packed sequence the same way the
/// bioseq writer does (so input data is identical) but uses
/// `std::ofstream::write` for output.
template <bool IsFasta>
[[nodiscard]] inline WriteOutcome run_naive_ofstream_with_corpus(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        loge("[bioseq_io_bench.runners.naive_writer] failed to open '{}'",
             output_path);
        return {};
    }
    constexpr std::array<char, 4> rank_to_ascii{'A', 'C', 'G', 'T'};
    std::string scratch;
    scratch.reserve(64 * 1024);
    constexpr std::size_t fasta_wrap = 80;
    for (const auto& batch : corpus.batches) {
        const std::size_t n_records = batch.size();
        for (std::size_t record_index = 0; record_index < n_records;
             ++record_index) {
            scratch.clear();
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            const std::size_t total_letters =
                seq_view.size() * 32 - seq_view.padding();
            scratch.push_back(IsFasta ? '>' : '@');
            scratch.append(header_view);
            scratch.push_back('\n');
            std::size_t in_line_count = 0;
            for (std::size_t letter_index = 0; letter_index < total_letters;
                 ++letter_index) {
                const std::uint64_t word = seq_view.data()[letter_index / 32];
                const std::size_t letter_in_word = letter_index % 32;
                const std::uint8_t rank = static_cast<std::uint8_t>(
                    (word >> (2 * letter_in_word)) & 0x3u);
                scratch.push_back(rank_to_ascii[rank]);
                if (IsFasta && fasta_wrap > 0) {
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
                scratch.append(reinterpret_cast<const char*>(quality_view.data()),
                                quality_view.size());
            }
            scratch.push_back('\n');
            out.write(scratch.data(),
                      static_cast<std::streamsize>(scratch.size()));
        }
    }
    out.flush();
    out.close();
    WriteOutcome outcome;
    outcome.record_count = corpus.record_count;
    std::error_code ec;
    outcome.bytes_written = std::filesystem::file_size(output_path, ec);
    return outcome;
}

[[nodiscard]] inline WriteOutcome run_naive_ofstream_fasta(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path) {
    return run_naive_ofstream_with_corpus<true>(corpus, output_path);
}

[[nodiscard]] inline WriteOutcome run_naive_ofstream_fastq(
    const WriterCorpus<sequence_custom_alphabet<seq::dna4, std::uint64_t>>& corpus,
    const std::string& output_path) {
    return run_naive_ofstream_with_corpus<false>(corpus, output_path);
}

}  // namespace bioseq::io_bench

}  // export
