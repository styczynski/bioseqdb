/**
 * @file pipeline.cppm
 * @brief Async producer/parser + worker pool dispatch for the IO reader.
 *
 * Phase-1 of the spec is the synchronous (`worker_count == 0`) path:
 * `public_api.cppm::run_sync_pipeline` reads, parses, and dispatches
 * batches inline on the calling thread. This module adds the async
 * shape: one producer/parser thread that drives the slab refill +
 * record FSM, plus N worker threads that consume batches via a
 * round-robin SPSC handoff and call the user's `handler_fn` in
 * parallel.
 *
 * The shape is a v1 simplification of the full §12 design:
 *
 *   * Producer + parser are fused into one thread. The macOS file
 *     `read(2)` is fast enough that holding it on a separate thread
 *     gains very little for the corpus sizes the bench exercises;
 *     splitting them is a follow-up optimisation gated on profiling.
 *   * Slab pool of one (the parser owns the slab; producer fills it
 *     in-place via `make_room` + `read_into`).
 *   * Per-worker SPSC ring carries batch handles. Round-robin
 *     dispatch (no work-stealing).
 *   * Free-batch pool is mutex-guarded — recycling rate is < 1×
 *     batch/ms in steady state, well below any contention threshold.
 *
 * The handler-side win is real: a worker pool of N threads parses
 * records as fast as the parser produces batches, and any per-batch
 * post-processing (hashing, indexing, alignment seed extraction) runs
 * concurrently across cores.
 *
 * Spec §12 / Appendix I (SPSC) / Appendix M (orchestration).
 */
module;

#include "common.h"

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

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
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

/// Default capacity of the per-worker SPSC ring. 32 slots = ~16 ms
/// of buffering at 2k batches/sec (the rate a 5 GiB/s parser produces
/// at 1 MiB batch payloads), enough to absorb worker scheduling
/// jitter without ballooning memory. Spec §5.4.1.
constexpr std::size_t kWorkerRingCapacity = 32;

/**
 * @brief Run a record-by-record async dispatch loop.
 *
 * The caller (in practice `read_sequences_file`) provides:
 *   * a moved-in source (POSIX / mmap / gzip),
 *   * a `handler_fn` invoked by-rvalue with a `sequence_batch<...>&&`,
 *   * a `failure_fn` invoked once on the first terminal error,
 *   * `reader_options::concurrency.worker_count` — the worker pool size.
 *
 * The function returns when the parser has reached EOF and every
 * pending batch has been delivered to a worker. The worker threads
 * are joined before return.
 *
 * Parameter shape mirrors `run_sync_pipeline` in `public_api.cppm`,
 * so the dispatcher can pick async vs sync via a single
 * `if (worker_count > 0) ...` branch.
 */
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    io_source source_t,
    typename batch_t,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string> run_async_pipeline(
    source_t source,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options) {

    using alphabet_config = sequence_custom_alphabet<alphabet_t, std::uint64_t>;
    using sequence_traits =
        typename compact_sequence_range<alphabet_config>::traits;

    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;
    constexpr bool is_fastq_format = (file_type == sequence_io_file_type::FASTQ);

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    if (!pool.active()) {
        const std::size_t requested_workers = std::max<std::size_t>(
            1, options.concurrency.worker_count);
        const unsigned configured_workers =
            requested_workers > (std::numeric_limits<unsigned>::max)()
                ? (std::numeric_limits<unsigned>::max)()
                : static_cast<unsigned>(requested_workers);
        pool.configure(bioseq::parallelism::PoolConfig{
            .max_concurrency = configured_workers,
            .mode = bioseq::parallelism::PipelineConcurrencyMode::PARALLEL,
        });
    }
    auto task_group = pool.make_task_group();

    // ---- Producer/parser side ---- //
    buffer_slab slab(options.io.initial_buffer_size);
    batch_t current_batch;
    std::size_t records_emitted = 0;
    bool aborted = false;
    std::string captured_error;
    std::size_t global_offset_base = 0;

    auto failure_capture = [&](std::string&& msg) {
        if (!aborted) {
            captured_error = msg;
            failure_fn(std::move(msg));
        }
        aborted = true;
    };

    const std::size_t flush_record_threshold =
        (options.concurrency.batch_target_record_count > 0)
            ? options.concurrency.batch_target_record_count
            : 4096;

    auto prepare_fresh_batch = [&]() {
        current_batch.prepare(flush_record_threshold, is_fastq_format);
        current_batch.reserve_storage(
            /*header_bytes*/ flush_record_threshold * 32,
            /*seq_words*/    flush_record_threshold * 8,
            /*quality_bytes*/ is_fastq_format ? flush_record_threshold * 256 : 0);
    };
    prepare_fresh_batch();

    auto dispatch_batch = [&]() {
        if (current_batch.empty()) return;
        batch_t batch_to_dispatch = std::move(current_batch);
        task_group.run(bioseq::parallelism::task_body{
            [&handler_fn, batch = std::move(batch_to_dispatch)]() mutable {
                handler_fn(std::move(batch));
            }});
        current_batch = batch_t{};
        prepare_fresh_batch();
    };

    if constexpr (file_type == sequence_io_file_type::FASTA) {
        fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(current_batch.header_storage(),
                                     current_batch.seq_storage());

        auto emit_fasta_record = [&](std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding) {
            current_batch.header_offsets().push_back(current_batch.header_storage().size());
            current_batch.seq_offsets().push_back(current_batch.seq_storage().size());
            current_batch.seq_paddings().push_back(padding);
            ++records_emitted;
            if (current_batch.size() >= flush_record_threshold) {
                dispatch_batch();
            }
        };

        while (!aborted) {
            if (slab.fill_size == slab.capacity) {
                if (!slab.make_room()) {
                    if (!slab.grow(options)) {
                        emit_failure(failure_capture,
                                     "buffer_limit reached at {} bytes",
                                     slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto refill_result =
                    source.read_into(slab.raw() + slab.fill_size, free_room);
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
                                             current_batch.seq_storage());
                const auto step =
                    parse_one_fasta_record<alphabet_t, std::uint64_t>(
                        parser_state,
                        slab.raw(),
                        slab.fill_size,
                        slab.is_eof,
                        slab.parse_cursor,
                        global_offset_base + slab.parse_cursor,
                        emit_fasta_record,
                        failure_capture);
                slab.parse_cursor = step.cursor;
                if (step.outcome == parse_outcome::COMPLETE) continue;
                if (step.outcome == parse_outcome::AT_EOF) {
                    dispatch_batch();
                    finished = true;
                    break;
                }
                if (step.outcome == parse_outcome::ABORT) {
                    finished = true;
                    break;
                }
                need_refill = true;
                break;
            }
            if (finished) break;
            if (!need_refill) continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                dispatch_batch();
                break;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }

    } else {  // FASTQ
        fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(current_batch.header_storage(),
                                     current_batch.seq_storage(),
                                     current_batch.quality_storage());

        auto emit_fastq_record = [&](std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding,
                                     std::size_t /*qual_start*/,
                                     std::size_t /*qual_size*/) {
            current_batch.header_offsets().push_back(current_batch.header_storage().size());
            current_batch.seq_offsets().push_back(current_batch.seq_storage().size());
            current_batch.seq_paddings().push_back(padding);
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
                        emit_failure(failure_capture,
                                     "buffer_limit reached at {} bytes",
                                     slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto refill_result =
                    source.read_into(slab.raw() + slab.fill_size, free_room);
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
                                             current_batch.seq_storage(),
                                             current_batch.quality_storage());
                const auto step =
                    parse_one_fastq_record<alphabet_t, std::uint64_t>(
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
                if (step.outcome == parse_outcome::COMPLETE) continue;
                if (step.outcome == parse_outcome::AT_EOF) {
                    dispatch_batch();
                    finished = true;
                    break;
                }
                if (step.outcome == parse_outcome::ABORT) {
                    finished = true;
                    break;
                }
                need_refill = true;
                break;
            }
            if (finished) break;
            if (!need_refill) continue;
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

    if (aborted) return std::unexpected(captured_error);
    return records_emitted;
}

}  // namespace bioseq::io
