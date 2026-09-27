/**
 * @file options.cppm
 * @brief Reader and writer configuration with default policies.
 */
module;

#include <cstddef>
#include <cstdint>

export module bioseq.io.options;

export namespace bioseq::io {

/// Sequence format selected by the eader template for the whole input.
enum class sequence_file_format : std::uint8_t {
    fasta = 0,
    fastq = 1,
};

/// Input buffers, memory mapping, and gzip settings.
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

    /// Enables parallel gzip for mapped files at least as large as the configured threshold.
    bool parallel_gzip_enabled = true;
    std::size_t parallel_gzip_min_compressed_bytes = 16ULL * 1024 * 1024;
    /// 0 = use default_parallel_gzip_chunk_size = 4 MiB.
    std::size_t parallel_gzip_chunk_bytes = 0;
    /// Zero chooses an automatic count, leaving two worker slots free when  possible.
    std::size_t parallel_gzip_thread_count = 0;
    bool parallel_gzip_verify_crc = true;
};

/// Worker counts, queue sizes, and batch sizes for the reader.
struct concurrency_options {
    std::size_t worker_count = 0;
    /// Parser count for a large memory-mapped input when `worker_count` is zero
    /// Zero uses the available pool size, capped by `mmap_parallel_max_shards`.
    /// One selects sequential parsing. Parallel parsing also requires the zero-copy
    /// path and a file at least `mmap_parallel_min_bytes` long.
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
    /// Maximum parser count chosen automatically when `parser_count` is zero.
    std::size_t mmap_parallel_max_shards = 8;
};

/// Sequence alphabet and FASTQ parsing settings.
struct encoder_options {
    bool strict_alphabet = false;
    bool strict_fastq = true;
};

/// Progress-reporting settings.
struct diagnostics_options {
    bool trace_progress = false;
    std::size_t trace_interval_bytes = 64ull * 1024 * 1024;
};

/// Reader settings grouped by input, concurrency, encoding, and diagnostics.
struct reader_options {
    io_options io{};
    concurrency_options concurrency{};
    encoder_options encoder{};
    diagnostics_options diagnostics{};
};

inline constexpr reader_options default_reader_options{};

/// Formatting and  output-buffer settings for FASTA and FASTQ writers.
struct writer_options {
    /// Letters per FASTA body line. Zero writes each sequence on one lne.
    std::size_t fasta_line_width = 0;

    /// Initial capacity, of each producer's output and decode buffers, in bytes.
    /// A large record can grow the buffers beyond this value.
    std::size_t chunk_capacity_bytes = 64 * 1024 * 1024;

    /// Flush a producer's pending bytes before the next record once this size is reached.
    std::size_t flush_threshold_bytes = 64 * 1024 * 1024 - 64 * 1024;

    /// Reject FASTQ quality lengths that differ from the decoded sequence length.
    /// When false, excess quality bytes are removed and missing bytes become '!'.
    bool strict_fastq_quality_length = false;

    /// Reserved setting. Oversized records currently follow the chosen write method.
    /// Use `write_record_streamed` to  write a record in slices.
    bool stream_oversize_records = true;

    /// Maximum file size to map for output. Zero selects the pwrite sink.
    /// A mapped write fails if it exceeds this capacity. Closing the mapped
    /// sink resizes the file to the total number of reserved bytes.
    std::size_t output_capacity_hint = 0;
};

inline constexpr writer_options default_writer_options{};

} // namespace bioseq::io
