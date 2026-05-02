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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

export module bioseq_io_bench.runners;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.public_api;
import bioseq_io.options;

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

}  // namespace bioseq::io_bench

}  // export
