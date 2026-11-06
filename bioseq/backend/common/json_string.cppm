/** @file json_string.cppm
 *  @brief Bounded JSON string escaping shared by PostgreSQL and neutral IO. */
module;
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module bioseq.common.json_string;

export namespace bioseq::common {

/** JSON output selection: preserve UTF-8 bytes, or validate and emit ASCII escapes. */
enum class json_escape_mode : std::uint8_t { utf8, ascii };

/** A synchronous sink must consume or copy the complete span before returning. */
template<class Sink>
concept json_byte_sink = requires(Sink& sink, std::span<const char> bytes) {
    { sink.append(bytes) } -> std::same_as<std::expected<void, std::string>>;
};

/** UTF-8 decoder state retained across input fragments of one JSON string. */
struct json_utf8_state {
    std::uint32_t scalar{};
    std::uint32_t minimum{};
    std::uint8_t continuation_bytes{};
    std::size_t byte_offset{};
};

/** Optional scalar-ABI SIMD scanner: first byte needing escaping/UTF-8 processing. */
using json_safe_prefix_scanner = std::size_t (*)(const char*, std::size_t);

/** Existing PG users own their complete string; IO uses its bounded sink instead. */
struct json_string_sink {
    std::string& output;
    /** Append a borrowed span synchronously to the caller-owned string. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const char> bytes) {
        output.append(bytes.data(), bytes.size());
        return {};
    }
};

/** Validate one UTF-8 byte and yield a completed scalar without retaining input.
 * The caller advances byte_offset; continuation state is reusable across windows. */
[[nodiscard]] inline std::expected<std::optional<std::uint32_t>, std::string>
consume_json_utf8_byte(json_utf8_state& state, unsigned char character) {
    if (state.continuation_bytes != 0) {
        if (character < 0x80u || character > 0xBFu)
            return std::unexpected("invalid UTF-8 continuation in JSON string");
        state.scalar = (state.scalar << 6) | (character & 0x3Fu);
        --state.continuation_bytes;
        if (state.continuation_bytes != 0) return std::optional<std::uint32_t>{};
        if (state.scalar < state.minimum || state.scalar > 0x10FFFFu
            || (state.scalar >= 0xD800u && state.scalar <= 0xDFFFu))
            return std::unexpected("invalid UTF-8 scalar in JSON string");
        return std::optional{state.scalar};
    }
    if (character < 0x80u) return std::optional{static_cast<std::uint32_t>(character)};
    if (character >= 0xC2u && character <= 0xDFu) {
        state.scalar = character & 0x1Fu; state.minimum = 0x80u; state.continuation_bytes = 1;
    } else if (character >= 0xE0u && character <= 0xEFu) {
        state.scalar = character & 0x0Fu; state.minimum = 0x800u; state.continuation_bytes = 2;
    } else if (character >= 0xF0u && character <= 0xF4u) {
        state.scalar = character & 0x07u; state.minimum = 0x10000u; state.continuation_bytes = 3;
    } else return std::unexpected("invalid UTF-8 leading byte in JSON string");
    return std::optional<std::uint32_t>{};
}

/** Write one UTF-16 code unit as a JSON Unicode escape. */
template<json_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_json_code_unit(
    Sink& sink, std::uint32_t code_unit) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    const std::array<char, 6> escape{'\\', 'u', digits[(code_unit >> 12) & 15u],
        digits[(code_unit >> 8) & 15u], digits[(code_unit >> 4) & 15u], digits[code_unit & 15u]};
    return sink.append(escape);
}

/** Escape one input fragment without quotes. The caller retains state between tiles.
 *  ASCII mode rejects malformed UTF-8; finish_json_string checks truncated input.
 *  A supplied SIMD scanner consumes safe runs without scalar per-byte dispatch. */
template<json_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_json_string_fragment(
    Sink& sink, std::span<const char> input, json_utf8_state& state,
    json_escape_mode mode, json_safe_prefix_scanner scanner = nullptr) {
    std::size_t position = 0;
    while (position < input.size()) {
        if (state.continuation_bytes == 0 && scanner != nullptr) {
            const auto safe_count = scanner(input.data() + position, input.size() - position);
            if (safe_count > input.size() - position)
                return std::unexpected("JSON safe-prefix scanner returned an invalid length");
            if (safe_count != 0) {
                if (auto result = sink.append(input.subspan(position, safe_count)); !result) return result;
                position += safe_count;
                state.byte_offset += safe_count;
                if (position == input.size()) break;
            }
        }
        const unsigned char character = static_cast<unsigned char>(input[position++]);
        ++state.byte_offset;
        if (mode == json_escape_mode::ascii && (state.continuation_bytes != 0 || character >= 0x80u)) {
            const auto decoded = consume_json_utf8_byte(state, character);
            if (!decoded) return std::unexpected(decoded.error());
            if (!*decoded) continue;
            const auto scalar = **decoded;
            if (scalar <= 0xFFFFu) {
                if (auto result = append_json_code_unit(sink, scalar); !result) return result;
            } else {
                const auto supplementary = scalar - 0x10000u;
                if (auto result = append_json_code_unit(sink, 0xD800u + (supplementary >> 10)); !result) return result;
                if (auto result = append_json_code_unit(sink, 0xDC00u + (supplementary & 0x3FFu)); !result) return result;
            }
            continue;
        }
        if (character == '"' || character == '\\') {
            const std::array<char, 2> escape{'\\', static_cast<char>(character)};
            if (auto result = sink.append(escape); !result) return result;
        } else if (character < 0x20u || (mode == json_escape_mode::ascii && character == 0x7Fu)) {
            if (auto result = append_json_code_unit(sink, character); !result) return result;
        } else {
            const char literal = static_cast<char>(character);
            if (auto result = sink.append({&literal, 1}); !result) return result;
        }
    }
    return {};
}

/** Require a complete Unicode scalar at the end of a JSON string. */
[[nodiscard]] inline std::expected<void, std::string> finish_json_string(const json_utf8_state& state) {
    if (state.continuation_bytes != 0)
        return std::unexpected("truncated UTF-8 in JSON string at byte " + std::to_string(state.byte_offset));
    return {};
}

/** Append UTF-8 JSON text for existing string-owning callers, without quotes. */
inline void append_json_escaped(std::string& output, std::string_view text) {
    json_string_sink sink{output};
    json_utf8_state state;
    // The string sink and UTF-8 pass-through mode have no recoverable codec errors.
    static_cast<void>(append_json_string_fragment(sink, {text.data(), text.size()}, state,
        json_escape_mode::utf8));
}

} // namespace bioseq::common
