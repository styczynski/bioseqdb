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

    buffer_slab slab(options.io.initial_buffer_size);
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

    // Deliver a nonempty batch synchronously and prepare its replacement.
    auto flush_batch = [&batch, &handler_fn, &prepare_fresh_batch]() {
        if (batch.empty())
            return;
        handler_fn(std::move(batch));
        batch = batch_type{};
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
                    global_offset_base + slab.parse_cursor,
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
                    global_offset_base + slab.parse_cursor,
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

/// Parses a mapped file directly on the calling thread. The parser reads
/// the complete mapping, so input does not need copying into refill buffers.
/// Parsed headers and packed sequences still go into owned batch storage.
/// File-size estimates reserve that storage before parsing.
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

    const std::size_t file_size = source.mapped_size();
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

    // Estimate batch storage from the input size. These are capacity hints,
    // and the vectors can grow if the encoded records need more room.
    const std::size_t sequence_words_hint =
        std::max<std::size_t>(file_size / 16, flush_record_threshold * 8);
    const std::size_t header_bytes_hint =
        std::max<std::size_t>(file_size / 256, flush_record_threshold * 32);

    // Prime offsets and reserve file-scaled storage for the next batch.
    auto prepare_fresh_batch =
        [&batch, &flush_record_threshold, &header_bytes_hint, &sequence_words_hint]() {
            batch.prepare(flush_record_threshold, is_fastq_format);
            batch.reserve_storage(header_bytes_hint,
                                  sequence_words_hint,
                                  is_fastq_format ? flush_record_threshold * 256 : 0);
        };

    // Deliver a nonempty direct-path batch and prepare its replacement.
    auto flush_batch = [&batch, &handler_fn, &prepare_fresh_batch]() {
        if (batch.empty())
            return;
        handler_fn(std::move(batch));
        batch = batch_type{};
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
                                                                                cursor,
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
                                                                  cursor,
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
/// newline lies before the search.
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

/// Finds a possible FASTQ header following a newline from `target` onward.
/// It accepts an '@' line followed by one sequence line and a '+' line.
/// Quality length is not checked, and this boundary test assumes single-line
/// sequence bodies. Zero selects the file start. `view_size` means no match.
[[nodiscard]] inline std::size_t find_fastq_record_boundary(const std::uint8_t* base,
                                                            std::size_t view_size,
                                                            std::size_t target) noexcept {
    if (target == 0)
        return 0;
    if (target >= view_size)
        return view_size;
    std::size_t cursor = target;
    while (cursor < view_size) {
        // Find next '\n' (a candidate record boundary lies just after).
        const std::size_t remaining = view_size - cursor;
        const std::size_t newline_offset = simd::scan_first_byte('\n', base + cursor, remaining);
        if (newline_offset >= remaining)
            return view_size;
        const std::size_t at_header = cursor + newline_offset + 1;
        if (at_header >= view_size)
            return view_size;
        if (base[at_header] != '@') {
            cursor = at_header;
            continue;
        }
        // Check for a header line, one sequence line, and then a separator.
        const std::size_t hdr_remaining = view_size - at_header;
        const std::size_t hdr_newline =
            simd::scan_first_byte('\n', base + at_header, hdr_remaining);
        if (hdr_newline >= hdr_remaining)
            return view_size;
        const std::size_t at_seq = at_header + hdr_newline + 1;
        if (at_seq >= view_size)
            return view_size;
        const std::size_t seq_remaining = view_size - at_seq;
        const std::size_t seq_newline = simd::scan_first_byte('\n', base + at_seq, seq_remaining);
        if (seq_newline >= seq_remaining)
            return view_size;
        const std::size_t at_sep = at_seq + seq_newline + 1;
        if (at_sep >= view_size)
            return view_size;
        if (base[at_sep] == '+') {
            return at_header;
        }
        // This candidate lacks the expected separator. Continue at its next line.
        cursor = at_header;
    }
    return view_size;
}

/// Parses separate mapped regions in parallel, each with its own batch buffers.
/// Region starts are adjusted by the format's boundary finder. FASTQ splitting
/// uses the single-line sequence pattern documented by that helper. Handler
/// calls are serialized, with records kept in order within each region.
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
    boundaries.reserve(shard_count + 1);
    boundaries.push_back(0);
    for (std::size_t shard_index = 1; shard_index < shard_count; ++shard_index) {
        const std::size_t target = (view_size * shard_index) / shard_count;
        std::size_t actual = p_file_format == sequence_file_format::fasta
                                 ? find_fasta_record_boundary(base_ptr, view_size, target)
                                 : find_fastq_record_boundary(base_ptr, view_size, target);
        // Keep boundaries ordered. Equal boundaries leave an empty region.
        if (actual < boundaries.back())
            actual = boundaries.back();
        if (actual > view_size)
            actual = view_size;
        boundaries.push_back(actual);
    }
    boundaries.push_back(view_size);

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
                      &total_records](std::size_t shard_index) {
        const std::size_t shard_start = boundaries[shard_index];
        const std::size_t shard_end = boundaries[shard_index + 1];
        if (shard_start >= shard_end)
            return;
        const std::size_t shard_size = shard_end - shard_start;

        // Reserve each region's output buffers using its size, with a minimum
        // estimate for short records. The buffers can grow beyond these hints.
        batch_type batch;
        const std::size_t shard_seq_words_hint =
            std::max<std::size_t>(shard_size / 16, flush_record_threshold * 8);
        const std::size_t shard_header_bytes_hint =
            std::max<std::size_t>(shard_size / 256, flush_record_threshold * 32);
        const std::size_t shard_quality_bytes_hint =
            is_fastq_format ? std::max<std::size_t>(shard_size / 4, flush_record_threshold * 256)
                            : 0;

        // Prime offsets and reserve shard-scaled storage for the next batch.
        auto prepare_fresh_batch = [&batch,
                                    &flush_record_threshold,
                                    &shard_header_bytes_hint,
                                    &shard_quality_bytes_hint,
                                    &shard_seq_words_hint]() {
            batch.prepare(flush_record_threshold, is_fastq_format);
            batch.reserve_storage(
                shard_header_bytes_hint, shard_seq_words_hint, shard_quality_bytes_hint);
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
            batch = batch_type{};
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
                                                                      cursor,
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
                const auto step =
                    parse_one_fastq_record<p_alphabet, std::uint64_t>(parser_state,
                                                                      base_ptr,
                                                                      shard_end,
                                                                      /*is_eof=*/true,
                                                                      cursor,
                                                                      cursor,
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
/// can use direct sequential or parallel parsing, and other sources use
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
                // The automatic choice is capped by mmap_parallel_max_shards.
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
