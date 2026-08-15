/**
 * @file pipeline.cppm
 * @brief Asynchronous batch dispatch for sequence input.
 *
 * The calling thread reads and parses records while worker tasks handle
 * completed batches. All submitted tasks finish before return.
 */
module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

export module bioseq.io.pipeline;

import bioseq.sequences;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.buffer;
import bioseq.io.fasta;
import bioseq.io.fastq;
import bioseq.io.sources;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

export namespace bioseq::io {

/**
 * @brief Run a record-by-record async dispatch loop.
 *
 * The function returns when the parser has reached EOF and every
 * pending batch has completed in the shared task pool.
 *
 * Parameter shape mirrors `run_sync_pipeline` in the reader module,
 * so the dispatcher can pick async vs sync via a single
 * `if (worker_count > 0) ...` branch.
 *
 * @tparam p_alphabet Alphabet used to encode record bodies
 * @tparam p_file_format Input record format
 * @tparam p_source Source satisfying the I/O source protocol
 * @tparam p_batch Movable batch receiving parsed records
 * @tparam p_handler Callable accepting a completed batch by rvalue
 * @tparam p_failure Callable accepting the first terminal diagnostic
 * @param[in] source Source consumed until EOF or failure
 * @param[in,out] handler_fn Handler kept alive until all queued tasks finish
 * @param[in,out] failure_fn Failure callback invoked at most once
 * @param[in] options Buffer, batch, and worker configuration
 * @return Emitted record count or the terminal diagnostic
 */
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          io_source p_source,
          typename p_batch,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
run_async_pipeline(p_source source,
                   p_handler&& handler_fn,
                   p_failure&& failure_fn,
                   const reader_options& options) {

    using alphabet_config_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    using sequence_traits_type = typename compact_sequence_range<alphabet_config_type>::traits;

    constexpr std::size_t bits_per_char = sequence_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits_type::letters_per_unit;
    constexpr bool is_fastq_format = p_file_format == sequence_file_format::fastq;

    auto& pool = bioseq::parallelism::global_pool::instance();
    if (!pool.is_active()) {
        const std::size_t requested_workers =
            std::max<std::size_t>(1, options.concurrency.worker_count);
        const unsigned configured_workers =
            requested_workers > (std::numeric_limits<unsigned>::max)()
                ? (std::numeric_limits<unsigned>::max)()
                : static_cast<unsigned>(requested_workers);
        pool.configure(bioseq::parallelism::pool_config{
            .maximum_concurrency = configured_workers,
            .mode = bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel,
        });
    }
    auto task_group = pool.make_task_group();

    // ---- Producer/parser side ---- //
    buffer_slab slab(options.io.initial_buffer_size);
    p_batch current_batch;
    std::size_t records_emitted = 0;
    bool aborted = false;
    std::string captured_error;
    std::size_t global_offset_base = 0;

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

    auto prepare_fresh_batch = [&current_batch, &flush_record_threshold]() {
        current_batch.prepare(flush_record_threshold, is_fastq_format);
        current_batch.reserve_storage(
            /*header_bytes*/ flush_record_threshold * 32,
            /*seq_words*/ flush_record_threshold * 8,
            /*quality_bytes*/ is_fastq_format ? flush_record_threshold * 256 : 0);
    };
    prepare_fresh_batch();

    auto dispatch_batch = [&current_batch, &handler_fn, &prepare_fresh_batch, &task_group]() {
        if (current_batch.empty())
            return;
        p_batch batch_to_dispatch = std::move(current_batch);
        task_group.run(bioseq::parallelism::task_body{
            [&handler_fn, batch = std::move(batch_to_dispatch)]() mutable {
                handler_fn(std::move(batch));
            }});
        current_batch = p_batch{};
        prepare_fresh_batch();
    };

    if constexpr (p_file_format == sequence_file_format::fasta) {
        fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(current_batch.header_storage(),
                                     current_batch.sequence_storage());

        auto emit_fasta_record = [&current_batch,
                                  &dispatch_batch,
                                  &records_emitted,
                                  &flush_record_threshold](std::size_t /*header_start*/,
                                                           std::size_t /*header_size*/,
                                                           std::size_t /*seq_start*/,
                                                           std::size_t /*seq_word_count*/,
                                                           std::size_t padding) {
            current_batch.header_offsets().push_back(current_batch.header_storage().size());
            current_batch.sequence_offsets().push_back(current_batch.sequence_storage().size());
            current_batch.sequence_paddings().push_back(padding);
            ++records_emitted;
            if (current_batch.size() >= flush_record_threshold) {
                dispatch_batch();
            }
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
            bool finished = false;
            while (!aborted) {
                parser_state.attach_to_batch(current_batch.header_storage(),
                                             current_batch.sequence_storage());
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
                    dispatch_batch();
                    finished = true;
                    break;
                }
                if (step.outcome == parse_outcome::abort) {
                    finished = true;
                    break;
                }
                need_refill = true;
                break;
            }
            if (finished)
                break;
            if (!need_refill)
                continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                dispatch_batch();
                break;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }

    } else { // FASTQ
        fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(current_batch.header_storage(),
                                     current_batch.sequence_storage(),
                                     current_batch.quality_storage());

        auto emit_fastq_record = [&current_batch,
                                  &dispatch_batch,
                                  &records_emitted,
                                  &flush_record_threshold](std::size_t /*header_start*/,
                                                           std::size_t /*header_size*/,
                                                           std::size_t /*seq_start*/,
                                                           std::size_t /*seq_word_count*/,
                                                           std::size_t padding,
                                                           std::size_t /*qual_start*/,
                                                           std::size_t /*qual_size*/) {
            current_batch.header_offsets().push_back(current_batch.header_storage().size());
            current_batch.sequence_offsets().push_back(current_batch.sequence_storage().size());
            current_batch.sequence_paddings().push_back(padding);
            current_batch.quality_offsets().push_back(current_batch.quality_storage().size());
            ++records_emitted;
            if (current_batch.size() >= flush_record_threshold) {
                dispatch_batch();
            }
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
            bool finished = false;
            while (!aborted) {
                parser_state.attach_to_batch(current_batch.header_storage(),
                                             current_batch.sequence_storage(),
                                             current_batch.quality_storage());
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
                    dispatch_batch();
                    finished = true;
                    break;
                }
                if (step.outcome == parse_outcome::abort) {
                    finished = true;
                    break;
                }
                need_refill = true;
                break;
            }
            if (finished)
                break;
            if (!need_refill)
                continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                dispatch_batch();
                break;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }
    }

    // Final flush of any partial batch before workers tear down.
    dispatch_batch();

    task_group.wait();

    if (aborted)
        return std::unexpected(captured_error);
    return records_emitted;
}

} // namespace bioseq::io
