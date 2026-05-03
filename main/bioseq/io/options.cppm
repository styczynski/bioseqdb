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

}  // namespace bioseq::io
