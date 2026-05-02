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
    std::size_t initial_buffer_size = 64 * 1024;
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
    std::size_t parser_count = 1;
    std::size_t io_buffer_pool_size = 8;
    std::size_t ring_capacity = 32;
    std::size_t batch_target_record_count = 0;
    std::size_t batch_min_payload_bytes = 256 * 1024;
    bool        compact_batch = true;
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
