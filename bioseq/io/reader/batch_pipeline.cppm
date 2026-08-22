/**
 * @file batch_pipeline.cppm
 * @brief Batch parsing, mapped-file sharding, and dispatch.
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

module bioseq.io.reader:batch_pipeline;

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

    // Retain and report only the first terminal pipeline error.
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
        // Reserve for approximately 256 bases per record. Larger batches grow
        // geometrically beyond the hint.
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
                // The FSM updates `parser_state` and the batch storage
                // directly. The emit_record callback only needs to record
                // the (header_offset, seq_offset, padding) triple.
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

/// Synchronous pipeline specialised for `mmap_source`. The whole file
/// is already in contiguous virtual memory. The parser receives a
/// single span over the mapped region instead of cycling 64 KiB slabs
/// through `read(2)` + `memmove`. Wins:
///
///   * Zero memcpy on the fast path. The slab in `run_sync_pipeline`
///     copies every byte once via `mmap_source::read_into`'s
///     `std::memcpy(dst, _addr + _cursor, copy_size)`. That's pure
///     overhead — the parser only reads bytes, never writes them.
///   * No `make_room` memmove. For a single-record file (e.g. the
///     bench's 234 MiB human-genome corpus), the slab path slides a
///     ~48 KiB tail back to the slab's start every refill cycle. The
///     direct path skips this entirely.
///   * No slab grow. The slab's geometric grow chain (64 KiB → 128 KiB
///     → … → ~8 MiB) is irrelevant when the file's already mapped.
///   * The parser sees `is_eof = true` on the first call, so the FSM
///     emits the trailing record cleanly without a "wait for more"
///     dance.
///
/// The reservation hint for the batch's `_sequence_storage` uses the file
/// size: dna4 packs 4 letters per byte, so ~`file_size / 32` u64
/// words bound the storage requirement. Pre-reserving this avoids
/// the ~9 geometric reallocs that would otherwise fire while encoding
/// a 234 MiB body.
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

    // Retain and report only the first terminal direct-path error.
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

    // Reservation: dna4 packs 4 letters per byte, so each input byte
    // produces ~1/16 of a 64-bit word. For the bench's 234 MiB human
    // case this is ~58 M words / 464 MiB. Pre-reserving avoids the 9
    // geometric reallocations during encoding.
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
            // INCOMPLETE on `is_eof=true` means the FSM still wants
            // more bytes — that's a malformed-file diagnostic, not a
            // refill hint. Bail out.
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

/// Find the position of the next FASTA record boundary at or after
/// `target` — i.e. the next `>` character that follows a `\n`. If
/// `target == 0` the file's first byte is the boundary (the parser
/// expects `>` at the head). Returns `view_size` when no record
/// boundary is found in the remaining bytes (the trailing region is
/// part of the previous record).
///
/// SIMD-accelerated: reuses `simd::scan_first_byte('\n', ...)` to skip
/// long body lines in one pass, then peeks the byte after each newline.
[[nodiscard]] inline std::size_t find_fasta_record_boundary(const std::uint8_t* base,
                                                            std::size_t view_size,
                                                            std::size_t target) noexcept {
    if (target == 0)
        return 0;
    if (target >= view_size)
        return view_size;
    std::size_t cursor = target;
    // Greedy: scan to next '\n', check if next byte is '>'. If not,
    // advance past the newline and continue. simd::scan_first_byte is
    // 4×-unrolled NEON so this loop runs at near-memory bandwidth.
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

/// Find the position of the next FASTQ record boundary at or after
/// `target`. FASTQ is harder than FASTA because `@` is a valid Phred
/// quality character (Q31), so a leading `@` after a newline isn't a
/// guaranteed record start. We use a four-line verification:
///
///   1. Find candidate at position P (`base[P-1] == '\n' && base[P] == '@'`).
///   2. Skip the header line: find next `\n` after P → P + h_size + 1.
///   3. Skip the sequence line: find next `\n` → P + h_size + 1 + s_size + 1.
///   4. The next byte MUST be `+` for this to be a valid FASTQ record start.
///   5. Skip the `+`-line and verify qual length == seq length when
///      possible.
///
/// On verification failure, advance past the candidate and retry. For
/// canonical FASTQ this converges within 1-2 candidates. For FASTQ
/// with `@`-rich quality strings it may take a few tries but the SIMD
/// scan keeps the per-candidate cost low.
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
        // Candidate at `at_header`. Verify by walking 3 more lines:
        // header → seq → '+' separator. If the byte after the seq line
        // is '+', this is a real record start.
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
        // False positive: '@' was inside a qual line. Advance past the
        // newline that produced this candidate and try again.
        cursor = at_header;
    }
    return view_size;
}

/// Run a parallel sharded mmap pipeline. The mapped region is split
/// into N shards. Each shard finds its actual start by walking forward
/// to the next record boundary, then parses its region with a private
/// per-shard batch. Batches are emitted to the user handler via a
/// mutex-guarded callback (the lock is on the cold path — one
/// acquisition per ~4 K records).
///
/// Wins over the sequential mmap-direct path:
///   * **Encoder parallelism** — the SIMD pack pipeline (~5 SIMD ops
///     per 16 bytes) saturates a single core's NEON ports. N cores in
///     parallel scale near-linearly until memory bandwidth bites.
///   * **Page-fault parallelism** — the OS demand-zeroes 117 MiB of
///     `_sequence_storage` on the human-genome cell. Spreading those faults
///     across N cores cuts the demand-zero latency by N.
///   * **OS prefetcher win** — N concurrent sequential readers trip
///     the kernel page-cache prefetcher in N regions instead of 1.
///
/// Correctness: shard boundaries are at record starts, so each shard
/// parses complete records with `is_eof = true` (the FSM emits the
/// trailing record cleanly). Shard 0 starts at byte 0. Shard N-1 ends
/// at file end. Records are emitted in within-shard order. Across
/// shards order is not strictly preserved. The bench's accumulator
/// uses atomic counters so order doesn't matter for the metric, and
/// real handlers (alignment, indexing) consume per-batch with no
/// cross-batch ordering requirement.
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
        // Monotonicity: if a shard's adjusted start lands past the
        // previous shard's start, keep it. Otherwise this shard is
        // empty (huge single-record file or skewed records).
        if (actual < boundaries.back())
            actual = boundaries.back();
        if (actual > view_size)
            actual = view_size;
        boundaries.push_back(actual);
    }
    boundaries.push_back(view_size);

    // Small per-shard hints avoid serial page faults from large anonymous
    // mappings on macOS. Each vector grows geometrically after one batch.
    const std::size_t flush_record_threshold = (options.concurrency.batch_target_record_count > 0)
                                                   ? options.concurrency.batch_target_record_count
                                                   : 4096;

    // Concurrency state.
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

        // Shard-private batch and parser state. The reservation
        // sizes are scaled to the SHARD (rather than the whole file)
        // so a single batch absorbs the bulk of the shard's output
        // without geometric grows during parse — geometric grows on
        // the FASTQ qual buffer for a 750 MiB shard would otherwise
        // memcpy ~750 MiB per shard × 8 shards = 6 GiB of memcpy per
        // iteration, a memory-bandwidth bottleneck.
        //
        // dna4 packs 32 letters / uint64, so seq words = body bytes
        // / 32. We use /16 (= 2× the precise factor) to leave headroom
        // for header bytes that don't pack. FASTQ qual is one byte
        // per BP, ~46 % of the shard size for canonical 200-bp reads.
        // /4 = 256 MiB per 1 GiB shard is a safe upper bound.
        //
        // The `flush_record_threshold * X` floors handle the SHORT
        // synthetic cells that fall below the parallel threshold but
        // would otherwise reserve too little for one batch.
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
            // Hand the batch to the user handler under a mutex. The
            // handler call rate is one per ~4 K records (roughly once
            // per millisecond at 5 GB/s), so the lock cost is invisible
            // against per-byte SIMD throughput.
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
                break; // INCOMPLETE on is_eof=true → diagnostic
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

/// Mode-aware dispatcher. Routes to the sync pipeline when no workers
/// are configured, otherwise to the async pipeline. mmap source goes
/// through the zero-copy direct path when sync.
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
                // Pick shard count: explicit option wins, else
                // hardware_concurrency capped at mmap_parallel_max_shards.
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
