/**
 * @file samples.cppm
 * @brief Domain types for the IO bench's measurement record.
 *
 * Mirrors the shape of `bench/aligners/samples.cppm`: per-(reader,
 * format, length) measurement rows captured into a process-wide
 * accumulator the chart builder + regression glue read after Catch2's
 * session returns.
 *
 * Three axes:
 *
 *   * `IoFormat` — FASTA vs FASTQ. Same enum the rest of the reader
 *     uses (`bioseq::io::sequence_io_file_type`); aliased here so the
 *     bench's chart titles read naturally.
 *   * `IoLengthBucket` — coarse record-length bucket (SHORT / MEDIUM /
 *     LONG / VERY_LONG). Calibrated against typical FASTA shapes.
 *   * `IoSourceKind` — the I/O backend exercised (POSIX read vs mmap
 *     vs gzip-decompressed). The bench matrix walks plain-text plus
 *     mmap; gzip is correctness-tested elsewhere.
 *
 * Each `ReaderSample` carries:
 *   * `throughput_mibs` — bytes-on-disk / wall-clock; the headline
 *     metric the bench prints.
 *   * `records_per_second` — secondary metric for "is the parser
 *     allocating per-record or amortising?".
 *   * `peak_rss_delta_bytes` — process RSS growth across the read.
 *   * `record_count` — sanity-check; cross-readers should agree.
 *
 * Stored in a process-wide `io_bench_samples()` singleton so the chart
 * builder and the regression glue read the same store the matrix
 * runner writes.
 */
module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

export module bioseq_io_bench.samples;

export {

namespace bioseq::io_bench {

/// Format axis. Same values as `sequence_io_file_type` but stored as
/// a domain-local enum so the bench code doesn't have to re-include
/// `bioseq_io.options` everywhere.
enum class IoFormat : std::uint8_t {
    FASTA = 0,
    FASTQ = 1,
};

[[nodiscard]] constexpr const char* io_format_name(IoFormat format) noexcept {
    switch (format) {
        case IoFormat::FASTA: return "fasta";
        case IoFormat::FASTQ: return "fastq";
    }
    return "?";
}

/// Coarse record-length axis. Calibrated to common FASTA workloads:
/// short Illumina reads (~200 bp), medium contigs (~2 kbp), long
/// nanopore reads (~50 kbp), assembly-scale (~1 Mbp). The matrix
/// runner picks corpus parameters per bucket so each cell exercises
/// a representative corpus.
enum class IoLengthBucket : std::uint8_t {
    SHORT = 0,      ///< ~200 bp records (Illumina-style short reads)
    MEDIUM = 1,     ///< ~2 kbp records (contigs / mid-length reads)
    LONG = 2,       ///< ~50 kbp records (PacBio / Nanopore)
    VERY_LONG = 3,  ///< ~1 Mbp records (chromosome scaffolds)
    /// Real-data cell: `main/data/homo_sapiens_GRCh38_dna4.fasta`
    /// — a ~234 MiB FASTA with 24 records (chromosomes + scaffolds).
    /// Used to validate reader throughput on a "real" workload size,
    /// not the synthetic generator output. The file is committed to
    /// the repo at `main/data/`, so the bench is reproducible across
    /// machines.
    REAL_HUMAN_GENOME = 4,
    /// Real-data cell: `main/data/1000_genomes_human_SRR062634_sample_1_dna5.fastq`
    /// — a ~6 GiB FASTQ file. Stresses the FASTQ hot path at sizes
    /// that typical short-read sequencing workloads actually produce
    /// (Illumina HiSeq runs routinely emit ~6 GiB per lane). Streamed
    /// only when the file is present; otherwise the cell is skipped.
    REAL_HUMAN_GENOME_LARGE_FASTQ_6GB = 5,
    /// Same SRR062634 corpus but gzipped. ~1.96 GiB compressed. Drives
    /// the new `bioseq_io_pgzip_*` and `bioseq_io_zlib_*` runners; the
    /// reader-skip rule in matrix_runner restricts the cell to
    /// gzip-aware readers.
    REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ = 6,
};

[[nodiscard]] constexpr const char* io_length_bucket_name(
    IoLengthBucket bucket) noexcept {
    switch (bucket) {
        case IoLengthBucket::SHORT:             return "short (~200bp)";
        case IoLengthBucket::MEDIUM:            return "medium (~2kbp)";
        case IoLengthBucket::LONG:              return "long (~50kbp)";
        case IoLengthBucket::VERY_LONG:         return "very long (~1Mbp)";
        case IoLengthBucket::REAL_HUMAN_GENOME: return "real human genome (~234MiB)";
        case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB:
            return "real human fastq (~6GiB)";
        case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB_GZ:
            return "real human fastq (~1.9GiB gz)";
    }
    return "?";
}

/// I/O backend axis.
enum class IoSourceKind : std::uint8_t {
    POSIX_READ    = 0,
    MMAP          = 1,
    GZIP          = 2,
    PARALLEL_GZIP = 3,
};

[[nodiscard]] constexpr const char* io_source_kind_name(
    IoSourceKind kind) noexcept {
    switch (kind) {
        case IoSourceKind::POSIX_READ:    return "posix_read";
        case IoSourceKind::MMAP:          return "mmap";
        case IoSourceKind::GZIP:          return "gzip";
        case IoSourceKind::PARALLEL_GZIP: return "parallel_gzip";
    }
    return "?";
}

/// One measurement row. Captures every axis the chart builder pivots
/// on plus the per-iteration latency vector so the builder can report
/// mean / P50 / P99 throughput without re-running the corpus.
struct ReaderSample {
    /// Reader being measured ("bioseq_io_sync", "bioseq_io_async_4",
    /// "kseqpp", "naive_ifstream", ...). Becomes the per-trace label
    /// on charts.
    std::string reader_label;

    /// Same shape as the `algorithm_kind` field in the aligner bench
    /// — used to colour-code chart traces. Values:
    ///   "bioseq_io" / "kseqpp" / "naive".
    std::string reader_kind;

    IoFormat       format         = IoFormat::FASTA;
    IoLengthBucket length_bucket  = IoLengthBucket::SHORT;
    IoSourceKind   source_kind    = IoSourceKind::POSIX_READ;

    /// Worker thread count the reader was configured for. 0 means
    /// "synchronous" (no extra worker pool); non-zero means async
    /// pipeline with N workers consuming batches.
    std::size_t worker_count = 0;

    /// Stable display labels (bucket + format + source) so the chart
    /// builder doesn't have to re-call the *_name() helpers per cell.
    std::string format_label;
    std::string length_bucket_label;
    std::string source_kind_label;

    /// Path to the corpus file the reader read from. Carried for
    /// debugging diagnostics; the chart layer ignores it.
    std::string corpus_path;

    /// Bytes-on-disk in the corpus. Used to compute throughput from
    /// wall-clock latency.
    std::size_t corpus_bytes_on_disk = 0;

    /// Decompressed corpus size — only differs from
    /// `corpus_bytes_on_disk` for compressed cells. The chart layer
    /// pivots compressed-cell throughput on this value (decompressed
    /// MiB/s).
    std::size_t corpus_decompressed_bytes = 0;

    /// Total record count across the corpus.
    std::size_t record_count = 0;

    /// Per-iteration wall-clock latency (ns). Sorted ascending.
    std::vector<double> sample_latencies_ns;

    /// Per-iteration peak-RSS delta (bytes). Same length as
    /// `sample_latencies_ns`. Loop-mean replicated per slot — see
    /// `bench.scheduler.measurement::collect_timed_samples`.
    std::vector<double> sample_peak_rss_delta_bytes;
};

/// Process-wide accumulator. Lives in a function-local static so the
/// matrix runner appends from worker threads while Catch2's TEST_CASE
/// is still spinning, and the chart builder reads the same vector
/// after `session.run()` returns.
inline std::vector<ReaderSample>& io_bench_samples() {
    static std::vector<ReaderSample> samples;
    return samples;
}

/// Writer-side measurement row. Same shape as `ReaderSample` but
/// keyed by the writer label and the on-disk byte count of the
/// **output** corpus (since the writer's input is in-memory and the
/// throughput is measured against the bytes the writer emitted).
struct WriterSample {
    /// Writer being measured ("bioseq_writer_sync_fasta",
    /// "bioseq_writer_parallel4_fasta", "naive_ofstream_fasta", ...).
    std::string writer_label;

    /// Kind tag used by the chart layer to colour-code traces.
    /// Values: "bioseq_writer" / "naive".
    std::string writer_kind;

    IoFormat       format         = IoFormat::FASTA;
    IoLengthBucket length_bucket  = IoLengthBucket::SHORT;

    /// Worker thread count the writer was configured with. 0 = single
    /// producer thread (synchronous); non-zero = parallel producer
    /// pool sharing one writer object via the lock-free pwrite path.
    std::size_t worker_count = 0;

    std::string format_label;
    std::string length_bucket_label;

    /// Path to the source corpus the writer pulled records from.
    std::string corpus_path;

    /// Bytes of FASTA / FASTQ the writer emitted to its output file.
    /// Used to compute throughput from wall-clock latency.
    std::size_t bytes_written = 0;

    /// Total record count the writer emitted (same number per
    /// iteration; sanity-checked across runs).
    std::size_t record_count = 0;

    /// Roundtrip validation: number of records the reader saw when
    /// re-reading the writer's output. 0 if validation was skipped.
    std::size_t roundtrip_record_count = 0;

    /// Per-iteration wall-clock latency (ns). Sorted ascending.
    std::vector<double> sample_latencies_ns;

    /// Per-iteration peak-RSS delta (bytes). Same length as
    /// `sample_latencies_ns`.
    std::vector<double> sample_peak_rss_delta_bytes;
};

/// Process-wide writer accumulator. Same shape as `io_bench_samples()`
/// but for writer rows. Kept as a separate vector so the chart layer
/// can render reader and writer charts independently without filter
/// gymnastics.
inline std::vector<WriterSample>& io_bench_writer_samples() {
    static std::vector<WriterSample> samples;
    return samples;
}

}  // namespace bioseq::io_bench

}  // export
