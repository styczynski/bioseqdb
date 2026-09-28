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

/// Input backend selected for a read. Benchmarks use this value to report
/// the source that ran after file checks and fallback decisions.
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

/// Borrows one record's packed words from a batch. `size()` counts words,
/// and `padding()` counts unused letter positions in the final word.
/// Iterators read the words directly. The batch storage must remain alive
/// and unchanged while the view is used.
template <supports_compact_sequence_range p_range> class compact_sequence_view {
  public:
    /// Traits of the packed sequence range represented by this view.
    using traits = typename compact_sequence_range<p_range>::traits;
    /// Packed storage word type borrowed from the owning batch.
    using storage_unit = typename traits::storage_unit;
    /// Read-only iterator over packed storage words.
    using iterator = const storage_unit*;
    /// Read-only iterator over packed storage words.
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

    /// Returns a pointer to the first packed word.
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

/// Packed sequence words passed to a streaming sink. The borrowed span is
/// valiw during `sink.chunk` only. Non-final chunks contain full words.
/// The final chunk may have  padding, given by the, difference between the
/// words' capacity in letters and `logical_length`.
template <typename p_storage_unit> struct packed_sequence_chunk {
    std::span<const p_storage_unit> words; // packed words aligned for their storage type
    std::uint64_t logical_length = 0;      // valid letters in this chunk
    bool end_of_record = false;
};

/// Owns record headers, packed sequences, and qualities in shared buffers.
/// Offset arrays locate each record without separate per-record allocations.
/// Accessors return borrowed views. Keep the batch storage alive and unchanged
/// while those views are in use.
template <supports_compact_sequence_range p_range> class sequence_batch {
  public:
    /// Owning compact range used to define the batch alphabet and packing.
    using sequence_range = compact_sequence_range<p_range>;
    /// Borrowed packed sequence returned for one record.
    using sequence_view = compact_sequence_view<p_range>;
    /// Packed word type used by the batch
    using storage_unit = typename sequence_view::storage_unit;

    /// Return the number of logical records in the batch.
    [[nodiscard]] std::size_t size() const noexcept {
        return _sequence_paddings.size();
    }

    /// Return whether the batch contains no logical records.
    [[nodiscard]] bool empty() const noexcept {
        return _sequence_paddings.empty();
    }

    /// Borrows the header of a record. The index  must be below `size()`.
    [[nodiscard]] std::string_view header(std::size_t index) const noexcept {
        const std::size_t start = _header_offsets[index];
        const std::size_t stop = _header_offsets[index + 1];
        return std::string_view{_header_storage.data() + start, stop - start};
    }

    /// Borrows a record's packed words and padding. The index must be below `size()`.
    [[nodiscard]] sequence_view sequence(std::size_t index) const noexcept {
        const std::size_t start = _sequence_offsets[index];
        const std::size_t stop = _sequence_offsets[index + 1];
        return sequence_view{
            _sequence_storage.data() + start, stop - start, _sequence_paddings[index]};
    }

    /// Borrows a record's quality bytes, or returns empty when qualities are absent.
    /// For a batch with qualities, the index must be below `size()`.
    [[nodiscard]] std::span<const std::uint8_t> quality(std::size_t index) const noexcept {
        if (_quality_offsets.empty())
            return {};
        const std::size_t start = _quality_offsets[index];
        const std::size_t stop = _quality_offsets[index + 1];
        return std::span<const std::uint8_t>{_quality_storage.data() + start, stop - start};
    }

    /// Returns the header buffer that parsers append to while building the batch.
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

    /// Starts a batch with empty header and sequence storage and zero first offsets.
    /// When `with_quality` is true, also resets the quality storage and offsets.
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

    /// Reserves estimated capacity before records are appended.
    /// Sequence capacity is measured  in packed words. Header and quality
    /// capacities are measured in bytes. The buffers can still wrow later.
    void reserve_storage(std::size_t header_bytes_hint,
                         std::size_t sequence_words_hint,
                         std::size_t quality_bytes_hint) {
        _header_storage.reserve(header_bytes_hint);
        _sequence_storage.reserve(sequence_words_hint);
        if (quality_bytes_hint > 0) {
            _quality_storage.reserve(quality_bytes_hint);
        }
    }

    /// Release every logical record while keeping each vector's allocation.
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
        _header_offsets; // Size N+1, with each adjacent pair delimiting one header.
    std::vector<storage_unit> _sequence_storage;
    std::vector<std::size_t> _sequence_offsets;  // size N+1
    std::vector<std::size_t> _sequence_paddings; // size N
    std::vector<std::uint8_t> _quality_storage;
    std::vector<std::size_t> _quality_offsets; // Size N+1 for FASTQ and zero for FASTA.
};

/// Handler contract for accepting a batch rvalue.
template <typename p_handler, typename p_batch>
concept sequence_batch_handler = std::invocable<p_handler, p_batch&&>;

} // namespace bioseq::io
