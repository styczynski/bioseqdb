/**
 * @file writer.cppm
 * @brief Concurrent FASTA and FASTQ writer.
 *
 * Each producer formats records in private reusable storage and reserves a
 * disjoint output range when flushing. Record order is preserved within one
 * producer but not across concurrent producers.
 */
module;

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <hwy/highway.h>
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

/// Growable output bytes for one producer. New capacity is uninitialized
/// because the writer fills it immediately with sequence or quality text.
struct writer_byte_buffer {
    std::unique_ptr<std::uint8_t[]> data;
    std::size_t size = 0;
    std::size_t capacity = 0;

    /// Ensure capacity for `cap` bytes while preserving the live prefix.
    void reserve(std::size_t cap) {
        if (cap <= capacity)
            return;
        auto new_data = std::make_unique_for_overwrite<std::uint8_t[]>(cap);
        if (size > 0) {
            std::memcpy(new_data.get(), data.get(), size);
        }
        data = std::move(new_data);
        capacity = cap;
    }

    /// Appends uninitialized space and returns its first byte. Fill the space
    /// before flushing. A later buffer growth invalidates this pointer.
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

    /// Discard the live bytes without releasing the allocation.
    void clear() noexcept {
        size = 0;
    }

    /// Return whether the buffer contains no live bytes.
    [[nodiscard]] bool empty() const noexcept {
        return size == 0;
    }
};

/// Output and decode buffers owned by one producer thread. The writer creates
/// this state on first use and keeps it until the writer is destroyed.
/// Each flush writes the output buffer to a newly reserved file range.
struct writer_producer_state {
    writer_byte_buffer chunk;          ///< current output bytes (not yet flushed)
    writer_byte_buffer decode_scratch; ///< intermediate decoded bytes for wrapped path
};

/// FASTA/FASTQ writer with per-thread buffers for concurrent record and batch writes.
template <sequence_alphabet p_alphabet, sequence_file_format p_file_format> class sequence_writer {
  public:
    /// Packed alphabet configuration accepted by this writer.
    using alphabet_config = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    /// Batch type consumed by the concurrent append path.
    using batch_t = sequence_batch<alphabet_config>;
    /// Borrowed view of one packed sequence in a batch.
    using sequence_view = compact_sequence_view<alphabet_config>;
    /// Packing traits shared with the sequence view.
    using traits = typename compact_sequence_range<alphabet_config>::traits;
    /// Number of bits used for one encoded letter.
    static constexpr std::size_t bits_per_char = traits::bits_per_char;
    /// Number of encoded letters stored in one packed word.
    static constexpr std::size_t letters_per_unit = traits::letters_per_unit;
    /// Packed storage word type decoded by this writer.
    using storage_unit = typename traits::storage_unit;
    /// Storage backend selected when the output is opened.
    using sink_variant = std::variant<posix_pwrite_sink, mmap_pwrite_sink>;

    /// Open `path` for writing. When `options.output_capacity_hint`
    /// is zero it uses the pwrite-based sink, otherwise it pre-allocates
    /// the file to the hint and writes through a shared mapping.
    [[nodiscard]] static std::expected<sequence_writer, std::string>
    open(std::string_view path, const writer_options& options = default_writer_options) {
        sequence_writer writer;
        writer._options = options;
        if (options.output_capacity_hint == 0) {
            auto sink_or_err = posix_pwrite_sink::try_open(path);
            if (!sink_or_err) {
                return std::unexpected(std::move(sink_or_err.error()));
            }
            writer._sink = std::make_unique<sink_variant>(std::move(*sink_or_err));
        } else {
            auto sink_or_err = mmap_pwrite_sink::try_open(path, options.output_capacity_hint);
            if (!sink_or_err) {
                return std::unexpected(std::move(sink_or_err.error()));
            }
            writer._sink = std::make_unique<sink_variant>(std::move(*sink_or_err));
        }
        return writer;
    }

    /// Construct a closed writer without a sink.
    sequence_writer() = default;

    /// Transfer the sink, producer buffers, options, and diagnostic state.
    sequence_writer(sequence_writer&& other) noexcept
        : _sink(std::move(other._sink)), _options(other._options),
          _producers_mutex(std::make_unique<std::mutex>()), _producers(std::move(other._producers)),
          _failed(other._failed.load(std::memory_order_relaxed)),
          _error_mutex(std::make_unique<std::mutex>()),
          _first_error_message(std::move(other._first_error_message)) {
        other._failed.store(false, std::memory_order_relaxed);
    }
    /// Replace this writer with transferred sink and producer state.
    sequence_writer& operator=(sequence_writer&& other) noexcept {
        if (this != &other) {
            _sink = std::move(other._sink);
            _options = other._options;
            _producers = std::move(other._producers);
            _failed.store(other._failed.load(std::memory_order_relaxed), std::memory_order_relaxed);
            _first_error_message = std::move(other._first_error_message);
            other._failed.store(false, std::memory_order_relaxed);
        }
        return *this;
    }
    /// Writers exclusively own their sink and producer buffers.
    sequence_writer(const sequence_writer&) = delete;

    /// Writers cannot copy sink or producer ownership.
    sequence_writer& operator=(const sequence_writer&) = delete;

    /// Attempts to flush pending output before releasing the sink.
    ~sequence_writer() {
        // An explicitly closed writer has no sink left to drain.
        if (_sink) {
            (void)close();
        }
    }

    /// Appends a complete record. Concurrent producers keep their records intact.
    /// Returns false for a saved write error, a quality-length error, or a
    /// failed flush. The writer must be open.
    [[nodiscard]] bool write_record(std::string_view header,
                                    const sequence_view& seq,
                                    std::span<const std::uint8_t> quality = {}) {
        if (_failed.load(std::memory_order_relaxed))
            return false;

        auto& producer = acquire_producer_state();
        const std::size_t total_letters = seq.size() * letters_per_unit - seq.padding();

        if constexpr (p_file_format == sequence_file_format::fastq) {
            if (_options.strict_fastq_quality_length && quality.size() != total_letters) {
                stamp_error(fmt::format("FASTQ quality length {} does not match sequence "
                                        "length {} for header '{}'",
                                        quality.size(),
                                        total_letters,
                                        header));
                return false;
            }
        }

        // Flush only between records. A flush in the middle of this record could
        // let another producer write between its header and trailing bytes.
        if (producer.chunk.size >= _options.flush_threshold_bytes) {
            if (!flush_producer(producer))
                return false;
        }

        emit_header(producer.chunk, header);
        emit_body(producer, seq, total_letters);
        if constexpr (p_file_format == sequence_file_format::fastq) {
            // Append the FASTQ separator + qual + newline as one bulk
            // write per record. Layout: '\n' '+' '\n' <qual> '\n'.
            const std::size_t qsize = std::min(quality.size(), total_letters);
            const std::size_t pad_count = total_letters - qsize;
            const std::size_t total_qual_bytes = qsize + pad_count;
            // Separator bytes, quality bytes, and the final newline.
            std::uint8_t* dst = producer.chunk.extend(3 + total_qual_bytes + 1);
            dst[0] = static_cast<std::uint8_t>('\n');
            dst[1] = static_cast<std::uint8_t>('+');
            dst[2] = static_cast<std::uint8_t>('\n');
            std::memcpy(dst + 3, quality.data(), qsize);
            if (pad_count > 0) {
                std::memset(dst + 3 + qsize, static_cast<int>('!'), pad_count);
            }
            dst[3 + total_qual_bytes] = static_cast<std::uint8_t>('\n');
        } else {
            producer.chunk.push_back(static_cast<std::uint8_t>('\n'));
        }
        return true;
    }

    /// Writes a large record in word-aligned slices, flushing after each slice.
    /// Decodes one slice at a time and reuses this thread's output buffers. FASTA
    /// wrapping continues across slices. FASTQ uses '!' for every quality byte.
    /// Returns false after a write error.
    ///
    /// No other producer may write through this writer until the call returns,
    /// because the record spans several separately reserved file ranges.
    [[nodiscard]] bool write_record_streamed(std::string_view header,
                                             const sequence_view& seq,
                                             std::size_t max_body_bp) {
        if (_failed.load(std::memory_order_relaxed))
            return false;

        auto& producer = acquire_producer_state();
        const std::size_t total_letters = seq.size() * letters_per_unit - seq.padding();

        // Flush earlier records before beginning this streamed record.
        if (!producer.chunk.empty()) {
            if (!flush_producer(producer))
                return false;
        }
        emit_header(producer.chunk, header);

        const std::size_t wrap_width =
            (p_file_format == sequence_file_format::fasta) ? _options.fasta_line_width : 0;
        const std::size_t units_per_slice =
            std::max<std::size_t>(std::size_t{1}, max_body_bp / letters_per_unit);
        const std::size_t total_words = seq.size();
        std::size_t line_position = 0; // carried FASTA line-wrap state

        for (std::size_t word_cursor = 0; word_cursor < total_words;
             word_cursor += units_per_slice) {
            const std::size_t slice_words = std::min(units_per_slice, total_words - word_cursor);
            const bool is_final = (word_cursor + slice_words == total_words);
            const std::size_t slice_letters =
                slice_words * letters_per_unit - (is_final ? seq.padding() : 0);
            const storage_unit* words_ptr = seq.data() + word_cursor;

            if (wrap_width == 0) {
                std::uint8_t* dst = producer.chunk.extend(slice_letters);
                bioseq::simd::HWY_NAMESPACE::decode_words_to_ascii<p_alphabet,
                                                                   storage_unit,
                                                                   bits_per_char,
                                                                   letters_per_unit>(
                    words_ptr, dst, slice_letters);
            } else {
                // Exact byte count the wrapped kernel writes for this slice:
                // slice_letters + one '\n' per completed line.
                const std::size_t newline_count = (line_position + slice_letters) / wrap_width;
                producer.decode_scratch.clear();
                std::uint8_t* scratch = producer.decode_scratch.extend(slice_letters);
                std::uint8_t* dst = producer.chunk.extend(slice_letters + newline_count);
                line_position =
                    bioseq::simd::HWY_NAMESPACE::decode_words_to_ascii_wrapped<p_alphabet,
                                                                               storage_unit,
                                                                               bits_per_char,
                                                                               letters_per_unit>(
                        words_ptr, slice_letters, wrap_width, line_position, scratch, dst);
            }
            // Write this slice before decoding the next one.
            if (!flush_producer(producer))
                return false;
        }

        if constexpr (p_file_format == sequence_file_format::fastq) {
            // This interface has no quality input. Write one '!' per sequence
            // position in chunks, followed by a newline.
            std::uint8_t* sep = producer.chunk.extend(3);
            sep[0] = static_cast<std::uint8_t>('\n');
            sep[1] = static_cast<std::uint8_t>('+');
            sep[2] = static_cast<std::uint8_t>('\n');
            std::size_t remaining = total_letters;
            const std::size_t qual_chunk = std::max<std::size_t>(std::size_t{1}, max_body_bp);
            while (remaining > 0) {
                const std::size_t take = std::min(qual_chunk, remaining);
                std::uint8_t* q = producer.chunk.extend(take);
                std::memset(q, static_cast<int>('!'), take);
                remaining -= take;
                if (!flush_producer(producer))
                    return false;
            }
            producer.chunk.push_back(static_cast<std::uint8_t>('\n'));
        } else {
            producer.chunk.push_back(static_cast<std::uint8_t>('\n'));
        }
        if (!flush_producer(producer))
            return false;
        return true;
    }

    /// Writes every record in a batch using this thread's reusable output buffers.
    [[nodiscard]] bool write_batch(const batch_t& batch) {
        if (_failed.load(std::memory_order_relaxed))
            return false;

        auto& producer = acquire_producer_state();
        const std::size_t record_count = batch.size();
        for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            const std::size_t total_letters =
                seq_view.size() * letters_per_unit - seq_view.padding();

            // Flush before the next header so each chunk ends at a record
            // boundary. Flushing from body emission could separate one
            // producer's header from its body.
            if (producer.chunk.size >= _options.flush_threshold_bytes) {
                if (!flush_producer(producer))
                    return false;
            }

            if constexpr (p_file_format == sequence_file_format::fastq) {
                const auto quality_view = batch.quality(record_index);
                if (_options.strict_fastq_quality_length && quality_view.size() != total_letters) {
                    stamp_error(fmt::format("FASTQ quality length {} does not match "
                                            "sequence length {} for header '{}'",
                                            quality_view.size(),
                                            total_letters,
                                            header_view));
                    return false;
                }
                emit_header(producer.chunk, header_view);
                emit_body(producer, seq_view, total_letters);
                const std::size_t qsize = std::min(quality_view.size(), total_letters);
                const std::size_t pad_count = total_letters - qsize;
                const std::size_t total_qual_bytes = qsize + pad_count;
                std::uint8_t* dst = producer.chunk.extend(3 + total_qual_bytes + 1);
                dst[0] = static_cast<std::uint8_t>('\n');
                dst[1] = static_cast<std::uint8_t>('+');
                dst[2] = static_cast<std::uint8_t>('\n');
                std::memcpy(dst + 3, quality_view.data(), qsize);
                if (pad_count > 0) {
                    std::memset(dst + 3 + qsize, static_cast<int>('!'), pad_count);
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

    /// Flushes pending output from every producer. Call after all producer writes
    /// have stopped, such as after joining their threads. The map mutex protects
    /// the producer list, while each write continues outside that mutex.
    [[nodiscard]] bool flush() {
        if (_failed.load(std::memory_order_relaxed))
            return false;
        std::lock_guard guard{*_producers_mutex};
        for (auto& entry : _producers) {
            auto& producer = *entry.second;
            if (!flush_producer(producer))
                return false;
        }
        return true;
    }

    /// Flushes pending output and closes the file. Producers must have stopped.
    /// A flush error is returned with the sink still open. Closing an already
    /// closed writer succeeds.
    [[nodiscard]] std::expected<void, std::string> close() {
        if (!_sink)
            return {};
        if (!flush()) {
            std::lock_guard guard{*_error_mutex};
            return std::unexpected(_first_error_message);
        }
        auto close_outcome = std::visit([](auto& sink) { return sink.close(); }, *_sink);
        _sink.reset();
        return close_outcome;
    }

    /// Returns the sink's reserved byte count, or zero after the writer closes.
    [[nodiscard]] std::uint64_t bytes_reserved() const noexcept {
        if (!_sink)
            return 0;
        return std::visit([](const auto& sink) { return sink.reserved_byte_count(); }, *_sink);
    }

  private:
    /// Finds this thread's buffers, creating them on first use.
    /// The writer owns them in its map, so each writer instance has separate
    /// producer state with the same lifetime as its owner.
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

    /// Appends the format's header marker, header text, and newline.
    inline void emit_header(writer_byte_buffer& chunk, std::string_view header) {
        constexpr std::uint8_t sigil = (p_file_format == sequence_file_format::fasta)
                                           ? static_cast<std::uint8_t>('>')
                                           : static_cast<std::uint8_t>('@');
        std::uint8_t* dst = chunk.extend(1 + header.size() + 1);
        dst[0] = sigil;
        std::memcpy(dst + 1, header.data(), header.size());
        dst[1 + header.size()] = static_cast<std::uint8_t>('\n');
    }

    /// Decodes the entire sequence body into the producer's output buffer.
    /// FASTA line wrapping uses a temporary ASCII buffer. Capacity grows for
    /// large records, and flushing stays with the caller so a concurrent
    /// producer cannot split this record's output.
    inline void emit_body(writer_producer_state& producer,
                          const sequence_view& seq,
                          std::size_t total_letters) {
        if (total_letters == 0)
            return;
        const std::size_t wrap_width =
            (p_file_format == sequence_file_format::fasta) ? _options.fasta_line_width : 0;
        if (wrap_width == 0) {
            std::uint8_t* dst = producer.chunk.extend(total_letters);
            bioseq::simd::HWY_NAMESPACE::
                decode_words_to_ascii<p_alphabet, storage_unit, bits_per_char, letters_per_unit>(
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
        (void)bioseq::simd::HWY_NAMESPACE::decode_words_to_ascii_wrapped<p_alphabet,
                                                                         storage_unit,
                                                                         bits_per_char,
                                                                         letters_per_unit>(
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
        if (producer.chunk.empty())
            return true;
        const std::size_t bytes_to_write = producer.chunk.size;
        const std::uint64_t base_offset = std::visit(
            [bytes_to_write](auto& sink) { return sink.reserve_offset(bytes_to_write); }, *_sink);
        const auto outcome = std::visit(
            [&producer, bytes_to_write, base_offset](auto& sink) {
                return sink.write_at(producer.chunk.data.get(), bytes_to_write, base_offset);
            },
            *_sink);
        if (outcome.has_error) {
            stamp_error(
                std::visit([](const auto& sink) { return sink.last_error_message(); }, *_sink));
            return false;
        }
        producer.chunk.clear();
        return true;
    }

    /// Saves and logs the first error. Later failures leave its message unchanged.
    void stamp_error(std::string&& msg) {
        std::lock_guard guard{*_error_mutex};
        if (!_failed.exchange(true, std::memory_order_acq_rel)) {
            _first_error_message = std::move(msg);
            bioseq_log<logging_level::error>("[bioseq_io.writer] write failed: {}",
                                             _first_error_message);
        }
    }

    /// Copy the first terminal sink diagnostic and discard later failures.
    void stamp_error(const std::string& msg) {
        std::lock_guard guard{*_error_mutex};
        if (!_failed.exchange(true, std::memory_order_acq_rel)) {
            _first_error_message = msg;
            bioseq_log<logging_level::error>("[bioseq_io.writer] write failed: {}",
                                             _first_error_message);
        }
    }

    std::unique_ptr<sink_variant> _sink;
    writer_options _options{};
    std::unique_ptr<std::mutex> _producers_mutex = std::make_unique<std::mutex>();
    std::unordered_map<std::thread::id, std::unique_ptr<writer_producer_state>> _producers;
    std::atomic<bool> _failed{false};
    std::unique_ptr<std::mutex> _error_mutex = std::make_unique<std::mutex>();
    std::string _first_error_message;
};

/// Open a writer, write every batch, close it, and return the record count.
/// Returns the first error without replacing its diagnostic.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          typename p_path,
          typename p_batch_range>
[[nodiscard]] std::expected<std::size_t, std::string>
write_sequences_file(p_path&& path,
                     p_batch_range&& batches,
                     const writer_options& options = default_writer_options) {
    static_assert(std::convertible_to<p_path, std::string_view>,
                  "path must be convertible to std::string_view");
    const std::string_view path_view = std::string_view{std::forward<p_path>(path)};

    auto writer_or_err = sequence_writer<p_alphabet, p_file_format>::open(path_view, options);
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
            return std::unexpected(std::string{"write_batch failed (see writer log)"});
        }
        records_written += batch.size();
    }
    auto close_result = writer.close();
    if (!close_result) {
        return std::unexpected(std::move(close_result.error()));
    }
    return records_written;
}

} // namespace bioseq::io
