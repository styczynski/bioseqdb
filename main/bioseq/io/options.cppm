module;

#include "common.h"

#include <cstddef>
#include <cstdint>

export module bioseq_io.options;

export namespace bioseq::io {

/// Which file format `read_sequences_file` is being asked to parse. The
/// user MUST commit to one format up front (template parameter); we do
/// not auto-detect per record. See tech spec §3.3.
enum class sequence_io_file_type : std::uint8_t {
    FASTA = 0,
    FASTQ = 1,
};

/// I/O backend knobs (buffer sizes, mmap thresholds, gzip auto-detection).
struct io_options {
    // Larger initial buffer = fewer slab cycles + fewer kernel→user
    // copies on the posix_read path. 1 MiB matches kseqpp's 128 KiB +
    // headroom; on the 234-MiB human-genome cell this cuts refill
    // cycles from ~3650 → 234. Past 1 MiB the marginal win drops
    // (memcpy bandwidth dominates the syscall overhead) and very
    // large slabs fight the page-cache prefetcher. Empirically 4 MiB
    // regressed across all the synthetic cells (build_26) — the
    // additional `make_room` memmove cost on each refill outweighed
    // the syscall amortisation.
    std::size_t initial_buffer_size = 1 * 1024 * 1024;
    std::size_t max_buffer_size = static_cast<std::size_t>(1) * 1024 * 1024 * 1024;
    bool        prefer_mmap = true;
    std::size_t max_mmap_bytes = static_cast<std::size_t>(1) << 35;
    bool        mmap_zero_copy = true;
    bool        stream_madvise = true;
    bool        gzip_auto_detect = true;
};

/// Concurrency knobs (thread counts, ring sizes, batch shape).
struct concurrency_options {
    std::size_t worker_count = 0;
    /// Number of parsing threads for mmap parallel mode. 0 = auto
    /// (use std::thread::hardware_concurrency capped to 8); 1 = force
    /// sequential single-threaded mmap path; N > 1 = exactly N parser
    /// shards. The parallel path activates only on the mmap zero-copy
    /// path with file_size >= mmap_parallel_min_bytes.
    std::size_t parser_count = 0;
    std::size_t io_buffer_pool_size = 8;
    std::size_t ring_capacity = 32;
    std::size_t batch_target_record_count = 0;
    std::size_t batch_min_payload_bytes = 256 * 1024;
    bool        compact_batch = true;
    /// Minimum mmap'd file size that activates parallel sharded
    /// parsing. Below this, the sequential mmap path runs.
    /// Empirically: the synthetic LONG / VERY_LONG cells (≤ 16 MiB)
    /// don't have enough records to balance shards. The 234 MiB
    /// human-genome FASTA cell has 24 records — 8 shards = 3 records
    /// per shard, each ~10 MiB — and the macOS `vm_map` lock
    /// serialises the demand-zero of 8 × 30 MiB anonymous-mmap
    /// regions per iteration, hurting throughput by ~50% vs
    /// sequential. The 6 GiB SRR FASTQ cell has ~30 M short records
    /// spread across 8 shards of 750 MiB each — the per-shard demand
    /// -zero amortises across millions of records and parallel
    /// parsing wins by ~5× over single-threaded. 1 GiB threshold
    /// keeps the human cell sequential and the SRR cell parallel.
    std::size_t mmap_parallel_min_bytes =
        static_cast<std::size_t>(1024) * 1024 * 1024;
    /// Hard cap on shard count even if hardware_concurrency is higher.
    /// 8 perf cores on M1 Pro is the sweet spot; oversubscription past
    /// 8 contends on the L1 ports the SIMD pack pipeline already
    /// saturates.
    std::size_t mmap_parallel_max_shards = 8;
};

/// Encoder knobs (alphabet validation, FASTQ strictness).
struct encoder_options {
    bool strict_alphabet = false;
    bool strict_fastq = true;
};

/// Diagnostics knobs (progress traces).
struct diagnostics_options {
    bool        trace_progress = false;
    std::size_t trace_interval_bytes = 64ull * 1024 * 1024;
};

/// Top-level options aggregate. Composed of four orthogonal sub-structs
/// so designated initialisation reads cleanly. Default constructor gives
/// sensible defaults for every field.
struct reader_options {
    io_options          io{};
    concurrency_options concurrency{};
    encoder_options     encoder{};
    diagnostics_options diagnostics{};
};

inline constexpr reader_options default_reader_options{};

/// Knobs for the FASTA / FASTQ writer. The writer's hot path is
/// (a) decode packed sequence words → ASCII, (b) inject newlines for
/// FASTA wrap, (c) pwrite the per-thread output chunk to the file's
/// disjoint range claimed via an atomic offset. The defaults are
/// calibrated for the bench's largest cells (234 MiB human FASTA,
/// 6 GiB SRR FASTQ).
struct writer_options {
    /// Per-line width for FASTA bodies (letters before a `\n`). 0 = no
    /// wrapping (one giant line per record). Default 0 = no wrapping
    /// for maximum throughput; the FASTA spec accepts arbitrary line
    /// lengths and bioseq's reader handles single-line bodies fine.
    /// Set to 80 for kseq++-canonical output.
    std::size_t fasta_line_width = 0;

    /// Size (in bytes) of one producer-thread's output chunk. The
    /// chunk fills with formatted bytes; once it crosses
    /// `flush_threshold_bytes` the thread reserves a disjoint file
    /// range via atomic-fetch-add and pwrites the chunk in one
    /// syscall. 64 MiB strikes a balance between syscall amortisation
    /// (only ~96 syscalls for the 6 GiB FASTQ cell) and per-thread
    /// memory footprint (with 8 producer threads, peak working set
    /// is 512 MiB).
    std::size_t chunk_capacity_bytes = 64 * 1024 * 1024;

    /// Threshold at which a producer thread flushes its chunk. Below
    /// the chunk capacity by a small margin so that a single record's
    /// formatting can't overrun the chunk in one write call.
    std::size_t flush_threshold_bytes = 64 * 1024 * 1024 - 64 * 1024;

    /// When the format is FASTQ, refuse to write a record whose
    /// quality span length doesn't match the sequence's letter count.
    /// Default false: real-world FASTQ corpora (e.g. SRR datasets)
    /// occasionally contain records where the qual string is one byte
    /// off from the sequence; the writer truncates qual to the
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

}  // namespace bioseq::io
