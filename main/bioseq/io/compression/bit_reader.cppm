/**
 * @file bit_reader.cppm
 * @brief Forward-only bit reader for DEFLATE-style streams.
 *
 * Span-input, non-templated. DEFLATE is the only consumer; templating on
 * `<LsbFirst, BufferT>` is YAGNI for this PR. Buffer is hardcoded
 * `std::uint64_t`. `peek<N>()` and `read<N>()` static_assert on N at
 * compile time. The variable-N `peek_bits(n)` is the Huffman fast loop's
 * entry point.
 *
 * The bit buffer holds up to 64 bits LSB-first. `refill()` fills the
 * buffer until either 8 bytes have been consumed from `_data` or the
 * buffer holds at least 57 bits (`kMaxPeekBits`). After `refill()` the
 * buffer holds in `[57, 63]` bits (assuming source has data); if source
 * runs out, the buffer holds all remaining bits.
 */
module;

#include "common.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

export module bioseq_io.compression.bit_reader;

import mlogging;

export namespace bioseq::io::compression {

class bit_reader {
public:
    static constexpr std::size_t kBufferRefillSize = 128 * 1024;
    /// Max bits we can guarantee the buffer holds after a `refill()`
    /// call. Must be `<= 64 - 8` because `refill` shifts the loaded
    /// byte by `_bits_buffered`; with `<= 57`, the case
    /// `_bits_buffered == 57` would load a new byte at shift 57,
    /// pushing bit 7 of that byte to position 64 (OUT of uint64_t),
    /// silently losing it. Keeping at most `64 - 8 = 56` ensures the
    /// loaded byte fully fits.
    static constexpr std::size_t kMaxPeekBits      = sizeof(std::uint64_t) * 8 - 8;

    bit_reader() = default;

    explicit bit_reader(std::span<const std::uint8_t> compressed_view) noexcept
        : _data(compressed_view) {}

    bit_reader(const bit_reader&) = default;
    bit_reader(bit_reader&&) = default;
    bit_reader& operator=(const bit_reader&) = default;
    bit_reader& operator=(bit_reader&&) = default;

    /// Peek N bits from the current position WITHOUT advancing. N is a
    /// compile-time constant in [1, kMaxPeekBits].
    template <std::size_t N>
    [[nodiscard]] std::uint64_t peek() noexcept {
        static_assert(N >= 1 && N <= kMaxPeekBits,
                      "bit_reader::peek<N>: N must be in [1, kMaxPeekBits].");
        if (_bits_buffered < N) [[unlikely]] refill();
        return _bit_buffer & ((std::uint64_t{1} << N) - 1);
    }

    /// Peek + advance. Same N constraints as `peek<N>()`.
    template <std::size_t N>
    [[nodiscard]] std::uint64_t read() noexcept {
        static_assert(N >= 1 && N <= kMaxPeekBits,
                      "bit_reader::read<N>: N must be in [1, kMaxPeekBits].");
        if (_bits_buffered < N) [[unlikely]] refill();
        const std::uint64_t result = _bit_buffer & ((std::uint64_t{1} << N) - 1);
        _bit_buffer  >>= N;
        _bits_buffered -= N;
        return result;
    }

    /// Variable-N peek. The Huffman fast loop calls this to extract a
    /// 45-bit window of the bit buffer in a single op. Caller is
    /// responsible for ensuring `_bits_buffered >= n`.
    [[nodiscard]] std::uint64_t peek_bits(std::size_t n) noexcept {
        if (_bits_buffered < n) [[unlikely]] refill();
        if (n >= 64) return _bit_buffer;
        return _bit_buffer & ((std::uint64_t{1} << n) - 1);
    }

    /// Variable-N read.
    [[nodiscard]] std::uint64_t read_bits(std::size_t n) noexcept {
        if (_bits_buffered < n) [[unlikely]] refill();
        std::uint64_t mask = (n >= 64) ? ~std::uint64_t{0}
                                       : ((std::uint64_t{1} << n) - 1);
        const std::uint64_t result = _bit_buffer & mask;
        if (n >= 64) {
            _bit_buffer = 0;
        } else {
            _bit_buffer >>= n;
        }
        _bits_buffered -= n;
        return result;
    }

    /// Advance the bit cursor by `n_bits` without consuming. Caller MUST
    /// have peeked the bits via `peek_bits(n_bits)` first so the buffer
    /// is guaranteed to hold them.
    void skip(std::size_t n_bits) noexcept {
        if (n_bits >= 64) {
            _bit_buffer = 0;
        } else {
            _bit_buffer >>= n_bits;
        }
        _bits_buffered -= n_bits;
    }

    /// Bit-position-from-start. Useful for blockfinder candidate
    /// reporting and chunk boundary tracking.
    [[nodiscard]] std::size_t bit_position() const noexcept {
        return _byte_cursor * 8 - _bits_buffered;
    }

    [[nodiscard]] std::size_t bits_buffered() const noexcept {
        return _bits_buffered;
    }

    /// Reset the bit-cursor and buffer to a specific bit-offset. Used
    /// by the chunk decoder to seek to a candidate block start.
    void seek_to_bit(std::size_t bit_offset) noexcept {
        _byte_cursor   = bit_offset / 8;
        _bit_buffer    = 0;
        _bits_buffered = 0;
        const std::size_t intra_byte = bit_offset % 8;
        if (intra_byte > 0) {
            refill();
            _bit_buffer  >>= intra_byte;
            _bits_buffered -= intra_byte;
        }
    }

    /// Drop bits until the cursor sits at a byte boundary. Required
    /// before reading the LEN/NLEN fields of a stored block per
    /// RFC 1951 §3.2.4.
    void align_to_byte_boundary() noexcept {
        const std::size_t excess = _bits_buffered % 8;
        if (excess > 0) {
            _bit_buffer  >>= excess;
            _bits_buffered -= excess;
        }
    }

    /// Fill the bit buffer. After this call the buffer holds at least
    /// `min(8 * remaining_bytes, 57)` bits.
    void refill() noexcept {
        while (_bits_buffered <= kMaxPeekBits && _byte_cursor < _data.size()) {
            const std::uint64_t byte =
                static_cast<std::uint64_t>(_data[_byte_cursor]);
            _bit_buffer |= (byte << _bits_buffered);
            _bits_buffered += 8;
            ++_byte_cursor;
        }
    }

    /// Read 8 aligned bytes when the cursor sits at a byte boundary.
    /// Used by the gzip footer parser to consume CRC32 + ISIZE in one
    /// shot. Asserts byte alignment.
    [[nodiscard]] std::uint64_t read_aligned_le_u64() noexcept {
        DEBUG_ASSERT(_bits_buffered % 8 == 0,
                     "read_aligned_le_u64 requires byte alignment");
        // Drain any buffered bytes first.
        std::uint64_t result = 0;
        std::size_t   shift  = 0;
        while (_bits_buffered >= 8 && shift < 64) {
            result |= ((_bit_buffer & 0xFFu) << shift);
            _bit_buffer  >>= 8;
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

    [[nodiscard]] bool exhausted() const noexcept {
        return _byte_cursor >= _data.size() && _bits_buffered == 0;
    }

    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return _data.size();
    }

private:
    std::span<const std::uint8_t> _data;
    std::uint64_t                 _bit_buffer    = 0;
    std::size_t                   _bits_buffered = 0;
    std::size_t                   _byte_cursor   = 0;
};

}  // namespace bioseq::io::compression
