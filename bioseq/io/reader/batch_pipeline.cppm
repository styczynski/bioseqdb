/**
 * @file batch_pipeline.cppm
 * @brief Reads sequence batches using input buffers or mapped file regions.
 */
module;

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.io.reader:batch_pipeline;

import :types;
import bioseq.sequences;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.buffer;
import bioseq.io.fasta;
import bioseq.io.fastq;
import bioseq.io.simd;
import bioseq.io.sources;
import bioseq.io.pipeline;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

namespace bioseq::io::detail {

/// Run the synchronous read loop with a concrete source. Single-thread
/// path: the same thread reads bytes, parses records, and invokes the
/// user's handler.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          io_source p_source,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
run_sync_pipeline(p_source source,
                  p_handler&& handler_fn,
                  p_failure&& failure_fn,
                  const reader_options& options) {

    using alphabet_config_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    using batch_type = sequence_batch<alphabet_config_type>;
    using sequence_traits_type = typename compact_sequence_range<alphabet_config_type>::traits;

    constexpr std::size_t bits_per_char = sequence_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits_type::letters_per_unit;

    buffer_slab slab(options);
    batch_type batch;
    std::size_t records_emitted = 0;
    bool aborted = false;
    std::string captured_error;
    std::size_t global_offset_base = 0;

    // Save and report only the first terminal pipeline error.
    auto failure_capture = [&aborted, &captured_error, &failure_fn](std::string&& msg) {
        if (!aborted) {
            captured_error = msg;
            failure_fn(std::move(msg));
        }
        aborted = true;
    };

    // No refill, parser or output callback may observe an unallocated slab.
    if (slab.raw() == nullptr) {
        if (options.io.max_buffer_size == 0u)
            emit_failure(failure_capture, "input buffer limit is zero");
        else
            emit_failure(failure_capture, "cannot allocate input buffer (initial={}, maximum={})",
                options.io.initial_buffer_size, options.io.max_buffer_size);
        return std::unexpected(std::move(captured_error));
    }

    const std::size_t flush_record_threshold = (options.concurrency.batch_target_record_count > 0)
                                                   ? options.concurrency.batch_target_record_count
                                                   : 4096;

    constexpr bool is_fastq_format = p_file_format == sequence_file_format::fastq;
    // Prime offsets and reserve storage for the next output batch.
    auto prepare_fresh_batch = [&batch, &flush_record_threshold]() {
        batch.prepare(flush_record_threshold, is_fastq_format);
        // Reserve an initial batch estimate. The vectors can grow for longer records.
        batch.reserve_storage(
            /*header_bytes*/ flush_record_threshold * 32,
            /*seq_words*/ flush_record_threshold * 8,
        /*quality_bytes*/ is_fastq_format ? flush_record_threshold * 256 : 0);
    };

    // Reuse storage still owned after delivery; moved buffers remain with the handler.
    auto flush_batch = [&batch, &handler_fn, &prepare_fresh_batch]() {
        if (batch.empty())
            return;
        handler_fn(std::move(batch));
        batch.clear();
        prepare_fresh_batch();
    };

    prepare_fresh_batch();

    if constexpr (p_file_format == sequence_file_format::fasta) {
        fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(batch.header_storage(), batch.sequence_storage());

        // Finalize one FASTA record and flush a full batch.
        auto emit_fasta_record = [&batch, &flush_batch, &records_emitted, &flush_record_threshold](
                                     std::size_t header_start,
                                     std::size_t header_size,
                                     std::size_t seq_start,
                                     std::size_t seq_word_count,
                                     std::size_t padding) {
            (void)header_size;
            (void)seq_word_count;
            // Header and sequence storage are already populated. Record their
            // offsets and padding.
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.sequence_offsets().push_back(batch.sequence_storage().size());
            batch.sequence_paddings().push_back(padding);
            (void)header_start;
            (void)seq_start;
            ++records_emitted;
            if (batch.size() >= flush_record_threshold)
                flush_batch();
        };

        while (!aborted) {
            if (slab.fill_size == slab.capacity) {
                if (!slab.make_room()) {
                    if (!slab.grow(options)) {
                        emit_failure(
                            failure_capture, "buffer_limit reached at {} bytes", slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto refill_result = source.read_into(slab.raw() + slab.fill_size, free_room);
                if (refill_result.has_error) {
                    emit_failure(failure_capture, "{}", source.last_error_message());
                    break;
                }
                slab.fill_size += refill_result.bytes_read;
                slab.is_eof = refill_result.is_eof;
            }

            bool need_refill = false;
            while (!aborted) {
                // The parser appends to the batch buffers. Its callback records the
                // completed record's offsets and padding.
                parser_state.attach_to_batch(batch.header_storage(), batch.sequence_storage());
                const auto step = parse_one_fasta_record<p_alphabet, std::uint64_t>(
                    parser_state,
                    slab.raw(),
                    slab.fill_size,
                    slab.is_eof,
                    slab.parse_cursor,
                    global_offset_base,
                    emit_fasta_record,
                    failure_capture);
                slab.parse_cursor = step.cursor;
                if (step.outcome == parse_outcome::complete)
                    continue;
                if (step.outcome == parse_outcome::at_end_of_file) {
                    flush_batch();
                    if (aborted) {
                        return std::unexpected(captured_error);
                    }
                    return records_emitted;
                }
                if (step.outcome == parse_outcome::abort) {
                    return std::unexpected(captured_error);
                }
                need_refill = true;
                break;
            }
            if (!need_refill)
                continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                flush_batch();
                if (aborted) {
                    return std::unexpected(captured_error);
                }
                return records_emitted;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }
        return std::unexpected(captured_error);

    } else {
        fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(
            batch.header_storage(), batch.sequence_storage(), batch.quality_storage());

        // Finalize one FASTQ record and flush a full batch.
        auto emit_fastq_record = [&batch, &flush_batch, &records_emitted, &flush_record_threshold](
                                     std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding,
                                     std::size_t /*qual_start*/,
                                     std::size_t /*qual_size*/) {
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.sequence_offsets().push_back(batch.sequence_storage().size());
            batch.sequence_paddings().push_back(padding);
            batch.quality_offsets().push_back(batch.quality_storage().size());
            ++records_emitted;
            if (batch.size() >= flush_record_threshold)
                flush_batch();
        };

        while (!aborted) {
            if (slab.fill_size == slab.capacity) {
                if (!slab.make_room()) {
                    if (!slab.grow(options)) {
                        emit_failure(
                            failure_capture, "buffer_limit reached at {} bytes", slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto refill_result = source.read_into(slab.raw() + slab.fill_size, free_room);
                if (refill_result.has_error) {
                    emit_failure(failure_capture, "{}", source.last_error_message());
                    break;
                }
                slab.fill_size += refill_result.bytes_read;
                slab.is_eof = refill_result.is_eof;
            }
            bool need_refill = false;
            while (!aborted) {
                parser_state.attach_to_batch(
                    batch.header_storage(), batch.sequence_storage(), batch.quality_storage());
                const auto step = parse_one_fastq_record<p_alphabet, std::uint64_t>(
                    parser_state,
                    slab.raw(),
                    slab.fill_size,
                    slab.is_eof,
                    slab.parse_cursor,
                    global_offset_base,
                    options.encoder.strict_fastq,
                    emit_fastq_record,
                    failure_capture);
                slab.parse_cursor = step.cursor;
                if (step.outcome == parse_outcome::complete)
                    continue;
                if (step.outcome == parse_outcome::at_end_of_file) {
                    flush_batch();
                    if (aborted) {
                        return std::unexpected(captured_error);
                    }
                    return records_emitted;
                }
                if (step.outcome == parse_outcome::abort) {
                    return std::unexpected(captured_error);
                }
                need_refill = true;
                break;
            }
            if (!need_refill)
                continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                flush_batch();
                if (aborted) {
                    return std::unexpected(captured_error);
                }
                return records_emitted;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }
        return std::unexpected(captured_error);
    }
}

/// Parse a mapped file directly on the calling thread without refill copies.
/// Parsed headers, packed sequences, and qualities use the same initial batch
/// hints as the buffered path, growing as required by complete records.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
run_sync_pipeline_mmap_direct(mmap_source source,
                              p_handler&& handler_fn,
                              p_failure&& failure_fn,
                              const reader_options& options) {

    using alphabet_config_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    using batch_type = sequence_batch<alphabet_config_type>;
    using sequence_traits_type = typename compact_sequence_range<alphabet_config_type>::traits;
    constexpr std::size_t bits_per_char = sequence_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits_type::letters_per_unit;
    constexpr bool is_fastq_format = p_file_format == sequence_file_format::fastq;

    auto [base_ptr, view_size] = source.direct_view();

    batch_type batch;
    std::size_t records_emitted = 0;
    bool aborted = false;
    std::string captured_error;

    // Save and report only the first terminal direct-path error.
    auto failure_capture = [&aborted, &captured_error, &failure_fn](std::string&& msg) {
        if (!aborted) {
            captured_error = msg;
            failure_fn(std::move(msg));
        }
        aborted = true;
    };

    const std::size_t flush_record_threshold = (options.concurrency.batch_target_record_count > 0)
                                                   ? options.concurrency.batch_target_record_count
                                                   : 4096;

    // Prime offsets and reuse the buffered reader's initial capacity hints.
    // These hints do not limit record length; batch vectors grow as needed.
    auto prepare_fresh_batch = [&batch, &flush_record_threshold]() {
        batch.prepare(flush_record_threshold, is_fastq_format);
        batch.reserve_storage(
            /*header_bytes*/ flush_record_threshold * 32,
            /*seq_words*/ flush_record_threshold * 8,
            /*quality_bytes*/ is_fastq_format ? flush_record_threshold * 256 : 0);
    };

    // Reuse storage still owned after delivery; moved buffers remain with the handler.
    auto flush_batch = [&batch, &handler_fn, &prepare_fresh_batch]() {
        if (batch.empty())
            return;
        handler_fn(std::move(batch));
        batch.clear();
        prepare_fresh_batch();
    };

    prepare_fresh_batch();

    if constexpr (p_file_format == sequence_file_format::fasta) {
        fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(batch.header_storage(), batch.sequence_storage());

        // Finalize one mapped FASTA record and flush a full batch.
        auto emit_fasta_record = [&batch, &flush_batch, &records_emitted, &flush_record_threshold](
                                     std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding) {
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.sequence_offsets().push_back(batch.sequence_storage().size());
            batch.sequence_paddings().push_back(padding);
            ++records_emitted;
            if (batch.size() >= flush_record_threshold)
                flush_batch();
        };

        std::size_t cursor = 0;
        while (cursor < view_size && !aborted) {
            parser_state.attach_to_batch(batch.header_storage(), batch.sequence_storage());
            const auto step = parse_one_fasta_record<p_alphabet, std::uint64_t>(parser_state,
                                                                                base_ptr,
                                                                                view_size,
                                                                                /*is_eof=*/true,
                                                                                cursor,
                                                                                /*global_offset=*/0u,
                                                                                emit_fasta_record,
                                                                                failure_capture);
            cursor = step.cursor;
            if (step.outcome == parse_outcome::complete)
                continue;
            if (step.outcome == parse_outcome::at_end_of_file)
                break;
            if (step.outcome == parse_outcome::abort) {
                return std::unexpected(captured_error);
            }
            // The full mapping is already available. Stop if the parser still
            // requests more input.
            break;
        }
        flush_batch();
        if (aborted)
            return std::unexpected(captured_error);
        return records_emitted;
    } else {
        fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(
            batch.header_storage(), batch.sequence_storage(), batch.quality_storage());

        // Finalize one mapped FASTQ record and flush a full batch.
        auto emit_fastq_record = [&batch, &flush_batch, &records_emitted, &flush_record_threshold](
                                     std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding,
                                     std::size_t /*qual_start*/,
                                     std::size_t /*qual_size*/) {
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.sequence_offsets().push_back(batch.sequence_storage().size());
            batch.sequence_paddings().push_back(padding);
            batch.quality_offsets().push_back(batch.quality_storage().size());
            ++records_emitted;
            if (batch.size() >= flush_record_threshold)
                flush_batch();
        };

        std::size_t cursor = 0;
        while (cursor < view_size && !aborted) {
            parser_state.attach_to_batch(
                batch.header_storage(), batch.sequence_storage(), batch.quality_storage());
            const auto step =
                parse_one_fastq_record<p_alphabet, std::uint64_t>(parser_state,
                                                                  base_ptr,
                                                                  view_size,
                                                                  /*is_eof=*/true,
                                                                  cursor,
                                                                  /*global_offset=*/0u,
                                                                  options.encoder.strict_fastq,
                                                                  emit_fastq_record,
                                                                  failure_capture);
            cursor = step.cursor;
            if (step.outcome == parse_outcome::complete)
                continue;
            if (step.outcome == parse_outcome::at_end_of_file)
                break;
            if (step.outcome == parse_outcome::abort) {
                return std::unexpected(captured_error);
            }
            break;
        }
        flush_batch();
        if (aborted)
            return std::unexpected(captured_error);
        return records_emitted;
    }
}

/// Finds a FASTA header following a newline found from `target` onward.
/// Returns zero for `target == 0`, or `view_size` if no later boundary exists.
/// A header exactly at a nonzero target is skipped because its preceding
/// newline lies before  the search.
[[nodiscard]] inline std::size_t find_fasta_record_boundary(const std::uint8_t* base,
                                                            std::size_t view_size,
                                                            std::size_t target) noexcept {
    if (target == 0)
        return 0;
    if (target >= view_size)
        return view_size;
    std::size_t cursor = target;
    // Scan whole lines and check whether the following byte starts a header.
    while (cursor < view_size) {
        const std::size_t remaining = view_size - cursor;
        const std::size_t newline_offset = simd::scan_first_byte('\n', base + cursor, remaining);
        if (newline_offset >= remaining)
            return view_size;
        const std::size_t after_newline = cursor + newline_offset + 1;
        if (after_newline >= view_size)
            return view_size;
        if (base[after_newline] == '>')
            return after_newline;
        cursor = after_newline;
    }
    return view_size;
}

/// Establish only the requested FASTQ shard starts using the shared structural parser.
/// Counts include quality marker lines; no packed sequence/header/quality is retained.
/// Every returned nonterminal offset follows a complete record, and storage is O(shards).
template <sequence_alphabet Alphabet>
[[nodiscard]] std::expected<std::vector<std::size_t>, std::string> find_fastq_record_boundaries(
    const std::uint8_t* base, const std::size_t view_size, const std::size_t shard_count,
    const bool strict_fastq) {
    using traits = typename compact_sequence_range<sequence_custom_alphabet<Alphabet, std::uint64_t>>::traits;
    fastq_parser_state<std::uint64_t, traits::bits_per_char, traits::letters_per_unit> state;
    std::vector<char> unused_headers;
    std::vector<std::uint64_t> unused_sequences;
    std::vector<std::uint8_t> unused_qualities;
    state.attach_to_batch(unused_headers, unused_sequences, unused_qualities);
    std::string failure;
    const auto capture_failure = [&failure](std::string&& message) { failure = std::move(message); };
    const auto ignore_storage = [](std::size_t, std::size_t, std::size_t, std::size_t,
                                   std::size_t, std::size_t, std::size_t) {};
    std::vector<std::size_t> boundaries;
    boundaries.reserve(shard_count + 1);
    boundaries.push_back(0);
    // Incremental quotient/remainder avoids overflowing view_size * shard_index.
    const auto stride = view_size / shard_count;
    const auto remainder = view_size % shard_count;
    std::size_t target = stride, fraction = remainder, cursor{};
    while (boundaries.size() < shard_count) {
        if (cursor >= target || cursor == view_size) {
            boundaries.push_back(cursor);
            target += stride;
            if (fraction >= shard_count - remainder) {
                ++target;
                fraction -= shard_count - remainder;
            } else fraction += remainder;
            continue;
        }
        const auto step = parse_one_fastq_record<Alphabet, std::uint64_t, fastq_parse_mode::frame>(
            state, base, view_size, true, cursor, 0, strict_fastq, ignore_storage, capture_failure);
        if (step.outcome == parse_outcome::abort) return std::unexpected(std::move(failure));
        if (step.outcome != parse_outcome::complete || step.cursor <= cursor)
            return std::unexpected(std::string{"FASTQ framing did not complete a record"});
        cursor = step.cursor;
    }
    boundaries.push_back(view_size);
    return boundaries;
}

/// Parses separate mapped regions in parallel, each with its own batch buffers.
/// FASTA starts follow header sigils; FASTQ starts follow complete count-framed records.
/// Handler calls are serialized, with records kept in order within each region.
/// The order between regions is unspecified.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
run_sync_pipeline_mmap_parallel(mmap_source source,
                                std::size_t shard_count,
                                p_handler&& handler_fn,
                                p_failure&& failure_fn,
                                const reader_options& options) {

    using alphabet_config_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    using batch_type = sequence_batch<alphabet_config_type>;
    using sequence_traits_type = typename compact_sequence_range<alphabet_config_type>::traits;
    constexpr std::size_t bits_per_char = sequence_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits_type::letters_per_unit;
    constexpr bool is_fastq_format = p_file_format == sequence_file_format::fastq;

    auto [base_ptr, view_size] = source.direct_view();
    auto& pool = bioseq::parallelism::global_pool::instance();
    if (!pool.is_active()) {
        const unsigned configured_shards =
            shard_count > (std::numeric_limits<unsigned>::max)()
                ? (std::numeric_limits<unsigned>::max)()
                : static_cast<unsigned>(std::max<std::size_t>(1, shard_count));
        pool.configure(bioseq::parallelism::pool_config{
            .maximum_concurrency = configured_shards,
            .mode = bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel,
        });
    }
    shard_count = std::min<std::size_t>(shard_count, pool.maximum_concurrency());
    if (view_size == 0 || shard_count <= 1) {
        // Fall back to the sequential direct path.
        return run_sync_pipeline_mmap_direct<p_alphabet, p_file_format>(
            std::move(source),
            std::forward<p_handler>(handler_fn),
            std::forward<p_failure>(failure_fn),
            options);
    }

    // Determine shard boundaries. shard[i] = [boundary[i], boundary[i+1]).
    std::vector<std::size_t> boundaries;
    if constexpr (is_fastq_format) {
        auto framed = find_fastq_record_boundaries<p_alphabet>(base_ptr, view_size,
            shard_count, options.encoder.strict_fastq);
        if (!framed) {
            failure_fn(std::string{framed.error()});
            return std::unexpected(std::move(framed.error()));
        }
        boundaries = std::move(*framed);
    } else {
        boundaries.reserve(shard_count + 1);
        boundaries.push_back(0);
        const auto stride = view_size / shard_count;
        const auto remainder = view_size % shard_count;
        std::size_t target = stride, fraction = remainder;
        for (std::size_t shard_index = 1; shard_index < shard_count; ++shard_index) {
            const auto actual = find_fasta_record_boundary(base_ptr, view_size, target);
            boundaries.push_back(std::clamp(actual, boundaries.back(), view_size));
            target += stride;
            if (fraction >= shard_count - remainder) {
                ++target;
                fraction -= shard_count - remainder;
            } else fraction += remainder;
        }
        boundaries.push_back(view_size);
    }

    // Use the configured number of records per batch, or the default count.
    const std::size_t flush_record_threshold = (options.concurrency.batch_target_record_count > 0)
                                                   ? options.concurrency.batch_target_record_count
                                                   : 4096;

    // Shared record count, stop flag, and callback locks.
    std::mutex emit_mutex;
    std::atomic<std::size_t> total_records{0};
    std::atomic<bool> aborted{false};
    std::mutex error_mutex;
    std::string captured_error;

    // Serialize and report only the first shard failure.
    auto failure_capture =
        [&aborted, &captured_error, &error_mutex, &failure_fn](std::string&& msg) {
            std::lock_guard guard{error_mutex};
            if (!aborted.exchange(true, std::memory_order_acq_rel)) {
                captured_error = std::move(msg);
                failure_fn(std::string{captured_error});
            }
        };

    // Parse one disjoint mapped shard into independently owned batches.
    auto run_shard = [&aborted,
                      &base_ptr,
                      &boundaries,
                      &emit_mutex,
                      &failure_capture,
                      &flush_record_threshold,
                      &handler_fn,
                      &is_fastq_format,
                      &options,
                      &total_records,
                      view_size](std::size_t shard_index) {
        const std::size_t shard_start = boundaries[shard_index];
        const std::size_t shard_end = boundaries[shard_index + 1];
        if (shard_start >= shard_end)
            return;

        // Use bounded initial hints per batch, independent of mapped shard size.
        // Longer records grow their buffers without changing batch boundaries.
        batch_type batch;
        auto prepare_fresh_batch = [&batch, &flush_record_threshold]() {
            batch.prepare(flush_record_threshold, is_fastq_format);
            batch.reserve_storage(
                /*header_bytes*/ flush_record_threshold * 32,
                /*seq_words*/ flush_record_threshold * 8,
                /*quality_bytes*/ is_fastq_format ? flush_record_threshold * 256 : 0);
        };
        prepare_fresh_batch();

        std::size_t shard_records = 0;

        // Serialize batch delivery while keeping parsing shard-local.
        auto flush_batch = [&batch, &emit_mutex, &handler_fn, &prepare_fresh_batch]() {
            if (batch.empty())
                return;
            // Serialize handler calls from different regions.
            {
                std::lock_guard guard{emit_mutex};
                handler_fn(std::move(batch));
            }
            // Clear only storage still owned here after the handler returns.
            batch.clear();
            prepare_fresh_batch();
        };

        if constexpr (p_file_format == sequence_file_format::fasta) {
            fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
            parser_state.attach_to_batch(batch.header_storage(), batch.sequence_storage());

            // Finalize one sharded FASTA record and flush a full batch.
            auto emit_fasta_record = [&batch,
                                      &flush_batch,
                                      &shard_records,
                                      &flush_record_threshold](std::size_t /*hs*/,
                                                               std::size_t /*hsz*/,
                                                               std::size_t /*ss*/,
                                                               std::size_t /*sw*/,
                                                               std::size_t padding) {
                batch.header_offsets().push_back(batch.header_storage().size());
                batch.sequence_offsets().push_back(batch.sequence_storage().size());
                batch.sequence_paddings().push_back(padding);
                ++shard_records;
                if (batch.size() >= flush_record_threshold)
                    flush_batch();
            };

            std::size_t cursor = shard_start;
            while (cursor < shard_end && !aborted.load(std::memory_order_relaxed)) {
                parser_state.attach_to_batch(batch.header_storage(), batch.sequence_storage());
                const auto step =
                    parse_one_fasta_record<p_alphabet, std::uint64_t>(parser_state,
                                                                      base_ptr,
                                                                      shard_end,
                                                                      /*is_eof=*/true,
                                                                      cursor,
                                                                      /*global_offset=*/0u,
                                                                      emit_fasta_record,
                                                                      failure_capture);
                cursor = step.cursor;
                if (step.outcome == parse_outcome::complete)
                    continue;
                if (step.outcome == parse_outcome::at_end_of_file)
                    break;
                if (step.outcome == parse_outcome::abort)
                    return;
                break; // no further input is available in this region
            }
            flush_batch();
        } else {
            fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
            parser_state.attach_to_batch(
                batch.header_storage(), batch.sequence_storage(), batch.quality_storage());

            // Finalize one sharded FASTQ record and flush a full batch.
            auto emit_fastq_record = [&batch,
                                      &flush_batch,
                                      &shard_records,
                                      &flush_record_threshold](std::size_t /*hs*/,
                                                               std::size_t /*hsz*/,
                                                               std::size_t /*ss*/,
                                                               std::size_t /*sw*/,
                                                               std::size_t padding,
                                                               std::size_t /*qs*/,
                                                               std::size_t /*qsz*/) {
                batch.header_offsets().push_back(batch.header_storage().size());
                batch.sequence_offsets().push_back(batch.sequence_storage().size());
                batch.sequence_paddings().push_back(padding);
                batch.quality_offsets().push_back(batch.quality_storage().size());
                ++shard_records;
                if (batch.size() >= flush_record_threshold)
                    flush_batch();
            };

            std::size_t cursor = shard_start;
            while (cursor < shard_end && !aborted.load(std::memory_order_relaxed)) {
                parser_state.attach_to_batch(
                    batch.header_storage(), batch.sequence_storage(), batch.quality_storage());
                // Lenient records without '+' finish by peeking the next '@'.
                // Verified boundaries stop record starts, while the complete mapping
                // keeps that non-consuming lookahead available at a shard end.
                const auto step =
                    parse_one_fastq_record<p_alphabet, std::uint64_t>(parser_state,
                                                                      base_ptr,
                                                                      view_size,
                                                                      /*is_eof=*/true,
                                                                      cursor,
                                                                      /*global_offset=*/0u,
                                                                      options.encoder.strict_fastq,
                                                                      emit_fastq_record,
                                                                      failure_capture);
                cursor = step.cursor;
                if (step.outcome == parse_outcome::complete)
                    continue;
                if (step.outcome == parse_outcome::at_end_of_file)
                    break;
                if (step.outcome == parse_outcome::abort)
                    return;
                break;
            }
            flush_batch();
        }

        total_records.fetch_add(shard_records, std::memory_order_relaxed);
    };

    pool.parallel_for(0,
                      shard_count,
                      1,
                      bioseq::parallelism::range_body{[&run_shard](std::size_t lo, std::size_t hi) {
                          for (std::size_t shard_index = lo; shard_index < hi; ++shard_index) {
                              run_shard(shard_index);
                          }
                      }});

    if (aborted.load(std::memory_order_acquire)) {
        return std::unexpected(captured_error);
    }
    return total_records.load(std::memory_order_relaxed);
}

/// Selects batch parsing and delivery from the reader options. Nonzero
/// `worker_count` sends batches to worker handlers. Otherwise, mapped input
/// can use direct pequential or parallel parsing, and other sources use
/// the buffered synchronous loop.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          io_source p_source,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
run_pipeline_for_source(p_source source,
                        p_handler&& handler_fn,
                        p_failure&& failure_fn,
                        const reader_options& options) {
    using alphabet_config_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    using batch_type = sequence_batch<alphabet_config_type>;
    if (options.concurrency.worker_count == 0) {
        if constexpr (std::is_same_v<p_source, mmap_source>) {
            if (options.io.mmap_zero_copy) {
                // Use the requested parser count, or choose from available pool workers.
                // The automatic choice  is capped by mmap_parallel_max_shards.
                std::size_t requested_shards = options.concurrency.parser_count;
                if (requested_shards == 0) {
                    auto& pool = bioseq::parallelism::global_pool::instance();
                    const unsigned worker_slots =
                        pool.is_active() ? pool.maximum_concurrency()
                                         : bioseq::parallelism::default_concurrency_limit();
                    requested_shards = std::min<std::size_t>(
                        worker_slots, options.concurrency.mmap_parallel_max_shards);
                }
                if (requested_shards > 1 &&
                    source.mapped_size() >= options.concurrency.mmap_parallel_min_bytes) {
                    return run_sync_pipeline_mmap_parallel<p_alphabet, p_file_format>(
                        std::move(source),
                        requested_shards,
                        std::forward<p_handler>(handler_fn),
                        std::forward<p_failure>(failure_fn),
                        options);
                }
                return run_sync_pipeline_mmap_direct<p_alphabet, p_file_format>(
                    std::move(source),
                    std::forward<p_handler>(handler_fn),
                    std::forward<p_failure>(failure_fn),
                    options);
            }
        }
        return run_sync_pipeline<p_alphabet, p_file_format, p_source>(
            std::move(source),
            std::forward<p_handler>(handler_fn),
            std::forward<p_failure>(failure_fn),
            options);
    }
    return run_async_pipeline<p_alphabet, p_file_format, p_source, batch_type>(
        std::move(source),
        std::forward<p_handler>(handler_fn),
        std::forward<p_failure>(failure_fn),
        options);
}

} // namespace bioseq::io::detail
