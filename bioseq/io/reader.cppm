/**
 * @file reader.cppm
 * @brief Public sequence-reader entry points.
 *
 * Re-exports the batch and view types from `:types` while keeping batch
 * execution and bounded streaming behind private module partitions.
 */
module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.io.reader;

export import :types;
import :batch_pipeline;
import :stream_pipeline;
import bioseq.common.logging;
import bioseq.sequences;
import bioseq.io.options;
import bioseq.io.simd;
import bioseq.io.sources;
import bioseq.io.compression.parallel_gzip_source;

export namespace bioseq::io {

/// Public entry point. Reads FASTA / FASTQ from a file path, emits
/// per-batch records via `handler_fn`, and reports the first terminal
/// failure via `failure_fn`. Returns the count of records emitted on
/// success, or the diagnostic message on failure (as a `std::expected`).
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          typename p_path,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
read_sequences_file(p_path&& file_path,
                    p_handler&& handler_fn,
                    p_failure&& failure_fn,
                    const reader_options& options = default_reader_options,
                    reader_backend* observed_backend = nullptr) {

    static_assert(std::convertible_to<p_path, std::string_view>,
                  "file_path must be convertible to std::string_view");
    const std::string_view path_view = std::string_view{std::forward<p_path>(file_path)};

    const auto record_backend = [&observed_backend](reader_backend backend) noexcept {
        if (observed_backend)
            *observed_backend = backend;
    };
    record_backend(reader_backend::unknown);

    // Backend dispatch:
    //   1. If `prefer_mmap` is set: open with magic-byte sniff.
    //      - Gzipped + parallel + size>=min → parallel_gzip_source.
    //      - Gzipped, fallback path → gzip_source.
    //      - Plain → existing mmap pipeline.
    //   2. mmap failed / large file → posix_read_source.
    //   3. `gzip_auto_detect=true` without prefer_mmap →
    //      gzip_source (covers the "user explicitly asked for gzip
    //      auto-detect on a non-mmap'd reader" path).
    if (options.io.prefer_mmap) {
        auto mmap_or_err = mmap_source::try_open_with_magic(path_view, options);
        if (mmap_or_err) {
            if (mmap_or_err->is_gzipped) {
                const bool size_ok =
                    mmap_or_err->file_size >= options.io.parallel_gzip_min_compressed_bytes;
                if (options.io.parallel_gzip_enabled && size_ok) {
                    auto pgz_or_err = parallel_gzip_source::try_open_from_mmap(
                        std::move(mmap_or_err->source), options);
                    if (pgz_or_err) {
                        record_backend(reader_backend::parallel_gzip);
                        return detail::run_pipeline_for_source<p_alphabet,
                                                               p_file_format,
                                                               parallel_gzip_source>(
                            std::move(*pgz_or_err),
                            std::forward<p_handler>(handler_fn),
                            std::forward<p_failure>(failure_fn),
                            options);
                    }
                    bioseq_log<logging_level::debug>("[bioseq_io] parallel_gzip_source failed for "
                                                     "'{}': {}; falling back to single-thread "
                                                     "gzip_source",
                                                     path_view,
                                                     pgz_or_err.error());
                }
                // Construction-time fallback. Mid-stream errors are
                // TERMINAL — never restart from byte 0; that would
                // re-emit records.
                auto gz_or_err = gzip_source::try_open(path_view);
                if (!gz_or_err) {
                    failure_fn(std::string{gz_or_err.error()});
                    return std::unexpected(gz_or_err.error());
                }
                record_backend(reader_backend::gzip);
                return detail::run_pipeline_for_source<p_alphabet, p_file_format, gzip_source>(
                    std::move(*gz_or_err),
                    std::forward<p_handler>(handler_fn),
                    std::forward<p_failure>(failure_fn),
                    options);
            }
            record_backend(reader_backend::memory_mapped);
            return detail::run_pipeline_for_source<p_alphabet, p_file_format, mmap_source>(
                std::move(mmap_or_err->source),
                std::forward<p_handler>(handler_fn),
                std::forward<p_failure>(failure_fn),
                options);
        }
        bioseq_log<logging_level::debug>("[bioseq_io] mmap_source unavailable for '{}': {}; "
                                         "falling back to posix_read_source",
                                         path_view,
                                         mmap_or_err.error());
    }
    if (options.io.gzip_auto_detect) {
        auto gzip_source_or_err = gzip_source::try_open(path_view);
        if (!gzip_source_or_err) {
            failure_fn(std::string{gzip_source_or_err.error()});
            return std::unexpected(gzip_source_or_err.error());
        }
        record_backend(reader_backend::gzip);
        return detail::run_pipeline_for_source<p_alphabet, p_file_format, gzip_source>(
            std::move(*gzip_source_or_err),
            std::forward<p_handler>(handler_fn),
            std::forward<p_failure>(failure_fn),
            options);
    }
    auto posix_source_or_err = posix_read_source::try_open(path_view);
    if (!posix_source_or_err) {
        failure_fn(std::string{posix_source_or_err.error()});
        return std::unexpected(posix_source_or_err.error());
    }
    record_backend(reader_backend::posix_read);
    return detail::run_pipeline_for_source<p_alphabet, p_file_format, posix_read_source>(
        std::move(*posix_source_or_err),
        std::forward<p_handler>(handler_fn),
        std::forward<p_failure>(failure_fn),
        options);
}

/// Bounded streaming read of a FASTA/FASTQ file into the
/// caller's `p_storage_unit`-packed column layout. Unlike `read_sequences_file`
/// (which accumulates whole `sequence_batch`es and is hard-wired to a
/// `uint64_t` storage unit), this drives the per-record FSM single-threaded and
/// emits bounded `packed_sequence_chunk<p_storage_unit>` values to `sink`, so a
/// >MaxAllocSize record (e.g. a 6 Gbp reference) is never materialised as one
/// buffer. Always single-threaded (the columnar write state + seq_caches are
/// single-backend); gzip is still magic-byte sniffed by the source. Returns the
/// record count, or the first terminal diagnostic.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          sequence_file_format p_file_format,
          typename p_sink,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
stream_sequences_file(const char* path,
                      p_sink&& sink,
                      p_failure&& failure_fn,
                      reader_options options = default_reader_options) {
    // Streaming is single-threaded by contract: no async workers and no
    // mmap-parallel shards.
    options.concurrency.worker_count = 0;
    options.concurrency.parser_count = 1;
    const std::string_view path_view{path};

    // The source selection is shared with
    // `read_decompressed_prefix` via `detail::with_selected_source`; the chosen
    // (concrete) source is streamed through `run_sync_stream`.
    return detail::with_selected_source(
        path_view, options, failure_fn, [&failure_fn, &options, &sink](auto&& src) {
            using source_type = std::remove_reference_t<decltype(src)>;
            return detail::run_sync_stream<p_alphabet, p_storage_unit, p_file_format, source_type>(
                std::move(src), sink, failure_fn, options);
        });
}

/// Read at most `max_bytes` of decompressed bytes from the start
/// of `path`, using the same source selection as
/// `stream_sequences_file` (`detail::with_selected_source`). Used by auto-mode
/// header sampling; gzip is transparently decompressed by `gzip_source`.
[[nodiscard]] inline std::expected<std::string, std::string>
read_decompressed_prefix(const char* path, std::size_t max_bytes, const reader_options& options) {
    const std::string_view path_view{path};
    std::string open_error;
    auto on_open_fail = [&open_error](std::string&& msg) {
        if (open_error.empty())
            open_error = std::move(msg);
    };
    return detail::with_selected_source(
        path_view,
        options,
        on_open_fail,
        [&max_bytes](auto&& src) -> std::expected<std::string, std::string> {
            std::string out;
            out.resize(max_bytes);
            std::size_t total = 0;
            while (total < max_bytes) {
                const auto r = src.read_into(reinterpret_cast<std::uint8_t*>(out.data()) + total,
                                             max_bytes - total);
                if (r.has_error)
                    return std::unexpected(
                        std::string{"bioseq.load: failed to read file prefix for header sampling"});
                total += r.bytes_read;
                if (r.is_eof || r.bytes_read == 0)
                    break;
            }
            out.resize(total);
            return out;
        });
}

/// sample up to `max_headers` header strings (no leading
/// '>' / '@', carriage returns stripped) from the decompressed prefix of `path`. The FASTQ
/// path mirrors the loader's logical record rule (sequence lines end at the `+`
/// line; quality lines end after the same non-newline byte count) so multiline
/// FASTQ never mistakes a `@` inside a quality line for a header (HIGH-03). Line
/// ends are found via the existing SIMD `scan_first_byte('\n', …)` kernel.
[[nodiscard]] inline std::expected<std::vector<std::string>, std::string>
sample_sequence_headers(const char* path,
                        sequence_file_format file_type,
                        const reader_options& options,
                        std::size_t max_headers = 16,
                        std::size_t max_bytes = 256 * 1024) {
    auto prefix_or = read_decompressed_prefix(path, max_bytes, options);
    if (!prefix_or)
        return std::unexpected(prefix_or.error());
    const std::string& prefix = *prefix_or;
    const auto* base = reinterpret_cast<const std::uint8_t*>(prefix.data());
    const std::size_t n = prefix.size();

    std::vector<std::string> headers;
    headers.reserve(max_headers);
    auto rstrip_cr = [](std::string_view s) {
        if (!s.empty() && s.back() == '\r')
            s.remove_suffix(1);
        return s;
    };
    auto line_end_at = [&base, &n](std::size_t cursor) {
        return cursor + simd::scan_first_byte('\n', base + cursor, n - cursor);
    };

    std::size_t cursor = 0;
    if (file_type == sequence_file_format::fasta) {
        while (headers.size() < max_headers && cursor < n) {
            if ((cursor == 0 || prefix[cursor - 1] == '\n') && prefix[cursor] == '>') {
                const std::size_t le = line_end_at(cursor);
                headers.emplace_back(rstrip_cr(std::string_view{prefix}.substr(
                    cursor + 1, le > cursor + 1 ? le - (cursor + 1) : 0)));
                cursor = le + 1;
            } else {
                const std::size_t le = line_end_at(cursor);
                if (le >= n)
                    break;
                cursor = le + 1;
            }
        }
    } else { // FASTQ: records may have multiline sequence / quality sections.
        while (headers.size() < max_headers && cursor < n) {
            if (prefix[cursor] != '@')
                break; // truncated/garbled prefix
            const std::size_t he = line_end_at(cursor);
            headers.emplace_back(rstrip_cr(
                std::string_view{prefix}.substr(
                    cursor + 1, he > cursor + 1 ? he - (cursor + 1) : 0)));
            cursor = he + 1;

            std::size_t seq_bytes = 0;
            while (cursor < n && prefix[cursor] != '+') {
                const std::size_t le = line_end_at(cursor);
                for (std::size_t i = cursor; i < le && i < n; ++i)
                    if (prefix[i] != '\r') ++seq_bytes;
                cursor = le + 1;
            }
            if (cursor >= n || prefix[cursor] != '+') break;
            cursor = line_end_at(cursor) + 1;  // skip the '+' line

            std::size_t qual_bytes = 0;
            while (cursor < n && qual_bytes < seq_bytes) {
                const std::size_t le = line_end_at(cursor);
                for (std::size_t i = cursor; i < le && i < n; ++i)
                    if (prefix[i] != '\r') ++qual_bytes;
                cursor = le + 1;
            }
            if (qual_bytes < seq_bytes) break;   // prefix ended mid-record
        }
    }
    return headers;
}

/// Plural-API convenience: read a sequence of files in declaration
/// order, accumulating record counts. The handler / failure
/// callbacks are shared across files. Each file is fully drained before
/// the next is opened, preserving declaration order.
template <
    sequence_alphabet p_alphabet,
    sequence_file_format p_file_format,
    typename p_path,
    typename p_handler,
    typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string> read_sequences_files(
    std::span<const p_path> file_paths,
    p_handler&& handler_fn,
    p_failure&& failure_fn,
    const reader_options& options = default_reader_options) {
    std::size_t total_records = 0;
    for (const auto& file_path : file_paths) {
        auto outcome = read_sequences_file<
            p_alphabet,
            p_file_format,
            const p_path&>(
            file_path,
            handler_fn,
            failure_fn,
            options);
        if (!outcome) return outcome;
        total_records += *outcome;
    }
    return total_records;
}

}  // namespace bioseq::io
