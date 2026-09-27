/**
 * @file deflate.cppm
 * @brief Decodes DEFLATE blocks with references to an unknown prior window.
 *
 * When the preceding bytes are known, output contains ordinary bytes. Otherwise,
 * 16-bit entries hold literals or positions in that prior window. A worker can
 * decode its chunk before the preceding chunk has finished.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>
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

// DEFLATE constants

/// 32 KiB, fixed by RFC 1951 §3.2.5 (max back-reference distance).
inline constexpr std::size_t maximum_window_size = 32 * 1024;
/// Number of 16-bit window entries. Its power-of-two size allows masked wrapping.
inline constexpr std::size_t predecoded_buffer_size = 2 * maximum_window_size;
inline constexpr std::size_t predecoded_buffer_mask = predecoded_buffer_size - 1;
static_assert((predecoded_buffer_size & predecoded_buffer_mask) == 0,
              "predecoded_buffer_size must be a power of two for AND-wrap.");

/// Largest stored block, whose byte count is encoded in 16 bits.
inline constexpr std::size_t maximum_uncompressed_size = std::numeric_limits<std::uint16_t>::max();

/// RFC 1951 max code length (precode is 7).
inline constexpr std::uint8_t maximum_code_length = 15;
inline constexpr std::uint8_t precode_bit_count = 3;
inline constexpr std::size_t maximum_precode_count = 19;

/// Symbol order used when reading the DEFLATE code-length alphabet.
/// Entry i names the symbol whose length is encoded in position i.
inline constexpr std::array<std::uint8_t, maximum_precode_count> precode_alphabet = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/// Maximum literal/length alphabet accepted from a dynamic block header.
/// The combined length buffer below includes extra room for repeat codes.
inline constexpr std::size_t maximum_literal_or_length_symbols = 286;
inline constexpr std::size_t maximum_distance_symbol_count = 30;
inline constexpr std::size_t literal_and_distance_code_length_buffer_size =
    maximum_literal_or_length_symbols + maximum_distance_symbol_count + 256;

inline constexpr std::uint16_t end_of_block_symbol = 256;
inline constexpr std::size_t maximum_run_length = 258;

/// Builds base lengths for the length-code groups that also read extra bits.
[[nodiscard]] consteval std::array<std::uint16_t, 24> build_length_lut() noexcept {
    std::array<std::uint16_t, 24> lut{};
    for (std::uint16_t i = 0; i < 24; ++i) {
        const std::uint16_t extra = i / 4u;
        lut[i] = static_cast<std::uint16_t>(3u + (1u << (extra + 2u)) + ((i % 4u) << extra));
    }
    return lut;
}
inline constexpr auto length_table = build_length_lut();

/// Distance code → base distance.
[[nodiscard]] consteval std::array<std::uint16_t, 30> build_distance_lut() noexcept {
    std::array<std::uint16_t, 30> lut{};
    for (std::uint16_t i = 0; i < 4; ++i)
        lut[i] = static_cast<std::uint16_t>(i + 1);
    for (std::uint16_t i = 4; i < 30; ++i) {
        const std::uint16_t extra = static_cast<std::uint16_t>((i - 2u) / 2u);
        lut[i] = static_cast<std::uint16_t>(1u + (1u << (extra + 1u)) + ((i % 2u) << extra));
    }
    return lut;
}
inline constexpr auto distance_table = build_distance_lut();

// Decode results and output views

/// Compression mode encoded by a DEFLATE block header.
enum class block_compression : std::uint8_t {
    uncompressed = 0,
    fixed_huffman = 1,
    dynamic_huffman = 2,
    reserved = 3,
};

/// Result of decoding a DEFLATE header or body segment.
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

/// Return a stable diagnostic name for a decode status.
[[nodiscard]] constexpr const char* decode_status_name(decode_status status) noexcept {
    switch (status) {
    case decode_status::ok:
        return "ok";
    case decode_status::end_of_block:
        return "end_of_block";
    case decode_status::end_of_stream:
        return "end_of_stream";
    case decode_status::invalid_code:
        return "invalid_code";
    case decode_status::invalid_distance:
        return "invalid_distance";
    case decode_status::out_of_input:
        return "out_of_input";
    case decode_status::invalid_huffman_code:
        return "invalid_huffman_code";
    case decode_status::invalid_code_length_backreference:
        return "invalid_cl_backreference";
    case decode_status::exceeded_literal_range:
        return "exceeded_literal_range";
    case decode_status::exceeded_distance_range:
        return "exceeded_distance_range";
    case decode_status::exceeded_window_range:
        return "exceeded_window_range";
    case decode_status::invalid_code_lengths:
        return "invalid_code_lengths";
    case decode_status::length_checksum_mismatch:
        return "length_checksum_mismatch";
    case decode_status::non_zero_padding:
        return "non_zero_padding";
    case decode_status::invalid_compression:
        return "invalid_compression";
    case decode_status::generic_error:
        return "generic_error";
    }
    return "?";
}

/// Borrowed output from `deflate_block_decoder::read`, split at a ring-buffer wrap.
/// A result uses either byte spans or 16-bit spans containing literals and markers.
/// Consume the view before the decoder overwrites or rearranges its window.
struct decoded_data_view {
    /// Resolved bytes (direct mode).
    std::array<std::span<const std::uint8_t>, 2> data{};
    /// Literals and references to the prior window, stored as 16-bit entries.
    /// Values at least `maximum_window_size` encode an offset in that window.
    std::array<std::span<const std::uint16_t>, 2> data_with_markers{};

    /// Return the total number of resolved and marker lanes.
    [[nodiscard]] std::size_t size() const noexcept {
        return data[0].size() + data[1].size() + data_with_markers[0].size() +
               data_with_markers[1].size();
    }
    /// Return the number of resolved byte lanes.
    [[nodiscard]] std::size_t data_size() const noexcept {
        return data[0].size() + data[1].size();
    }
    /// Returns the number of entries in marker-mode storage.
    [[nodiscard]] std::size_t data_with_markers_size() const noexcept {
        return data_with_markers[0].size() + data_with_markers[1].size();
    }
    /// Reports whether this view uses marker-mode storage.
    [[nodiscard]] bool contains_markers() const noexcept {
        return data_with_markers_size() > 0;
    }
};

// Decoder helpers

namespace detail {

/// Creates the starting ring buffer for an unknown 32 KiB prior window.
/// Its trailing entries encode positions in that window. A back-reference
/// copies these markers until the real bytes are supplied. The output half
/// starts at zero.
[[nodiscard]] inline const std::array<std::uint16_t, predecoded_buffer_size>&
get_initial_marker_buffer() noexcept {
    static const auto initial_marker_buffer = []() noexcept {
        std::array<std::uint16_t, predecoded_buffer_size> result{};
        for (std::size_t i = 0; i < maximum_window_size; ++i) {
            result[maximum_window_size + i] = static_cast<std::uint16_t>(maximum_window_size + i);
        }
        return result;
    }();
    return initial_marker_buffer;
}

/// Expands the literal and distance code lengths from a dynamic header.
/// Repeat codes can write into the extra buffer padding. The final count
/// check rejects repeats that extend beyond the declared alphabet.
[[nodiscard]] inline decode_status read_distance_and_literal_code_lengths(
    std::array<std::uint8_t, literal_and_distance_code_length_buffer_size>& code_lengths,
    bit_reader& reader,
    const precode_decoder& precode,
    std::size_t count) noexcept {
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
            // Write the longest possible repeat before reading its actual length.
            // Later entries overwrite the unused tail, and padding keeps it in bounds.
            code_lengths[i + 0] = last;
            code_lengths[i + 1] = last;
            code_lengths[i + 2] = last;
            code_lengths[i + 3] = last;
            code_lengths[i + 4] = last;
            code_lengths[i + 5] = last;
            const auto extra = static_cast<std::size_t>(reader.read<2>());
            i += extra + 3u;
        } else if (code == 17) {
            // A zero repeat can cover up to ten code lengths.
            const auto extra = static_cast<std::size_t>(reader.read<3>());
            // Clear the full repeat range before advancing by the encoded count.
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
            // Expand the encoded zero run, which can contain up to 138 lengths.
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
    return i == count ? decode_status::ok : decode_status::exceeded_literal_range;
}

} // namespace detail

// DEFLATE block decoder

/// Decodes consecutive DEFLATE blocks using one shared history window.
/// Read the next header at each block boundary. Use `reset()` when starting
/// an independent chunk or member, then supply the prior window if it is known.
class deflate_block_decoder {
  public:
    using window_buffer_t = std::array<std::uint16_t, predecoded_buffer_size>;

    /// Constructs a decoder with zeroed window storage. Call `reset()` before use.
    deflate_block_decoder() noexcept = default;

    /// Decoder instances exclusively own their circular window and tables.
    deflate_block_decoder(const deflate_block_decoder&) = delete;

    /// Decoder state cannot be copied between instances.
    deflate_block_decoder& operator=(const deflate_block_decoder&) = delete;

    /// Decoder state cannot be transferred between instances.
    deflate_block_decoder(deflate_block_decoder&&) = delete;

    /// Decoder state cannot replace another instance through move assignment.
    deflate_block_decoder& operator=(deflate_block_decoder&&) = delete;

    /// Release the decoder's inline window and Huffman tables.
    ~deflate_block_decoder() = default;

    // State queries

    /// Return whether the current block body has been exhausted.
    [[nodiscard]] bool is_end_of_block() const noexcept {
        return _at_end_of_block;
    }
    /// Return whether the final block body has been exhausted.
    [[nodiscard]] bool is_end_of_stream() const noexcept {
        return _at_end_of_block && _is_last_block;
    }
    /// Return the current block's BFINAL flag.
    [[nodiscard]] bool is_last_block() const noexcept {
        return _is_last_block;
    }
    /// Reports whether the decoder still uses 16-bit marker-mode storage.
    [[nodiscard]] bool contains_marker_bytes() const noexcept {
        return _contains_markers;
    }
    /// Return the current block's compression mode.
    [[nodiscard]] block_compression compression_type() const noexcept {
        return _compression_type;
    }
    /// Returns bytes decoded since reset, including history supplied before decoding starts.
    [[nodiscard]] std::size_t decoded_bytes() const noexcept {
        return _decoded_bytes;
    }
    /// Return the current write position in the circular window.
    [[nodiscard]] std::size_t window_position() const noexcept {
        return _window_position;
    }
    /// Returns the padding bits read before an uncompressed block.
    [[nodiscard]] std::uint8_t padding() const noexcept {
        return _padding;
    }
    /// Return the declared byte size of an uncompressed block.
    [[nodiscard]] std::size_t uncompressed_size() const noexcept {
        return _compression_type == block_compression::uncompressed ? _uncompressed_size : 0;
    }

    // Reset and prior-window setup

    /// Starts a fresh decoding interval and clears the block state.
    /// A supplied prior window enables direct byte output. An empty argument
    /// starts with markers for unknown history. Call `set_initial_window({})`
    /// afterwards when the interval is known to have no preceding bytes.
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
            // Restore the marker tail before decoding a chunk with unknown history.
            std::memcpy(
                _window16.data(), detail::get_initial_marker_buffer().data(), sizeof(_window16));
        } else {
            set_initial_window(prior_window);
        }
    }

    /// Supplies preceding bytes, resolves saved markers, and switches to byte storage.
    /// Markers outside the supplied window become zero. An empty window selects
    /// direct mode with no history on a fresh decoder.
    /// @pre `initial_window` contains at most `maximum_window_size` bytes.
    void set_initial_window(std::span<const std::uint8_t> initial_window = {}) noexcept {
        if (!_contains_markers)
            return;

        // A fresh decoder can copy the prior bytes directly into byte storage.
        if (_decoded_bytes == 0 && _window_position == 0) {
            if (!initial_window.empty()) {
                std::uint8_t* uint8_window = reinterpret_cast<std::uint8_t*>(_window16.data());
                std::memcpy(uint8_window, initial_window.data(), initial_window.size());
                _window_position = initial_window.size();
                _decoded_bytes = initial_window.size();
            }
            _contains_markers = false;
            return;
        }

        // Discard unwritten entries and replace saved markers with prior bytes.
        // The following copy rearranges the ring for direct byte output.
        for (std::size_t i = 0; _decoded_bytes + i < _window16.size(); ++i) {
            _window16[(_window_position + i) & predecoded_buffer_mask] = 0;
        }
        for (std::size_t i = 0; i < _window16.size(); ++i) {
            const std::uint16_t value = _window16[i];
            if (value < 256u)
                continue;
            // Values between literal bytes and the marker range are invalid.
            // Replace them with zero.
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

        // Put the oldest entry first and narrow the resolved values to bytes.
        std::array<std::uint8_t, predecoded_buffer_size> conflated{};
        for (std::size_t i = 0; i < _window16.size(); ++i) {
            const std::uint16_t value = _window16[(i + _window_position) & predecoded_buffer_mask];
            conflated[i] = static_cast<std::uint8_t>(value & 0xFFu);
        }
        std::uint8_t* uint8_window = reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_window_size = _window16.size() * sizeof(std::uint16_t);
        // Place the saved history at the end of byte storage, leaving the next
        // write at offset zero.
        std::memcpy(uint8_window + (uint8_window_size - conflated.size()),
                    conflated.data(),
                    conflated.size());
        _window_position = 0;
        _contains_markers = false;
    }

    // Block headers

    /// Reads a block header and prepares its lengths or Huffman tables for decoding.
    [[nodiscard]] decode_status read_header(bit_reader& reader) noexcept {
        if (reader.bits_buffered() < 3)
            reader.refill();
        if (reader.bits_buffered() < 3)
            return decode_status::out_of_input;
        const std::uint64_t bfinal = reader.read<1>();
        const std::uint64_t btype = reader.read<2>();
        _is_last_block = (bfinal != 0);
        _at_end_of_block = false;

        switch (btype) {
        case 0: {
            _compression_type = block_compression::uncompressed;
            // This decoder requires zero padding up to the next byte boundary.
            const std::size_t pos = reader.bit_position();
            const std::size_t excess = pos % 8;
            if (excess != 0) {
                const std::size_t to_read = 8u - excess;
                if (reader.bits_buffered() < to_read)
                    reader.refill();
                if (reader.bits_buffered() < to_read) {
                    return decode_status::out_of_input;
                }
                _padding = static_cast<std::uint8_t>(reader.read_bits(to_read));
                if (_padding != 0) {
                    return decode_status::non_zero_padding;
                }
            } else {
                _padding = 0;
            }
            if (reader.bits_buffered() < 32)
                reader.refill();
            if (reader.bits_buffered() < 32) {
                return decode_status::out_of_input;
            }
            _uncompressed_size = static_cast<std::uint16_t>(reader.read<16>());
            const std::uint16_t nlen = static_cast<std::uint16_t>(reader.read<16>());
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
    [[nodiscard]] decode_status read_dynamic_huffman(bit_reader& reader) noexcept {
        if (reader.bits_buffered() < 14)
            reader.refill();
        if (reader.bits_buffered() < 14)
            return decode_status::out_of_input;
        const std::size_t literal_count = static_cast<std::size_t>(reader.read<5>()) + 257u;
        if (literal_count > maximum_literal_or_length_symbols) {
            return decode_status::exceeded_literal_range;
        }
        const std::size_t distance_count = static_cast<std::size_t>(reader.read<5>()) + 1u;
        if (distance_count > maximum_distance_symbol_count) {
            return decode_status::exceeded_distance_range;
        }
        const std::size_t precode_count = static_cast<std::size_t>(reader.read<4>()) + 4u;

        // Precode code-length table (3 bits each, max 19 entries).
        std::array<std::uint8_t, maximum_precode_count> precode_cl{};
        for (std::size_t i = 0; i < precode_count; ++i) {
            if (reader.bits_buffered() < precode_bit_count)
                reader.refill();
            precode_cl[precode_alphabet[i]] =
                static_cast<std::uint8_t>(reader.read<precode_bit_count>());
        }

        const auto precode_or = precode_decoder::build(
            std::span<const std::uint8_t>{precode_cl.data(), precode_cl.size()});
        if (!precode_or.is_valid())
            return decode_status::invalid_code_lengths;

        // Read both alphabets into one buffer. Extra padding keeps repeat-code
        // writes in bounds until their final count is checked.
        std::array<std::uint8_t, literal_and_distance_code_length_buffer_size> literal_cl{};
        if (auto err = detail::read_distance_and_literal_code_lengths(
                literal_cl, reader, precode_or, literal_count + distance_count);
            err != decode_status::ok) {
            return err;
        }

        // Verify EOB symbol exists.
        if (literal_cl[end_of_block_symbol] == 0) {
            return decode_status::invalid_code_lengths;
        }

        _literal_decoder =
            lit_len_decoder::build(std::span<const std::uint8_t>{literal_cl.data(), literal_count});
        if (!_literal_decoder.is_valid()) {
            return decode_status::invalid_code_lengths;
        }
        _distance_decoder = distance_decoder::build(
            std::span<const std::uint8_t>{literal_cl.data() + literal_count, distance_count});
        if (!_distance_decoder.is_valid()) {
            return decode_status::invalid_code_lengths;
        }
        return decode_status::ok;
    }

    // Decode output

    /// Decodes output from the current block into borrowed window spans.
    /// `n_max_to_decode` is a stopping target. A back-reference can extend
    /// past it, and a stored block is read in full. At a block boundary,
    /// call `read_header` before reading again.
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
            std::tie(produced, status) = read_compressed_marker(reader, n_max_to_decode);
            // Once unknown history can no longer affect the current window,
            // switch to byte storage and avoid later marker replacement.
            if ((_distance_to_last_marker_byte >= _window16.size()) ||
                ((_distance_to_last_marker_byte >= maximum_window_size) &&
                 (_distance_to_last_marker_byte == _decoded_bytes))) {
                set_initial_window();
                view.data = last_buffers_uint8(produced);
            } else {
                view.data_with_markers = last_buffers_uint16(produced);
            }
        } else {
            std::tie(produced, status) = read_compressed_direct(reader, n_max_to_decode);
            view.data = last_buffers_uint8(produced);
        }
        (void)before;
        return {view, status};
    }

  private:
    // Stored blocks

    /// Decode the current uncompressed block into marker or direct storage.
    [[nodiscard]] std::pair<decoded_data_view, decode_status>
    read_uncompressed(bit_reader& reader) noexcept {
        // Reader was byte-aligned at the end of the header.
        const std::size_t total = _uncompressed_size;
        if (_contains_markers) {
            // 16-bit append into the marker buffer.
            for (std::size_t i = 0; i < total; ++i) {
                if (reader.bits_buffered() < 8)
                    reader.refill();
                if (reader.bits_buffered() < 8) {
                    _decoded_bytes += i;
                    return {decoded_data_view{}, decode_status::out_of_input};
                }
                const std::uint8_t byte = static_cast<std::uint8_t>(reader.read<8>());
                append_to_window_uint16(static_cast<std::uint16_t>(byte));
            }
            _at_end_of_block = true;
            _decoded_bytes += total;

            decoded_data_view view;
            // Stored blocks contain literal bytes only. Keep the current 16-bit
            // representation so the caller can narrow each value when copying it.
            view.data_with_markers = last_buffers_uint16(total);
            return {view, decode_status::ok};
        }

        // Read into byte storage, splitting the writes if the ring wraps.
        std::uint8_t* uint8_window = reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_size = _window16.size() * sizeof(std::uint16_t);
        const std::size_t mask = uint8_size - 1u;

        std::size_t produced = 0;
        while (produced < total) {
            const std::size_t left = total - produced;
            const std::size_t at = _window_position;
            // Copy in two segments if wrapping.
            const std::size_t span_a = std::min(left, uint8_size - at);
            for (std::size_t i = 0; i < span_a; ++i) {
                if (reader.bits_buffered() < 8)
                    reader.refill();
                if (reader.bits_buffered() < 8) {
                    _decoded_bytes += produced;
                    _window_position = (at + i) & mask;
                    return {decoded_data_view{}, decode_status::out_of_input};
                }
                uint8_window[at + i] = static_cast<std::uint8_t>(reader.read<8>());
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

    // Compressed blocks in marker mode

    /// Decode compressed symbols while keeping unresolved window markers.
    [[nodiscard]] std::pair<std::size_t, decode_status>
    read_compressed_marker(bit_reader& reader, std::size_t n_max_to_decode) noexcept {
        // Cap to leave room for a max-length back-ref (258).
        const std::size_t cap = std::min(n_max_to_decode, _window16.size() - maximum_run_length);

        const auto& lit_decoder = _compression_type == block_compression::fixed_huffman
                                      ? fixed_literal_length_decoder
                                      : _literal_decoder;

        std::size_t produced = 0;
        while (produced < cap) {
            // A length/distance pair can consume 48 bits, including extra bits.
            // Refill before each symbol so the entire pair is available.
            if (reader.bits_buffered() < 48) [[unlikely]]
                reader.refill();

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

    /// Decode compressed symbols against an already resolved circular window.
    [[nodiscard]] std::pair<std::size_t, decode_status>
    read_compressed_direct(bit_reader& reader, std::size_t n_max_to_decode) noexcept {
        const std::size_t uint8_size = _window16.size() * sizeof(std::uint16_t);
        const std::size_t cap = std::min(n_max_to_decode, uint8_size - maximum_run_length);

        const auto& lit_decoder = _compression_type == block_compression::fixed_huffman
                                      ? fixed_literal_length_decoder
                                      : _literal_decoder;

        std::size_t produced = 0;
        while (produced < cap) {
            if (reader.bits_buffered() < 48) [[unlikely]]
                reader.refill();

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
            // A direct-mode back-reference must stay within the bytes available so far.
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

    // Length and distance helpers

    /// Converts a length symbol and its extra bits to the number of bytes to copy.
    [[nodiscard]] BIOSEQ_DEFLATE_FORCEINLINE std::size_t read_length(bit_reader& reader,
                                                                     std::uint16_t code) noexcept {
        // RFC 1951 §3.2.5: codes 257..285.
        if (code <= 264)
            return static_cast<std::size_t>(code) - 257u + 3u;
        if (code == 285)
            return 258u;
        const std::uint16_t adj = static_cast<std::uint16_t>(code - 261u);
        const std::uint8_t extra_bits = static_cast<std::uint8_t>(adj / 4u);
        const std::uint64_t extra = reader.read_bits(extra_bits);
        return static_cast<std::size_t>(length_table[adj]) + static_cast<std::size_t>(extra);
    }

    /// Decode one distance symbol and its extra bits.
    [[nodiscard]] BIOSEQ_DEFLATE_FORCEINLINE std::pair<std::uint16_t, decode_status>
    read_distance(bit_reader& reader) noexcept {
        std::uint16_t distance;
        if (_compression_type == block_compression::fixed_huffman) {
            // Fixed distance codes have five bits in Huffman code order. Reverse
            // the bit-reader value to recover the distance symbol.
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

        if (distance <= 3u)
            return {static_cast<std::uint16_t>(distance + 1u), decode_status::ok};
        const std::uint8_t extra_bits = static_cast<std::uint8_t>((distance - 2u) / 2u);
        const std::uint64_t extra = reader.read_bits(extra_bits);
        return {static_cast<std::uint16_t>(distance_table[distance] + extra), decode_status::ok};
    }

    /// Reverse the five low bits used by the fixed distance alphabet.
    [[nodiscard]] static constexpr std::uint8_t reverse_bits_5(std::uint8_t value) noexcept {
        std::uint8_t result = 0;
        for (int b = 0; b < 5; ++b)
            result |= static_cast<std::uint8_t>(((value >> b) & 1u) << (4 - b));
        return result;
    }

    // Window writes

    /// Append one literal or marker lane to the marker-mode window.
    BIOSEQ_DEFLATE_FORCEINLINE void append_to_window_uint16(std::uint16_t symbol) noexcept {
        if (symbol > 255u) {
            _distance_to_last_marker_byte = 0;
        } else {
            ++_distance_to_last_marker_byte;
        }
        _window16[_window_position] = symbol;
        _window_position = (_window_position + 1u) & predecoded_buffer_mask;
    }

    /// Append one resolved byte to the direct-mode window.
    BIOSEQ_DEFLATE_FORCEINLINE void append_to_window_uint8(std::uint8_t byte) noexcept {
        std::uint8_t* uint8_window = reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_size = _window16.size() * sizeof(std::uint16_t);
        uint8_window[_window_position] = byte;
        _window_position = (_window_position + 1u) & (uint8_size - 1u);
    }

    /// Expand one back-reference into marker-mode storage.
    BIOSEQ_DEFLATE_FORCEINLINE void resolve_backreference_uint16(std::uint16_t distance,
                                                                 std::size_t length) noexcept {
        const std::size_t buf_size = _window16.size();
        const std::size_t mask = predecoded_buffer_mask;
        const std::size_t offset = (_window_position + buf_size - distance) & mask;

        if (length <= distance && _window_position + length < buf_size &&
            distance <= _window_position) [[likely]] {
            std::memcpy(
                &_window16[_window_position], &_window16[offset], length * sizeof(std::uint16_t));
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
        // Copy repeated entries one at a time to handle overlap and ring wrapping.
        const std::size_t per_repeat = std::min(static_cast<std::size_t>(distance), length);
        for (std::size_t copied = 0; copied < length;) {
            for (std::size_t pos = offset; pos < offset + per_repeat && copied < length;
                 ++pos, ++copied) {
                const std::uint16_t sym = _window16[pos & mask];
                append_to_window_uint16(sym);
            }
        }
    }

    /// Expand one back-reference into direct-mode storage.
    BIOSEQ_DEFLATE_FORCEINLINE void resolve_backreference_uint8(std::uint16_t distance,
                                                                std::size_t length) noexcept {
        std::uint8_t* uint8_window = reinterpret_cast<std::uint8_t*>(_window16.data());
        const std::size_t uint8_size = _window16.size() * sizeof(std::uint16_t);
        const std::size_t mask = uint8_size - 1u;
        const std::size_t offset = (_window_position + uint8_size - distance) & mask;

        // Copy the whole range when it neither overlaps nor wraps.
        if (length <= distance && _window_position + length < uint8_size &&
            distance <= _window_position) [[likely]] {
            std::memcpy(uint8_window + _window_position, uint8_window + offset, length);
            _window_position += length;
            return;
        }
        // A distance of one repeats the previous byte. Use memset only while
        // the entire repeat fits before the ring boundary.
        if (distance == 1u && _window_position + length < uint8_size) [[unlikely]] {
            std::memset(uint8_window + _window_position, uint8_window[offset], length);
            _window_position += length;
            return;
        }
        // Slow path: per-byte copy with wrap. Handles both
        // overlap (length > distance) and wrap-around at uint8_size.
        const std::size_t per_repeat = std::min(static_cast<std::size_t>(distance), length);
        for (std::size_t copied = 0; copied < length;) {
            for (std::size_t pos = offset; pos < offset + per_repeat && copied < length;
                 ++pos, ++copied) {
                uint8_window[(_window_position + copied) & mask] = uint8_window[pos & mask];
            }
        }
        _window_position = (_window_position + length) & mask;
    }

    // Views of the most recent output

    /// Return the most recently appended direct bytes as at most two spans.
    [[nodiscard]] std::array<std::span<const std::uint8_t>, 2>
    last_buffers_uint8(std::size_t size) const noexcept {
        const std::uint8_t* uint8_window = reinterpret_cast<const std::uint8_t*>(_window16.data());
        const std::size_t total = _window16.size() * sizeof(std::uint16_t);
        std::array<std::span<const std::uint8_t>, 2> result{};
        if (size == 0)
            return result;
        const std::size_t begin = (_window_position + total - (size % total)) & (total - 1u);
        if (begin < _window_position) {
            result[0] =
                std::span<const std::uint8_t>(uint8_window + begin, _window_position - begin);
            return result;
        }
        result[0] = std::span<const std::uint8_t>(uint8_window + begin, total - begin);
        result[1] = std::span<const std::uint8_t>(uint8_window, _window_position);
        return result;
    }

    /// Returns the most recent marker-mode entries as at most two spans.
    [[nodiscard]] std::array<std::span<const std::uint16_t>, 2>
    last_buffers_uint16(std::size_t size) const noexcept {
        const std::size_t total = _window16.size();
        std::array<std::span<const std::uint16_t>, 2> result{};
        if (size == 0)
            return result;
        const std::size_t begin = (_window_position + total - (size % total)) & (total - 1u);
        if (begin < _window_position) {
            result[0] =
                std::span<const std::uint16_t>(_window16.data() + begin, _window_position - begin);
            return result;
        }
        result[0] = std::span<const std::uint16_t>(_window16.data() + begin, total - begin);
        result[1] = std::span<const std::uint16_t>(_window16.data(), _window_position);
        return result;
    }

    // Decoder state

    /// Ring with 65,536 16-bit entries for literals and unresolved markers.
    /// Direct mode reuses the same memory as a 128 KiB byte ring.
    alignas(64) window_buffer_t _window16{};

    /// Per-block Huffman decoders (dynamic case).
    lit_len_decoder _literal_decoder{};
    distance_decoder _distance_decoder{};

    std::uint16_t _uncompressed_size{0};
    std::uint8_t _padding{0};
    bool _at_end_of_block{false};
    bool _is_last_block{false};
    block_compression _compression_type{block_compression::reserved};
    /// Where the next byte/marker will be appended in the circular
    /// buffer (uint16_t-indexed in marker mode, uint8_t-indexed in
    /// direct mode).
    std::size_t _window_position{0};
    /// True while the window uses the 16-bit marker representation.
    bool _contains_markers{true};
    /// Bytes decoded since reset, plus history supplied before decoding starts.
    std::size_t _decoded_bytes{0};
    /// Consecutive bytes written since the last unresolved marker.
    /// The read path uses this count to decide when the saved history
    /// can safely switch to direct byte storage.
    std::size_t _distance_to_last_marker_byte{0};
};

} // namespace bioseq::io::compression
