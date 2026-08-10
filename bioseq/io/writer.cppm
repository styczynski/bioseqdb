/**
 * @file writer.cppm
 * @brief Public entry point for the IO writer.
 *
 * Mirror of `reader.cppm` for the inverse direction. Holds the
 * user-visible types (`sequence_writer`, `write_sequences_file`) and
 * the lock-free per-thread chunk pipeline.
 *
 * **Threading model.** Each producer thread owns a `producer_state`
 * (chunk buffer + scratch) keyed by thread ID. `write_record` /
 * `write_batch` decode + format directly into the thread-local chunk
 * with no synchronisation. When a chunk crosses the flush threshold
 * the producer reserves a disjoint output range via
 * `posix_pwrite_sink::reserve_offset` (atomic fetch-add — lock-free)
 * and pwrites the chunk into it without contention. POSIX `pwrite(2)`
 * with disjoint offset ranges does not interleave at the page-cache
 * level, so the resulting file is well-defined regardless of issue
 * order.
 *
 * **Decoder pipeline.** The dna4 fast path pulls 32-letter packed
 * uint64 words and emits 32 ASCII bytes per word via the
 * `bioseq::simd::HWY_NAMESPACE::decode_dna4_words_to_ascii` Highway
 * kernel; the dna5+uint8 path takes the symmetric
 * `decode_dna5_uint8_words_to_ascii`; other shapes use the scalar
 * `decode_words_to_ascii_scalar` path.
 *
 * **Line wrapping.** FASTA bodies can be wrapped at a configurable
 * width (default 80 to match the bench corpus). The wrap is folded
 * into the decoder via the `_wrapped` variant: the SIMD path decodes
 * to a per-thread scratch then memcpys with newline injection; the
 * scalar path inlines the wrap directly.
 *
 * **Record ordering.** The writer does not preserve input record
 * order across producer threads (one thread's records land
 * contiguously inside its chunk, but two threads' chunks are ordered
 * by their `reserve_offset` issue order — i.e. roughly by who flushed
 * first). FASTA / FASTQ formats don't carry positional semantics so
 * this matches the parallel-reader contract. Single-threaded writers
 * preserve issue order trivially.
 */
module;

#include <hwy/highway.h>


#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.io.writer;

import bioseq.common.logging;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.io.options;
import bioseq.io.reader;
import bioseq.io.sinks;

export namespace bioseq::io {

/// Raw byte buffer for the writer's per-thread output chunk. Unlike
/// `std::vector<uint8_t>`, a `resize`/`extend` call does NOT
/// zero-initialise the new bytes — the writer's append paths
/// immediately overwrite them with decoded ASCII / qualities. With a
/// 16 MiB chunk this saves 16 MiB of memory traffic per flush cycle
/// (the `vector` would zero the buffer once on geometric grow then
/// zero-init each `resize` past the previous size, even within the
/// pre-reserved capacity).
struct writer_byte_buffer {
    std::unique_ptr<std::uint8_t[]> data;
    std::size_t size = 0;
    std::size_t capacity = 0;

    void reserve(std::size_t cap) {
        if (cap <= capacity) return;
        auto new_data = std::make_unique_for_overwrite<std::uint8_t[]>(cap);
        if (size > 0) {
            std::memcpy(new_data.get(), data.get(), size);
        }
        data = std::move(new_data);
        capacity = cap;
    }

    /// Reserve `bytes` bytes at the end and return a pointer to the
    /// first new byte. Bytes are uninitialised; caller MUST overwrite
    /// them before the next `extend` / `clear` / `pwrite` cycle.
    [[nodiscard]] std::uint8_t* extend(std::size_t bytes) {
        if (size + bytes > capacity) {
            const std::size_t needed = size + bytes;
            const std::size_t doubled = capacity * 2;
            reserve(doubled > needed ? doubled : needed);
        }
        std::uint8_t* result = data.get() + size;
        size += bytes;
        return result;
    }

    /// Append a single byte. Equivalent to `*extend(1) = b`.
    void push_back(std::uint8_t b) {
        if (size + 1 > capacity) {
            reserve(capacity > 0 ? capacity * 2 : 64);
        }
        data[size++] = b;
    }

    /// Append a contiguous byte range.
    void append(const std::uint8_t* src, std::size_t bytes) {
        std::memcpy(extend(bytes), src, bytes);
    }

    void clear() noexcept { size = 0; }

    [[nodiscard]] bool empty() const noexcept { return size == 0; }
};

/// Per-producer-thread state. Each thread fills its own `chunk` then
/// flushes via `pwrite` at a sink-reserved offset. The state is
/// allocated on first use per (writer, thread) and pinned for the
/// writer's lifetime; the writer's destructor drains every active
/// producer.
struct writer_producer_state {
    writer_byte_buffer chunk;          ///< current output bytes (not yet flushed)
    writer_byte_buffer decode_scratch; ///< intermediate decoded bytes for wrapped path
};

/// Thread-safe FASTA / FASTQ writer.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format>
class sequence_writer {
public:
    using alphabet_config =
        sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    using sequence_view = compact_sequence_view<alphabet_config>;
    using traits = typename compact_sequence_range<alphabet_config>::traits;
    static constexpr std::size_t bits_per_char = traits::bits_per_char;
    static constexpr std::size_t letters_per_unit = traits::letters_per_unit;
    using storage_unit = typename traits::storage_unit;
    using sink_variant = std::variant<posix_pwrite_sink, mmap_pwrite_sink>;

    /// Open `path` for writing. When `options.output_capacity_hint`
    /// is zero, uses the pwrite-based sink; otherwise pre-allocates
    /// the file to the hint and mmap-writes into it (faster on macOS
    /// because it skips the kernel pwrite memcpy).
    [[nodiscard]] static std::expected<sequence_writer, std::string>
    open(std::string_view path,
         const writer_options& options = default_writer_options) {
        sequence_writer writer;
        writer._options = options;
        if (options.output_capacity_hint == 0) {
            auto sink_or_err = posix_pwrite_sink::try_open(path);
            if (!sink_or_err) {
                return std::unexpected(std::move(sink_or_err.error()));
            }
            writer._sink = std::make_unique<sink_variant>(
                std::move(*sink_or_err));
        } else {
            auto sink_or_err = mmap_pwrite_sink::try_open(
                path, options.output_capacity_hint);
            if (!sink_or_err) {
                return std::unexpected(std::move(sink_or_err.error()));
            }
            writer._sink = std::make_unique<sink_variant>(
                std::move(*sink_or_err));
        }
        return writer;
    }

    sequence_writer() = default;
    sequence_writer(sequence_writer&& other) noexcept
        : _sink(std::move(other._sink)),
          _options(other._options),
          _producers_mutex(std::make_unique<std::mutex>()),
          _producers(std::move(other._producers)),
          _failed(other._failed.load(std::memory_order_relaxed)),
          _error_mutex(std::make_unique<std::mutex>()),
          _first_error_message(std::move(other._first_error_message)) {
        other._failed.store(false, std::memory_order_relaxed);
    }
    sequence_writer& operator=(sequence_writer&& other) noexcept {
        if (this != &other) {
            _sink = std::move(other._sink);
            _options = other._options;
            _producers = std::move(other._producers);
            _failed.store(
                other._failed.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            _first_error_message = std::move(other._first_error_message);
            other._failed.store(false, std::memory_order_relaxed);
        }
        return *this;
    }
    sequence_writer(const sequence_writer&) = delete;
    sequence_writer& operator=(const sequence_writer&) = delete;

    ~sequence_writer() {
        // Drain every active producer's residual chunk. close() may
        // have been called already; if so, _sink is null and this is
        // a no-op.
        if (_sink) {
            (void)close();
        }
    }

    /// Append one record to the output. Thread-safe: many producers
    /// may call concurrently. Returns false if a previous flush
    /// errored.
    [[nodiscard]] bool write_record(
        std::string_view header,
        const sequence_view& seq,
        std::span<const std::uint8_t> quality = {}) {
        if (_failed.load(std::memory_order_relaxed)) return false;

        auto& producer = acquire_producer_state();
        const std::size_t total_letters =
            seq.size() * letters_per_unit - seq.padding();

        if constexpr (p_file_format == sequence_file_format::fastq) {
            if (_options.strict_fastq_quality_length &&
                quality.size() != total_letters) {
                stamp_error(fmt::format(
                    "FASTQ quality length {} does not match sequence "
                    "length {} for header '{}'",
                    quality.size(), total_letters, header));
                return false;
            }
        }

        // Flush BEFORE starting a new record. This ensures every flushed
        // chunk ends at a record boundary — never mid-header, mid-body,
        // or between header and body. With multi-thread writers, chunks
        // from different producers interleave on disk via pwrite at
        // disjoint offsets; any non-record-aligned chunk would leave a
        // record split across threads' chunks, producing malformed
        // FASTQ on disk. Pre-record flushing keeps every produced byte
        // sequence valid.
        if (producer.chunk.size >= _options.flush_threshold_bytes) {
            if (!flush_producer(producer)) return false;
        }

        emit_header(producer.chunk, header);
        emit_body(producer, seq, total_letters);
        if constexpr (p_file_format == sequence_file_format::fastq) {
            // Append the FASTQ separator + qual + newline as one bulk
            // write per record. Layout: '\n' '+' '\n' <qual> '\n'.
            const std::size_t qsize = std::min(quality.size(), total_letters);
            const std::size_t pad_count = total_letters - qsize;
            const std::size_t total_qual_bytes = qsize + pad_count;
            // 3 sigils ('\n+\n') + qual bytes + trailing '\n'.
            std::uint8_t* dst = producer.chunk.extend(3 + total_qual_bytes + 1);
            dst[0] = static_cast<std::uint8_t>('\n');
            dst[1] = static_cast<std::uint8_t>('+');
            dst[2] = static_cast<std::uint8_t>('\n');
            std::memcpy(dst + 3, quality.data(), qsize);
            if (pad_count > 0) {
                std::memset(dst + 3 + qsize,
                            static_cast<int>('!'), pad_count);
            }
            dst[3 + total_qual_bytes] = static_cast<std::uint8_t>('\n');
        } else {
            producer.chunk.push_back(static_cast<std::uint8_t>('\n'));
        }
        return true;
    }

    /// Append one record while bounding peak memory to a
    /// single `max_body_bp`-letter slice. Unlike `write_record` (which
    /// decodes the whole body into one ASCII buffer — fine for ≤5 Mbp, but a
    /// 6 Gbp reference would materialise a >6 GB buffer), this emits the
    /// header once, then SIMD-decodes storage-unit-aligned slices of the view,
    /// carrying FASTA line-wrap state across slices, and flushes the producer
    /// between slices so RSS never exceeds ~one slice + the chunk capacity.
    /// It flushes mid-record, so — like the multi-thread invariant in
    /// `write_record` — it must be used single-producer for the record (the
    /// E2E bench's FASTA emission is single-threaded). Returns false on a
    /// prior/own flush error.
    [[nodiscard]] bool write_record_streamed(
        std::string_view header,
        const sequence_view& seq,
        std::size_t max_body_bp) {
        if (_failed.load(std::memory_order_relaxed)) return false;

        auto& producer = acquire_producer_state();
        const std::size_t total_letters =
            seq.size() * letters_per_unit - seq.padding();

        // Flush any pending bytes so the header begins a clean chunk (keeps
        // every flushed chunk record-aligned at the boundary).
        if (!producer.chunk.empty()) {
            if (!flush_producer(producer)) return false;
        }
        emit_header(producer.chunk, header);

        const std::size_t wrap_width =
            (p_file_format == sequence_file_format::fasta)
                ? _options.fasta_line_width
                : 0;
        const std::size_t units_per_slice =
            std::max<std::size_t>(std::size_t{1}, max_body_bp / letters_per_unit);
        const std::size_t total_words = seq.size();
        std::size_t line_position = 0;  // carried FASTA line-wrap state

        for (std::size_t word_cursor = 0; word_cursor < total_words;
             word_cursor += units_per_slice) {
            const std::size_t slice_words =
                std::min(units_per_slice, total_words - word_cursor);
            const bool is_final = (word_cursor + slice_words == total_words);
            const std::size_t slice_letters =
                slice_words * letters_per_unit - (is_final ? seq.padding() : 0);
            const storage_unit* words_ptr = seq.data() + word_cursor;

            if (wrap_width == 0) {
                std::uint8_t* dst = producer.chunk.extend(slice_letters);
                bioseq::simd::HWY_NAMESPACE::decode_words_to_ascii<
                    p_alphabet, storage_unit, bits_per_char, letters_per_unit>(
                        words_ptr, dst, slice_letters);
            } else {
                // Exact byte count the wrapped kernel writes for this slice:
                // slice_letters + one '\n' per completed line.
                const std::size_t newline_count =
                    (line_position + slice_letters) / wrap_width;
                producer.decode_scratch.clear();
                std::uint8_t* scratch =
                    producer.decode_scratch.extend(slice_letters);
                std::uint8_t* dst =
                    producer.chunk.extend(slice_letters + newline_count);
                line_position = bioseq::simd::HWY_NAMESPACE::
                    decode_words_to_ascii_wrapped<
                        p_alphabet,
                        storage_unit,
                        bits_per_char,
                        letters_per_unit>(
                            words_ptr, slice_letters, wrap_width, line_position,
                            scratch, dst);
            }
            // Flush this slice to disk before decoding the next — bounded RSS.
            if (!flush_producer(producer)) return false;
        }

        if constexpr (p_file_format == sequence_file_format::fastq) {
            // No quality is supplied on the streamed path; emit a Phred-0
            // ('!') quality of matching length in bounded chunks. (Quality is
            // dropped on bioseq.load anyway.) Layout: '\n' '+' '\n' <qual> '\n'.
            std::uint8_t* sep = producer.chunk.extend(3);
            sep[0] = static_cast<std::uint8_t>('\n');
            sep[1] = static_cast<std::uint8_t>('+');
            sep[2] = static_cast<std::uint8_t>('\n');
            std::size_t remaining = total_letters;
            const std::size_t qual_chunk =
                std::max<std::size_t>(std::size_t{1}, max_body_bp);
            while (remaining > 0) {
                const std::size_t take = std::min(qual_chunk, remaining);
                std::uint8_t* q = producer.chunk.extend(take);
                std::memset(q, static_cast<int>('!'), take);
                remaining -= take;
                if (!flush_producer(producer)) return false;
            }
            producer.chunk.push_back(static_cast<std::uint8_t>('\n'));
        } else {
            producer.chunk.push_back(static_cast<std::uint8_t>('\n'));
        }
        if (!flush_producer(producer)) return false;
        return true;
    }

    /// Append a whole batch in one call. Hot path for the bench: keeps
    /// the per-record metadata accesses tight and the decoder LUT in
    /// registers across many records.
    [[nodiscard]] bool write_batch(const batch_t& batch) {
        if (_failed.load(std::memory_order_relaxed)) return false;

        auto& producer = acquire_producer_state();
        const std::size_t record_count = batch.size();
        for (std::size_t record_index = 0; record_index < record_count;
             ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            const std::size_t total_letters =
                seq_view.size() * letters_per_unit - seq_view.padding();

            // Pre-record flush: drains the chunk if it crossed the
            // flush threshold during the previous record. Doing it
            // BEFORE writing this record's header means every flushed
            // chunk ends at a record boundary. The previous design
            // checked AFTER writing the record, then again INSIDE
            // emit_body; with multi-thread writers, the latter could
            // flush a chunk that ended just after the new record's
            // header (with its body still pending), leaving on-disk
            // chunks where one thread's @hdr line is followed by a
            // different thread's records — producing malformed FASTQ.
            if (producer.chunk.size >= _options.flush_threshold_bytes) {
                if (!flush_producer(producer)) return false;
            }

            if constexpr (p_file_format == sequence_file_format::fastq) {
                const auto quality_view = batch.quality(record_index);
                if (_options.strict_fastq_quality_length &&
                    quality_view.size() != total_letters) {
                    stamp_error(fmt::format(
                        "FASTQ quality length {} does not match "
                        "sequence length {} for header '{}'",
                        quality_view.size(), total_letters, header_view));
                    return false;
                }
                emit_header(producer.chunk, header_view);
                emit_body(producer, seq_view, total_letters);
                const std::size_t qsize = std::min(quality_view.size(),
                                                    total_letters);
                const std::size_t pad_count = total_letters - qsize;
                const std::size_t total_qual_bytes = qsize + pad_count;
                std::uint8_t* dst = producer.chunk.extend(
                    3 + total_qual_bytes + 1);
                dst[0] = static_cast<std::uint8_t>('\n');
                dst[1] = static_cast<std::uint8_t>('+');
                dst[2] = static_cast<std::uint8_t>('\n');
                std::memcpy(dst + 3, quality_view.data(), qsize);
                if (pad_count > 0) {
                    std::memset(dst + 3 + qsize,
                                static_cast<int>('!'), pad_count);
                }
                dst[3 + total_qual_bytes] = static_cast<std::uint8_t>('\n');
            } else {
                emit_header(producer.chunk, header_view);
                emit_body(producer, seq_view, total_letters);
                producer.chunk.push_back(static_cast<std::uint8_t>('\n'));
            }
        }
        return true;
    }

    /// Drain every active producer's residual chunk to the sink.
    /// MUST be called from a quiescent point — caller is responsible
    /// for ensuring no other thread is mid-`write_record` /
    /// `write_batch` when `flush()` runs (typically by joining
    /// producer threads first). The call locks `_producers_mutex`
    /// while iterating but does NOT serialise against producers
    /// already past the mutex inside an in-flight write.
    [[nodiscard]] bool flush() {
        if (_failed.load(std::memory_order_relaxed)) return false;
        std::lock_guard guard{*_producers_mutex};
        for (auto& entry : _producers) {
            auto& producer = *entry.second;
            if (!flush_producer(producer)) return false;
        }
        return true;
    }

    /// Flush + close the underlying file. Idempotent.
    [[nodiscard]] std::expected<void, std::string> close() {
        if (!_sink) return {};
        if (!flush()) {
            std::lock_guard guard{*_error_mutex};
            return std::unexpected(_first_error_message);
        }
        auto close_outcome = std::visit(
            [](auto& sink) { return sink.close(); }, *_sink);
        _sink.reset();
        return close_outcome;
    }

    /// Total bytes reserved through the sink so far. After all
    /// flushes complete this matches the on-disk file size.
    [[nodiscard]] std::uint64_t bytes_reserved() const noexcept {
        if (!_sink) return 0;
        return std::visit(
            [](const auto& sink) { return sink.reserved_byte_count(); },
            *_sink);
    }

private:
    /// Fetch (or lazily create) this thread's producer state. The
    /// producer state lives in the writer's `_producers` map; the
    /// only contention is on the map's mutex on each access (a single
    /// hash lookup — bounded cost).
    ///
    /// We deliberately do NOT use a thread_local pointer cache: the
    /// writer is templated, so across multiple writer instances of
    /// the same template the thread_local pointer would be shared and
    /// could outlive its owner (writers are constructed and destroyed
    /// in tight loops in the bench, often at recurring stack
    /// addresses). The mutex-guarded find is cheap relative to the
    /// per-batch decode work, and the lock is uncontended in
    /// steady-state because each thread reads only its own slot.
    [[nodiscard]] writer_producer_state& acquire_producer_state() {
        const std::thread::id thread_id = std::this_thread::get_id();
        std::lock_guard guard{*_producers_mutex};
        auto it = _producers.find(thread_id);
        if (it != _producers.end()) {
            return *it->second;
        }
        auto producer = std::make_unique<writer_producer_state>();
        producer->chunk.reserve(_options.chunk_capacity_bytes);
        producer->decode_scratch.reserve(_options.chunk_capacity_bytes);
        auto* raw = producer.get();
        _producers.emplace(thread_id, std::move(producer));
        return *raw;
    }

    /// Append the FASTA `>` / FASTQ `@` sigil + header + newline to
    /// `chunk`. Single bulk extend to amortise the size update.
    inline void emit_header(writer_byte_buffer& chunk,
                             std::string_view header) {
        constexpr std::uint8_t sigil =
            (p_file_format == sequence_file_format::fasta)
                ? static_cast<std::uint8_t>('>')
                : static_cast<std::uint8_t>('@');
        std::uint8_t* dst = chunk.extend(1 + header.size() + 1);
        dst[0] = sigil;
        std::memcpy(dst + 1, header.data(), header.size());
        dst[1 + header.size()] = static_cast<std::uint8_t>('\n');
    }

    /// Decode `total_letters` of `seq` into `producer.chunk` with
    /// optional FASTA line-wrapping. Single-shot decode — the whole
    /// record's body lands in the chunk in one call. The chunk's
    /// `extend` doubles capacity if needed, so very large records
    /// transiently grow the per-thread chunk to record_size + previous
    /// chunk content. Mid-record flushing was removed: with
    /// multi-thread writers, a flush issued mid-record would commit a
    /// chunk that ended without the record's trailing bytes (e.g.
    /// `\n+\n<qual>\n` for FASTQ), and another thread's chunk
    /// reserved at a later offset would land between the two halves on
    /// disk, splitting one record across non-contiguous file regions.
    /// The pre-record flush in `write_record`/`write_batch` ensures
    /// the chunk is empty (or contains only complete records) before
    /// each new record begins.
    inline void emit_body(writer_producer_state& producer,
                           const sequence_view& seq,
                           std::size_t total_letters) {
        if (total_letters == 0) return;
        const std::size_t wrap_width =
            (p_file_format == sequence_file_format::fasta)
                ? _options.fasta_line_width
                : 0;
        if (wrap_width == 0) {
            std::uint8_t* dst = producer.chunk.extend(total_letters);
            bioseq::simd::HWY_NAMESPACE::decode_words_to_ascii<
                p_alphabet, storage_unit, bits_per_char, letters_per_unit>(
                    seq.data(), dst, total_letters);
            return;
        }
        // Wrapped FASTA: decode into a scratch first, then memcpy
        // out in `wrap_width`-sized strides separated by `'\n'`.
        const std::size_t newline_count = (total_letters - 1) / wrap_width;
        const std::size_t output_size = total_letters + newline_count;
        producer.decode_scratch.clear();
        std::uint8_t* scratch = producer.decode_scratch.extend(total_letters);
        std::uint8_t* dst = producer.chunk.extend(output_size);
        (void)bioseq::simd::HWY_NAMESPACE::decode_words_to_ascii_wrapped<
            p_alphabet, storage_unit, bits_per_char, letters_per_unit>(
                seq.data(),
                total_letters,
                wrap_width,
                /*letters_in_current_line=*/0,
                scratch,
                dst);
    }

    /// Drain a producer's chunk to the sink at a sink-reserved
    /// offset. The producer's chunk is reset on success.
    [[nodiscard]] bool flush_producer(writer_producer_state& producer) {
        if (producer.chunk.empty()) return true;
        const std::size_t bytes_to_write = producer.chunk.size;
        const std::uint64_t base_offset = std::visit(
            [bytes_to_write](auto& sink) {
                return sink.reserve_offset(bytes_to_write);
            }, *_sink);
        const auto outcome = std::visit(
            [&producer, bytes_to_write, base_offset](auto& sink) {
                return sink.write_at(
                    producer.chunk.data.get(), bytes_to_write, base_offset);
            }, *_sink);
        if (outcome.has_error) {
            stamp_error(std::visit([](const auto& sink) {
                return sink.last_error_message();
            }, *_sink));
            return false;
        }
        producer.chunk.clear();
        return true;
    }

    /// First-error capture: subsequent failures are dropped to keep
    /// shutdown deterministic.
    void stamp_error(std::string&& msg) {
        std::lock_guard guard{*_error_mutex};
        if (!_failed.exchange(true, std::memory_order_acq_rel)) {
            _first_error_message = std::move(msg);
            bioseq_log<logging_level::error>("[bioseq_io.writer] write failed: {}", _first_error_message);
        }
    }
    void stamp_error(const std::string& msg) {
        std::lock_guard guard{*_error_mutex};
        if (!_failed.exchange(true, std::memory_order_acq_rel)) {
            _first_error_message = msg;
            bioseq_log<logging_level::error>("[bioseq_io.writer] write failed: {}", _first_error_message);
        }
    }

    std::unique_ptr<sink_variant> _sink;
    writer_options _options{};
    std::unique_ptr<std::mutex> _producers_mutex =
        std::make_unique<std::mutex>();
    std::unordered_map<std::thread::id,
                        std::unique_ptr<writer_producer_state>> _producers;
    std::atomic<bool> _failed{false};
    std::unique_ptr<std::mutex> _error_mutex =
        std::make_unique<std::mutex>();
    std::string _first_error_message;
};

/// One-shot convenience: open a writer, push every batch from
/// `batches` into it, close. Returns the count of records written or
/// the first error.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          typename p_path,
          typename p_batch_range>
[[nodiscard]] std::expected<std::size_t, std::string> write_sequences_file(
    p_path&& path,
    p_batch_range&& batches,
    const writer_options& options = default_writer_options) {
    static_assert(std::convertible_to<p_path, std::string_view>,
                  "path must be convertible to std::string_view");
    const std::string_view path_view = std::string_view{
        std::forward<p_path>(path)};

    auto writer_or_err = sequence_writer<p_alphabet, p_file_format>::open(
        path_view, options);
    if (!writer_or_err) {
        return std::unexpected(std::move(writer_or_err.error()));
    }
    auto& writer = *writer_or_err;

    std::size_t records_written = 0;
    for (const auto& batch : batches) {
        if (!writer.write_batch(batch)) {
            auto close_result = writer.close();
            if (!close_result) {
                return std::unexpected(std::move(close_result.error()));
            }
            return std::unexpected(
                std::string{"write_batch failed (see writer log)"});
        }
        records_written += batch.size();
    }
    auto close_result = writer.close();
    if (!close_result) {
        return std::unexpected(std::move(close_result.error()));
    }
    return records_written;
}

}  // namespace bioseq::io
