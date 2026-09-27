/**
 * @file bit_reader.cppm
 * @brief Read DEFLATE bits from a borrowed byte span, least significant bit first
 *
 * A local 64-bit buffer holds upcoming bits. Callers must ensure enough input
 * remains before consuming bits.
 */
module;

#include <bit>
#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>
#include <span>

export module bioseq.io.compression.bit_reader;

export namespace bioseq::io::compression {

/// Read compressed bits from borrowed bytes. Reads advance the cursor, and
/// seek_to_bit can reposition it within the input.
class bit_reader {
  public:
    static constexpr std::size_t buffer_refill_size = 128 * 1024;
    /// Largest supported fixed-width peek. Leaving room for a whole byte keeps
    /// refill from shifting any loaded bits beyond the 64-bit buffer.
    static constexpr std::size_t maximum_peek_bits = sizeof(std::uint64_t) * 8 - 8;

    /// Construct an exhausted reader with no borrowed input.
    bit_reader() = default;

    /// Construct a reader at bit offset zero over borrowed compressed bytes.
    explicit bit_reader(std::span<const std::uint8_t> compressed_view) noexcept
        : _data(compressed_view) {}

    /// Copy the borrowed view and the current bit cursor.
    bit_reader(const bit_reader&) = default;

    /// Copy the borrowed input and, cursor  state from another reader.
    bit_reader(bit_reader&&) = default;

    /// Replace this reader with a copied view and cursor.
    bit_reader& operator=(const bit_reader&) = default;

    /// Replace the borrowed input and cursor state from another reader.
    bit_reader& operator=(bit_reader&&) = default;

    /// Inspect the next p_bit_count bits without advancing. The count must be
    /// in [1, maximum_peek_bits], and enough input must remain.
    template <std::size_t p_bit_count> [[nodiscard]] std::uint64_t peek() noexcept {
        static_assert(
            p_bit_count >= 1 && p_bit_count <= maximum_peek_bits,
            "bit_reader::peek<p_bit_count>: p_bit_count must be in [1, maximum_peek_bits].");
        if (_bits_buffered < p_bit_count) [[unlikely]]
            refill();
        return _bit_buffer & ((std::uint64_t{1} << p_bit_count) - 1);
    }

    /// Read and consume p_bit_count bits. The ount must be in
    /// [1, maximum_peek_bits], and enough input must remain.
    template <std::size_t p_bit_count> [[nodiscard]] std::uint64_t read() noexcept {
        static_assert(
            p_bit_count >= 1 && p_bit_count <= maximum_peek_bits,
            "bit_reader::read<p_bit_count>: p_bit_count must be in [1, maximum_peek_bits].");
        if (_bits_buffered < p_bit_count) [[unlikely]]
            refill();
        const std::uint64_t result = _bit_buffer & ((std::uint64_t{1} << p_bit_count) - 1);
        _bit_buffer >>= p_bit_count;
        _bits_buffered -= p_bit_count;
        return result;
    }

    /// Inspect the next bit_count bits without advancing. The caller must ensure
    /// the requested bits fit in the buffer after refill.
    [[nodiscard]] std::uint64_t peek_bits(std::size_t bit_count) noexcept {
        if (_bits_buffered < bit_count) [[unlikely]]
            refill();
        if (bit_count >= 64)
            return _bit_buffer;
        return _bit_buffer & ((std::uint64_t{1} << bit_count) - 1);
    }

    /// Read and consume bit_count bits. The caller must ensure  the requested
    /// bits fit in the buffer after refill.
    [[nodiscard]] std::uint64_t read_bits(std::size_t bit_count) noexcept {
        if (_bits_buffered < bit_count) [[unlikely]]
            refill();
        std::uint64_t mask =
            (bit_count >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << bit_count) - 1);
        const std::uint64_t result = _bit_buffer & mask;
        if (bit_count >= 64) {
            _bit_buffer = 0;
        } else {
            _bit_buffer >>= bit_count;
        }
        _bits_buffered -= bit_count;
        return result;
    }

    /// Discard n_bits already held in the buffer. Refill first when needed.
    void skip(std::size_t n_bits) noexcept {
        if (n_bits >= 64) {
            _bit_buffer = 0;
        } else {
            _bit_buffer >>= n_bits;
        }
        _bits_buffered -= n_bits;
    }

    /// Return the current bit offset from the start of the input.
    [[nodiscard]] std::size_t bit_position() const noexcept {
        return _byte_cursor * 8 - _bits_buffered;
    }

    /// Return the number of unread bits currently held in the local buffer.
    [[nodiscard]] std::size_t bits_buffered() const noexcept {
        return _bits_buffered;
    }

    /// Reposition the reader at a valid bit offset in the borrowed input
    /// Chunk decoders use this to try a candidate block start.
    void seek_to_bit(std::size_t bit_offset) noexcept {
        _byte_cursor = bit_offset / 8;
        _bit_buffer = 0;
        _bits_buffered = 0;
        const std::size_t intra_byte = bit_offset % 8;
        if (intra_byte > 0) {
            refill();
            _bit_buffer >>= intra_byte;
            _bits_buffered -= intra_byte;
        }
    }

    /// Discard bits up to the next byte boundary, as required before the
    /// LEN and NLEN fields of a stored DEFLATE block.
    void align_to_byte_boundary() noexcept {
        const std::size_t excess = _bits_buffered % 8;
        if (excess > 0) {
            _bit_buffer >>= excess;
            _bits_buffered -= excess;
        }
    }

    /// Load whole bytes until the buffer holds at least 57 bits or input runs out.
    void refill() noexcept {
        while (_bits_buffered <= maximum_peek_bits && _byte_cursor < _data.size()) {
            const std::uint64_t byte = static_cast<std::uint64_t>(_data[_byte_cursor]);
            _bit_buffer |= (byte << _bits_buffered);
            _bits_buffered += 8;
            ++_byte_cursor;
        }
    }

    /// Read up to eight bytes as a  little-endian value at a byte boundary.
    /// Missing bytes at the end of input leave the high bits zero.
    [[nodiscard]] std::uint64_t read_aligned_le_u64() noexcept {
        DEBUG_ASSERT(_bits_buffered % 8 == 0, "read_aligned_le_u64 requires byte alignment");
        // Drain any buffered bytes first.
        std::uint64_t result = 0;
        std::size_t shift = 0;
        while (_bits_buffered >= 8 && shift < 64) {
            result |= ((_bit_buffer & 0xFFu) << shift);
            _bit_buffer >>= 8;
            _bits_buffered -= 8;
            shift += 8;
        }
        while (shift < 64 && _byte_cursor < _data.size()) {
            result |= (static_cast<std::uint64_t>(_data[_byte_cursor]) << shift);
            ++_byte_cursor;
            shift += 8;
        }
        return result;
    }

    /// Return whether both the source span and local bit buffer are exhausted.
    [[nodiscard]] bool exhausted() const noexcept {
        return _byte_cursor >= _data.size() && _bits_buffered == 0;
    }

    /// Return the size of the borrowed compressed span.
    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return _data.size();
    }

  private:
    std::span<const std::uint8_t> _data;
    std::uint64_t _bit_buffer = 0;
    std::size_t _bits_buffered = 0;
    std::size_t _byte_cursor = 0;
};

} // namespace bioseq::io::compression
