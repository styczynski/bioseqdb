/**
 * @file matrix_runner.cppm
 * @brief IO bench matrix sweep over (reader × format × length × source).
 *
 * The Catch2 entry point in `io_bench/bench_test.cpp` calls
 * `run_io_bench_matrix(...)`. Each cell:
 *
 *   1. Generates a synthetic corpus once via `corpus.cppm`.
 *   2. Spawns the reader inventory once and times each reader N times
 *      (warm-up + repeats) via `bench_scheduler::collect_timed_samples`.
 *   3. Records the per-iteration latencies + RSS deltas + record
 *      counts into the process-wide `io_bench_samples()` accumulator.
 *   4. Deletes the corpus so the temp filesystem doesn't accumulate
 *      gigabytes of synthetic FASTA across runs.
 *
 * The cells dispatch through `bench_scheduler::run_parallel_indexed`
 * even though they are usually IO-bound — because the runner pool
 * gives us the FTXUI dashboard wiring (per-bucket throughput +
 * progress) without the harness having to talk to it directly.
 */
module;

#include "common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

export module bioseq_io_bench.matrix_runner;

import mlogging;
import bench.scheduler.measurement;
import bench.scheduler.parallel_runner;
import bench.scheduler.progress_ui;
import bench.scheduler.run_config;
import bench.scheduler.options;
import bioseq_io_bench.samples;
import bioseq_io_bench.corpus;
import bioseq_io_bench.runners;

export {

namespace bioseq::io_bench {

/// Handle to one runner. The lambda closes over its own state
/// (corpus path, async worker count); the matrix runner only sees
/// `(label, kind, source_kind, worker_count, callable)`.
struct ReaderEntry {
    std::string label;
    std::string kind;             // "bioseq_io" / "kseqpp" / "naive"
    IoSourceKind source_kind = IoSourceKind::POSIX_READ;
    std::size_t worker_count = 0;
    std::function<RunOutcome(const std::string&)> run;
};

/// Inventory of every reader the bench compares for FASTA dna4. The
/// matrix runner walks this list per cell.
[[nodiscard]] inline std::vector<ReaderEntry> default_fasta_readers() {
    std::vector<ReaderEntry> readers;
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_sync_fasta",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::POSIX_READ,
        .worker_count = 0,
        .run = [](const std::string& path) {
            return run_bioseq_io_sync_fasta(path);
        },
    });
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_mmap_fasta",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::MMAP,
        .worker_count = 0,
        .run = [](const std::string& path) {
            return run_bioseq_io_mmap_fasta(path);
        },
    });
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_async4_fasta",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::POSIX_READ,
        .worker_count = 4,
        .run = [](const std::string& path) {
            return run_bioseq_io_async_fasta(path, 4);
        },
    });
    readers.push_back(ReaderEntry{
        .label = "kseqpp",
        .kind  = "kseqpp",
        .source_kind = IoSourceKind::GZIP,
        .worker_count = 0,
        .run = [](const std::string& path) { return run_kseqpp(path); },
    });
    readers.push_back(ReaderEntry{
        .label = "naive_ifstream",
        .kind  = "naive",
        .source_kind = IoSourceKind::POSIX_READ,
        .worker_count = 0,
        .run = [](const std::string& path) {
            return run_naive_ifstream_fasta(path);
        },
    });
    return readers;
}

[[nodiscard]] inline std::vector<ReaderEntry> default_fastq_readers() {
    std::vector<ReaderEntry> readers;
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_sync_fastq",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::POSIX_READ,
        .worker_count = 0,
        .run = [](const std::string& path) {
            return run_bioseq_io_sync_fastq(path);
        },
    });
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_mmap_fastq",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::MMAP,
        .worker_count = 0,
        .run = [](const std::string& path) {
            return run_bioseq_io_mmap_fastq(path);
        },
    });
    readers.push_back(ReaderEntry{
        .label = "kseqpp",
        .kind  = "kseqpp",
        .source_kind = IoSourceKind::GZIP,
        .worker_count = 0,
        .run = [](const std::string& path) { return run_kseqpp(path); },
    });
    readers.push_back(ReaderEntry{
        .label = "naive_ifstream",
        .kind  = "naive",
        .source_kind = IoSourceKind::POSIX_READ,
        .worker_count = 0,
        .run = [](const std::string& path) {
            return run_naive_ifstream_fastq(path);
        },
    });
    return readers;
}

/// Number of timed samples per (reader, cell) combination. Lower for
/// the long buckets so a full matrix completes inside the bench-io
/// budget (~17 min hard cap).
[[nodiscard]] inline std::size_t samples_per_cell(IoLengthBucket bucket,
                                                   bool fast_mode) noexcept {
    if (fast_mode) {
        switch (bucket) {
            case IoLengthBucket::SHORT:             return 4;
            case IoLengthBucket::MEDIUM:            return 3;
            case IoLengthBucket::LONG:              return 2;
            case IoLengthBucket::VERY_LONG:         return 1;
            case IoLengthBucket::REAL_HUMAN_GENOME: return 2;
            // 6 GiB cell: a single sample is already ~1-2 s wall-clock
            // even at 5+ GB/s; one sample under fast mode keeps the cycle
            // tight without changing the regression signal.
            case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB: return 1;
        }
        return 1;
    }
    switch (bucket) {
        case IoLengthBucket::SHORT:             return 8;
        case IoLengthBucket::MEDIUM:            return 5;
        case IoLengthBucket::LONG:              return 3;
        case IoLengthBucket::VERY_LONG:         return 2;
        case IoLengthBucket::REAL_HUMAN_GENOME: return 3;
        case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB: return 2;
    }
    return 1;
}

/// Run one cell: prepare corpus (generated or loaded from disk for
/// the real-data buckets), time every reader, append samples to the
/// global accumulator, delete temp corpus when applicable.
inline void run_cell(IoFormat format,
                     IoLengthBucket length_bucket,
                     std::span<const ReaderEntry> readers,
                     std::uint64_t corpus_seed,
                     std::size_t samples_per_reader,
                     std::string_view test_data_dir) {
    CorpusHandle corpus;
    if (length_bucket == IoLengthBucket::REAL_HUMAN_GENOME ||
        length_bucket == IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB) {
        corpus = try_open_real_corpus(test_data_dir, format, length_bucket);
        if (corpus.path.empty()) {
            loge("[bioseq_io_bench.matrix_runner] real-data cell {} {} skipped "
                 "— file missing under '{}'",
                 io_format_name(format), io_length_bucket_name(length_bucket),
                 test_data_dir);
            return;
        }
    } else {
        corpus = generate_corpus(format, length_bucket, corpus_seed);
        if (corpus.bytes_on_disk == 0) {
            loge("[bioseq_io_bench.matrix_runner] empty corpus for {} {} — skipping cell",
                 io_format_name(format), io_length_bucket_name(length_bucket));
            delete_corpus(corpus);
            return;
        }
    }

    // Warm-up via every reader once so file-cache and allocator
    // first-call costs don't pollute the percentile estimate. For
    // real-data cells the corpus record_count is unknown a priori
    // (we don't pre-scan the file), so the first reader's warm-up
    // observation establishes the reference; subsequent readers
    // verify against it.
    std::size_t reference_record_count = corpus.record_count;
    bool reference_count_set = (reference_record_count > 0);
    for (const auto& reader : readers) {
        const auto warm_outcome = reader.run(corpus.path);
        if (!reference_count_set) {
            reference_record_count = warm_outcome.record_count;
            reference_count_set = true;
            corpus.record_count = reference_record_count;
            JustLog("[bioseq_io_bench.matrix_runner] real-data cell {} {} "
                    "warm-up established reference record_count={}",
                    io_format_name(format),
                    io_length_bucket_name(length_bucket),
                    reference_record_count);
        } else if (warm_outcome.record_count != reference_record_count) {
            loge("[bioseq_io_bench.matrix_runner] reader '{}' "
                 "produced {} records on warm-up, expected {} (corpus {})",
                 reader.label, warm_outcome.record_count,
                 reference_record_count, corpus.path);
        }
    }

    for (const auto& reader : readers) {
        ReaderSample sample;
        sample.reader_label = reader.label;
        sample.reader_kind  = reader.kind;
        sample.format       = format;
        sample.length_bucket = length_bucket;
        sample.source_kind  = reader.source_kind;
        sample.worker_count = reader.worker_count;
        sample.format_label = io_format_name(format);
        sample.length_bucket_label = io_length_bucket_name(length_bucket);
        sample.source_kind_label = io_source_kind_name(reader.source_kind);
        sample.corpus_path  = corpus.path;
        sample.corpus_bytes_on_disk = corpus.bytes_on_disk;
        sample.record_count = corpus.record_count;

        // Sanity counters: how many timed runs disagree with the
        // expected record count or with a reference body-byte sum.
        // The first run sets the reference; subsequent runs compare.
        std::size_t mismatched_record_counts = 0;
        std::size_t mismatched_body_bytes = 0;
        std::size_t reference_body_bytes = 0;
        bool body_bytes_initialised = false;

        const auto outcome = bioseq::bench_scheduler::collect_timed_samples(
            samples_per_reader, [&]() {
                const RunOutcome run_outcome = reader.run(corpus.path);
                if (run_outcome.record_count != corpus.record_count) {
                    ++mismatched_record_counts;
                }
                if (!body_bytes_initialised) {
                    reference_body_bytes = run_outcome.total_body_bytes;
                    body_bytes_initialised = true;
                } else if (run_outcome.total_body_bytes != reference_body_bytes) {
                    ++mismatched_body_bytes;
                }
            });
        sample.sample_latencies_ns = std::move(outcome.sample_latencies_ns);
        sample.sample_peak_rss_delta_bytes =
            std::move(outcome.sample_peak_rss_delta_bytes);

        if (mismatched_record_counts > 0) {
            loge("[bioseq_io_bench.matrix_runner] reader '{}' record-count "
                 "drift on {} samples (cell {} {})",
                 reader.label, mismatched_record_counts,
                 io_format_name(format), io_length_bucket_name(length_bucket));
        }
        if (mismatched_body_bytes > 0) {
            loge("[bioseq_io_bench.matrix_runner] reader '{}' body-byte "
                 "drift on {} samples (cell {} {})",
                 reader.label, mismatched_body_bytes,
                 io_format_name(format), io_length_bucket_name(length_bucket));
        }

        io_bench_samples().push_back(std::move(sample));
    }

    delete_corpus(corpus);
}

/// Walk every (format × length_bucket) cell, dispatch through the
/// inventory of readers. The return is `void` because the side-effect
/// is appending to `io_bench_samples()`.
///
/// `test_data_dir` is the path to `main/data/` baked in via the
/// `BIOSEQ_IO_TEST_DATA_DIR` build-time macro. The real-data cell
/// (`REAL_HUMAN_GENOME`) loads `homo_sapiens_GRCh38_dna4.fasta`
/// from there; if the file is absent the cell is skipped.
inline void run_io_bench_matrix(bool fast_mode,
                                std::uint64_t base_seed,
                                std::string_view test_data_dir) {
    constexpr std::array<IoFormat, 2> formats{
        IoFormat::FASTA, IoFormat::FASTQ};
    constexpr std::array<IoLengthBucket, 6> length_buckets{
        IoLengthBucket::SHORT, IoLengthBucket::MEDIUM,
        IoLengthBucket::LONG,  IoLengthBucket::VERY_LONG,
        IoLengthBucket::REAL_HUMAN_GENOME,
        IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB};

    const auto fasta_readers = default_fasta_readers();
    const auto fastq_readers = default_fastq_readers();

    JustLog("[bioseq_io_bench.matrix_runner] starting matrix sweep "
            "(fast_mode={}) with test_data_dir='{}'",
            fast_mode ? "true" : "false", test_data_dir);

    std::size_t cell_index = 0;
    for (auto format : formats) {
        const auto& readers = (format == IoFormat::FASTA)
            ? fasta_readers : fastq_readers;
        for (auto length_bucket : length_buckets) {
            const std::size_t samples_per_reader =
                samples_per_cell(length_bucket, fast_mode);
            const std::uint64_t cell_seed = base_seed
                + cell_index * 0x9E3779B97F4A7C15ull;
            run_cell(format, length_bucket,
                     std::span<const ReaderEntry>{readers},
                     cell_seed, samples_per_reader,
                     test_data_dir);
            cell_index += 1;
        }
    }
    JustLog("[bioseq_io_bench.matrix_runner] matrix sweep complete: {} samples accumulated",
            io_bench_samples().size());
}

}  // namespace bioseq::io_bench

}  // export
