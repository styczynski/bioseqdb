/**
 * @file types.cppm
 * @brief Sequence reader views, batches, and handler contracts.
 */
module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

export module bioseq.io.reader:types;

import bioseq.sequences;

export namespace bioseq::io {

/// Which I/O backend the dispatcher in `read_sequences_file` actually
/// chose for a given call. The dispatcher's branch tree depends on
/// magic-byte sniff + the runtime options + size thresholds, so the
/// backend that really ran can differ from what the caller "asked
/// for" via `reader_options`. Bench harnesses use this so they can
/// label samples with the path that actually executed instead of the
/// runner's nominal source kind.
enum class reader_backend : std::uint8_t {
    unknown = 0,   ///< No backend was selected.
    memory_mapped, ///< Plain `mmap_source`.
    posix_read,    ///< `posix_read_source` (mmap unavailable or disabled).
    gzip,          ///< Single-threaded `gzip_source`.
    parallel_gzip, ///< Parallel `parallel_gzip_source`.
};

/// Return the stable benchmark label for a selected reader backend.
[[nodiscard]] constexpr const char* reader_backend_name(reader_backend backend) noexcept {
    switch (backend) {
    case reader_backend::unknown:
        return "unknown";
    case reader_backend::memory_mapped:
        return "mmap";
    case reader_backend::posix_read:
        return "posix_read";
    case reader_backend::gzip:
        return "gzip";
    case reader_backend::parallel_gzip:
        return "parallel_gzip";
    }
    return "?";
}

/// View of one record's packed sequence inside a batch's shared
/// `_sequence_storage`. Models the same surface used by
/// `compact_sequence_range<p_range>`: `size()` (storage-unit count),
/// `padding()` (count of trailing dummy letters), and a const
/// random-access iterator pair so range-based `for` over storage
/// units works unchanged. Templated on the same custom alphabet range as
/// `compact_sequence_range<p_range>` so callers can use the same
/// `traits` typedef.
///
/// The view is non-owning — its lifetime is bound to the batch that
/// produced it. Once the batch is destroyed the view is dangling.
/// In practice the handler always processes the batch in scope.
template <supports_compact_sequence_range p_range> class compact_sequence_view {
  public:
    using traits = typename compact_sequence_range<p_range>::traits;
    using storage_unit = typename traits::storage_unit;
    using iterator = const storage_unit*;
    using const_iterator = const storage_unit*;

    /// Construct an empty view.
    constexpr compact_sequence_view() = default;

    /// Construct a view over `word_count` packed units with trailing padding.
    constexpr compact_sequence_view(const storage_unit* base,
                                    std::size_t word_count,
                                    std::size_t padding) noexcept
        : _base(base), _word_count(word_count), _padding(padding) {}

    /// Return the number of packed storage units.
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return _word_count;
    }

    /// Return the number of unused letters in the final storage unit.
    [[nodiscard]] constexpr std::size_t padding() const noexcept {
        return _padding;
    }

    /// Return the first packed storage unit.
    [[nodiscard]] constexpr const storage_unit* data() const noexcept {
        return _base;
    }

    /// Return an iterator to the first packed storage unit.
    [[nodiscard]] constexpr iterator begin() const noexcept {
        return _base;
    }

    /// Return an iterator one past the final packed storage unit.
    [[nodiscard]] constexpr iterator end() const noexcept {
        return _base + _word_count;
    }

    /// Return a const iterator to the first packed storage unit.
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept {
        return begin();
    }

    /// Return a const iterator one past the final packed storage unit.
    [[nodiscard]] constexpr const_iterator cend() const noexcept {
        return end();
    }

    /// Return one packed storage unit without bounds checking.
    [[nodiscard]] constexpr storage_unit operator[](std::size_t index) const noexcept {
        return _base[index];
    }

  private:
    const storage_unit* _base = nullptr;
    std::size_t _word_count = 0;
    std::size_t _padding = 0;
};

/// One bounded, column-unit-packed chunk emitted by
/// `stream_sequences_file`. `words` is a non-owning view of naturally-aligned
/// packed storage units valid only for the duration of the `sink.chunk(...)`
/// call (the consumer must copy out before returning). For all non-final
/// chunks of a record `logical_length == words.size() * letters_per_unit`. The final
/// chunk (`end_of_record == true`) may carry fewer valid bases, with the
/// trailing padding implied by `logical_length`
/// (i.e. `words.size()*letters_per_unit - logical_length`). This is the surface that
/// lets a 6 Gbp FASTA record be loaded
/// without ever materialising a whole-record `sequence_batch`.
template <typename p_storage_unit> struct packed_sequence_chunk {
    std::span<const p_storage_unit> words; // column-unit-packed, naturally aligned
    std::uint64_t logical_length = 0;      // valid bases in this chunk
    bool end_of_record = false;
};

/// A batch of records delivered to the user's handler. It holds three
/// shared buffers (`_header_storage`, `_sequence_storage`,
/// `_quality_storage`) and per-record offset / metadata arrays. This
/// eliminates per-record `std::string` + `std::vector<uint64_t>`
/// allocations that dominated short-read throughput.
///
/// The accessors `header(i)` / `sequence(i)` / `quality(i)` return
/// non-owning views (`std::string_view`, `compact_sequence_view<p_range>`,
/// `std::span<const std::uint8_t>`) into the batch's shared storage.
/// Returned views remain valid as long as the batch.
template <supports_compact_sequence_range p_range> class sequence_batch {
  public:
    using sequence_range = compact_sequence_range<p_range>;
    using sequence_view = compact_sequence_view<p_range>;
    using storage_unit = typename sequence_view::storage_unit;

    /// Return the number of logical records in the batch.
    [[nodiscard]] std::size_t size() const noexcept {
        return _sequence_paddings.size();
    }

    /// Return whether the batch contains no logical records.
    [[nodiscard]] bool empty() const noexcept {
        return _sequence_paddings.empty();
    }

    /// Return the borrowed header bytes for one record.
    [[nodiscard]] std::string_view header(std::size_t index) const noexcept {
        const std::size_t start = _header_offsets[index];
        const std::size_t stop = _header_offsets[index + 1];
        return std::string_view{_header_storage.data() + start, stop - start};
    }

    /// Return the borrowed packed sequence for one record.
    [[nodiscard]] sequence_view sequence(std::size_t index) const noexcept {
        const std::size_t start = _sequence_offsets[index];
        const std::size_t stop = _sequence_offsets[index + 1];
        return sequence_view{
            _sequence_storage.data() + start, stop - start, _sequence_paddings[index]};
    }

    /// Return the borrowed quality bytes for one record, or an empty span.
    [[nodiscard]] std::span<const std::uint8_t> quality(std::size_t index) const noexcept {
        if (_quality_offsets.empty())
            return {};
        const std::size_t start = _quality_offsets[index];
        const std::size_t stop = _quality_offsets[index + 1];
        return std::span<const std::uint8_t>{_quality_storage.data() + start, stop - start};
    }

    /// Mutable accessors used by the FSM emit-record callbacks. The
    /// FSM appends through raw pointers. The batch is only
    /// observed through the public `header(i)` / `sequence(i)` /
    /// `quality(i)` views once handed to the user handler.
    /// Return mutable header storage for the parser callback.
    [[nodiscard]] std::vector<char>& header_storage() noexcept {
        return _header_storage;
    }

    /// Return mutable header offsets for the parser callback.
    [[nodiscard]] std::vector<std::size_t>& header_offsets() noexcept {
        return _header_offsets;
    }

    /// Return mutable packed-sequence storage for the parser callback.
    [[nodiscard]] std::vector<storage_unit>& sequence_storage() noexcept {
        return _sequence_storage;
    }

    /// Return mutable packed-sequence offsets for the parser callback.
    [[nodiscard]] std::vector<std::size_t>& sequence_offsets() noexcept {
        return _sequence_offsets;
    }

    /// Return mutable per-record padding counts for the parser callback.
    [[nodiscard]] std::vector<std::size_t>& sequence_paddings() noexcept {
        return _sequence_paddings;
    }

    /// Return mutable quality storage for the parser callback.
    [[nodiscard]] std::vector<std::uint8_t>& quality_storage() noexcept {
        return _quality_storage;
    }

    /// Return mutable quality offsets for the parser callback.
    [[nodiscard]] std::vector<std::size_t>& quality_offsets() noexcept {
        return _quality_offsets;
    }

    /// Initialise the offset prefix arrays so the first record's
    /// offset is 0. Called once per batch by the FSM driver.
    void prepare(std::size_t expected_record_count, bool with_quality) {
        _sequence_paddings.clear();
        _sequence_paddings.reserve(expected_record_count);
        _sequence_offsets.clear();
        _sequence_offsets.reserve(expected_record_count + 1);
        _sequence_offsets.push_back(0);
        _header_offsets.clear();
        _header_offsets.reserve(expected_record_count + 1);
        _header_offsets.push_back(0);
        _sequence_storage.clear();
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
    /// caller passes a rough byte estimate. The batch grows
    /// geometrically beyond that if the corpus exceeds the hint.
    void reserve_storage(std::size_t header_bytes_hint,
                         std::size_t sequence_words_hint,
                         std::size_t quality_bytes_hint) {
        _header_storage.reserve(header_bytes_hint);
        _sequence_storage.reserve(sequence_words_hint);
        if (quality_bytes_hint > 0) {
            _quality_storage.reserve(quality_bytes_hint);
        }
    }

    /// Release every logical record while retaining each vector's allocation.
    void clear() noexcept {
        _header_storage.clear();
        _header_offsets.clear();
        _sequence_storage.clear();
        _sequence_offsets.clear();
        _sequence_paddings.clear();
        _quality_storage.clear();
        _quality_offsets.clear();
    }

  private:
    std::vector<char> _header_storage;
    std::vector<std::size_t>
        _header_offsets; // size N+1; header[i] = [_header_offsets[i], _header_offsets[i+1])
    std::vector<storage_unit> _sequence_storage;
    std::vector<std::size_t> _sequence_offsets;  // size N+1
    std::vector<std::size_t> _sequence_paddings; // size N
    std::vector<std::uint8_t> _quality_storage;
    std::vector<std::size_t> _quality_offsets; // size N+1 if FASTQ; size 0 if FASTA
};

/// Handler contract for accepting a batch rvalue.
template <typename p_handler, typename p_batch>
concept sequence_batch_handler = std::invocable<p_handler, p_batch&&>;

} // namespace bioseq::io
