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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

export module bioseq_io_bench.matrix_runner;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bench.scheduler.measurement;
import bench.scheduler.parallel_runner;
import bench.scheduler.progress_ui;
import bench.scheduler.run_config;
import bench.scheduler.options;
import bioseq_io.public_api;
import bioseq_io.options;
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
    /// Nominal source-kind label. Whatever the bench's `run_cell` writes
    /// into `sample.source_kind` is the *actually observed* backend the
    /// dispatcher picked at run time, not this static value — see the
    /// per-cell wiring below for how the runner reports back. Kept as a
    /// classification axis (e.g. for the `requires_gz_input` flag).
    IoSourceKind source_kind = IoSourceKind::POSIX_READ;
    std::size_t worker_count = 0;
    /// True when this reader can only meaningfully process gzipped input.
    /// The bench's plain cells skip it. Without this filter the
    /// dispatcher's magic-byte sniff falls through to plain mmap and
    /// the runner posts mmap-speed numbers under a `parallel_gzip`
    /// label, conflating the two paths in the bar chart.
    /// kseqpp handles both plain and gz natively (libz handles both),
    /// so it stays `false` and runs on every cell.
    bool requires_gz_input = false;
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
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_pgzip_fasta",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::PARALLEL_GZIP,
        .worker_count = 6,
        .requires_gz_input = true,
        .run = [](const std::string& path) {
            return run_bioseq_io_pgzip_fasta(path);
        },
    });
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_zlib_fasta",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::GZIP,
        .worker_count = 0,
        .requires_gz_input = true,
        .run = [](const std::string& path) {
            return run_bioseq_io_zlib_fasta(path);
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
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_pgzip_fastq",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::PARALLEL_GZIP,
        .worker_count = 6,
        .requires_gz_input = true,
        .run = [](const std::string& path) {
            return run_bioseq_io_pgzip_fastq(path);
        },
    });
    readers.push_back(ReaderEntry{
        .label = "bioseq_io_zlib_fastq",
        .kind  = "bioseq_io",
        .source_kind = IoSourceKind::GZIP,
        .worker_count = 0,
        .requires_gz_input = true,
        .run = [](const std::string& path) {
            return run_bioseq_io_zlib_fastq(path);
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
            case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ: return 1;
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
        case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ: return 1;
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
        length_bucket == IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB ||
        length_bucket == IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ) {
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

    // Symmetric filter: gzip cells only run gzip-capable readers; plain
    // cells skip readers flagged `requires_gz_input` so the dispatcher's
    // magic-byte sniff fall-through to plain mmap doesn't get charted as
    // "gzip throughput". Applies to BOTH the warm-up and timed loop.
    const bool is_gzip_cell =
        (length_bucket ==
         IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ);
    const auto skip_reader = [&](const ReaderEntry& candidate) noexcept {
        if (is_gzip_cell) {
            return candidate.source_kind != IoSourceKind::GZIP &&
                   candidate.source_kind != IoSourceKind::PARALLEL_GZIP;
        }
        return candidate.requires_gz_input;
    };

    // Warm-up via every reader once so file-cache and allocator
    // first-call costs don't pollute the percentile estimate. For
    // real-data cells the corpus record_count is unknown a priori
    // (we don't pre-scan the file), so the first reader's warm-up
    // observation establishes the reference; subsequent readers
    // verify against it.
    std::size_t reference_record_count = corpus.record_count;
    bool reference_count_set = (reference_record_count > 0);
    for (const auto& reader : readers) {
        if (skip_reader(reader)) continue;
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

    // Convert the dispatcher-reported `read_backend` (from public_api)
    // into the bench's `IoSourceKind`. UNKNOWN means "this reader did
    // not go through the dispatcher" (e.g. kseqpp / naive_ifstream)
    // and we keep the reader's nominal source_kind in that case.
    const auto observed_to_source_kind = [](
            bioseq::io::read_backend backend,
            IoSourceKind nominal) noexcept -> IoSourceKind {
        switch (backend) {
            case bioseq::io::read_backend::MMAP:
                return IoSourceKind::MMAP;
            case bioseq::io::read_backend::POSIX_READ:
                return IoSourceKind::POSIX_READ;
            case bioseq::io::read_backend::GZIP:
                return IoSourceKind::GZIP;
            case bioseq::io::read_backend::PARALLEL_GZIP:
                return IoSourceKind::PARALLEL_GZIP;
            case bioseq::io::read_backend::UNKNOWN:
                break;
        }
        return nominal;
    };

    for (const auto& reader : readers) {
        if (skip_reader(reader)) continue;
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
        // For the gzipped cell, record the decompressed byte count
        // separately so the chart layer can pivot throughput on
        // decompressed MiB/s. Assume 3.2x for FASTQ from the SRR cell
        // shape (~6.0 GiB decompressed / ~1.96 GiB compressed). Real
        // value is computed by the runner's `total_bytes_processed`
        // counter at run time once the runner exposes it.
        if (length_bucket ==
            IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ) {
            sample.corpus_decompressed_bytes =
                static_cast<std::size_t>(corpus.bytes_on_disk * 3.2);
        } else {
            sample.corpus_decompressed_bytes = corpus.bytes_on_disk;
        }
        sample.record_count = corpus.record_count;

        // Sanity counters: how many timed runs disagree with the
        // expected record count or with a reference body-byte sum.
        // The first run sets the reference; subsequent runs compare.
        std::size_t mismatched_record_counts = 0;
        std::size_t mismatched_body_bytes = 0;
        std::size_t reference_body_bytes = 0;
        bool body_bytes_initialised = false;
        // Captured out of the lambda so the matrix runner can label
        // the sample with the dispatcher's actual choice rather than
        // the reader's nominal source_kind. Stable across timed
        // iterations (the dispatcher decides from the file path
        // and options, both fixed for this loop).
        bioseq::io::read_backend last_observed_backend =
            bioseq::io::read_backend::UNKNOWN;

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
                last_observed_backend = run_outcome.observed_backend;
            });
        sample.sample_latencies_ns = std::move(outcome.sample_latencies_ns);
        sample.sample_peak_rss_delta_bytes =
            std::move(outcome.sample_peak_rss_delta_bytes);

        // Re-label the sample with the actual dispatcher backend if
        // the runner reported one. kseqpp / naive_ifstream stay at
        // their nominal source_kind because they don't go through
        // `read_sequences_file`.
        const IoSourceKind observed_kind = observed_to_source_kind(
            last_observed_backend, reader.source_kind);
        sample.source_kind        = observed_kind;
        sample.source_kind_label  = io_source_kind_name(observed_kind);

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

// =====================================================================
// Writer-side matrix
// =====================================================================
//
// Mirrors `ReaderEntry` for the writer. Each entry holds a label and
// a callable that takes (in-memory corpus, output path) and returns
// a `WriteOutcome`. The matrix runner times every entry against the
// pre-loaded corpus.

using BenchAlphabet = sequence_custom_alphabet<seq::dna4, std::uint64_t>;

struct WriterEntry {
    std::string label;
    std::string kind;             // "bioseq_writer" / "naive"
    std::size_t worker_count = 0;
    std::function<WriteOutcome(
        const WriterCorpus<BenchAlphabet>&,
        const std::string& /*output_path*/)> run;
};

/// Pre-defined writer inventory for FASTA dna4. Includes the bioseq
/// writer (sync + 4-thread parallel) and the naive ofstream baseline.
[[nodiscard]] inline std::vector<WriterEntry> default_fasta_writers() {
    std::vector<WriterEntry> writers;
    writers.push_back(WriterEntry{
        .label = "bioseq_writer_sync_fasta",
        .kind  = "bioseq_writer",
        .worker_count = 0,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_bioseq_writer_sync_fasta(corpus, path);
        },
    });
    writers.push_back(WriterEntry{
        .label = "bioseq_writer_parallel4_fasta",
        .kind  = "bioseq_writer",
        .worker_count = 4,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_bioseq_writer_parallel_fasta(corpus, path, 4);
        },
    });
    writers.push_back(WriterEntry{
        .label = "bioseq_writer_parallel8_fasta",
        .kind  = "bioseq_writer",
        .worker_count = 8,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_bioseq_writer_parallel_fasta(corpus, path, 8);
        },
    });
    writers.push_back(WriterEntry{
        .label = "naive_ofstream_fasta",
        .kind  = "naive",
        .worker_count = 0,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_naive_ofstream_fasta(corpus, path);
        },
    });
    return writers;
}

[[nodiscard]] inline std::vector<WriterEntry> default_fastq_writers() {
    std::vector<WriterEntry> writers;
    writers.push_back(WriterEntry{
        .label = "bioseq_writer_sync_fastq",
        .kind  = "bioseq_writer",
        .worker_count = 0,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_bioseq_writer_sync_fastq(corpus, path);
        },
    });
    writers.push_back(WriterEntry{
        .label = "bioseq_writer_parallel4_fastq",
        .kind  = "bioseq_writer",
        .worker_count = 4,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_bioseq_writer_parallel_fastq(corpus, path, 4);
        },
    });
    writers.push_back(WriterEntry{
        .label = "bioseq_writer_parallel8_fastq",
        .kind  = "bioseq_writer",
        .worker_count = 8,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_bioseq_writer_parallel_fastq(corpus, path, 8);
        },
    });
    writers.push_back(WriterEntry{
        .label = "naive_ofstream_fastq",
        .kind  = "naive",
        .worker_count = 0,
        .run = [](const WriterCorpus<BenchAlphabet>& corpus,
                  const std::string& path) {
            return run_naive_ofstream_fastq(corpus, path);
        },
    });
    return writers;
}

/// Lighter sample budget for the writer matrix — the writer cycle is
/// (slurp once + N timed write+roundtrip iterations). The 6 GiB FASTQ
/// cell at 4 GB/s = 1.5 s per iteration plus ~1.2 s roundtrip read,
/// so 1-2 samples is enough.
[[nodiscard]] inline std::size_t writer_samples_per_cell(
    IoLengthBucket bucket, bool fast_mode) noexcept {
    if (fast_mode) {
        switch (bucket) {
            case IoLengthBucket::SHORT:             return 4;
            case IoLengthBucket::MEDIUM:            return 3;
            case IoLengthBucket::LONG:              return 2;
            case IoLengthBucket::VERY_LONG:         return 1;
            case IoLengthBucket::REAL_HUMAN_GENOME: return 2;
            case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB: return 1;
            case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ: return 1;
        }
        return 1;
    }
    switch (bucket) {
        case IoLengthBucket::SHORT:             return 6;
        case IoLengthBucket::MEDIUM:            return 4;
        case IoLengthBucket::LONG:              return 3;
        case IoLengthBucket::VERY_LONG:         return 2;
        case IoLengthBucket::REAL_HUMAN_GENOME: return 3;
        case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB: return 2;
        case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ: return 1;
    }
    return 1;
}

/// Validate the writer's output by re-reading it via the bioseq
/// reader and counting records. Returns the observed record count.
template <bool IsFasta>
[[nodiscard]] inline std::size_t roundtrip_validate(
    const std::string& output_path) {
    using namespace bioseq::io;
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;
    std::atomic<std::size_t> records{0};
    auto handler_fn = [&records](batch_t&& batch) {
        records.fetch_add(batch.size(), std::memory_order_relaxed);
    };
    auto failure_fn = [&output_path](std::string&& msg) {
        loge("[bioseq_io_bench.matrix_runner.roundtrip] failure on '{}': {}",
             output_path, msg);
    };
    bioseq::io::reader_options reader_opts;
    reader_opts.io.gzip_auto_detect = false;
    reader_opts.io.prefer_mmap      = true;
    reader_opts.concurrency.worker_count = 0;
    auto rc = read_sequences_file<seq::dna4, file_type>(
        output_path, handler_fn, failure_fn, reader_opts);
    if (!rc.has_value()) {
        loge("[bioseq_io_bench.matrix_runner.roundtrip] reader error: {}",
             rc.error());
    }
    return records.load(std::memory_order_relaxed);
}

/// Run one writer cell: load corpus once via reader, time every
/// writer N times, validate roundtrip on the first iteration, append
/// samples.
inline void run_writer_cell(IoFormat format,
                             IoLengthBucket length_bucket,
                             std::span<const WriterEntry> writers,
                             std::uint64_t corpus_seed,
                             std::size_t samples_per_writer,
                             std::string_view test_data_dir) {
    CorpusHandle source;
    // The gzip cell is reader-only — writers have no compress
    // path. Skip the cell silently when the writer matrix encounters it.
    if (length_bucket == IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ) {
        return;
    }
    if (length_bucket == IoLengthBucket::REAL_HUMAN_GENOME ||
        length_bucket == IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB) {
        source = try_open_real_corpus(test_data_dir, format, length_bucket);
        if (source.path.empty()) {
            loge("[bioseq_io_bench.writer_matrix] real-data cell {} {} skipped "
                 "— file missing under '{}'",
                 io_format_name(format), io_length_bucket_name(length_bucket),
                 test_data_dir);
            return;
        }
    } else {
        source = generate_corpus(format, length_bucket, corpus_seed);
        if (source.bytes_on_disk == 0) {
            loge("[bioseq_io_bench.writer_matrix] empty corpus for {} {} — skipping",
                 io_format_name(format), io_length_bucket_name(length_bucket));
            delete_corpus(source);
            return;
        }
    }

    // Slurp the corpus into memory ONCE outside the timed loop so the
    // writer benches measure pure write throughput.
    using alphabet_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    WriterCorpus<alphabet_config> corpus;
    if (format == IoFormat::FASTA) {
        corpus = slurp_corpus_dna4<true>(source.path);
    } else {
        corpus = slurp_corpus_dna4<false>(source.path);
    }
    if (corpus.batches.empty()) {
        loge("[bioseq_io_bench.writer_matrix] slurp produced 0 batches "
             "for {} {} — skipping cell",
             io_format_name(format), io_length_bucket_name(length_bucket));
        delete_corpus(source);
        return;
    }
    JustLog("[bioseq_io_bench.writer_matrix] slurped {} batches / {} records "
            "/ {} body letters for cell {} {}",
            corpus.batches.size(), corpus.record_count,
            corpus.total_body_letters,
            io_format_name(format), io_length_bucket_name(length_bucket));

    namespace fs = std::filesystem;
    for (const auto& writer : writers) {
        // Allocate a unique temp output path for this writer.
        static std::atomic<std::uint64_t> writer_counter{0};
        const fs::path temp_dir = fs::temp_directory_path();
        const fs::path out_path = temp_dir / fmt::format(
            "bioseq_io_writer_bench_{}_{}_{}_{}",
            writer.label,
            ::getpid(),
            writer_counter.fetch_add(1, std::memory_order_relaxed),
            std::random_device{}() & 0xFFFFFFu);
        const std::string out_path_str = out_path.string();

        // Warm-up + roundtrip validation run.
        const auto warm_outcome = writer.run(corpus, out_path_str);
        std::size_t roundtrip_count = 0;
        if (warm_outcome.bytes_written > 0) {
            roundtrip_count = (format == IoFormat::FASTA)
                ? roundtrip_validate<true>(out_path_str)
                : roundtrip_validate<false>(out_path_str);
        }
        if (roundtrip_count != corpus.record_count) {
            loge("[bioseq_io_bench.writer_matrix] roundtrip mismatch for "
                 "writer '{}': wrote {} records, read back {} (cell {} {})",
                 writer.label, corpus.record_count, roundtrip_count,
                 io_format_name(format), io_length_bucket_name(length_bucket));
        }

        WriterSample sample;
        sample.writer_label = writer.label;
        sample.writer_kind  = writer.kind;
        sample.format       = format;
        sample.length_bucket = length_bucket;
        sample.worker_count  = writer.worker_count;
        sample.format_label  = io_format_name(format);
        sample.length_bucket_label = io_length_bucket_name(length_bucket);
        sample.corpus_path  = source.path;
        sample.bytes_written = warm_outcome.bytes_written;
        sample.record_count  = corpus.record_count;
        sample.roundtrip_record_count = roundtrip_count;

        const auto outcome = bioseq::bench_scheduler::collect_timed_samples(
            samples_per_writer, [&]() {
                const auto run_outcome = writer.run(corpus, out_path_str);
                (void)run_outcome;
            });
        sample.sample_latencies_ns = std::move(outcome.sample_latencies_ns);
        sample.sample_peak_rss_delta_bytes =
            std::move(outcome.sample_peak_rss_delta_bytes);

        io_bench_writer_samples().push_back(std::move(sample));

        std::error_code ec;
        fs::remove(out_path, ec);
    }

    delete_corpus(source);
}

inline void run_writer_bench_matrix(bool fast_mode,
                                     std::uint64_t base_seed,
                                     std::string_view test_data_dir) {
    constexpr std::array<IoFormat, 2> formats{
        IoFormat::FASTA, IoFormat::FASTQ};
    constexpr std::array<IoLengthBucket, 7> length_buckets{
        IoLengthBucket::SHORT, IoLengthBucket::MEDIUM,
        IoLengthBucket::LONG,  IoLengthBucket::VERY_LONG,
        IoLengthBucket::REAL_HUMAN_GENOME,
        IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB,
        IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ};

    const auto fasta_writers = default_fasta_writers();
    const auto fastq_writers = default_fastq_writers();

    JustLog("[bioseq_io_bench.writer_matrix] starting writer matrix sweep "
            "(fast_mode={})",
            fast_mode ? "true" : "false");

    std::size_t cell_index = 0;
    for (auto format : formats) {
        const auto& writers = (format == IoFormat::FASTA)
            ? fasta_writers : fastq_writers;
        for (auto length_bucket : length_buckets) {
            const std::size_t samples_per_writer =
                writer_samples_per_cell(length_bucket, fast_mode);
            const std::uint64_t cell_seed = base_seed
                + cell_index * 0xC2B2AE3D27D4EB4Full;
            run_writer_cell(format, length_bucket,
                             std::span<const WriterEntry>{writers},
                             cell_seed, samples_per_writer,
                             test_data_dir);
            cell_index += 1;
        }
    }
    JustLog("[bioseq_io_bench.writer_matrix] sweep complete: {} samples accumulated",
            io_bench_writer_samples().size());
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
    constexpr std::array<IoLengthBucket, 7> length_buckets{
        IoLengthBucket::SHORT, IoLengthBucket::MEDIUM,
        IoLengthBucket::LONG,  IoLengthBucket::VERY_LONG,
        IoLengthBucket::REAL_HUMAN_GENOME,
        IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB,
        IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ};

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
