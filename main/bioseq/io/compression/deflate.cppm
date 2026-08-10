/**
 * @file deflate.cppm
 * @brief DEFLATE block decoder — port of rapidgzip's marker-mode Block.
 *
 * RFC 1951. Two execution modes share one buffer:
 *
 *   * **direct mode** (`m_contains_marker_bytes == false`) — the prior
 *     32 KiB window is known. Output bytes go into a `uint8_t` view of
 *     `m_window16`; every back-reference can be resolved on the spot
 *     because all referenced bytes are already in the buffer.
 *
 *   * **marker mode** (`m_contains_marker_bytes == true`) — the prior
 *     window is unknown. Output goes into the `uint16_t` view: real
 *     literals are stored as `< 256`, but back-references that reach
 *     into the unknown prefix become "marker" symbols
 *     `MAX_WINDOW_SIZE + i` (i.e. `0x8000 + i`) referencing offset `i`
 *     of the missing window. `set_initial_window` later patches them.
 *
 * Once `m_distance_to_last_marker_byte >= MAX_WINDOW_SIZE` we know no
 * downstream back-reference will reach into the marker territory, so
 * we narrow to the `uint8_t` view via `set_initial_window` and skip
 * the marker checks for the remainder of the chunk.
 *
 * The Huffman codings — literal/length, distance, precode — are
 * provided by `canonical_huffman_decoder` from `huffman.cppm`. The
 * primary-table-cached fast path (11 / 9 / 7 bit primary LUT) is what
 * upstream calls `HuffmanCodingShortBitsCached` /
 * `HuffmanCodingReversedBitsCached*`, and the structure here mirrors
 * them: a peek-and-skip-after-peek inner loop, with primary table
 * size sized to fit L1.
 *
 * The buffer is 64 KiB of `uint16_t` (= 128 KiB raw). Its size is a
 * power of two so the circular-buffer wrap is a single bit-AND, not a
 * `%`. The trailing 32 KiB starts pre-filled with the identity marker
 * sequence `i + MAX_WINDOW_SIZE` so the very first back-reference
 * pointing into the unknown prefix lands on its corresponding marker
 * symbol without further setup work.
 */
module;


#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#define BIOSEQ_DEFLATE_FORCEINLINE __attribute__((always_inline)) inline
#else
#define BIOSEQ_DEFLATE_FORCEINLINE inline
#endif

export module bioseq.io.compression.deflate;

import bioseq.io.compression.bit_reader;
import bioseq.io.compression.huffman;

export namespace bioseq::io::compression {

// =====================================================================
// RFC 1951 constants
// =====================================================================

/// 32 KiB, fixed by RFC 1951 §3.2.5 (max back-reference distance).
inline constexpr std::size_t maximum_window_size = 32 * 1024;
/// `m_window16` size: 2× window size, power-of-two (modulo via AND).
inline constexpr std::size_t predecoded_buffer_size = 2 * maximum_window_size;
inline constexpr std::size_t predecoded_buffer_mask = predecoded_buffer_size - 1;
static_assert((predecoded_buffer_size & predecoded_buffer_mask) == 0,
              "predecoded_buffer_size must be a power of two for AND-wrap.");

/// Max uncompressed deflate block (16-bit length field). Fits in the
/// uint8_t reinterpret of `m_window16` (128 KiB > 65535 B).
inline constexpr std::size_t maximum_uncompressed_size =
    std::numeric_limits<std::uint16_t>::max();

/// RFC 1951 max code length (precode is 7).
inline constexpr std::uint8_t maximum_code_length = 15;
inline constexpr std::uint8_t precode_bit_count   = 3;
inline constexpr std::size_t  maximum_precode_count = 19;

/// Per RFC 1951 §3.2.7 the *encoded* order of precode code lengths is
/// not natural. The `i`-th entry of the encoded stream is the
/// code-length of `precode_alphabet[i]`.
inline constexpr std::array<std::uint8_t, maximum_precode_count>
    precode_alphabet = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11,
                        4,  12, 3,  13, 2, 14, 1, 15};

/// 286 in spec, +2 padding so the back-to-back literal+distance
/// code-length array fits without an explicit bounds check (RFC §3.2.7
/// lists max literal+length + max distance = 286 + 30 = 316; +256
/// safety pad lets the manual-unroll filling for codes 16/17/18 write
/// past the live area without a per-loop check).
inline constexpr std::size_t maximum_literal_or_length_symbols = 286;
inline constexpr std::size_t maximum_distance_symbol_count    = 30;
inline constexpr std::size_t literal_and_distance_code_length_buffer_size =
    maximum_literal_or_length_symbols + maximum_distance_symbol_count + 256;

inline constexpr std::uint16_t end_of_block_symbol = 256;
inline constexpr std::size_t   maximum_run_length     = 258;

/// Length code → base length (before adding extra bits). RFC 1951 §3.2.5.
[[nodiscard]] consteval std::array<std::uint16_t, 24>
build_length_lut() noexcept {
    std::array<std::uint16_t, 24> lut{};
    for (std::uint16_t i = 0; i < 24; ++i) {
        const std::uint16_t extra = i / 4u;
        lut[i] = static_cast<std::uint16_t>(
            3u + (1u << (extra + 2u)) + ((i % 4u) << extra));
    }
    return lut;
}
inline constexpr auto length_table = build_length_lut();

/// Distance code → base distance.
[[nodiscard]] consteval std::array<std::uint16_t, 30>
build_distance_lut() noexcept {
    std::array<std::uint16_t, 30> lut{};
    for (std::uint16_t i = 0; i < 4; ++i) lut[i] = static_cast<std::uint16_t>(i + 1);
    for (std::uint16_t i = 4; i < 30; ++i) {
        const std::uint16_t extra = static_cast<std::uint16_t>((i - 2u) / 2u);
        lut[i] = static_cast<std::uint16_t>(
            1u + (1u << (extra + 1u)) + ((i % 2u) << extra));
    }
    return lut;
}
inline constexpr auto distance_table = build_distance_lut();

// =====================================================================
// Public types
// =====================================================================

enum class block_compression : std::uint8_t {
    uncompressed = 0,
    fixed_huffman = 1,
    dynamic_huffman = 2,
    reserved = 3,
};

enum class decode_status : std::uint8_t {
    ok = 0,
    end_of_block = 1,
    end_of_stream = 2,
    invalid_code = 3,
    invalid_distance = 4,
    out_of_input = 5,
    invalid_huffman_code = 6,
    invalid_code_length_backreference = 7,
    exceeded_literal_range = 8,
    exceeded_distance_range = 9,
    exceeded_window_range = 10,
    invalid_code_lengths = 11,
    length_checksum_mismatch = 12,
    non_zero_padding = 13,
    invalid_compression = 14,
    generic_error = 15,
};

[[nodiscard]] constexpr const char*
decode_status_name(decode_status status) noexcept {
    switch (status) {
        case decode_status::ok:                       return "ok";
        case decode_status::end_of_block:             return "end_of_block";
        case decode_status::end_of_stream:            return "end_of_stream";
        case decode_status::invalid_code:             return "invalid_code";
        case decode_status::invalid_distance:         return "invalid_distance";
        case decode_status::out_of_input:             return "out_of_input";
        case decode_status::invalid_huffman_code:     return "invalid_huffman_code";
        case decode_status::invalid_code_length_backreference: return "invalid_cl_backreference";
        case decode_status::exceeded_literal_range:   return "exceeded_literal_range";
        case decode_status::exceeded_distance_range:  return "exceeded_distance_range";
        case decode_status::exceeded_window_range:    return "exceeded_window_range";
        case decode_status::invalid_code_lengths:     return "invalid_code_lengths";
        case decode_status::length_checksum_mismatch: return "length_checksum_mismatch";
        case decode_status::non_zero_padding:         return "non_zero_padding";
        case decode_status::invalid_compression:      return "invalid_compression";
        case decode_status::generic_error:            return "generic_error";
    }
    return "?";
}

/// View returned from `deflate_block_decoder::read`. Only one of
/// {data, data_with_markers}
/// is non-empty depending on whether the prior window is known.
struct decoded_data_view {
    /// Resolved bytes (direct mode).
    std::array<std::span<const std::uint8_t>, 2>  data{};
    /// Marker stream (marker mode). `set_initial_window` later replaces
    /// `value >= MAX_WINDOW_SIZE` lanes with `prior_window[value -
    /// MAX_WINDOW_SIZE]` and demotes the chunk to direct mode.
    std::array<std::span<const std::uint16_t>, 2> data_with_markers{};

    [[nodiscard]] std::size_t size() const noexcept {
        return data[0].size() + data[1].size()
             + data_with_markers[0].size() + data_with_markers[1].size();
    }
    [[nodiscard]] std::size_t data_size() const noexcept {
        return data[0].size() + data[1].size();
    }
    [[nodiscard]] std::size_t data_with_markers_size() const noexcept {
        return data_with_markers[0].size() + data_with_markers[1].size();
    }
    [[nodiscard]] bool contains_markers() const noexcept {
        return data_with_markers_size() > 0;
    }
};

// =====================================================================
// Internals
// =====================================================================

namespace detail {

/// Pre-filled marker buffer for a freshly-reset block. The trailing
/// 32 KiB of slots `[maximum_window_size, predecoded_buffer_size)` hold the
/// identity sequence `i + maximum_window_size` so that a back-reference of
/// distance `d` into the unknown prefix lands on marker
/// `(predecoded_buffer_size - d) - maximum_window_size = maximum_window_size - d`,
/// which is exactly the byte offset that the resolver will look up in
/// `prior_window`. The prefix `[0, maximum_window_size)` is left zero — it
/// gets written to as the chunk decodes.
[[nodiscard]] inline const std::array<std::uint16_t, predecoded_buffer_size>&
get_initial_marker_buffer() noexcept {
    static const auto buf = []() noexcept {
        std::array<std::uint16_t, predecoded_buffer_size> result{};
        for (std::size_t i = 0; i < maximum_window_size; ++i) {
            result[maximum_window_size + i] =
                static_cast<std::uint16_t>(maximum_window_size + i);
        }
        return result;
    }();
    return buf;
}

/// Read the dynamic literal/distance code-length stream. Codes are
/// emitted by the precode (per RFC 1951 §3.2.7); codes 16/17/18 are
/// run-length encodings expanded inline. The "+ 256 safety pad" in
/// `literal_and_distance_code_length_buffer_size` lets the run-length expansions
/// write past the live area without per-iteration bounds checks; the
/// `i == count` exit-test catches over-runs.
[[nodiscard]] inline decode_status read_distance_and_literal_code_lengths(
    std::array<std::uint8_t, literal_and_distance_code_length_buffer_size>& code_lengths,
    bit_reader&                                                reader,
    const precode_decoder&                                     precode,
    std::size_t                                                count) noexcept {
    std::size_t i = 0;
    while (i < count) {
        const std::uint16_t decoded = precode.decode(reader);
        if (decoded == precode_decoder::invalid_symbol) [[unlikely]] {
            return decode_status::invalid_huffman_code;
        }
        const std::uint8_t code = static_cast<std::uint8_t>(decoded);

        if (code <= 15) {
            code_lengths[i] = code;
            ++i;
        } else if (code == 16) {
            if (i == 0) [[unlikely]] {
                return decode_status::invalid_code_length_backreference;
            }
            const std::uint8_t last = code_lengths[i - 1];
            // 6 = 3 + 0b11 (max). Manually-unrolled fill, avoids inner
            // branch on the run length while letting the compiler
            // still vectorise the store. Safety pad guarantees the
            // writes don't fall off the end.
            code_lengths[i + 0] = last;
            code_lengths[i + 1] = last;
            code_lengths[i + 2] = last;
            code_lengths[i + 3] = last;
            code_lengths[i + 4] = last;
            code_lengths[i + 5] = last;
            const auto extra = static_cast<std::size_t>(reader.read<2>());
            i += extra + 3u;
        } else if (code == 17) {
            // 10 = 3 + 0b111 (max). Already zero-init from caller; skip
            // the writes — they would be no-ops.
            const auto extra = static_cast<std::size_t>(reader.read<3>());
            // Defensive: if caller didn't zero, restore.
            code_lengths[i + 0] = 0;
            code_lengths[i + 1] = 0;
            code_lengths[i + 2] = 0;
            code_lengths[i + 3] = 0;
            code_lengths[i + 4] = 0;
            code_lengths[i + 5] = 0;
            code_lengths[i + 6] = 0;
            code_lengths[i + 7] = 0;
            code_lengths[i + 8] = 0;
            code_lengths[i + 9] = 0;
            i += extra + 3u;
        } else if (code == 18) {
            // 138 = 11 + 0b1111111 (max). Use a tight loop with
            // `#pragma GCC unroll`-friendly bound.
            const auto extra = static_cast<std::size_t>(reader.read<7>());
            const std::size_t run = extra + 11u;
            for (std::size_t j = 0; j < run; ++j) {
                code_lengths[i + j] = 0;
            }
            i += run;
        } else [[unlikely]] {
            return decode_status::invalid_huffman_code;
        }
    }
    return i == count ? decode_status::ok
                      : decode_status::exceeded_literal_range;
}

}  // namespace detail

// =====================================================================
// block — DEFLATE block decoder
// =====================================================================

/// Decodes one DEFLATE block at a time. Reuse via `reset()` between
/// blocks; one decoder instance per worker task (its 128 KiB
/// `m_window16` buffer is too big to live on a chunk arena).
class deflate_block_decoder {
public:
    using window_buffer_t = std::array<std::uint16_t, predecoded_buffer_size>;

    deflate_block_decoder() noexcept = default;
    deflate_block_decoder(const deflate_block_decoder&) = delete;
    deflate_block_decoder& operator=(const deflate_block_decoder&) = delete;
    deflate_block_decoder(deflate_block_decoder&&) = delete;
    deflate_block_decoder& operator=(deflate_block_decoder&&) = delete;
    ~deflate_block_decoder() = default;

    // ---------------------------------------------------------------
    // State queries
    // ---------------------------------------------------------------

    [[nodiscard]] bool is_end_of_block() const noexcept { return _at_end_of_block; }
    [[nodiscard]] bool is_end_of_stream() const noexcept { return _at_end_of_block && _is_last_block; }
    [[nodiscard]] bool is_last_block() const noexcept { return _is_last_block; }
    [[nodiscard]] bool contains_marker_bytes() const noexcept { return _contains_markers; }
    [[nodiscard]] block_compression compression_type() const noexcept { return _compression_type; }
    [[nodiscard]] std::size_t decoded_bytes() const noexcept { return _decoded_bytes; }
    [[nodiscard]] std::size_t window_position() const noexcept { return _window_position; }
    [[nodiscard]] std::uint8_t padding() const noexcept { return _padding; }
    [[nodiscard]] std::size_t uncompressed_size() const noexcept {
        return _compression_type == block_compression::uncompressed ? _uncompressed_size : 0;
    }

    // ---------------------------------------------------------------
    // Reset / set-initial-window
    // ---------------------------------------------------------------

    /// Reset between blocks. If `prior_window` is provided, the block
    /// starts in direct mode (markers disabled). If empty, marker mode
    /// is on and `_window16`'s tail 32 KiB is the identity-marker
    /// initialiser. `_at_end_of_block` is set TRUE so the caller's
    /// drive-loop knows to call `read_header` before the first
    /// `read()` — otherwise `read()` would early-return on the
    /// RESERVED compression type.
    void reset(std::span<const std::uint8_t> prior_window = {}) noexcept {
        _uncompressed_size = 0;
        _at_end_of_block = true;
        _is_last_block = false;
        _compression_type = block_compression::reserved;
        _padding = 0;
        _decoded_bytes = 0;
        _distance_to_last_marker_byte = 0;
        _window_position = 0;
        _contains_markers = true;

        if (prior_window.empty()) {
            // Restore the identity marker tail. We can skip this
            // memcpy if `_window_position == 0` and the buffer was
            // freshly constructed, but block reuse is the common path,
            // so a 64 KiB memcpy beats correctness bugs. ~6 GB/s on
            // Apple Silicon — invisible vs 50 ms decode time per chunk.
            std::memcpy(_window16.data(),
                        detail::get_initial_marker_buffer().data(),
                        sizeof(_window16));
        } else {
            set_initial_window(prior_window);
        }
    }

    /// Hand the prior 32 KiB window to a marker-mode block, replacing
    /// any markers in the buffer with the resolved bytes and switching
    /// to direct mode for the remainder of this block.
    void set_initial_window(std::span<const std::uint8_t> initial_window = {}) noexcept {
        if (!_contains_markers) return;

        // Empty fresh block → just adopt the window directly into the
        // uint8 view.
        if (_decoded_bytes == 0 && _window_position == 0) {
            if (!initial_window.empty()) {
                std::uint8_t* uint8_window =
                    reinterpret_cast<std::uint8_t*>(_window16.data());
                std::memcpy(uint8_window, initial_window.data(),
                            initial_window.size());
                _window_position = initial_window.size();
                _decoded_bytes = initial_window.size();
            }
            _contains_markers = false;
            return;
        }

        // Mid-block: clear any unwritten slots (those past the
        // already-decoded bytes), resolve every marker, then conflate
        // the wrapped circular buffer into a contiguous byte view that
        // ends at slot index = window_size − 1.
        for (std::size_t i = 0; _decoded_bytes + i < _window16.size(); ++i) {
            _window16[(_window_position + i) & predecoded_buffer_mask] = 0;
        }
        for (std::size_t i = 0; i < _window16.size(); ++i) {
            const std::uint16_t value = _window16[i];
            if (value < 256u) continue;
            // Markers always have value >= MAX_WINDOW_SIZE; values in
            // [256, MAX_WINDOW_SIZE) are illegal (would mean an
            // unresolvable mid-window code) — those become 0 here.
            if (value < maximum_window_size) {
                _window16[i] = 0;
                continue;
            }
            const std::size_t window_offset = value - maximum_window_size;
            if (window_offset < initial_window.size()) {
                _window16[i] = initial_window[window_offset];
            } else {
                _window16[i] = 0;
            }
        }

        // Conflate so the circular buffer is contiguous, then move it
        // into the uint8 view of the same memory.
        std::array<std::uint8_t, predecoded_buffer_size> conflated{};
        for (std::size_t i = 0; i < _window16.size(); ++i) {
            const std::uint16_t value =
                _window16[(i + _window_position) & predecoded_buffer_mask];
            conflated[i] = static_cast<std::uint8_t>(value & 0xFFu);
        }
        std::uint8_t* uint8_window =
            reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_window_size =
            _window16.size() * sizeof(std::uint16_t);
        // Place the conflated content at the end of the uint8 view so
        // wrap of the circular buffer is at index 0 again.
        std::memcpy(uint8_window + (uint8_window_size - conflated.size()),
                    conflated.data(), conflated.size());
        _window_position = 0;
        _contains_markers = false;
    }

    // ---------------------------------------------------------------
    // Block header
    // ---------------------------------------------------------------

    /// Read the 3-bit DEFLATE block header (BFINAL + BTYPE).
    [[nodiscard]] decode_status read_header(bit_reader& reader) noexcept {
        if (reader.bits_buffered() < 3) reader.refill();
        if (reader.bits_buffered() < 3) return decode_status::out_of_input;
        const std::uint64_t bfinal = reader.read<1>();
        const std::uint64_t btype  = reader.read<2>();
        _is_last_block = (bfinal != 0);
        _at_end_of_block = false;

        switch (btype) {
            case 0: {
                _compression_type = block_compression::uncompressed;
                // Pad to byte boundary; verify zero (RFC §3.2.4
                // recommends).
                const std::size_t pos = reader.bit_position();
                const std::size_t excess = pos % 8;
                if (excess != 0) {
                    const std::size_t to_read = 8u - excess;
                    if (reader.bits_buffered() < to_read) reader.refill();
                    if (reader.bits_buffered() < to_read) {
                        return decode_status::out_of_input;
                    }
                    _padding =
                        static_cast<std::uint8_t>(reader.read_bits(to_read));
                    if (_padding != 0) {
                        return decode_status::non_zero_padding;
                    }
                } else {
                    _padding = 0;
                }
                if (reader.bits_buffered() < 32) reader.refill();
                if (reader.bits_buffered() < 32) {
                    return decode_status::out_of_input;
                }
                _uncompressed_size = static_cast<std::uint16_t>(reader.read<16>());
                const std::uint16_t nlen =
                    static_cast<std::uint16_t>(reader.read<16>());
                if (_uncompressed_size != static_cast<std::uint16_t>(~nlen)) {
                    return decode_status::length_checksum_mismatch;
                }
                return decode_status::ok;
            }
            case 1:
                _compression_type = block_compression::fixed_huffman;
                return decode_status::ok;
            case 2:
                _compression_type = block_compression::dynamic_huffman;
                return read_dynamic_huffman(reader);
            default:
                _compression_type = block_compression::reserved;
                return decode_status::invalid_compression;
        }
    }

    /// Read the dynamic-Huffman tables: precode, then literal and
    /// distance via precode.
    [[nodiscard]] decode_status
    read_dynamic_huffman(bit_reader& reader) noexcept {
        if (reader.bits_buffered() < 14) reader.refill();
        if (reader.bits_buffered() < 14) return decode_status::out_of_input;
        const std::size_t literal_count =
            static_cast<std::size_t>(reader.read<5>()) + 257u;
        if (literal_count > maximum_literal_or_length_symbols) {
            return decode_status::exceeded_literal_range;
        }
        const std::size_t distance_count =
            static_cast<std::size_t>(reader.read<5>()) + 1u;
        if (distance_count > maximum_distance_symbol_count) {
            return decode_status::exceeded_distance_range;
        }
        const std::size_t precode_count =
            static_cast<std::size_t>(reader.read<4>()) + 4u;

        // Precode code-length table (3 bits each, max 19 entries).
        std::array<std::uint8_t, maximum_precode_count> precode_cl{};
        for (std::size_t i = 0; i < precode_count; ++i) {
            if (reader.bits_buffered() < precode_bit_count) reader.refill();
            precode_cl[precode_alphabet[i]] =
                static_cast<std::uint8_t>(reader.read<precode_bit_count>());
        }

        const auto precode_or = precode_decoder::build(
            std::span<const std::uint8_t>{precode_cl.data(), precode_cl.size()});
        if (!precode_or.is_valid()) return decode_status::invalid_code_lengths;

        // Concatenate literal and distance code-length streams into one
        // (rapidgzip layout). Max length = 286 + 30 = 316; +256 safety
        // pad lets run-length expansions store past `count` without
        // bounds checks.
        std::array<std::uint8_t, literal_and_distance_code_length_buffer_size> literal_cl{};
        if (auto err = detail::read_distance_and_literal_code_lengths(
                literal_cl, reader, precode_or,
                literal_count + distance_count);
            err != decode_status::ok) {
            return err;
        }

        // Verify EOB symbol exists.
        if (literal_cl[end_of_block_symbol] == 0) {
            return decode_status::invalid_code_lengths;
        }

        _literal_decoder = lit_len_decoder::build(
            std::span<const std::uint8_t>{literal_cl.data(), literal_count});
        if (!_literal_decoder.is_valid()) {
            return decode_status::invalid_code_lengths;
        }
        _distance_decoder = distance_decoder::build(
            std::span<const std::uint8_t>{literal_cl.data() + literal_count,
                                          distance_count});
        if (!_distance_decoder.is_valid()) {
            return decode_status::invalid_code_lengths;
        }
        return decode_status::ok;
    }

    // ---------------------------------------------------------------
    // Block decoding — public read entry point
    // ---------------------------------------------------------------

    /// Decode up to `n_max_to_decode` bytes of this block. Returns
    /// either resolved bytes (direct mode) or marker symbols (marker
    /// mode) via `decoded_data_view`. Stops at end-of-block; loop
    /// outside re-enters with `read_header`.
    [[nodiscard]] std::pair<decoded_data_view, decode_status>
    read(bit_reader& reader,
         std::size_t n_max_to_decode = std::numeric_limits<std::size_t>::max()) noexcept {
        if (_at_end_of_block) {
            return {decoded_data_view{}, decode_status::ok};
        }
        if (_compression_type == block_compression::reserved) {
            return {decoded_data_view{}, decode_status::invalid_compression};
        }

        if (_compression_type == block_compression::uncompressed) {
            return read_uncompressed(reader);
        }

        decoded_data_view view;
        std::size_t produced = 0;
        decode_status status = decode_status::ok;
        const std::size_t before = _window_position;
        const bool marker_mode = _contains_markers;

        if (marker_mode) {
            std::tie(produced, status) =
                read_compressed_marker(reader, n_max_to_decode);
            // If the chunk has fully outgrown the unknown-prefix
            // dependency, narrow to direct mode now and return the
            // resolved bytes view instead of markers. This avoids the
            // marker-replacement cost on long all-literal runs.
            if ((_distance_to_last_marker_byte >= _window16.size())
                || ((_distance_to_last_marker_byte >= maximum_window_size)
                    && (_distance_to_last_marker_byte == _decoded_bytes))) {
                set_initial_window();
                view.data = last_buffers_uint8(produced);
            } else {
                view.data_with_markers = last_buffers_uint16(produced);
            }
        } else {
            std::tie(produced, status) =
                read_compressed_direct(reader, n_max_to_decode);
            view.data = last_buffers_uint8(produced);
        }
        (void)before;
        return {view, status};
    }

private:
    // ---------------------------------------------------------------
    // Uncompressed block decode
    // ---------------------------------------------------------------

    [[nodiscard]] std::pair<decoded_data_view, decode_status>
    read_uncompressed(bit_reader& reader) noexcept {
        // Aligned bytes; reader was byte-aligned at end of header.
        const std::size_t total = _uncompressed_size;
        if (_contains_markers) {
            // 16-bit append into the marker buffer.
            for (std::size_t i = 0; i < total; ++i) {
                if (reader.bits_buffered() < 8) reader.refill();
                if (reader.bits_buffered() < 8) {
                    _decoded_bytes += i;
                    return {decoded_data_view{}, decode_status::out_of_input};
                }
                const std::uint8_t byte =
                    static_cast<std::uint8_t>(reader.read<8>());
                append_to_window_uint16(static_cast<std::uint16_t>(byte));
            }
            _at_end_of_block = true;
            _decoded_bytes += total;

            decoded_data_view view;
            // Direct narrow / promote happens via set_initial_window
            // when the caller decides to. For simplicity, return the
            // marker view unconditionally — a contiguous uncompressed
            // block has no actual marker symbols (only literals < 256),
            // so the orchestrator's `narrow_lower_bytes_to_dst` step
            // resolves these trivially.
            view.data_with_markers = last_buffers_uint16(total);
            return {view, decode_status::ok};
        }

        // Direct: bulk memcpy if the byte_cursor is byte-aligned.
        std::uint8_t* uint8_window =
            reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_size =
            _window16.size() * sizeof(std::uint16_t);
        const std::size_t mask = uint8_size - 1u;

        std::size_t produced = 0;
        while (produced < total) {
            const std::size_t left = total - produced;
            const std::size_t at = _window_position;
            // Copy in two segments if wrapping.
            const std::size_t span_a = std::min(left, uint8_size - at);
            for (std::size_t i = 0; i < span_a; ++i) {
                if (reader.bits_buffered() < 8) reader.refill();
                if (reader.bits_buffered() < 8) {
                    _decoded_bytes += produced;
                    _window_position = (at + i) & mask;
                    return {decoded_data_view{}, decode_status::out_of_input};
                }
                uint8_window[at + i] =
                    static_cast<std::uint8_t>(reader.read<8>());
            }
            produced += span_a;
            _window_position = (at + span_a) & mask;
        }
        _at_end_of_block = true;
        _decoded_bytes += total;
        decoded_data_view view;
        view.data = last_buffers_uint8(total);
        return {view, decode_status::ok};
    }

    // ---------------------------------------------------------------
    // Compressed block decode (marker mode)
    // ---------------------------------------------------------------

    [[nodiscard]] std::pair<std::size_t, decode_status>
    read_compressed_marker(bit_reader& reader,
                           std::size_t n_max_to_decode) noexcept {
        // Cap to leave room for a max-length back-ref (258).
        const std::size_t cap = std::min(
            n_max_to_decode, _window16.size() - maximum_run_length);

        const auto& lit_decoder =
            _compression_type == block_compression::fixed_huffman
                ? fixed_literal_length_decoder
                : _literal_decoder;

        std::size_t produced = 0;
        while (produced < cap) {
            // Refill so the Huffman decoder + extra bits + distance
            // decode + extra bits all fit in the bit buffer without
            // re-refilling. Worst-case for one symbol:
            //   15 (lit/len) + 5 (length extra) + 15 (distance) + 13
            //   (distance extra) = 48 bits. Two symbols = 96 bits >
            //   the 57-bit fast-path budget, so refill before each
            //   symbol.
            if (reader.bits_buffered() < 48) [[unlikely]] reader.refill();

            const std::uint16_t code = lit_decoder.decode(reader);
            if (code == lit_len_decoder::invalid_symbol) [[unlikely]] {
                _decoded_bytes += produced;
                return {produced, decode_status::invalid_huffman_code};
            }
            if (code <= 255) {
                append_to_window_uint16(code);
                ++produced;
                continue;
            }
            if (code == end_of_block_symbol) [[unlikely]] {
                _at_end_of_block = true;
                _decoded_bytes += produced;
                return {produced, decode_status::ok};
            }
            if (code > 285) [[unlikely]] {
                _decoded_bytes += produced;
                return {produced, decode_status::invalid_huffman_code};
            }

            const std::size_t length = read_length(reader, code);
            const auto [distance, derr] = read_distance(reader);
            if (derr != decode_status::ok) [[unlikely]] {
                _decoded_bytes += produced;
                return {produced, derr};
            }
            resolve_backreference_uint16(distance, length);
            produced += length;
        }
        _decoded_bytes += produced;
        return {produced, decode_status::ok};
    }

    [[nodiscard]] std::pair<std::size_t, decode_status>
    read_compressed_direct(bit_reader& reader,
                           std::size_t n_max_to_decode) noexcept {
        const std::size_t uint8_size =
            _window16.size() * sizeof(std::uint16_t);
        const std::size_t cap = std::min(
            n_max_to_decode, uint8_size - maximum_run_length);

        const auto& lit_decoder =
            _compression_type == block_compression::fixed_huffman
                ? fixed_literal_length_decoder
                : _literal_decoder;

        std::size_t produced = 0;
        while (produced < cap) {
            if (reader.bits_buffered() < 48) [[unlikely]] reader.refill();

            const std::uint16_t code = lit_decoder.decode(reader);
            if (code == lit_len_decoder::invalid_symbol) [[unlikely]] {
                _decoded_bytes += produced;
                return {produced, decode_status::invalid_huffman_code};
            }
            if (code <= 255) {
                append_to_window_uint8(static_cast<std::uint8_t>(code));
                ++produced;
                continue;
            }
            if (code == end_of_block_symbol) [[unlikely]] {
                _at_end_of_block = true;
                _decoded_bytes += produced;
                return {produced, decode_status::ok};
            }
            if (code > 285) [[unlikely]] {
                _decoded_bytes += produced;
                return {produced, decode_status::invalid_huffman_code};
            }
            const std::size_t length = read_length(reader, code);
            const auto [distance, derr] = read_distance(reader);
            if (derr != decode_status::ok) [[unlikely]] {
                _decoded_bytes += produced;
                return {produced, derr};
            }
            // Direct-mode safety: the back-reference must point inside
            // the already-decoded region.
            if (distance > _decoded_bytes + produced) [[unlikely]] {
                _decoded_bytes += produced;
                return {produced, decode_status::exceeded_window_range};
            }
            resolve_backreference_uint8(distance, length);
            produced += length;
        }
        _decoded_bytes += produced;
        return {produced, decode_status::ok};
    }

    // ---------------------------------------------------------------
    // Length / distance decode helpers
    // ---------------------------------------------------------------

    [[nodiscard]] BIOSEQ_DEFLATE_FORCEINLINE std::size_t
    read_length(bit_reader& reader, std::uint16_t code) noexcept {
        // RFC 1951 §3.2.5: codes 257..285.
        if (code <= 264) return static_cast<std::size_t>(code) - 257u + 3u;
        if (code == 285) return 258u;
        const std::uint16_t adj = static_cast<std::uint16_t>(code - 261u);
        const std::uint8_t  extra_bits =
            static_cast<std::uint8_t>(adj / 4u);
        const std::uint64_t extra = reader.read_bits(extra_bits);
        return static_cast<std::size_t>(length_table[adj]) + static_cast<std::size_t>(extra);
    }

    [[nodiscard]] BIOSEQ_DEFLATE_FORCEINLINE std::pair<std::uint16_t, decode_status>
    read_distance(bit_reader& reader) noexcept {
        std::uint16_t distance;
        if (_compression_type == block_compression::fixed_huffman) {
            // RFC 1951 §3.2.6: fixed distance Huffman = 5 bits, MSB
            // first. Reverse to LSB-first peek-style.
            const std::uint64_t raw = reader.read<5>();
            distance = static_cast<std::uint16_t>(reverse_bits_5(static_cast<std::uint8_t>(raw)));
            if (distance >= maximum_distance_symbol_count) [[unlikely]] {
                return {0, decode_status::exceeded_distance_range};
            }
        } else {
            const std::uint16_t decoded = _distance_decoder.decode(reader);
            if (decoded == distance_decoder::invalid_symbol) [[unlikely]] {
                return {0, decode_status::invalid_huffman_code};
            }
            distance = decoded;
            if (distance >= maximum_distance_symbol_count) [[unlikely]] {
                return {0, decode_status::exceeded_distance_range};
            }
        }

        if (distance <= 3u) return {static_cast<std::uint16_t>(distance + 1u), decode_status::ok};
        const std::uint8_t extra_bits =
            static_cast<std::uint8_t>((distance - 2u) / 2u);
        const std::uint64_t extra = reader.read_bits(extra_bits);
        return {
            static_cast<std::uint16_t>(distance_table[distance] + extra),
            decode_status::ok};
    }

    [[nodiscard]] static constexpr std::uint8_t
    reverse_bits_5(std::uint8_t value) noexcept {
        std::uint8_t result = 0;
        for (int b = 0; b < 5; ++b) result |= static_cast<std::uint8_t>(((value >> b) & 1u) << (4 - b));
        return result;
    }

    // ---------------------------------------------------------------
    // Window appends
    // ---------------------------------------------------------------

    BIOSEQ_DEFLATE_FORCEINLINE void
    append_to_window_uint16(std::uint16_t symbol) noexcept {
        if (symbol > 255u) {
            _distance_to_last_marker_byte = 0;
        } else {
            ++_distance_to_last_marker_byte;
        }
        _window16[_window_position] = symbol;
        _window_position = (_window_position + 1u) & predecoded_buffer_mask;
    }

    BIOSEQ_DEFLATE_FORCEINLINE void
    append_to_window_uint8(std::uint8_t byte) noexcept {
        std::uint8_t* uint8_window =
            reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_size =
            _window16.size() * sizeof(std::uint16_t);
        uint8_window[_window_position] = byte;
        _window_position = (_window_position + 1u) & (uint8_size - 1u);
    }

    BIOSEQ_DEFLATE_FORCEINLINE void
    resolve_backreference_uint16(std::uint16_t distance, std::size_t length) noexcept {
        const std::size_t buf_size = _window16.size();
        const std::size_t mask     = predecoded_buffer_mask;
        const std::size_t offset   = (_window_position + buf_size - distance) & mask;

        if (length <= distance && _window_position + length < buf_size
            && distance <= _window_position) [[likely]] {
            std::memcpy(&_window16[_window_position],
                        &_window16[offset],
                        length * sizeof(std::uint16_t));
            _window_position += length;

            // Re-check distance_to_last_marker_byte: scan the just-copied
            // segment for marker bytes.
            std::size_t scanned = 0;
            for (; scanned < length; ++scanned) {
                if (_window16[_window_position - 1u - scanned] > 255u) {
                    _distance_to_last_marker_byte = scanned;
                    return;
                }
            }
            _distance_to_last_marker_byte += length;
            return;
        }
        // Slow path: per-byte copy with wrap.
        const std::size_t per_repeat = std::min(static_cast<std::size_t>(distance), length);
        for (std::size_t copied = 0; copied < length; ) {
            for (std::size_t pos = offset;
                 pos < offset + per_repeat && copied < length;
                 ++pos, ++copied) {
                const std::uint16_t sym = _window16[pos & mask];
                append_to_window_uint16(sym);
            }
        }
    }

    BIOSEQ_DEFLATE_FORCEINLINE void
    resolve_backreference_uint8(std::uint16_t distance, std::size_t length) noexcept {
        std::uint8_t* uint8_window =
            reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_size =
            _window16.size() * sizeof(std::uint16_t);
        const std::size_t mask = uint8_size - 1u;
        const std::size_t offset =
            (_window_position + uint8_size - distance) & mask;

        // FAST PATH: no overlap, no wrap on dest, no wrap on src.
        if (length <= distance && _window_position + length < uint8_size
            && distance <= _window_position) [[likely]] {
            std::memcpy(uint8_window + _window_position,
                        uint8_window + offset, length);
            _window_position += length;
            return;
        }
        // memset shortcut: distance=1 == repeat last byte. Only safe when
        // `wp + length < uint8_size` so memset doesn't overrun the
        // buffer's tail past the wrap point. This was the original cause
        // of the FASTQ-corruption bug at decoded position 12976134:
        // for QUAL strings encoded as a memset of the dominant `G`
        // character, the back-ref straddled the buffer wrap at wp ≈
        // 130k, the memset wrote past `uint8_size`, then subsequent
        // reads at wp = 0..5 picked up the stale tail of the
        // identity-marker pattern.
        if (distance == 1u && _window_position + length < uint8_size) [[unlikely]] {
            std::memset(uint8_window + _window_position,
                        uint8_window[offset], length);
            _window_position += length;
            return;
        }
        // Slow path: per-byte copy with wrap. Handles both
        // overlap (length > distance) and wrap-around at uint8_size.
        const std::size_t per_repeat = std::min(static_cast<std::size_t>(distance), length);
        for (std::size_t copied = 0; copied < length; ) {
            for (std::size_t pos = offset;
                 pos < offset + per_repeat && copied < length;
                 ++pos, ++copied) {
                uint8_window[(_window_position + copied) & mask] =
                    uint8_window[pos & mask];
            }
        }
        _window_position = (_window_position + length) & mask;
    }

    // ---------------------------------------------------------------
    // Last-N-bytes view computation
    // ---------------------------------------------------------------

    [[nodiscard]] std::array<std::span<const std::uint8_t>, 2>
    last_buffers_uint8(std::size_t size) const noexcept {
        const std::uint8_t* uint8_window =
            reinterpret_cast<const std::uint8_t*>(_window16.data());
        const std::size_t total =
            _window16.size() * sizeof(std::uint16_t);
        std::array<std::span<const std::uint8_t>, 2> result{};
        if (size == 0) return result;
        const std::size_t begin =
            (_window_position + total - (size % total)) & (total - 1u);
        if (begin < _window_position) {
            result[0] = std::span<const std::uint8_t>(
                uint8_window + begin, _window_position - begin);
            return result;
        }
        result[0] = std::span<const std::uint8_t>(
            uint8_window + begin, total - begin);
        result[1] = std::span<const std::uint8_t>(
            uint8_window, _window_position);
        return result;
    }

    [[nodiscard]] std::array<std::span<const std::uint16_t>, 2>
    last_buffers_uint16(std::size_t size) const noexcept {
        const std::size_t total = _window16.size();
        std::array<std::span<const std::uint16_t>, 2> result{};
        if (size == 0) return result;
        const std::size_t begin =
            (_window_position + total - (size % total)) & (total - 1u);
        if (begin < _window_position) {
            result[0] = std::span<const std::uint16_t>(
                _window16.data() + begin, _window_position - begin);
            return result;
        }
        result[0] = std::span<const std::uint16_t>(
            _window16.data() + begin, total - begin);
        result[1] = std::span<const std::uint16_t>(
            _window16.data(), _window_position);
        return result;
    }

    // ---------------------------------------------------------------
    // Members
    // ---------------------------------------------------------------

    /// 64 KiB uint16_t (= 128 KiB raw). In marker mode each lane is
    /// either a literal (< 256) or a marker (>= 0x8000). In direct
    /// mode the same memory is interpreted as 128 KiB of uint8_t.
    alignas(64) window_buffer_t _window16{};

    /// Per-block Huffman decoders (dynamic case).
    lit_len_decoder  _literal_decoder{};
    distance_decoder _distance_decoder{};

    std::uint16_t      _uncompressed_size{0};
    std::uint8_t       _padding{0};
    bool               _at_end_of_block{false};
    bool               _is_last_block{false};
    block_compression  _compression_type{block_compression::reserved};
    /// Where the next byte/marker will be appended in the circular
    /// buffer (uint16_t-indexed in marker mode, uint8_t-indexed in
    /// direct mode).
    std::size_t        _window_position{0};
    /// True until `set_initial_window` is called.
    bool               _contains_markers{true};
    /// Cumulative decoded bytes across blocks (resets only on `reset`
    /// with prior_window).
    std::size_t        _decoded_bytes{0};
    /// Bytes since the last marker byte was emitted. When this exceeds
    /// `maximum_window_size`, downstream back-refs can no longer reach the
    /// unknown prefix, so we narrow to direct mode.
    std::size_t        _distance_to_last_marker_byte{0};
};

}  // namespace bioseq::io::compression
