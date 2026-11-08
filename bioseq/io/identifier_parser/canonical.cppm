/** @file canonical.cppm
 *  @brief Lossless bracket headers shared by sequence readers and writers. */
module;
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.io.identifier_parser.canonical;

import bioseq.common.json_string;
import bioseq.parallelism.memory_admission;

import bioseq.io.sinks;
import bioseq.io.simd;

export namespace bioseq::io::identifier_parser {

/** A header metadata value; a disengaged value is SQL NULL, not empty text. */
struct canonical_field_view {
    std::string_view name;
    std::optional<std::string_view> value;
    char discriminator{'s'};
};

/** An admitted decoded field; views remain stable across owner moves. */
struct canonical_field {
    admitted_byte_buffer name_storage;
    admitted_byte_buffer value_storage;
    std::string_view name;
    std::optional<std::string_view> value;
    char discriminator{'s'};
};

/** Select only the materialized fields required by the current parser consumer. */
enum class canonical_parse_mode : std::uint8_t { complete, shape, routing };

/** Exact original identifier and preservation data with admitted/file-backed ownership. */
struct canonical_header {
    admitted_byte_buffer header_storage;
    admitted_byte_buffer alphabet_storage;
    admitted_byte_buffer payload_storage;
    std::string_view header;
    bool header_is_empty{true};
    std::string_view alphabet;
    std::string_view payload;
    parallelism::memory_permit fields_permit;
    std::vector<canonical_field> fields;

    /** Grow only the scalar descriptor array, admitting the simultaneous old/new capacity. */
    [[nodiscard]] std::expected<void, std::string> append_field(canonical_field field) {
        if (fields.size() == fields.capacity()) {
            if (fields.capacity() > std::numeric_limits<std::size_t>::max() / 3u / sizeof(canonical_field))
                return std::unexpected("canonical field descriptor capacity overflows");
            const auto capacity = fields.capacity() == 0u ? 4u : fields.capacity() * 2u;
            const auto peak = (capacity + fields.capacity()) * sizeof(canonical_field);
            bool admitted = fields_permit && fields_permit.grow_to(peak);
            if (!fields_permit) {
                auto permit = parallelism::memory_admission_controller::instance().try_reserve(peak);
                if (permit) { fields_permit = std::move(*permit); admitted = true; }
            }
            if (!admitted) return std::unexpected("cannot admit canonical metadata descriptors");
            fields.reserve(capacity);
            fields_permit.shrink_to(fields.capacity() * sizeof(canonical_field));
        }
        fields.push_back(std::move(field));
        return {};
    }
};

/** Output is written incrementally; sinks may enforce memory and IO admission. */
template<class Sink>
concept header_text_sink = requires(Sink& sink, std::string_view bytes) {
    { sink.append(bytes) } -> std::same_as<std::expected<void, std::string>>;
};

/** The canonical spelling leaves only RFC3986 unreserved ASCII bytes literal. */
[[nodiscard]] constexpr bool header_unreserved(unsigned char character) noexcept {
    return (character >= 'A' && character <= 'Z')
        || (character >= 'a' && character <= 'z')
        || (character >= '0' && character <= '9')
        || character == '.' || character == '_' || character == '~' || character == '-';
}

/** Decode one hex digit; invalid digits return a value outside the nibble range. */
[[nodiscard]] constexpr unsigned header_hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') return static_cast<unsigned>(character - '0');
    if (character >= 'A' && character <= 'F') return static_cast<unsigned>(character - 'A') + 10u;
    if (character >= 'a' && character <= 'f') return static_cast<unsigned>(character - 'a') + 10u;
    return 16u;
}

/** Stream percent encoding through bounded scratch; optionally escape the first byte. */
template<header_text_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_header_percent_encoded(
    Sink& sink, std::string_view input, bool escape_first = false) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::array<char, 768> scratch{};
    std::size_t used = 0;
    std::size_t position = 0;
    while (position < input.size()) {
        const auto safe = escape_first && position == 0u ? 0u
            : bioseq::io::simd::scan_header_unreserved_prefix(input.data() + position, input.size() - position);
        if (safe != 0u) {
            if (used != 0u) {
                if (auto result = sink.append({scratch.data(), used}); !result) return result;
                used = 0u;
            }
            if (auto result = sink.append(input.substr(position, safe)); !result) return result;
            position += safe;
            continue;
        }
        if (scratch.size() - used < 3u) {
            if (auto result = sink.append({scratch.data(), used}); !result) return result;
            used = 0u;
        }
        const auto character = static_cast<unsigned char>(input[position++]);
        scratch[used++] = '%';
        scratch[used++] = digits[character >> 4u];
        scratch[used++] = digits[character & 15u];
    }
    return sink.append({scratch.data(), used});
}

/** Decode canonical text in bounded fragments; malformed escapes and separators fail. */
template<header_text_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_header_percent_decoded(Sink& sink, std::string_view input) {
    std::array<char, 768> scratch{};
    std::size_t used{};
    std::size_t position{};
    bioseq::common::json_utf8_state unicode;
    while (position < input.size()) {
        const auto safe = bioseq::io::simd::scan_header_unreserved_prefix(input.data() + position, input.size() - position);
        if (safe != 0u) {
            if (unicode.continuation_bytes != 0u)
                return std::unexpected("invalid UTF-8 continuation in canonical header");
            if (used != 0u) {
                if (auto written = sink.append({scratch.data(), used}); !written) return written;
                used = 0u;
            }
            if (auto written = sink.append(input.substr(position, safe)); !written) return written;
            position += safe;
            unicode.byte_offset += safe;
            continue;
        }
        if (input[position] != '%') return std::unexpected("unescaped byte in canonical sequence header");
        if (input.size() - position < 3u) return std::unexpected("truncated percent escape in canonical sequence header");
        const auto high = header_hex_value(input[position + 1u]);
        const auto low = header_hex_value(input[position + 2u]);
        if (high >= 16u || low >= 16u) return std::unexpected("invalid percent escape in canonical sequence header");
        const auto character = static_cast<unsigned char>((high << 4u) | low);
        if (const auto decoded = bioseq::common::consume_json_utf8_byte(unicode, character); !decoded)
            return std::unexpected("canonical header: " + decoded.error());
        ++unicode.byte_offset;
        scratch[used++] = static_cast<char>(character);
        position += 3u;
        if (used == scratch.size()) {
            if (auto written = sink.append({scratch.data(), used}); !written) return written;
            used = 0u;
        }
    }
    if (const auto complete = bioseq::common::finish_json_string(unicode); !complete)
        return std::unexpected("canonical header: " + complete.error());
    return sink.append({scratch.data(), used});
}

/** Decode one field into the shared admitted byte owner, spilling only under memory pressure. */
[[nodiscard]] inline std::expected<admitted_byte_buffer, std::string> decode_header_percent(std::string_view input) {
    admitted_byte_buffer output;
    if (auto decoded = append_header_percent_decoded(output, input); !decoded) return std::unexpected(decoded.error());
    if (auto sealed = output.seal(); !sealed) return std::unexpected(sealed.error());
    return output;
}

/** Validate a skipped field without allocating its decoded contents. */
[[nodiscard]] inline std::expected<void, std::string> validate_header_percent(std::string_view input) {
    struct discard_sink {
        /** Consume validated bytes without retaining them. */
        [[nodiscard]] std::expected<void, std::string> append(std::string_view) const { return {}; }
    } sink;
    return append_header_percent_decoded(sink, input);
}

/** Percent-encode streamed JSON payload fragments without retaining their concatenation. */
template<header_text_sink Sink>
struct percent_header_sink {
    Sink& destination;
    /** Consume one complete fragment before its producer reuses its scratch. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view input) {
        return append_header_percent_encoded(destination, input);
    }
};

/** Write a canonical header with a streaming payload producer, without marker/newline. */
template<header_text_sink Sink, class PayloadWriter>
    requires requires(PayloadWriter& writer, percent_header_sink<Sink>& encoded) {
        { writer(encoded) } -> std::same_as<std::expected<void, std::string>>;
    }
[[nodiscard]] std::expected<void, std::string> append_canonical_header(
    Sink& sink, std::string_view header, std::string_view alphabet,
    PayloadWriter&& payload_writer, std::span<const canonical_field_view> fields) {
    if (auto result = append_header_percent_encoded(sink, header); !result) return result;
    if (auto result = sink.append(" [bq=1] [bq.alphabet="); !result) return result;
    if (auto result = append_header_percent_encoded(sink, alphabet); !result) return result;
    if (auto result = sink.append("] [bq.payload="); !result) return result;
    percent_header_sink<Sink> encoded{sink};
    if (auto result = payload_writer(encoded); !result) return result;
    if (auto result = sink.append("]"); !result) return result;
    for (const auto& field : fields) {
        if (field.name.empty()) return std::unexpected("canonical header metadata name is empty");
        if (auto result = sink.append(" ["); !result) return result;
        const bool reserved = field.name == "bq" || field.name.starts_with("bq.");
        if (auto result = append_header_percent_encoded(sink, field.name, reserved); !result) return result;
        const char discriminator = field.value ? field.discriminator : 'n';
        if (std::string_view{"nsiufbxj"}.find(discriminator) == std::string_view::npos
            || (field.value && discriminator == 'n'))
            return std::unexpected("invalid canonical metadata discriminator");
        const std::array<char, 3> separator{'=', discriminator, ':'};
        if (auto result = sink.append({separator.data(), separator.size()}); !result) return result;
        if (field.value) {
            if (auto result = append_header_percent_encoded(sink, *field.value); !result) return result;
        }
        if (auto result = sink.append("]"); !result) return result;
    }
    return {};
}

/** Write a pre-existing small payload through the same canonical streaming grammar. */
template<header_text_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_canonical_header(
    Sink& sink, std::string_view header, std::string_view alphabet,
    std::string_view payload, std::span<const canonical_field_view> fields) {
    return append_canonical_header(sink, header, alphabet,
        [payload](percent_header_sink<Sink>& encoded) { return encoded.append(payload); }, fields);
}

/** Find a delimiter using the existing Highway dispatch, returning input.size() if absent. */
[[nodiscard]] inline std::size_t canonical_delimiter(std::string_view input, char character) {
    return bioseq::io::simd::scan_first_byte(static_cast<unsigned char>(character),
        reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
}

/** Decode only an exact version marker. Ordinary external headers return nullopt.
 *  The original identifier is never parsed recursively. */
[[nodiscard]] inline std::expected<std::optional<canonical_header>, std::string>
parse_canonical_header(std::string_view input, canonical_parse_mode mode = canonical_parse_mode::complete) {
    const auto prefix_end = canonical_delimiter(input, ' ');
    if (prefix_end == input.size() || !input.substr(prefix_end).starts_with(" [bq="))
        return std::optional<canonical_header>{};
    // An external bracket named bq alone does not claim Bioseq canonical encoding.
    bool has_alphabet_marker = false;
    bool has_payload_marker = false;
    auto probe = input.substr(prefix_end);
    while (!probe.empty()) {
        const auto bracket = canonical_delimiter(probe, '[');
        if (bracket == probe.size()) break;
        probe.remove_prefix(bracket);
        has_alphabet_marker = has_alphabet_marker || probe.starts_with("[bq.alphabet=");
        has_payload_marker = has_payload_marker || probe.starts_with("[bq.payload=");
        probe.remove_prefix(1);
    }
    if (!has_alphabet_marker || !has_payload_marker) return std::optional<canonical_header>{};
    if (!input.substr(prefix_end).starts_with(" [bq=1] "))
        return std::unexpected("unsupported or malformed canonical sequence header version");
    canonical_header decoded;
    decoded.header_is_empty = prefix_end == 0u;
    if (mode == canonical_parse_mode::shape) {
        if (const auto valid = validate_header_percent(input.substr(0, prefix_end)); !valid)
            return std::unexpected(valid.error());
    } else {
        auto original = decode_header_percent(input.substr(0, prefix_end));
        if (!original) return std::unexpected(original.error());
        decoded.header_storage = std::move(*original);
        decoded.header = decoded.header_storage.text();
    }
    input.remove_prefix(prefix_end + std::string_view(" [bq=1]").size());
    bool has_alphabet = false;
    bool has_payload = false;
    while (!input.empty()) {
        if (!input.starts_with(" ["))
            return std::unexpected("malformed canonical sequence header field boundary");
        input.remove_prefix(2);
        const auto field_end = canonical_delimiter(input, ']');
        if (field_end == input.size()) return std::unexpected("unterminated canonical sequence header field");
        const auto field = input.substr(0, field_end);
        const auto equals = canonical_delimiter(field, '=');
        if (equals == 0 || equals == field.size())
            return std::unexpected("canonical sequence header field requires a name and equals sign");
        const auto name = field.substr(0, equals);
        const auto value = field.substr(equals + 1);
        if (name == "bq.alphabet" || name == "bq.payload") {
            bool& present = name == "bq.alphabet" ? has_alphabet : has_payload;
            if (present) return std::unexpected("duplicate canonical sequence header control field");
            present = true;
            if (name == "bq.payload" && mode == canonical_parse_mode::routing) {
                if (const auto valid = validate_header_percent(value); !valid) return std::unexpected(valid.error());
            } else {
                auto text = decode_header_percent(value);
                if (!text) return std::unexpected(text.error());
                if (name == "bq.alphabet") {
                    decoded.alphabet_storage = std::move(*text);
                    decoded.alphabet = decoded.alphabet_storage.text();
                } else {
                    decoded.payload_storage = std::move(*text);
                    decoded.payload = decoded.payload_storage.text();
                }
            }
        } else {
            if (name == "bq" || name.starts_with("bq."))
                return std::unexpected("unknown canonical sequence header control field");
            auto key = decode_header_percent(name);
            if (!key) return std::unexpected(key.error());
            for (const auto& prior : decoded.fields)
                if (prior.name == key->text()) return std::unexpected("duplicate canonical sequence metadata name");
            canonical_field metadata;
            metadata.name_storage = std::move(*key);
            metadata.name = metadata.name_storage.text();
            if (value.size() >= 2 && value[1] == ':' && value[0] != 'n'
                && std::string_view{"siufbxj"}.find(value[0]) != std::string_view::npos) {
                if (mode == canonical_parse_mode::shape) {
                    if (const auto valid = validate_header_percent(value.substr(2)); !valid) return std::unexpected(valid.error());
                } else {
                    auto text = decode_header_percent(value.substr(2));
                    if (!text) return std::unexpected(text.error());
                    metadata.value_storage = std::move(*text);
                    metadata.value = metadata.value_storage.text();
                }
                metadata.discriminator = value[0];
            } else if (value != "n:") {
                return std::unexpected("canonical metadata requires s: text or n: null");
            } else {
                metadata.discriminator = 'n';
            }
            if (const auto stored = decoded.append_field(std::move(metadata)); !stored) return std::unexpected(stored.error());
        }
        input.remove_prefix(field_end + 1);
    }
    if (!has_alphabet || decoded.alphabet.empty() || !has_payload)
        return std::unexpected("canonical sequence header requires alphabet and payload");
    return std::optional<canonical_header>{std::move(decoded)};
}

} // namespace bioseq::io::identifier_parser
