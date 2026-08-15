/**
 * @file options.cppm
 * @brief Reader and writer configuration with default policies.
 */
module;

#include <cstddef>
#include <cstdint>

export module bioseq.io.options;

export namespace bioseq::io {

/// Which file format `read_sequences_file` is being asked to parse. The
/// callers select one format through the template parameter. The reader does
/// not auto-detect per record.
enum class sequence_file_format : std::uint8_t {
    fasta = 0,
    fastq = 1,
};

/// I/O backend knobs (buffer sizes, mmap thresholds, gzip auto-detection).
struct io_options {
    /// Initial byte capacity of each aligned refill slab.
    std::size_t initial_buffer_size = 1 * 1024 * 1024;
    /// Maximum slab capacity available to an oversized record.
    std::size_t max_buffer_size = static_cast<std::size_t>(1) * 1024 * 1024 * 1024;
    bool prefer_mmap = true;
    std::size_t max_mmap_bytes = static_cast<std::size_t>(1) << 35;
    bool mmap_zero_copy = true;
    bool stream_madvise = true;
    bool gzip_auto_detect = true;

    /// Enable parallel gzip above the configured compressed-size threshold.
    bool parallel_gzip_enabled = true;
    std::size_t parallel_gzip_min_compressed_bytes = 16ULL * 1024 * 1024;
    /// 0 = use default_parallel_gzip_chunk_size = 4 MiB.
    std::size_t parallel_gzip_chunk_bytes = 0;
    /// 0 = auto (`hardware_concurrency() - 2`, capped at 6).
    std::size_t parallel_gzip_thread_count = 0;
    bool parallel_gzip_verify_crc = true;
};

/// Concurrency knobs (thread counts, ring sizes, batch shape).
struct concurrency_options {
    std::size_t worker_count = 0;
    /// Number of parsing threads for mmap parallel mode. 0 = auto
    /// (use std::thread::hardware_concurrency capped to 8). 1 forces the
    /// sequential single-threaded mmap path. N > 1 selects exactly N parser
    /// shards. The parallel path activates only on the mmap zero-copy
    /// path with file_size >= mmap_parallel_min_bytes.
    std::size_t parser_count = 0;
    std::size_t io_buffer_pool_size = 8;
    std::size_t ring_capacity = 32;
    std::size_t batch_target_record_count = 0;
    std::size_t batch_min_payload_bytes = 256 * 1024;
    bool compact_batch = true;
    /// Minimum mmap'd file size that activates parallel sharded
    /// parsing. Smaller files use the sequential mmap path to avoid
    /// shard setup and per-shard storage overhead.
    std::size_t mmap_parallel_min_bytes = static_cast<std::size_t>(1024) * 1024 * 1024;
    /// Hard cap on shard count even when more hardware threads are available.
    std::size_t mmap_parallel_max_shards = 8;
};

/// Encoder knobs (alphabet validation, FASTQ strictness).
struct encoder_options {
    bool strict_alphabet = false;
    bool strict_fastq = true;
};

/// Diagnostics knobs (progress traces).
struct diagnostics_options {
    bool trace_progress = false;
    std::size_t trace_interval_bytes = 64ull * 1024 * 1024;
};

/// Top-level options aggregate. Composed of four orthogonal sub-structs
/// so designated initialisation reads cleanly. Default constructor gives
/// sensible defaults for every field.
struct reader_options {
    io_options io{};
    concurrency_options concurrency{};
    encoder_options encoder{};
    diagnostics_options diagnostics{};
};

inline constexpr reader_options default_reader_options{};

/// Knobs for the FASTA / FASTQ writer. The writer's hot path is
/// (a) decode packed sequence words → ASCII, (b) inject newlines for
/// FASTA wrap, (c) pwrite the per-thread output chunk to the file's
/// disjoint range claimed via an atomic offset. The defaults are
/// selected to bound syscall frequency and per-thread memory use.
struct writer_options {
    /// Per-line width for FASTA bodies (letters before a `\n`). 0 = no
    /// wrapping (one giant line per record). Default 0 = no wrapping
    /// for maximum throughput. The FASTA specification accepts arbitrary line
    /// lengths and bioseq's reader handles single-line bodies fine.
    /// Set to 80 for kseq++-canonical output.
    std::size_t fasta_line_width = 0;

    /// Size (in bytes) of one producer-thread's output chunk. The
    /// chunk fills with formatted bytes. Once it crosses
    /// `flush_threshold_bytes` the thread reserves a disjoint file
    /// range via atomic-fetch-add and pwrites the chunk in one
    /// syscall.
    std::size_t chunk_capacity_bytes = 64 * 1024 * 1024;

    /// Threshold at which a producer thread flushes its chunk. Below
    /// the chunk capacity by a small margin so that a single record's
    /// formatting can't overrun the chunk in one write call.
    std::size_t flush_threshold_bytes = 64 * 1024 * 1024 - 64 * 1024;

    /// When the format is FASTQ, refuse to write a record whose
    /// quality span length doesn't match the sequence's letter count.
    /// Default false: real-world FASTQ corpora (e.g. SRR datasets)
    /// occasionally contain records where the qual string is one byte
    /// off from the sequence. The writer truncates quality to the
    /// sequence's letter count instead of aborting. Set true to
    /// enforce strict matching.
    bool strict_fastq_quality_length = false;

    /// If true, a record larger than `chunk_capacity_bytes` is
    /// streamed in pieces (multiple pwrite calls per record). If
    /// false, oversize records bypass the chunk entirely and pwrite
    /// directly. Default true keeps thread-local behaviour predictable.
    bool stream_oversize_records = true;

    /// Pre-allocate the output file to this size and mmap it for
    /// writing. The writer's per-thread chunks then commit via
    /// userspace memcpy into the mapped region (no kernel pwrite
    /// memcpy). After close, the file is `ftruncate`d down to the
    /// actual bytes written. Set to 0 (default) to use the
    /// pwrite-based sink (no size-hint required). When non-zero, the
    /// hint MUST be a generous upper bound on the output file size
    /// — exceeding it surfaces as a writer error at flush time.
    std::size_t output_capacity_hint = 0;
};

inline constexpr writer_options default_writer_options{};

} // namespace bioseq::io
