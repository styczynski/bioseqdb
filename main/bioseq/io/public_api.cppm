/**
 * @file public_api.cppm
 * @brief Public entry point for the IO reader.
 *
 * Owns the user-visible types (`sequence_batch`, `compact_sequence_view`,
 * the `sequence_batch_handler` concept) and the top-level dispatcher
 * `read_sequences_file`. The dispatcher picks one of three I/O
 * backends and one of two parser modes:
 *
 *   * **Backend.**
 *     - `gzip_source` when `options.io.gzip_auto_detect == true`
 *       (zlib's transparent-mode `gzopen` reads plain files too, so
 *       this path is the safe default for unknown inputs);
 *     - `mmap_source` when `gzip_auto_detect == false` AND
 *       `options.io.prefer_mmap == true` (FASTA only — mmap on a
 *       gzip-compressed stream would force a pre-decompression
 *       pass);
 *     - `posix_read_source` as the always-available fallback.
 *
 *   * **Parser mode.**
 *     - `run_sync_pipeline` when `worker_count == 0` — same thread
 *       reads, parses, and dispatches handler calls. Spec §12.6.
 *     - `run_async_pipeline` when `worker_count > 0` — producer/parser
 *       on the calling thread, N workers consume batches via SPSC
 *       round-robin. Spec §12.
 *
 * The two paths share the same per-record FSM (fasta.cppm, fastq.cppm)
 * and the same `sequence_batch` type, so the only divergence is in
 * the dispatch shell.
 *
 * **v2 storage layout.** Each `sequence_batch` owns one shared
 * sequence vector (`_seq_storage`), one shared header buffer
 * (`_header_storage`), and one shared quality buffer
 * (`_quality_storage`), plus per-record offset arrays. The encoder
 * writes the packed `uint64_t` words directly into `_seq_storage`
 * (via `encode_state::output_storage`); the FSM appends header
 * bytes directly into `_header_storage`. The result: the per-record
 * heap allocation count drops from 2 (`std::string` + `std::vector`)
 * to 0; the batch's three backing vectors grow geometrically across
 * thousands of records. This is the key v2 throughput win on short
 * reads. Spec §3.4 (`[[no_unique_address]]` arena).
 */
module;

#include "common.h"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module bioseq_io.public_api;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.options;
import bioseq_io.errors;
import bioseq_io.buffer;
import bioseq_io.encoder;
import bioseq_io.encoder_dispatch;
import bioseq_io.fasta;
import bioseq_io.fastq;
import bioseq_io.simd_kernels;
import bioseq_io.sources;
import bioseq_io.pipeline;
import bioseq_io.compression.parallel_gzip_source;

export namespace bioseq::io {

/// Which I/O backend the dispatcher in `read_sequences_file` actually
/// chose for a given call. The dispatcher's branch tree depends on
/// magic-byte sniff + the runtime options + size thresholds, so the
/// backend that really ran can differ from what the caller "asked
/// for" via `reader_options`. Bench harnesses use this so they can
/// label samples with the path that actually executed instead of the
/// runner's nominal source kind.
enum class read_backend : std::uint8_t {
    UNKNOWN = 0,    ///< pre-dispatch / failed before a backend was selected
    MMAP,           ///< plain `mmap_source`
    POSIX_READ,     ///< `posix_read_source` (mmap unavailable / disabled)
    GZIP,           ///< single-thread `gzip_source` (zlib)
    PARALLEL_GZIP,  ///< multi-producer `parallel_gzip_source`
};

[[nodiscard]] constexpr const char* read_backend_name(read_backend b) noexcept {
    switch (b) {
        case read_backend::UNKNOWN:        return "unknown";
        case read_backend::MMAP:           return "mmap";
        case read_backend::POSIX_READ:     return "posix_read";
        case read_backend::GZIP:           return "gzip";
        case read_backend::PARALLEL_GZIP:  return "parallel_gzip";
    }
    return "?";
}

/// View of one record's packed sequence inside a batch's shared
/// `_seq_storage`. Models the same surface used by the v1
/// `compact_sequence_range<C>`: `size()` (storage-unit count),
/// `padding()` (count of trailing dummy letters), and a const
/// random-access iterator pair so range-based `for` over storage
/// units works unchanged. Templated on the same `C` (custom alphabet
/// config) as `compact_sequence_range<C>` so callers can use the same
/// `traits` typedef.
///
/// The view is non-owning — its lifetime is bound to the batch that
/// produced it. Once the batch is destroyed the view is dangling.
/// In practice the handler always processes the batch in scope, so
/// this matches the v1 contract (which also returned a `const
/// sequence_range_t&` whose lifetime ended with the batch).
template <typename C>
class compact_sequence_view {
public:
    using traits = typename compact_sequence_range<C>::traits;
    using storage_unit = typename traits::storage_unit;
    using iterator = const storage_unit*;
    using const_iterator = const storage_unit*;

    constexpr compact_sequence_view() = default;
    constexpr compact_sequence_view(const storage_unit* base,
                                     std::size_t word_count,
                                     std::size_t padding) noexcept
        : _base(base), _word_count(word_count), _padding(padding) {}

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return _word_count;
    }
    [[nodiscard]] constexpr std::size_t padding() const noexcept {
        return _padding;
    }
    [[nodiscard]] constexpr const storage_unit* data() const noexcept {
        return _base;
    }

    [[nodiscard]] constexpr iterator begin() const noexcept { return _base; }
    [[nodiscard]] constexpr iterator end() const noexcept {
        return _base + _word_count;
    }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return end(); }

    [[nodiscard]] constexpr storage_unit operator[](std::size_t index) const noexcept {
        return _base[index];
    }

private:
    const storage_unit* _base = nullptr;
    std::size_t _word_count = 0;
    std::size_t _padding = 0;
};

/// A batch of records delivered to the user's handler. v2 holds three
/// shared buffers (`_header_storage`, `_seq_storage`,
/// `_quality_storage`) and per-record offset / metadata arrays. This
/// eliminates the v1 per-record `std::string` + `std::vector<uint64_t>`
/// allocations that dominated short-read throughput.
///
/// The accessors `header(i)` / `sequence(i)` / `quality(i)` return
/// non-owning views (`std::string_view`, `compact_sequence_view<C>`,
/// `std::span<const std::uint8_t>`) into the batch's shared storage.
/// Lifetimes match v1: the returned view is valid as long as the
/// batch is.
template <typename C>
class sequence_batch {
public:
    using sequence_range_t = compact_sequence_range<C>;
    using sequence_view_t = compact_sequence_view<C>;
    using storage_unit = typename sequence_view_t::storage_unit;

    [[nodiscard]] std::size_t size() const noexcept { return _seq_paddings.size(); }
    [[nodiscard]] bool empty() const noexcept { return _seq_paddings.empty(); }

    [[nodiscard]] std::string_view header(std::size_t index) const noexcept {
        const std::size_t start = _header_offsets[index];
        const std::size_t stop = _header_offsets[index + 1];
        return std::string_view{_header_storage.data() + start, stop - start};
    }

    [[nodiscard]] sequence_view_t sequence(std::size_t index) const noexcept {
        const std::size_t start = _seq_offsets[index];
        const std::size_t stop = _seq_offsets[index + 1];
        return sequence_view_t{_seq_storage.data() + start,
                               stop - start,
                               _seq_paddings[index]};
    }

    [[nodiscard]] std::span<const std::uint8_t> quality(std::size_t index) const noexcept {
        if (_quality_offsets.empty()) return {};
        const std::size_t start = _quality_offsets[index];
        const std::size_t stop = _quality_offsets[index + 1];
        return std::span<const std::uint8_t>{
            _quality_storage.data() + start, stop - start};
    }

    /// Mutable accessors used by the FSM emit-record callbacks. The
    /// FSM appends to these via raw pointers; the batch is only
    /// observed through the public `header(i)` / `sequence(i)` /
    /// `quality(i)` views once handed to the user handler.
    [[nodiscard]] std::vector<char>& header_storage() noexcept { return _header_storage; }
    [[nodiscard]] std::vector<std::size_t>& header_offsets() noexcept { return _header_offsets; }
    [[nodiscard]] std::vector<storage_unit>& seq_storage() noexcept { return _seq_storage; }
    [[nodiscard]] std::vector<std::size_t>& seq_offsets() noexcept { return _seq_offsets; }
    [[nodiscard]] std::vector<std::size_t>& seq_paddings() noexcept { return _seq_paddings; }
    [[nodiscard]] std::vector<std::uint8_t>& quality_storage() noexcept { return _quality_storage; }
    [[nodiscard]] std::vector<std::size_t>& quality_offsets() noexcept { return _quality_offsets; }

    /// Initialise the offset prefix arrays so the first record's
    /// offset is 0. Called once per batch by the FSM driver.
    void prepare(std::size_t expected_record_count, bool with_quality) {
        _seq_paddings.clear();
        _seq_paddings.reserve(expected_record_count);
        _seq_offsets.clear();
        _seq_offsets.reserve(expected_record_count + 1);
        _seq_offsets.push_back(0);
        _header_offsets.clear();
        _header_offsets.reserve(expected_record_count + 1);
        _header_offsets.push_back(0);
        _seq_storage.clear();
        _header_storage.clear();
        if (with_quality) {
            _quality_offsets.clear();
            _quality_offsets.reserve(expected_record_count + 1);
            _quality_offsets.push_back(0);
            _quality_storage.clear();
        }
    }

    /// Reserve the storage buffers up front so the per-record append
    /// path runs without reallocs once steady state is reached. The
    /// caller passes a rough byte estimate; the batch grows
    /// geometrically beyond that if the corpus exceeds the hint.
    void reserve_storage(std::size_t header_bytes_hint,
                         std::size_t seq_words_hint,
                         std::size_t quality_bytes_hint) {
        _header_storage.reserve(header_bytes_hint);
        _seq_storage.reserve(seq_words_hint);
        if (quality_bytes_hint > 0) {
            _quality_storage.reserve(quality_bytes_hint);
        }
    }

    void clear() noexcept {
        _header_storage.clear();
        _header_offsets.clear();
        _seq_storage.clear();
        _seq_offsets.clear();
        _seq_paddings.clear();
        _quality_storage.clear();
        _quality_offsets.clear();
    }

private:
    std::vector<char> _header_storage;
    std::vector<std::size_t> _header_offsets;  // size N+1; header[i] = [_header_offsets[i], _header_offsets[i+1])
    std::vector<storage_unit> _seq_storage;
    std::vector<std::size_t> _seq_offsets;     // size N+1
    std::vector<std::size_t> _seq_paddings;    // size N
    std::vector<std::uint8_t> _quality_storage;
    std::vector<std::size_t> _quality_offsets; // size N+1 if FASTQ; size 0 if FASTA
};

/// Concept matching the spec §3.5: a handler that accepts a batch
/// rvalue.
template <typename handler_fn_t, typename batch_t>
concept sequence_batch_handler = std::invocable<handler_fn_t, batch_t&&>;

namespace detail {

/// Run the synchronous read loop with a concrete source. Single-thread
/// path: the same thread reads bytes, parses records, and invokes the
/// user's handler. Spec §12.6 + Appendix M.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    io_source source_t,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string> run_sync_pipeline(
    source_t source,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options) {

    using alphabet_config = sequence_custom_alphabet<alphabet_t, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    using sequence_traits = typename compact_sequence_range<alphabet_config>::traits;

    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;

    buffer_slab slab(options.io.initial_buffer_size);
    batch_t batch;
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

    constexpr bool is_fastq_format = (file_type == sequence_io_file_type::FASTQ);
    auto prepare_fresh_batch = [&]() {
        batch.prepare(flush_record_threshold, is_fastq_format);
        // Generous up-front reservations sized for ~256 bp records.
        // Larger corpora simply grow geometrically beyond the hint.
        batch.reserve_storage(
            /*header_bytes*/ flush_record_threshold * 32,
            /*seq_words*/    flush_record_threshold * 8,
            /*quality_bytes*/ is_fastq_format ? flush_record_threshold * 256 : 0);
    };

    auto flush_batch = [&]() {
        if (batch.empty()) return;
        handler_fn(std::move(batch));
        batch = batch_t{};
        prepare_fresh_batch();
    };

    prepare_fresh_batch();

    if constexpr (file_type == sequence_io_file_type::FASTA) {
        fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(batch.header_storage(),
                                     batch.seq_storage());

        auto emit_fasta_record = [&](std::size_t header_start,
                                     std::size_t header_size,
                                     std::size_t seq_start,
                                     std::size_t seq_word_count,
                                     std::size_t padding) {
            (void)header_size;
            (void)seq_word_count;
            // The header bytes were already appended into `_header_storage` by
            // the FSM as it copied them out of the slab; sequence words were
            // pushed into `_seq_storage` by the encoder. We just record the
            // offsets and padding for this record.
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.seq_offsets().push_back(batch.seq_storage().size());
            batch.seq_paddings().push_back(padding);
            (void)header_start;
            (void)seq_start;
            ++records_emitted;
            if (batch.size() >= flush_record_threshold) flush_batch();
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
            while (!aborted) {
                // The FSM updates `parser_state` and the batch storage
                // directly; the emit_record callback only needs to record
                // the (header_offset, seq_offset, padding) triple.
                parser_state.attach_to_batch(batch.header_storage(),
                                             batch.seq_storage());
                const auto step = parse_one_fasta_record<alphabet_t, std::uint64_t>(
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
                    flush_batch();
                    if (aborted) {
                        return std::unexpected(captured_error);
                    }
                    return records_emitted;
                }
                if (step.outcome == parse_outcome::ABORT) {
                    return std::unexpected(captured_error);
                }
                need_refill = true;
                break;
            }
            if (!need_refill) continue;
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
        parser_state.attach_to_batch(batch.header_storage(),
                                     batch.seq_storage(),
                                     batch.quality_storage());

        auto emit_fastq_record = [&](std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding,
                                     std::size_t /*qual_start*/,
                                     std::size_t /*qual_size*/) {
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.seq_offsets().push_back(batch.seq_storage().size());
            batch.seq_paddings().push_back(padding);
            batch.quality_offsets().push_back(batch.quality_storage().size());
            ++records_emitted;
            if (batch.size() >= flush_record_threshold) flush_batch();
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
            while (!aborted) {
                parser_state.attach_to_batch(batch.header_storage(),
                                             batch.seq_storage(),
                                             batch.quality_storage());
                const auto step = parse_one_fastq_record<alphabet_t, std::uint64_t>(
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
                    flush_batch();
                    if (aborted) {
                        return std::unexpected(captured_error);
                    }
                    return records_emitted;
                }
                if (step.outcome == parse_outcome::ABORT) {
                    return std::unexpected(captured_error);
                }
                need_refill = true;
                break;
            }
            if (!need_refill) continue;
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
/// is already in contiguous virtual memory; we hand the parser a
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
/// The reservation hint for the batch's `_seq_storage` uses the file
/// size: dna4 packs 4 letters per byte, so ~`file_size / 32` u64
/// words bound the storage requirement. Pre-reserving this avoids
/// the ~9 geometric reallocs that would otherwise fire while encoding
/// a 234 MiB body.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string>
run_sync_pipeline_mmap_direct(
    mmap_source source,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options) {

    using alphabet_config = sequence_custom_alphabet<alphabet_t, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    using sequence_traits =
        typename compact_sequence_range<alphabet_config>::traits;
    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;
    constexpr bool is_fastq_format = (file_type == sequence_io_file_type::FASTQ);

    const std::size_t file_size = source.mapped_size();
    auto [base_ptr, view_size] = source.direct_view();

    batch_t batch;
    std::size_t records_emitted = 0;
    bool aborted = false;
    std::string captured_error;

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

    // Reservation: dna4 packs 4 letters per byte, so each input byte
    // produces ~1/16 of a 64-bit word. For the bench's 234 MiB human
    // case this is ~58 M words / 464 MiB; pre-reserving avoids the 9
    // geometric reallocs the v1 path would fire mid-encoding.
    const std::size_t seq_words_hint =
        std::max<std::size_t>(file_size / 16, flush_record_threshold * 8);
    const std::size_t header_bytes_hint =
        std::max<std::size_t>(file_size / 256, flush_record_threshold * 32);

    auto prepare_fresh_batch = [&]() {
        batch.prepare(flush_record_threshold, is_fastq_format);
        batch.reserve_storage(
            header_bytes_hint, seq_words_hint,
            is_fastq_format ? flush_record_threshold * 256 : 0);
    };

    auto flush_batch = [&]() {
        if (batch.empty()) return;
        handler_fn(std::move(batch));
        batch = batch_t{};
        prepare_fresh_batch();
    };

    prepare_fresh_batch();

    if constexpr (file_type == sequence_io_file_type::FASTA) {
        fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(batch.header_storage(),
                                     batch.seq_storage());

        auto emit_fasta_record = [&](std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding) {
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.seq_offsets().push_back(batch.seq_storage().size());
            batch.seq_paddings().push_back(padding);
            ++records_emitted;
            if (batch.size() >= flush_record_threshold) flush_batch();
        };

        std::size_t cursor = 0;
        while (cursor < view_size && !aborted) {
            parser_state.attach_to_batch(batch.header_storage(),
                                         batch.seq_storage());
            const auto step = parse_one_fasta_record<alphabet_t, std::uint64_t>(
                parser_state,
                base_ptr,
                view_size,
                /*is_eof=*/true,
                cursor,
                cursor,
                emit_fasta_record,
                failure_capture);
            cursor = step.cursor;
            if (step.outcome == parse_outcome::COMPLETE) continue;
            if (step.outcome == parse_outcome::AT_EOF) break;
            if (step.outcome == parse_outcome::ABORT) {
                return std::unexpected(captured_error);
            }
            // INCOMPLETE on `is_eof=true` means the FSM still wants
            // more bytes — that's a malformed-file diagnostic, not a
            // refill hint. Bail out.
            break;
        }
        flush_batch();
        if (aborted) return std::unexpected(captured_error);
        return records_emitted;
    } else {
        fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;
        parser_state.attach_to_batch(batch.header_storage(),
                                     batch.seq_storage(),
                                     batch.quality_storage());

        auto emit_fastq_record = [&](std::size_t /*header_start*/,
                                     std::size_t /*header_size*/,
                                     std::size_t /*seq_start*/,
                                     std::size_t /*seq_word_count*/,
                                     std::size_t padding,
                                     std::size_t /*qual_start*/,
                                     std::size_t /*qual_size*/) {
            batch.header_offsets().push_back(batch.header_storage().size());
            batch.seq_offsets().push_back(batch.seq_storage().size());
            batch.seq_paddings().push_back(padding);
            batch.quality_offsets().push_back(batch.quality_storage().size());
            ++records_emitted;
            if (batch.size() >= flush_record_threshold) flush_batch();
        };

        std::size_t cursor = 0;
        while (cursor < view_size && !aborted) {
            parser_state.attach_to_batch(batch.header_storage(),
                                         batch.seq_storage(),
                                         batch.quality_storage());
            const auto step = parse_one_fastq_record<alphabet_t, std::uint64_t>(
                parser_state,
                base_ptr,
                view_size,
                /*is_eof=*/true,
                cursor,
                cursor,
                options.encoder.strict_fastq,
                emit_fastq_record,
                failure_capture);
            cursor = step.cursor;
            if (step.outcome == parse_outcome::COMPLETE) continue;
            if (step.outcome == parse_outcome::AT_EOF) break;
            if (step.outcome == parse_outcome::ABORT) {
                return std::unexpected(captured_error);
            }
            break;
        }
        flush_batch();
        if (aborted) return std::unexpected(captured_error);
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
[[nodiscard]] inline std::size_t find_fasta_record_boundary(
    const std::uint8_t* base, std::size_t view_size, std::size_t target) noexcept {
    if (target == 0) return 0;
    if (target >= view_size) return view_size;
    std::size_t cursor = target;
    // Greedy: scan to next '\n', check if next byte is '>'. If not,
    // advance past the newline and continue. simd::scan_first_byte is
    // 4×-unrolled NEON so this loop runs at near-memory bandwidth.
    while (cursor < view_size) {
        const std::size_t remaining = view_size - cursor;
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', base + cursor, remaining);
        if (newline_offset >= remaining) return view_size;
        const std::size_t after_newline = cursor + newline_offset + 1;
        if (after_newline >= view_size) return view_size;
        if (base[after_newline] == '>') return after_newline;
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
/// canonical FASTQ this converges within 1-2 candidates; for FASTQ
/// with `@`-rich quality strings it may take a few tries but the SIMD
/// scan keeps the per-candidate cost low.
[[nodiscard]] inline std::size_t find_fastq_record_boundary(
    const std::uint8_t* base, std::size_t view_size, std::size_t target) noexcept {
    if (target == 0) return 0;
    if (target >= view_size) return view_size;
    std::size_t cursor = target;
    while (cursor < view_size) {
        // Find next '\n' (a candidate record boundary lies just after).
        const std::size_t remaining = view_size - cursor;
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', base + cursor, remaining);
        if (newline_offset >= remaining) return view_size;
        const std::size_t at_header = cursor + newline_offset + 1;
        if (at_header >= view_size) return view_size;
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
        if (hdr_newline >= hdr_remaining) return view_size;
        const std::size_t at_seq = at_header + hdr_newline + 1;
        if (at_seq >= view_size) return view_size;
        const std::size_t seq_remaining = view_size - at_seq;
        const std::size_t seq_newline =
            simd::scan_first_byte('\n', base + at_seq, seq_remaining);
        if (seq_newline >= seq_remaining) return view_size;
        const std::size_t at_sep = at_seq + seq_newline + 1;
        if (at_sep >= view_size) return view_size;
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
/// into N shards; each shard finds its actual start by walking forward
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
///     `_seq_storage` on the human-genome cell; spreading those faults
///     across N cores cuts the demand-zero latency by N.
///   * **OS prefetcher win** — N concurrent sequential readers trip
///     the kernel page-cache prefetcher in N regions instead of 1.
///
/// Correctness: shard boundaries are at record starts, so each shard
/// parses complete records with `is_eof = true` (the FSM emits the
/// trailing record cleanly). Shard 0 starts at byte 0; shard N-1 ends
/// at file end. Records are emitted in within-shard order; across
/// shards order is not strictly preserved. The bench's accumulator
/// uses atomic counters so order doesn't matter for the metric, and
/// real handlers (alignment, indexing) consume per-batch with no
/// cross-batch ordering requirement.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string>
run_sync_pipeline_mmap_parallel(
    mmap_source source,
    std::size_t shard_count,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options) {

    using alphabet_config = sequence_custom_alphabet<alphabet_t, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    using sequence_traits =
        typename compact_sequence_range<alphabet_config>::traits;
    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;
    constexpr bool is_fastq_format = (file_type == sequence_io_file_type::FASTQ);

    auto [base_ptr, view_size] = source.direct_view();
    if (view_size == 0 || shard_count <= 1) {
        // Fall back to the sequential direct path.
        return run_sync_pipeline_mmap_direct<alphabet_t, file_type>(
            std::move(source),
            std::forward<handler_fn_t>(handler_fn),
            std::forward<failure_fn_t>(failure_fn),
            options);
    }

    // Determine shard boundaries. shard[i] = [boundary[i], boundary[i+1]).
    std::vector<std::size_t> boundaries;
    boundaries.reserve(shard_count + 1);
    boundaries.push_back(0);
    for (std::size_t shard_index = 1; shard_index < shard_count; ++shard_index) {
        const std::size_t target =
            (view_size * shard_index) / shard_count;
        std::size_t actual = (file_type == sequence_io_file_type::FASTA)
            ? find_fasta_record_boundary(base_ptr, view_size, target)
            : find_fastq_record_boundary(base_ptr, view_size, target);
        // Monotonicity: if a shard's adjusted start lands past the
        // previous shard's start, keep it; otherwise this shard is
        // empty (huge single-record file or skewed records).
        if (actual < boundaries.back()) actual = boundaries.back();
        if (actual > view_size) actual = view_size;
        boundaries.push_back(actual);
    }
    boundaries.push_back(view_size);

    // Per-shard reservation hints. Keep them DELIBERATELY small —
    // macOS's `vm_map` lock serializes anonymous-mmap page faults
    // across threads, so eight shards each pre-reserving (and
    // demand-zeroing) 100+ MB up front costs more wall-clock than the
    // geometric grow-on-demand pattern. We leave just enough headroom
    // for one batch's worth of records without an immediate realloc;
    // beyond that the per-shard vector grows on its own and
    // amortises across the rest of the shard. The growth memcpy is
    // bandwidth-bound and parallelises across shards; the demand-zero
    // is kernel-serialised and does not.
    const std::size_t flush_record_threshold =
        (options.concurrency.batch_target_record_count > 0)
            ? options.concurrency.batch_target_record_count
            : 4096;

    // Concurrency state.
    std::mutex emit_mutex;
    std::atomic<std::size_t> total_records{0};
    std::atomic<bool> aborted{false};
    std::mutex error_mutex;
    std::string captured_error;

    auto failure_capture = [&](std::string&& msg) {
        std::lock_guard guard{error_mutex};
        if (!aborted.exchange(true, std::memory_order_acq_rel)) {
            captured_error = std::move(msg);
            failure_fn(std::string{captured_error});
        }
    };

    auto run_shard = [&](std::size_t shard_index) {
        const std::size_t shard_start = boundaries[shard_index];
        const std::size_t shard_end = boundaries[shard_index + 1];
        if (shard_start >= shard_end) return;
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
        // per BP, ~46 % of the shard size for canonical 200-bp reads;
        // /4 = 256 MiB per 1 GiB shard is a safe upper bound.
        //
        // The `flush_record_threshold * X` floors handle the SHORT
        // synthetic cells that fall below the parallel threshold but
        // would otherwise reserve too little for one batch.
        batch_t batch;
        const std::size_t shard_seq_words_hint = std::max<std::size_t>(
            shard_size / 16, flush_record_threshold * 8);
        const std::size_t shard_header_bytes_hint = std::max<std::size_t>(
            shard_size / 256, flush_record_threshold * 32);
        const std::size_t shard_quality_bytes_hint = is_fastq_format
            ? std::max<std::size_t>(shard_size / 4,
                                    flush_record_threshold * 256)
            : 0;

        auto prepare_fresh_batch = [&]() {
            batch.prepare(flush_record_threshold, is_fastq_format);
            batch.reserve_storage(shard_header_bytes_hint,
                                  shard_seq_words_hint,
                                  shard_quality_bytes_hint);
        };
        prepare_fresh_batch();

        std::size_t shard_records = 0;

        auto flush_batch = [&]() {
            if (batch.empty()) return;
            // Hand the batch to the user handler under a mutex. The
            // handler call rate is one per ~4 K records (roughly once
            // per millisecond at 5 GB/s), so the lock cost is invisible
            // against per-byte SIMD throughput.
            {
                std::lock_guard guard{emit_mutex};
                handler_fn(std::move(batch));
            }
            batch = batch_t{};
            prepare_fresh_batch();
        };

        if constexpr (file_type == sequence_io_file_type::FASTA) {
            fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit>
                parser_state;
            parser_state.attach_to_batch(batch.header_storage(),
                                         batch.seq_storage());

            auto emit_fasta_record = [&](std::size_t /*hs*/, std::size_t /*hsz*/,
                                         std::size_t /*ss*/, std::size_t /*sw*/,
                                         std::size_t padding) {
                batch.header_offsets().push_back(batch.header_storage().size());
                batch.seq_offsets().push_back(batch.seq_storage().size());
                batch.seq_paddings().push_back(padding);
                ++shard_records;
                if (batch.size() >= flush_record_threshold) flush_batch();
            };

            std::size_t cursor = shard_start;
            while (cursor < shard_end &&
                   !aborted.load(std::memory_order_relaxed)) {
                parser_state.attach_to_batch(batch.header_storage(),
                                             batch.seq_storage());
                const auto step =
                    parse_one_fasta_record<alphabet_t, std::uint64_t>(
                        parser_state,
                        base_ptr,
                        shard_end,
                        /*is_eof=*/true,
                        cursor,
                        cursor,
                        emit_fasta_record,
                        failure_capture);
                cursor = step.cursor;
                if (step.outcome == parse_outcome::COMPLETE) continue;
                if (step.outcome == parse_outcome::AT_EOF) break;
                if (step.outcome == parse_outcome::ABORT) return;
                break;  // INCOMPLETE on is_eof=true → diagnostic
            }
            flush_batch();
        } else {
            fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit>
                parser_state;
            parser_state.attach_to_batch(batch.header_storage(),
                                         batch.seq_storage(),
                                         batch.quality_storage());

            auto emit_fastq_record = [&](std::size_t /*hs*/, std::size_t /*hsz*/,
                                         std::size_t /*ss*/, std::size_t /*sw*/,
                                         std::size_t padding,
                                         std::size_t /*qs*/, std::size_t /*qsz*/) {
                batch.header_offsets().push_back(batch.header_storage().size());
                batch.seq_offsets().push_back(batch.seq_storage().size());
                batch.seq_paddings().push_back(padding);
                batch.quality_offsets().push_back(batch.quality_storage().size());
                ++shard_records;
                if (batch.size() >= flush_record_threshold) flush_batch();
            };

            std::size_t cursor = shard_start;
            while (cursor < shard_end &&
                   !aborted.load(std::memory_order_relaxed)) {
                parser_state.attach_to_batch(batch.header_storage(),
                                             batch.seq_storage(),
                                             batch.quality_storage());
                const auto step =
                    parse_one_fastq_record<alphabet_t, std::uint64_t>(
                        parser_state,
                        base_ptr,
                        shard_end,
                        /*is_eof=*/true,
                        cursor,
                        cursor,
                        options.encoder.strict_fastq,
                        emit_fastq_record,
                        failure_capture);
                cursor = step.cursor;
                if (step.outcome == parse_outcome::COMPLETE) continue;
                if (step.outcome == parse_outcome::AT_EOF) break;
                if (step.outcome == parse_outcome::ABORT) return;
                break;
            }
            flush_batch();
        }

        total_records.fetch_add(shard_records, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(shard_count);
    for (std::size_t shard_index = 0; shard_index < shard_count; ++shard_index) {
        threads.emplace_back(run_shard, shard_index);
    }
    for (auto& thread : threads) thread.join();

    if (aborted.load(std::memory_order_acquire)) {
        return std::unexpected(captured_error);
    }
    return total_records.load(std::memory_order_relaxed);
}

/// Mode-aware dispatcher. Routes to the sync pipeline when no workers
/// are configured, otherwise to the async pipeline. mmap source goes
/// through the zero-copy direct path when sync.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    io_source source_t,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string> run_pipeline_for_source(
    source_t source,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options) {
    using alphabet_config = sequence_custom_alphabet<alphabet_t, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    if (options.concurrency.worker_count == 0) {
        if constexpr (std::is_same_v<source_t, mmap_source>) {
            if (options.io.mmap_zero_copy) {
                // Pick shard count: explicit option wins, else
                // hardware_concurrency capped at mmap_parallel_max_shards.
                std::size_t requested_shards =
                    options.concurrency.parser_count;
                if (requested_shards == 0) {
                    const unsigned hw_threads =
                        std::thread::hardware_concurrency();
                    requested_shards = (hw_threads == 0)
                        ? 1
                        : std::min<std::size_t>(
                              hw_threads,
                              options.concurrency.mmap_parallel_max_shards);
                }
                if (requested_shards > 1 &&
                    source.mapped_size() >=
                        options.concurrency.mmap_parallel_min_bytes) {
                    return run_sync_pipeline_mmap_parallel<
                        alphabet_t, file_type>(
                        std::move(source),
                        requested_shards,
                        std::forward<handler_fn_t>(handler_fn),
                        std::forward<failure_fn_t>(failure_fn),
                        options);
                }
                return run_sync_pipeline_mmap_direct<alphabet_t, file_type>(
                    std::move(source),
                    std::forward<handler_fn_t>(handler_fn),
                    std::forward<failure_fn_t>(failure_fn),
                    options);
            }
        }
        return run_sync_pipeline<alphabet_t, file_type, source_t>(
            std::move(source),
            std::forward<handler_fn_t>(handler_fn),
            std::forward<failure_fn_t>(failure_fn),
            options);
    }
    return run_async_pipeline<alphabet_t, file_type, source_t, batch_t>(
        std::move(source),
        std::forward<handler_fn_t>(handler_fn),
        std::forward<failure_fn_t>(failure_fn),
        options);
}

}  // namespace detail

/// Public entry point. Reads FASTA / FASTQ from a file path, emits
/// per-batch records via `handler_fn`, and reports the first terminal
/// failure via `failure_fn`. Returns the count of records emitted on
/// success, or the diagnostic message on failure (as a `std::expected`).
/// Spec §3.5.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    typename PathLike,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string> read_sequences_file(
    PathLike&& file_path,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options = default_reader_options,
    read_backend* observed_backend = nullptr) {

    static_assert(std::convertible_to<PathLike, std::string_view>,
                  "file_path must be convertible to std::string_view");
    const std::string_view path_view = std::string_view{
        std::forward<PathLike>(file_path)};

    const auto record_backend = [&](read_backend backend) noexcept {
        if (observed_backend) *observed_backend = backend;
    };
    record_backend(read_backend::UNKNOWN);

    // Backend dispatch (spec §8.1):
    //   1. If `prefer_mmap` is set: open with magic-byte sniff.
    //      - Gzipped + parallel + size>=min → parallel_gzip_source.
    //      - Gzipped, fallback path → gzip_source.
    //      - Plain → existing mmap pipeline.
    //   2. mmap failed / large file → posix_read_source.
    //   3. Legacy `gzip_auto_detect=true` without prefer_mmap →
    //      gzip_source (covers the "user explicitly asked for gzip
    //      auto-detect on a non-mmap'd reader" path).
    if (options.io.prefer_mmap) {
        auto mmap_or_err =
            mmap_source::try_open_with_magic(path_view, options);
        if (mmap_or_err) {
            if (mmap_or_err->is_gzipped) {
                const bool size_ok = mmap_or_err->file_size >=
                    options.io.parallel_gzip_min_compressed_bytes;
                if (options.io.parallel_gzip_enabled && size_ok) {
                    auto pgz_or_err =
                        parallel_gzip_source::try_open_from_mmap(
                            std::move(mmap_or_err->source), options);
                    if (pgz_or_err) {
                        record_backend(read_backend::PARALLEL_GZIP);
                        return detail::run_pipeline_for_source<
                            alphabet_t, file_type, parallel_gzip_source>(
                            std::move(*pgz_or_err),
                            std::forward<handler_fn_t>(handler_fn),
                            std::forward<failure_fn_t>(failure_fn),
                            options);
                    }
                    JustLog("[bioseq_io] parallel_gzip_source failed for "
                            "'{}': {}; falling back to single-thread "
                            "gzip_source",
                            path_view, pgz_or_err.error());
                }
                // Construction-time fallback. Mid-stream errors are
                // TERMINAL — never restart from byte 0; that would
                // re-emit records.
                auto gz_or_err = gzip_source::try_open(path_view);
                if (!gz_or_err) {
                    failure_fn(std::string{gz_or_err.error()});
                    return std::unexpected(gz_or_err.error());
                }
                record_backend(read_backend::GZIP);
                return detail::run_pipeline_for_source<
                    alphabet_t, file_type, gzip_source>(
                    std::move(*gz_or_err),
                    std::forward<handler_fn_t>(handler_fn),
                    std::forward<failure_fn_t>(failure_fn),
                    options);
            }
            record_backend(read_backend::MMAP);
            return detail::run_pipeline_for_source<
                alphabet_t, file_type, mmap_source>(
                std::move(mmap_or_err->source),
                std::forward<handler_fn_t>(handler_fn),
                std::forward<failure_fn_t>(failure_fn),
                options);
        }
        JustLog("[bioseq_io] mmap_source unavailable for '{}': {}; "
                "falling back to posix_read_source",
                path_view, mmap_or_err.error());
    }
    if (options.io.gzip_auto_detect) {
        auto gzip_source_or_err = gzip_source::try_open(path_view);
        if (!gzip_source_or_err) {
            failure_fn(std::string{gzip_source_or_err.error()});
            return std::unexpected(gzip_source_or_err.error());
        }
        record_backend(read_backend::GZIP);
        return detail::run_pipeline_for_source<
            alphabet_t, file_type, gzip_source>(
            std::move(*gzip_source_or_err),
            std::forward<handler_fn_t>(handler_fn),
            std::forward<failure_fn_t>(failure_fn),
            options);
    }
    auto posix_source_or_err = posix_read_source::try_open(path_view);
    if (!posix_source_or_err) {
        failure_fn(std::string{posix_source_or_err.error()});
        return std::unexpected(posix_source_or_err.error());
    }
    record_backend(read_backend::POSIX_READ);
    return detail::run_pipeline_for_source<
        alphabet_t, file_type, posix_read_source>(
        std::move(*posix_source_or_err),
        std::forward<handler_fn_t>(handler_fn),
        std::forward<failure_fn_t>(failure_fn),
        options);
}

/// Plural-API convenience: read a sequence of files in declaration
/// order, accumulating record counts. The handler / failure
/// callbacks are shared across files. Spec §20.1.
///
/// v1 implementation is sequential — each file is fully drained
/// before the next is opened. The spec's parallel ingest model
/// (one producer per file, shared worker pool) is a follow-up
/// optimisation; the sequential shape is correct on its own and
/// matches typical "ingest a directory of FASTA files" use cases.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    typename PathLike,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string> read_sequences_files(
    std::span<const PathLike> file_paths,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options = default_reader_options) {
    std::size_t total_records = 0;
    for (const auto& file_path : file_paths) {
        auto outcome = read_sequences_file<alphabet_t, file_type, const PathLike&>(
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
