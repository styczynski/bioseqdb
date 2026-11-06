/** @file preservation.cppm
 *  @brief Versioned, bounded preservation payloads for text and alignment IO. */
module;
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.io.identifier_parser.preservation;
import bioseq.common.json_string;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.sequences.simd;
import bioseq.storage.lazy_sequence;
import bioseq.io.record_schema;
import bioseq.io.alignment.record_codec;
import bioseq.io.simd;
import bioseq.simd.utils;
import bioseq.io.identifier_parser.canonical;

export namespace bioseq::io::identifier_parser {

/** Projection decisions made once by the checked format normalizer. */
struct preservation_options {
    bool text_format{};
    bool synthetic_codons{};
    std::optional<std::uint16_t> projected_flag;
    std::optional<std::string_view> projected_qname;
    std::optional<std::uint16_t> original_flag;
    preservation_storage sequence_storage{preservation_storage::native};
    preservation_storage quality_storage{preservation_storage::native};
    std::optional<std::uint8_t> absent_sequence_alphabet;
    std::optional<std::uint8_t> absent_quality_alphabet;
    std::span<const auxiliary_field_view> projected_auxiliary;
};

/** File dictionary projection borrows source identity/attributes without copying. */
struct preservation_reference_view {
    const reference_descriptor& source;
    std::string_view wire_name;
    bool synthetic_codons{};
};

/** Strict pull cursor over retained payload bytes; strings may stream to a consumer.
 *  The surrounding record framer owns admission and may retain a mapped spool view.
 *  Only scalar fields use owned strings; rank/auxiliary strings need no whole copy. */
class preservation_json_cursor {
    std::string_view input_;
    std::size_t position_{};
    std::size_t scalar_limit_;
public:
    /** Borrow a validated record/spool view and its admitted scalar allocation limit. */
    explicit preservation_json_cursor(std::string_view input, std::size_t scalar_limit)
        : input_(input), scalar_limit_(scalar_limit) {}

    /** Current byte offset, used to retain replay slices without copying rank strings. */
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    /** Borrow a range already proven to be inside the input. */
    [[nodiscard]] std::string_view slice(std::size_t begin, std::size_t end) const {
        return input_.substr(begin, end - begin);
    }
    /** Skip legal JSON whitespace without accepting other control bytes. */
    void whitespace() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\n' && character != '\r' && character != '\t') break;
            ++position_;
        }
    }
    /** Consume one structural character if present after whitespace. */
    [[nodiscard]] bool take(char character) {
        whitespace();
        if (position_ == input_.size() || input_[position_] != character) return false;
        ++position_;
        return true;
    }
    /** Require one structural character or return its byte position. */
    [[nodiscard]] std::expected<void, std::string> require(char character) {
        if (take(character)) return {};
        return std::unexpected("invalid preservation JSON syntax at byte " + std::to_string(position_));
    }
    /** Consume a literal; following delimiters are validated by the enclosing grammar. */
    [[nodiscard]] bool literal(std::string_view value) {
        whitespace();
        if (!input_.substr(position_).starts_with(value)) return false;
        position_ += value.size();
        return true;
    }
    /** Require clean exhaustion after the complete top-level object. */
    [[nodiscard]] std::expected<void, std::string> finish() {
        whitespace();
        if (position_ != input_.size()) return std::unexpected("trailing preservation JSON bytes");
        return {};
    }
    /** Decode exactly four JSON hex digits; unlike rank hex, JSON permits lowercase. */
    [[nodiscard]] std::expected<std::uint32_t, std::string> unicode_unit() {
        if (input_.size() - position_ < 4) return std::unexpected("truncated JSON Unicode escape");
        std::uint32_t value = 0;
        for (std::size_t digit = 0; digit < 4; ++digit) {
            const auto nibble = header_hex_value(input_[position_++]);
            if (nibble >= 16) return std::unexpected("invalid JSON Unicode escape");
            value = (value << 4) | nibble;
        }
        return value;
    }
    /** Decode one JSON string into synchronous fragments; no concatenation is required. */
    template<class Consumer>
    [[nodiscard]] std::expected<void, std::string> string_to(Consumer&& consume) {
        if (auto opening = require('"'); !opening) return opening;
        while (position_ < input_.size()) {
            const auto safe = bioseq::io::simd::scan_json_ascii_prefix(
                input_.data() + position_, input_.size() - position_);
            if (safe != 0) {
                const auto result = consume(input_.substr(position_, safe));
                if (!result) return result;
                position_ += safe;
                if (position_ == input_.size()) break;
            }
            const auto character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return {};
            if (character == 0x7Fu) {
                if (auto delivered = consume(input_.substr(position_ - 1, 1)); !delivered) return delivered;
                continue;
            }
            if (character >= 0x80u) {
                const auto begin = position_ - 1;
                bioseq::common::json_utf8_state state;
                auto scalar = bioseq::common::consume_json_utf8_byte(state, character);
                if (!scalar) return std::unexpected(scalar.error());
                while (!*scalar && position_ < input_.size()) {
                    scalar = bioseq::common::consume_json_utf8_byte(state,
                        static_cast<unsigned char>(input_[position_++]));
                    if (!scalar) return std::unexpected(scalar.error());
                }
                if (!*scalar) return std::unexpected("truncated UTF-8 in preservation string");
                if (auto delivered = consume(input_.substr(begin, position_ - begin)); !delivered) return delivered;
                continue;
            }
            if (character != '\\' || position_ == input_.size())
                return std::unexpected("unescaped control byte or invalid escape in preservation JSON");
            const char escape = input_[position_++];
            std::array<char, 4> decoded{};
            std::size_t count = 1;
            switch (escape) {
            case '"': case '\\': case '/': decoded[0] = escape; break;
            case 'b': decoded[0] = '\b'; break;
            case 'f': decoded[0] = '\f'; break;
            case 'n': decoded[0] = '\n'; break;
            case 'r': decoded[0] = '\r'; break;
            case 't': decoded[0] = '\t'; break;
            case 'u': {
                auto scalar = unicode_unit();
                if (!scalar) return std::unexpected(scalar.error());
                if (*scalar >= 0xD800u && *scalar <= 0xDBFFu) {
                    if (!input_.substr(position_).starts_with("\\u"))
                        return std::unexpected("JSON high surrogate lacks a low surrogate");
                    position_ += 2;
                    const auto low = unicode_unit();
                    if (!low || *low < 0xDC00u || *low > 0xDFFFu)
                        return std::unexpected("invalid JSON low surrogate");
                    *scalar = 0x10000u + ((*scalar - 0xD800u) << 10) + (*low - 0xDC00u);
                } else if (*scalar >= 0xDC00u && *scalar <= 0xDFFFu)
                    return std::unexpected("unpaired JSON low surrogate");
                if (*scalar <= 0x7Fu) decoded[0] = static_cast<char>(*scalar);
                else if (*scalar <= 0x7FFu) {
                    count = 2;
                    decoded[0] = static_cast<char>(0xC0u | (*scalar >> 6));
                    decoded[1] = static_cast<char>(0x80u | (*scalar & 0x3Fu));
                } else if (*scalar <= 0xFFFFu) {
                    count = 3;
                    decoded[0] = static_cast<char>(0xE0u | (*scalar >> 12));
                    decoded[1] = static_cast<char>(0x80u | ((*scalar >> 6) & 0x3Fu));
                    decoded[2] = static_cast<char>(0x80u | (*scalar & 0x3Fu));
                } else {
                    count = 4;
                    decoded[0] = static_cast<char>(0xF0u | (*scalar >> 18));
                    decoded[1] = static_cast<char>(0x80u | ((*scalar >> 12) & 0x3Fu));
                    decoded[2] = static_cast<char>(0x80u | ((*scalar >> 6) & 0x3Fu));
                    decoded[3] = static_cast<char>(0x80u | (*scalar & 0x3Fu));
                }
                break;
            }
            default: return std::unexpected("invalid JSON string escape");
            }
            if (auto result = consume(std::string_view{decoded.data(), count}); !result) return result;
        }
        return std::unexpected("unterminated preservation JSON string");
    }
    /** Skip one JSON value with bounded nesting and no DOM or string allocation. */
    [[nodiscard]] std::expected<void, std::string> skip_value(std::size_t depth = 0) {
        if (depth > 128) return std::unexpected("preservation JSON nesting exceeds limit");
        whitespace();
        if (position_ == input_.size()) return std::unexpected("missing JSON value");
        if (input_[position_] == '"')
            return string_to([](std::string_view) -> std::expected<void, std::string> { return {}; });
        if (take('{')) {
            if (take('}')) return {};
            for (;;) {
                if (auto key = string_to([](std::string_view) -> std::expected<void, std::string> { return {}; }); !key) return key;
                if (auto colon = require(':'); !colon) return colon;
                if (auto value = skip_value(depth + 1); !value) return value;
                if (take('}')) return {};
                if (auto comma = require(','); !comma) return comma;
            }
        }
        if (take('[')) {
            if (take(']')) return {};
            for (;;) {
                if (auto value = skip_value(depth + 1); !value) return value;
                if (take(']')) return {};
                if (auto comma = require(','); !comma) return comma;
            }
        }
        if (literal("null") || literal("true") || literal("false")) return {};
        if (input_[position_] == '-') ++position_;
        if (position_ == input_.size()) return std::unexpected("truncated JSON number");
        if (input_[position_] == '0') ++position_;
        else {
            if (input_[position_] < '1' || input_[position_] > '9') return std::unexpected("invalid JSON value");
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            const auto begin = ++position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (position_ == begin) return std::unexpected("invalid JSON fraction");
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const auto begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (position_ == begin) return std::unexpected("invalid JSON exponent");
        }
        return {};
    }
    /** Decode a bounded scalar string, leaving large biological strings to string_to. */
    [[nodiscard]] std::expected<std::string, std::string> string(std::optional<std::size_t> limit = std::nullopt) {
        std::string output;
        const auto maximum = limit.value_or(scalar_limit_);
        const auto parsed = string_to([&output, maximum](std::string_view fragment)
            -> std::expected<void, std::string> {
            if (fragment.size() > maximum - output.size())
                return std::unexpected("preservation scalar exceeds admitted byte limit");
            output.append(fragment);
            return {};
        });
        if (!parsed) return std::unexpected(parsed.error());
        return output;
    }
    /** Decode optional text without conflating an empty string and JSON null. */
    [[nodiscard]] std::expected<std::optional<std::string>, std::string> optional_string() {
        if (literal("null")) return std::optional<std::string>{};
        auto value = string();
        if (!value) return std::unexpected(value.error());
        return std::optional<std::string>{std::move(*value)};
    }
    /** Visit all required named fields exactly once; reject unknown and duplicate keys. */
    template<std::size_t Count, class Visitor>
    [[nodiscard]] std::expected<void, std::string> object(
        const std::array<std::string_view, Count>& keys, Visitor&& visit) {
        static_assert(Count > 0 && Count < 64);
        if (auto opening = require('{'); !opening) return opening;
        std::uint64_t seen = 0;
        if (!take('}')) {
            for (;;) {
                const auto key = string(128);
                if (!key) return std::unexpected(key.error());
                std::size_t index = 0;
                while (index < Count && keys[index] != *key) ++index;
                if (index == Count) return std::unexpected("unknown preservation key: " + *key);
                const auto bit = std::uint64_t{1} << index;
                if ((seen & bit) != 0) return std::unexpected("duplicate preservation key: " + *key);
                seen |= bit;
                if (auto colon = require(':'); !colon) return colon;
                if (auto value = visit(index); !value) return value;
                if (take('}')) break;
                if (auto comma = require(','); !comma) return comma;
            }
        }
        if (seen != ((std::uint64_t{1} << Count) - 1))
            return std::unexpected("missing required preservation JSON field");
        return {};
    }
    /** Visit an array incrementally; the visitor consumes exactly one value. */
    template<class Visitor>
    [[nodiscard]] std::expected<std::size_t, std::string> array(Visitor&& visit) {
        if (auto opening = require('['); !opening) return std::unexpected(opening.error());
        std::size_t count = 0;
        if (take(']')) return count;
        for (;;) {
            if (count == std::numeric_limits<std::size_t>::max()) return std::unexpected("JSON array count overflow");
            if (auto value = visit(count); !value) return std::unexpected(value.error());
            ++count;
            if (take(']')) return count;
            if (auto comma = require(','); !comma) return std::unexpected(comma.error());
        }
    }
};

/** Parse a canonical signed/unsigned decimal string with no coercion or overflow. */
template<std::integral Integer>
[[nodiscard]] std::expected<Integer, std::string> preservation_integer(std::string_view text) {
    if (text.empty() || text.front() == '+' || text == "-0"
        || (text.size() > 1 && text.front() == '0')
        || (text.size() > 2 && text.starts_with("-0")))
        return std::unexpected("noncanonical preservation decimal integer");
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::unexpected("invalid or overflowing preservation decimal integer");
    return value;
}

/** Recognize a claimed Bioseq payload regardless of key order or JSON escaping.
 * Foreign bq tags remain opaque; a recognized own magic is validated by the full
 * decoder, even if the remainder is malformed. No untrusted payload is copied. */
[[nodiscard]] inline bool has_preservation_magic(std::string_view input) {
    preservation_json_cursor cursor{input, 16};
    if (!cursor.take('{') || cursor.take('}')) return false;
    for (;;) {
        constexpr std::string_view key_name = "magic";
        std::size_t key_length = 0;
        bool matching = true;
        const auto key = cursor.string_to([&key_length, &matching](std::string_view fragment)
            -> std::expected<void, std::string> {
            constexpr std::string_view expected_key = "magic";
            for (const char byte : fragment) {
                if (key_length >= expected_key.size() || byte != expected_key[key_length]) matching = false;
                ++key_length;
            }
            return {};
        });
        if (!key || !cursor.require(':')) return false;
        if (matching && key_length == key_name.size()) {
            auto probe = cursor;
            const auto value = probe.string(16);
            if (value && *value == "bioseq.io") return true;
            if (!cursor.skip_value()) return false;
        } else if (!cursor.skip_value()) return false;
        if (cursor.take('}')) return false;
        if (!cursor.require(',')) return false;
    }
}

/** Decode original ranks directly into an admitted destination builder.
 *  Required destination methods begin_sequence(role,descriptor),
 *  append_sequence_ranks(role,span) and end_sequence(role) return expected<void,string>.
 *  Native descriptors emit no ranks: the reader validates/inverts native fields. */
template<class Destination>
[[nodiscard]] std::expected<void, std::string> decode_preserved_sequence(
    preservation_json_cursor& cursor, Destination& destination, record_field_role role,
    std::size_t scalar_limit) {
    preserved_sequence_descriptor descriptor;
    std::optional<std::string_view> rank_token;
    constexpr std::array<std::string_view, 5> keys{"alphabet", "present", "length", "storage", "ranks"};
    const auto parsed = cursor.object(keys, [&cursor, &descriptor, &rank_token](std::size_t field)
        -> std::expected<void, std::string> {
        if (field == 0) {
            if (cursor.literal("null")) return {};
            const auto name = cursor.string(64);
            if (!name) return std::unexpected(name.error());
            descriptor.alphabet_code = supported_storage_alphabets::try_index_of_name(name->c_str());
            if (!descriptor.alphabet_code || supported_storage_alphabets::name_of_alphabet_code(*descriptor.alphabet_code) != *name)
                return std::unexpected("unknown preservation alphabet");
        } else if (field == 1) {
            if (cursor.literal("true")) descriptor.present = true;
            else if (!cursor.literal("false")) return std::unexpected("sequence present must be boolean");
        } else if (field == 2) {
            const auto text = cursor.string(32);
            if (!text) return std::unexpected(text.error());
            const auto length = preservation_integer<std::uint64_t>(*text);
            if (!length) return std::unexpected(length.error());
            descriptor.original_length = *length;
        } else if (field == 3) {
            const auto storage = cursor.string(16);
            if (!storage) return std::unexpected(storage.error());
            if (*storage == "rank_hex") descriptor.storage = preservation_storage::rank_hex;
            else if (*storage != "native") return std::unexpected("unknown preservation sequence storage");
        } else {
            if (cursor.literal("null")) return {};
            const auto begin = cursor.position();
            const auto value = cursor.string_to([](std::string_view) -> std::expected<void, std::string> { return {}; });
            if (!value) return value;
            rank_token = cursor.slice(begin, cursor.position());
        }
        return {};
    });
    if (!parsed) return parsed;
    if (descriptor.present && !descriptor.alphabet_code)
        return std::unexpected("present preserved sequence requires an alphabet");
    if (!descriptor.present && (descriptor.original_length != 0
        || descriptor.storage != preservation_storage::native || rank_token))
        return std::unexpected("absent preserved sequence has contradictory storage");
    if ((descriptor.storage == preservation_storage::rank_hex) != rank_token.has_value())
        return std::unexpected("preservation ranks disagree with storage descriptor");
    if (descriptor.alphabet_code) {
        const bool quality = is_quality_alphabet_code(*descriptor.alphabet_code);
        if ((role == record_field_role::quality) != quality
            || (role == record_field_role::sequence && *descriptor.alphabet_code >= supported_biological_alphabets::alphabet_count))
            return std::unexpected("preservation alphabet has the wrong sequence/quality capability");
    }
    if (auto result = destination.begin_sequence(role, descriptor); !result) return result;
    if (rank_token) {
        const auto cardinality = *supported_storage_alphabets::cardinality_for_code(*descriptor.alphabet_code);
        preservation_json_cursor ranks{*rank_token, scalar_limit};
        std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> tile{};
        std::size_t used = 0;
        std::uint64_t count = 0;
        std::optional<unsigned> high;
        const auto decoded = ranks.string_to([&destination, role, &tile, &used, &count, &high,
            cardinality, length = descriptor.original_length](std::string_view fragment)
            -> std::expected<void, std::string> {
            if (high && !fragment.empty()) {
                const auto digit = fragment.front();
                const auto low = header_hex_value(digit);
                if (low >= 16u || (digit >= 'a' && digit <= 'f'))
                    return std::unexpected("rank_hex requires uppercase hexadecimal digits");
                const auto rank = (*high << 4u) | low;
                if (count == length || rank >= cardinality)
                    return std::unexpected("preserved rank length or alphabet is invalid");
                tile[used++] = static_cast<std::uint8_t>(rank);
                ++count;
                high.reset();
                fragment.remove_prefix(1);
                if (used == tile.size()) {
                    if (auto delivered = destination.append_sequence_ranks(role, std::span<const std::uint8_t>{tile}); !delivered)
                        return delivered;
                    used = 0;
                }
            }
            while (fragment.size() >= 2u) {
                const auto pairs = std::min(fragment.size() / 2u, tile.size() - used);
                if (pairs > length - count)
                    return std::unexpected("preserved rank count exceeds descriptor");
                const std::span<const std::uint8_t> input{
                    reinterpret_cast<const std::uint8_t*>(fragment.data()), pairs * 2u};
                if (!bioseq::io::simd::decode_upper_hex_pairs(input,
                        std::span{tile}.subspan(used, pairs), cardinality))
                    return std::unexpected("invalid uppercase rank_hex or alphabet rank");
                used += pairs;
                count += pairs;
                fragment.remove_prefix(pairs * 2u);
                if (used == tile.size()) {
                    if (auto delivered = destination.append_sequence_ranks(role, std::span<const std::uint8_t>{tile}); !delivered)
                        return delivered;
                    used = 0;
                }
            }
            if (!fragment.empty()) {
                const auto digit = fragment.front();
                const auto nibble = header_hex_value(digit);
                if (nibble >= 16u || (digit >= 'a' && digit <= 'f'))
                    return std::unexpected("rank_hex requires uppercase hexadecimal digits");
                high = nibble;
            }
            return {};
        });
        if (!decoded) return decoded;
        if (high || count != descriptor.original_length) return std::unexpected("preserved rank count does not match descriptor");
        if (used != 0) {
            if (auto result = destination.append_sequence_ranks(role, std::span<const std::uint8_t>{tile.data(), used}); !result)
                return result;
        }
    }
    return destination.end_sequence(role);
}

/** Numeric auxiliary bytes retain unsigned32 and exact IEEE binary32 bit patterns. */
struct preserved_auxiliary_number {
    std::array<std::byte, 4> bytes{};
    std::size_t width{};
    char type{};
};

/** Convert canonical numeric text into checked little-endian native auxiliary bytes. */
[[nodiscard]] inline std::expected<preserved_auxiliary_number, std::string>
decode_preserved_auxiliary_number(char type, std::string_view value, bool array_element) {
    preserved_auxiliary_number result{.width = bioseq::io::alignment::auxiliary_element_bytes(type), .type = type};
    if (result.width == 0 || type == 'A') return std::unexpected("invalid preserved numeric auxiliary type");
    std::uint64_t bits = 0;
    if (type == 'f') {
        if (value.size() != 8) return std::unexpected("preserved float requires eight hex digits");
        for (const char digit : value) {
            if (header_hex_value(digit) >= 16 || (digit >= 'a' && digit <= 'f'))
                return std::unexpected("preserved float requires uppercase hex digits");
            bits = (bits << 4) | header_hex_value(digit);
        }
    } else if (!array_element && type == 'i') {
        const auto number = preservation_integer<std::int64_t>(value);
        if (!number || *number < std::numeric_limits<std::int32_t>::min()
            || *number > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected("preserved auxiliary integer is outside SAM range");
        bits = static_cast<std::uint64_t>(*number);
        if (*number > std::numeric_limits<std::int32_t>::max()) result.type = 'I';
    } else if (type == 'c' || type == 's' || type == 'i') {
        const auto number = preservation_integer<std::int64_t>(value);
        const auto maximum = (std::int64_t{1} << (result.width * 8 - 1)) - 1;
        if (!number || *number < -maximum - 1 || *number > maximum)
            return std::unexpected("preserved signed auxiliary array element overflows");
        bits = static_cast<std::uint64_t>(*number);
    } else {
        const auto number = preservation_integer<std::uint64_t>(value);
        const auto maximum = (std::uint64_t{1} << (result.width * 8)) - 1;
        if (!number || *number > maximum)
            return std::unexpected("preserved unsigned auxiliary array element overflows");
        bits = *number;
    }
    for (std::size_t byte = 0; byte < result.width; ++byte)
        result.bytes[byte] = static_cast<std::byte>((bits >> (byte * 8)) & 255u);
    return result;
}

/** Admitted auxiliary builder. For B arrays, begin reserves the four-byte count,
 * append accepts elements only, and end patches the count. Text excludes NUL. */
template<class Destination>
concept preservation_auxiliary_destination = requires(Destination& destination,
    bool projected, std::array<char, 2> tag, char type, char subtype,
    std::span<const std::byte> bytes, std::uint32_t count) {
    { destination.begin_auxiliary(projected, tag, type, subtype) } -> std::same_as<std::expected<void, std::string>>;
    { destination.append_auxiliary_bytes(bytes) } -> std::same_as<std::expected<void, std::string>>;
    { destination.end_auxiliary(count) } -> std::same_as<std::expected<void, std::string>>;
};

/** Decode typed auxiliary JSON through replay slices, never a JSON object tree. */
template<preservation_auxiliary_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_preserved_auxiliary(
    preservation_json_cursor& cursor, Destination& destination, bool projected,
    std::size_t scalar_limit) {
    std::bitset<52 * 62> tags;
    const auto decoded = cursor.array([&cursor, &destination, &tags, projected, scalar_limit](std::size_t)
        -> std::expected<void, std::string> {
        std::array<char, 2> tag{};
        std::string type;
        std::string_view value_token;
        bool value_array = false;
        constexpr std::array<std::string_view, 3> keys{"tag", "type", "value"};
        const auto object = cursor.object(keys, [&cursor, &tag, &type, &value_token, &value_array](std::size_t field)
            -> std::expected<void, std::string> {
            if (field == 0) {
                const auto value = cursor.string(2);
                if (!value || value->size() != 2) return std::unexpected("preserved auxiliary tag requires two characters");
                std::copy(value->begin(), value->end(), tag.begin());
            } else if (field == 1) {
                const auto value = cursor.string(3);
                if (!value) return std::unexpected(value.error());
                type = *value;
            } else {
                cursor.whitespace();
                const auto begin = cursor.position();
                auto probe = cursor;
                value_array = probe.take('[');
                if (value_array) {
                    const auto values = cursor.array([&cursor](std::size_t) {
                        return cursor.string_to([](std::string_view) -> std::expected<void, std::string> { return {}; });
                    });
                    if (!values) return std::unexpected(values.error());
                    if (*values > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
                        return std::unexpected("preserved auxiliary array count exceeds SAM/BAM limit");
                } else {
                    const auto value = cursor.string_to([](std::string_view) -> std::expected<void, std::string> { return {}; });
                    if (!value) return value;
                }
                value_token = cursor.slice(begin, cursor.position());
            }
            return {};
        });
        if (!object) return object;
        const auto tag_index = bioseq::io::alignment::auxiliary_tag_index(tag);
        if (!tag_index || tags.test(*tag_index)) return std::unexpected("invalid or duplicate preserved auxiliary tag");
        tags.set(*tag_index);
        if (tag == std::array<char, 2>{'C', 'G'} || (projected && tag == std::array<char, 2>{'b', 'q'}))
            return std::unexpected("transport/envelope tag appears in preserved ordinary auxiliary fields");
        preservation_json_cursor value{value_token, scalar_limit};
        if (value_array) {
            if (type.size() != 3 || !type.starts_with("B:")
                || bioseq::io::alignment::auxiliary_element_bytes(type[2]) == 0 || type[2] == 'A')
                return std::unexpected("preserved auxiliary array has an invalid type");
            if (auto begun = destination.begin_auxiliary(projected, tag, 'B', type[2]); !begun) return begun;
            const auto count = value.array([&value, &destination, subtype = type[2]](std::size_t)
                -> std::expected<void, std::string> {
                const auto text = value.string(32);
                if (!text) return std::unexpected(text.error());
                const auto number = decode_preserved_auxiliary_number(subtype, *text, true);
                if (!number) return std::unexpected(number.error());
                return destination.append_auxiliary_bytes({number->bytes.data(), number->width});
            });
            if (!count) return std::unexpected(count.error());
            return destination.end_auxiliary(static_cast<std::uint32_t>(*count));
        }
        if (type.size() != 1 || std::string_view{"AifZH"}.find(type[0]) == std::string_view::npos)
            return std::unexpected("invalid preserved auxiliary scalar type");
        if (type[0] == 'i' || type[0] == 'f') {
            const auto text = value.string(32);
            if (!text) return std::unexpected(text.error());
            const auto number = decode_preserved_auxiliary_number(type[0], *text, false);
            if (!number) return std::unexpected(number.error());
            if (auto begun = destination.begin_auxiliary(projected, tag, number->type, '\0'); !begun) return begun;
            if (auto bytes = destination.append_auxiliary_bytes({number->bytes.data(), number->width}); !bytes) return bytes;
            return destination.end_auxiliary(1);
        }
        if (auto begun = destination.begin_auxiliary(projected, tag, type[0], '\0'); !begun) return begun;
        std::size_t bytes = 0;
        const auto text = value.string_to([&destination, &bytes, kind = type[0]](std::string_view fragment)
            -> std::expected<void, std::string> {
            const auto valid = bioseq::io::simd::scan_sam_auxiliary_prefix(
                reinterpret_cast<const std::uint8_t*>(fragment.data()), fragment.size(), kind == 'H');
            if (valid != fragment.size())
                return std::unexpected("preserved auxiliary text must be printable ASCII or uppercase hex");
            if (kind == 'A' && (bytes > 1u || fragment.size() > 1u - bytes
                    || (!fragment.empty() && fragment.front() == ' ')))
                return std::unexpected("preserved character auxiliary must be one non-space printable byte");
            if (fragment.size() > std::numeric_limits<std::size_t>::max() - bytes)
                return std::unexpected("preserved auxiliary text length overflow");
            bytes += fragment.size();
            return destination.append_auxiliary_bytes(std::as_bytes(std::span<const char>{fragment.data(), fragment.size()}));
        });
        if (!text) return text;
        if ((type[0] == 'A' && bytes != 1) || (type[0] == 'H' && bytes % 2 != 0))
            return std::unexpected("invalid preserved character/hex auxiliary length");
        return destination.end_auxiliary(1);
    });
    if (!decoded) return std::unexpected(decoded.error());
    return {};
}

/** Share the same auxiliary grammar between SQL alignment composites and files. */
template<preservation_auxiliary_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_auxiliary_json(
    std::string_view input, Destination& destination, std::size_t admitted_scalar_limit) {
    preservation_json_cursor cursor{input, admitted_scalar_limit};
    if (auto decoded = decode_preserved_auxiliary(cursor, destination, false, admitted_scalar_limit); !decoded)
        return decoded;
    return cursor.finish();
}

/** A decoder destination owns admission and copies scalar views before returning.
 *  finish_record verifies cross-field consistency and native projection equality
 *  before exposing any restored tuple. Metadata names must be unique per record. */
template<class Destination>
concept preservation_record_destination = preservation_auxiliary_destination<Destination>
    && requires(Destination& destination, record_field_role role,
        const preserved_sequence_descriptor& descriptor, std::span<const std::uint8_t> ranks,
        std::string_view name, std::optional<std::string_view> value, bioseq::cigar_run run) {
        { destination.field(name, value) } -> std::same_as<std::expected<void, std::string>>;
        { destination.begin_sequence(role, descriptor) } -> std::same_as<std::expected<void, std::string>>;
        { destination.append_sequence_ranks(role, ranks) } -> std::same_as<std::expected<void, std::string>>;
        { destination.end_sequence(role) } -> std::same_as<std::expected<void, std::string>>;
        { destination.begin_cigar(true) } -> std::same_as<std::expected<void, std::string>>;
        { destination.append_cigar_run(run) } -> std::same_as<std::expected<void, std::string>>;
        { destination.end_cigar() } -> std::same_as<std::expected<void, std::string>>;
        { destination.metadata(name, name, value, true) } -> std::same_as<std::expected<void, std::string>>;
        { destination.finish_record() } -> std::same_as<std::expected<void, std::string>>;
    };

/** Decode a CIGAR string in bounded run tokens through the existing canonical parser. */
template<preservation_record_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_preserved_cigar(
    preservation_json_cursor& cursor, Destination& destination) {
    const bool absent = cursor.literal("null");
    if (auto begun = destination.begin_cigar(!absent); !begun) return begun;
    if (!absent) {
        std::array<char, 32> token{};
        std::size_t used = 0;
        const auto decoded = cursor.string_to([&destination, &token, &used](std::string_view fragment)
            -> std::expected<void, std::string> {
            for (const char character : fragment) {
                if (used == token.size()) return std::unexpected("preserved CIGAR run exceeds numeric size limit");
                token[used++] = character;
                if (character >= '0' && character <= '9') continue;
                const auto parsed = bioseq::parse_cigar_runs(std::string_view{token.data(), used},
                    [&destination](bioseq::cigar_run run) -> std::expected<void, bioseq::cigar_error> {
                        const auto appended = destination.append_cigar_run(run);
                        if (!appended) return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
                        return {};
                    });
                if (!parsed) return std::unexpected("invalid preserved CIGAR or destination admission failure");
                used = 0;
            }
            return {};
        });
        if (!decoded) return decoded;
        if (used != 0) return std::unexpected("truncated preserved CIGAR");
    }
    return destination.end_cigar();
}

/** Validate and deliver one optional alignment scalar in its original symbol domain. */
template<preservation_record_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_preserved_alignment_scalar(
    preservation_json_cursor& cursor, Destination& destination, std::string_view key) {
    const auto value = cursor.optional_string();
    if (!value) return std::unexpected(value.error());
    if (!*value) {
        if (key == "template_length" || key == "cigar_form" || key == "source_orientation")
            return std::unexpected("required alignment scalar cannot be null");
    } else {
        const std::string_view text = **value;
        if (key == "cigar_form") {
            if (text != "sam_complete" && text != "alignment_core") return std::unexpected("unknown preserved CIGAR form");
        } else if (key == "source_orientation") {
            if (text != "original_query" && text != "reference_forward") return std::unexpected("unknown preserved sequence orientation");
        } else if (key == "template_length") {
            if (!preservation_integer<std::int64_t>(text)) return std::unexpected("invalid preserved template length");
        } else if (key != "reference_name" && key != "mate_reference") {
            const auto number = preservation_integer<std::uint64_t>(text);
            if (!number || (key == "flag" && *number > 4095) || (key == "mapq" && *number > 255))
                return std::unexpected("invalid preserved alignment coordinate/flag/MAPQ");
        }
    }
    const std::string name = "alignment." + std::string{key};
    return destination.field(name, *value ? std::optional<std::string_view>{**value} : std::nullopt);
}

/** Decode the shared original alignment model; coordinate/base validation follows. */
template<preservation_record_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_preserved_alignment(
    preservation_json_cursor& cursor, Destination& destination, std::size_t scalar_limit) {
    if (cursor.literal("null")) return destination.field("alignment", std::nullopt);
    if (auto present = destination.field("alignment", "present"); !present) return present;
    constexpr std::array<std::string_view, 16> keys{"flag", "mapq", "reference_name", "reference_length",
        "target_start", "target_end", "query_start", "query_end", "query_length", "mate_reference",
        "mate_start", "template_length", "cigar", "cigar_form", "source_orientation", "aux"};
    return cursor.object(keys, [&cursor, &destination, scalar_limit, &keys](std::size_t field)
        -> std::expected<void, std::string> {
        if (keys[field] == "cigar") return decode_preserved_cigar(cursor, destination);
        if (keys[field] == "aux") return decode_preserved_auxiliary(cursor, destination, false, scalar_limit);
        return decode_preserved_alignment_scalar(cursor, destination, keys[field]);
    });
}

/** Decode scalar metadata entries without interpreting their qualified SQL type names. */
template<preservation_record_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_preserved_metadata(
    preservation_json_cursor& cursor, Destination& destination, bool types_only) {
    const auto entries = cursor.array([&cursor, &destination, types_only](std::size_t)
        -> std::expected<void, std::string> {
        std::string name;
        std::string type;
        std::optional<std::string> value;
        const auto field = [&cursor, &name, &type, &value](std::size_t index)
            -> std::expected<void, std::string> {
            if (index == 2) {
                auto text = cursor.optional_string();
                if (!text) return std::unexpected(text.error());
                value = std::move(*text);
            } else {
                auto text = cursor.string();
                if (!text) return std::unexpected(text.error());
                (index == 0 ? name : type) = std::move(*text);
            }
            return {};
        };
        const auto parsed = types_only
            ? cursor.object(std::array<std::string_view, 2>{"name", "type"}, field)
            : cursor.object(std::array<std::string_view, 3>{"name", "type", "value"}, field);
        if (!parsed) return parsed;
        if (name.empty() || type.empty()) return std::unexpected("preserved metadata requires name and type");
        return destination.metadata(name, type, value ? std::optional<std::string_view>{*value} : std::nullopt, types_only);
    });
    if (!entries) return std::unexpected(entries.error());
    return {};
}

/** Decode one complete canonical payload into an admitted builder.
 * All keys are required exactly once. The builder validates native projection
 * equality and all cross-field rules in finish_record before exposing its row. */
template<preservation_record_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_preservation_record(
    std::string_view input, Destination& destination, bool text_format,
    std::size_t admitted_scalar_limit) {
    preservation_json_cursor cursor{input, admitted_scalar_limit};
    const auto field = [&cursor, &destination, text_format, admitted_scalar_limit](std::string_view key)
        -> std::expected<void, std::string> {
        if (key == "version") {
            if (!cursor.literal("1")) return std::unexpected("unsupported preservation version");
            return {};
        }
        if (key == "sequence" || key == "quality")
            return decode_preserved_sequence(cursor, destination,
                key == "sequence" ? record_field_role::sequence : record_field_role::quality, admitted_scalar_limit);
        if (key == "alignment") return decode_preserved_alignment(cursor, destination, admitted_scalar_limit);
        if (key == "metadata" || key == "scalar_types") return decode_preserved_metadata(cursor, destination, text_format);
        if (key == "projected_aux") return decode_preserved_auxiliary(cursor, destination, true, admitted_scalar_limit);
        if (key == "header_null") {
            if (cursor.literal("true")) return destination.field(key, "true");
            if (cursor.literal("false")) return destination.field(key, "false");
            return std::unexpected("preservation header_null must be boolean");
        }
        if (key == "projected_qname") {
            if (cursor.literal("null")) return destination.field(key, std::nullopt);
            const auto value = cursor.string(254u);
            if (!value) return std::unexpected(value.error());
            if (!bioseq::io::alignment::valid_read_name(*value))
                return std::unexpected("invalid projected QNAME");
            return destination.field(key, std::string_view{*value});
        }
        const auto value = cursor.optional_string();
        if (!value) return std::unexpected(value.error());
        if (key == "magic") {
            if (!*value || **value != "bioseq.io") return std::unexpected("invalid preservation magic");
            return {};
        }
        if (key == "representation") {
            if (!*value || (text_format ? **value != "text" : (**value != "native" && **value != "synthetic-codon-v1")))
                return std::unexpected("invalid preservation representation");
        } else if (key == "projected_flag" || key == "header_context" || key == "template_id" || key == "segment_id" || key == "reference_id") {
            if (*value) {
                const auto number = preservation_integer<std::uint64_t>(**value);
                if (!number || (key == "projected_flag" && *number > 4095))
                    return std::unexpected("invalid preservation flag/context");
            }
        }
        return destination.field(key, *value ? std::optional<std::string_view>{**value} : std::nullopt);
    };
    std::expected<void, std::string> decoded;
    if (text_format) {
        constexpr std::array<std::string_view, 7> keys{"magic", "version", "representation", "header_null", "sequence", "quality", "scalar_types"};
        decoded = cursor.object(keys, [&field, &keys](std::size_t index) { return field(keys[index]); });
    } else {
        constexpr std::array<std::string_view, 16> keys{"magic", "version", "representation", "projected_flag", "projected_qname",
            "projected_aux", "header", "header_context", "qname", "template_id", "segment_id", "reference_id", "sequence", "quality", "alignment", "metadata"};
        decoded = cursor.object(keys, [&field, &keys](std::size_t index) { return field(keys[index]); });
    }
    if (!decoded) return decoded;
    if (auto finished = cursor.finish(); !finished) return finished;
    return destination.finish_record();
}

/** Inspect text record shape without retaining payload ranks or metadata values.
 * Used before PostgreSQL opens its destination writer, so automatic quality DDL
 * follows the original alphabet even for large legacy-quality payloads. */
struct text_record_shape {
    std::array<preserved_sequence_descriptor, 2> sequences{};
    std::array<std::uint64_t, 2> observed_ranks{};
    bool header_is_null{};

    /** Accept text-only scalar events; schema validation remains in the shared decoder. */
    [[nodiscard]] std::expected<void, std::string> field(std::string_view name, std::optional<std::string_view> value) {
        if (name == "header_null") header_is_null = value == "true";
        return {};
    }
    /** Retain only the two sequence descriptors. */
    [[nodiscard]] std::expected<void, std::string> begin_sequence(record_field_role role,
            const preserved_sequence_descriptor& descriptor) {
        const auto index = role == record_field_role::sequence ? 0u : 1u;
        sequences[index] = descriptor;
        return {};
    }
    /** Count discarded rank windows while validating the advertised length. */
    [[nodiscard]] std::expected<void, std::string> append_sequence_ranks(record_field_role role,
            std::span<const std::uint8_t> ranks) {
        const auto index = role == record_field_role::sequence ? 0u : 1u;
        if (ranks.size() > sequences[index].original_length - observed_ranks[index])
            return std::unexpected("text preservation ranks exceed their declared length");
        observed_ranks[index] += ranks.size();
        return {};
    }
    /** Require every inline rank payload to be complete. */
    [[nodiscard]] std::expected<void, std::string> end_sequence(record_field_role role) {
        const auto index = role == record_field_role::sequence ? 0u : 1u;
        if (sequences[index].storage == preservation_storage::rank_hex
            && observed_ranks[index] != sequences[index].original_length)
            return std::unexpected("text preservation rank payload is truncated");
        return {};
    }
    /** Text metadata is type-checked by the shared grammar and not retained. */
    [[nodiscard]] std::expected<void, std::string> metadata(std::string_view, std::string_view,
            std::optional<std::string_view>, bool) { return {}; }
    /** Text envelopes never contain native auxiliary payloads. */
    [[nodiscard]] std::expected<void, std::string> begin_auxiliary(bool, std::array<char, 2>, char, char) {
        return std::unexpected("text preservation cannot contain alignment auxiliary fields");
    }
    /** Reject a callback belonging to an alignment envelope. */
    [[nodiscard]] std::expected<void, std::string> append_auxiliary_bytes(std::span<const std::byte>) {
        return std::unexpected("unexpected text auxiliary bytes");
    }
    /** Reject a callback belonging to an alignment envelope. */
    [[nodiscard]] std::expected<void, std::string> end_auxiliary(std::uint32_t) {
        return std::unexpected("unexpected text auxiliary field");
    }
    /** Reject a callback belonging to an alignment envelope. */
    [[nodiscard]] std::expected<void, std::string> begin_cigar(bool) {
        return std::unexpected("text preservation cannot contain CIGAR");
    }
    /** Reject a callback belonging to an alignment envelope. */
    [[nodiscard]] std::expected<void, std::string> append_cigar_run(bioseq::cigar_run) {
        return std::unexpected("unexpected text CIGAR run");
    }
    /** Reject a callback belonging to an alignment envelope. */
    [[nodiscard]] std::expected<void, std::string> end_cigar() {
        return std::unexpected("unexpected text CIGAR completion");
    }
    /** Text biological symbols must be supplied by the native FASTA/FASTQ body. */
    [[nodiscard]] std::expected<void, std::string> finish_record() {
        if (sequences[0].storage != preservation_storage::native)
            return std::unexpected("text biological sequence must use native storage");
        return {};
    }
};

/** Manifest builders retain exact IDs and reject duplicates/cross-domain collisions
 * under their existing admission policy. References may precede contexts in JSON;
 * finish_manifest resolves all context links and native dictionary correspondences. */
template<class Destination>
concept preservation_manifest_destination = requires(Destination& destination,
    std::uint8_t code, std::uint64_t identity, std::string_view line,
    const reference_descriptor& reference, const sam_header_field& attribute) {
    { destination.manifest_quality_alphabet(code) } -> std::same_as<std::expected<void, std::string>>;
    { destination.manifest_alphabet(code) } -> std::same_as<std::expected<void, std::string>>;
    { destination.begin_context(identity) } -> std::same_as<std::expected<void, std::string>>;
    { destination.context_line(line) } -> std::same_as<std::expected<void, std::string>>;
    { destination.end_context() } -> std::same_as<std::expected<void, std::string>>;
    { destination.begin_reference(reference, line, true) } -> std::same_as<std::expected<void, std::string>>;
    { destination.reference_attribute(attribute) } -> std::same_as<std::expected<void, std::string>>;
    { destination.end_reference() } -> std::same_as<std::expected<void, std::string>>;
    { destination.finish_manifest() } -> std::same_as<std::expected<void, std::string>>;
};

/** Decode the complete version-one file manifest using bounded scalar/replay views. */
template<preservation_manifest_destination Destination>
[[nodiscard]] std::expected<void, std::string> decode_preservation_manifest(
    std::string_view input, Destination& destination, std::size_t admitted_scalar_limit) {
    preservation_json_cursor cursor{input, admitted_scalar_limit};
    constexpr std::array<std::string_view, 7> keys{"magic", "version", "record_payload", "sequence_alphabets", "contexts", "references", "quality_alphabets"};
    const auto decoded = cursor.object(keys, [&cursor, &destination, admitted_scalar_limit](std::size_t field)
        -> std::expected<void, std::string> {
        if (field == 1) {
            if (!cursor.literal("1")) return std::unexpected("unsupported manifest version");
            return {};
        }
        if (field == 0 || field == 2) {
            const auto value = cursor.string(16);
            if (!value || *value != (field == 0 ? "bioseq.io" : "bq"))
                return std::unexpected("invalid manifest magic/payload tag");
            return {};
        }
        if (field == 3 || field == 6) {
            std::bitset<supported_storage_alphabets::alphabet_count> seen;
            const auto alphabets = cursor.array([&cursor, &destination, &seen, field](std::size_t)
                -> std::expected<void, std::string> {
                const auto name = cursor.string(64);
                if (!name) return std::unexpected(name.error());
                const auto code = supported_storage_alphabets::try_index_of_name(name->c_str());
                if (!code || supported_storage_alphabets::name_of_alphabet_code(*code) != *name || seen.test(*code)
                    || (field == 3 ? *code >= supported_biological_alphabets::alphabet_count : !is_quality_alphabet_code(*code)))
                    return std::unexpected("invalid or duplicate manifest alphabet");
                seen.set(*code);
                return field == 3 ? destination.manifest_alphabet(*code) : destination.manifest_quality_alphabet(*code);
            });
            if (!alphabets) return std::unexpected(alphabets.error());
            return {};
        }
        if (field == 4) {
            const auto contexts = cursor.array([&cursor, &destination, admitted_scalar_limit](std::size_t)
                -> std::expected<void, std::string> {
                std::uint64_t id = 0;
                std::string_view lines_token;
                constexpr std::array<std::string_view, 2> context_keys{"id", "header_lines"};
                const auto context = cursor.object(context_keys, [&cursor, &id, &lines_token](std::size_t index)
                    -> std::expected<void, std::string> {
                    if (index == 0) {
                        const auto text = cursor.string(32);
                        if (!text) return std::unexpected(text.error());
                        const auto number = preservation_integer<std::uint64_t>(*text);
                        if (!number) return std::unexpected(number.error());
                        id = *number;
                    } else {
                        const auto begin = cursor.position();
                        const auto lines = cursor.array([&cursor](std::size_t) {
                            return cursor.string_to([](std::string_view) -> std::expected<void, std::string> { return {}; });
                        });
                        if (!lines) return std::unexpected(lines.error());
                        lines_token = cursor.slice(begin, cursor.position());
                    }
                    return {};
                });
                if (!context) return context;
                if (auto begun = destination.begin_context(id); !begun) return begun;
                preservation_json_cursor lines{lines_token, admitted_scalar_limit};
                const auto replay = lines.array([&lines, &destination](std::size_t)
                    -> std::expected<void, std::string> {
                    const auto text = lines.string();
                    if (!text) return std::unexpected(text.error());
                    if (text->find_first_of("\r\n") != std::string::npos || text->starts_with("@CO\tbioseq.io:"))
                        return std::unexpected("invalid or recursively nested original header context line");
                    return destination.context_line(*text);
                });
                if (!replay) return std::unexpected(replay.error());
                return destination.end_context();
            });
            if (!contexts) return std::unexpected(contexts.error());
            return {};
        }
        const auto references = cursor.array([&cursor, &destination, admitted_scalar_limit](std::size_t)
            -> std::expected<void, std::string> {
            reference_descriptor reference;
            std::string wire_name;
            bool synthetic = false;
            std::string_view attributes_token;
            constexpr std::array<std::string_view, 9> reference_keys{"wire_name", "source_id", "context", "original_name", "alphabet", "length", "representation", "topology", "attributes"};
            const auto parsed = cursor.object(reference_keys,
                [&cursor, &reference, &wire_name, &synthetic, &attributes_token](std::size_t index)
                -> std::expected<void, std::string> {
                if (index == 8) {
                    const auto begin = cursor.position();
                    if (auto opening = cursor.require('{'); !opening) return opening;
                    std::bitset<52 * 62> tags;
                    if (!cursor.take('}')) {
                        for (;;) {
                            const auto key = cursor.string(2);
                            if (!key || key->size() != 2) return std::unexpected("invalid manifest reference attribute name");
                            const std::array<char, 2> tag{(*key)[0], (*key)[1]};
                            const auto tag_index = bioseq::io::alignment::auxiliary_tag_index(tag);
                            if (!tag_index || tags.test(*tag_index) || *key == "SN" || *key == "LN")
                                return std::unexpected("duplicate/invalid manifest reference attribute");
                            tags.set(*tag_index);
                            if (auto colon = cursor.require(':'); !colon) return colon;
                            if (auto value = cursor.string_to([](std::string_view) -> std::expected<void, std::string> { return {}; }); !value)
                                return value;
                            if (cursor.take('}')) break;
                            if (auto comma = cursor.require(','); !comma) return comma;
                        }
                    }
                    attributes_token = cursor.slice(begin, cursor.position());
                    return {};
                }
                if ((index == 2 || index == 4) && cursor.literal("null")) return {};
                const auto text = cursor.string();
                if (!text) return std::unexpected(text.error());
                switch (index) {
                case 0: wire_name = *text; break;
                case 1: case 2: case 5: {
                    const auto number = preservation_integer<std::uint64_t>(*text);
                    if (!number) return std::unexpected(number.error());
                    if (index == 1) reference.identity = *number;
                    else if (index == 2) reference.context_id = *number;
                    else reference.length = *number;
                    break;
                }
                case 3: reference.name = *text; break;
                case 4:
                    reference.alphabet_code = supported_biological_alphabets::try_index_of_name(text->c_str());
                    if (!reference.alphabet_code
                        || supported_biological_alphabets::name_of_alphabet_code(*reference.alphabet_code) != *text)
                        return std::unexpected("unknown manifest reference alphabet");
                    break;
                case 6:
                    synthetic = *text == "synthetic-codon-v1";
                    if (!synthetic && *text != "native") return std::unexpected("unknown manifest reference representation");
                    break;
                case 7:
                    reference.circular = *text == "circular";
                    if (!reference.circular && *text != "linear") return std::unexpected("unknown manifest reference topology");
                    break;
                default: return std::unexpected("invalid manifest reference field");
                }
                return {};
            });
            if (!parsed) return parsed;
            if (!bioseq::io::alignment::valid_reference_name(wire_name) || reference.length == 0
                || reference.length > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) / (synthetic ? 3 : 1)
                || (synthetic && !reference.alphabet_code))
                return std::unexpected("manifest reference name/length/domain cannot be represented");
            if (reference.alphabet_code) {
                bool protein = false;
                supported_biological_alphabets::dispatch_alphabet_by_code(*reference.alphabet_code,
                    [&protein]<class Alphabet>() { protein = seqan3::aminoacid_alphabet<Alphabet>; });
                if (protein != synthetic)
                    return std::unexpected("manifest reference representation conflicts with its alphabet");
            }
            if (auto begun = destination.begin_reference(reference, wire_name, synthetic); !begun) return begun;
            preservation_json_cursor attributes{attributes_token, admitted_scalar_limit};
            if (auto opening = attributes.require('{'); !opening) return opening;
            if (!attributes.take('}')) {
                for (;;) {
                    const auto tag = attributes.string(2);
                    if (!tag) return std::unexpected(tag.error());
                    if (auto colon = attributes.require(':'); !colon) return colon;
                    auto value = attributes.string();
                    if (!value) return std::unexpected(value.error());
                    const sam_header_field attribute{{(*tag)[0], (*tag)[1]}, std::move(*value)};
                    if (auto added = destination.reference_attribute(attribute); !added) return added;
                    if (attributes.take('}')) break;
                    if (auto comma = attributes.require(','); !comma) return comma;
                }
            }
            return destination.end_reference();
        });
        if (!references) return std::unexpected(references.error());
        return {};
    });
    if (!decoded) return decoded;
    if (auto finished = cursor.finish(); !finished) return finished;
    return destination.finish_manifest();
}

/** Bridge the common byte-span JSON sink to the existing IO text sink contract. */
template<header_text_sink Sink>
struct preservation_json_sink {
    Sink& destination;
    /** Consume each escaped fragment before the common encoder advances. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const char> bytes) {
        return destination.append({bytes.data(), bytes.size()});
    }
};

/** Stateful encoder with one sticky error and bounded numeric/rank scratch.
 *  No biological string or JSON object tree is materialized during writing. */
template<header_text_sink Sink>
class preservation_encoder {
    Sink& sink_;
    std::optional<std::string> error_;
public:
    /** Borrow an admitted synchronous sink for one payload or manifest. */
    explicit preservation_encoder(Sink& sink) : sink_(sink) {}

    /** Retain the first failure so formatting never masks an earlier IO error. */
    void fail(std::string message) { if (!error_) error_ = std::move(message); }

    /** Append fixed JSON syntax or already validated digits. */
    void text(std::string_view value) {
        if (error_) return;
        const auto result = sink_.append(value);
        if (!result) fail(result.error());
    }

    /** Emit an optional string as validated ASCII JSON, including its quotes. */
    void string(std::optional<std::string_view> value) {
        if (!value) { text("null"); return; }
        text("\"");
        if (error_) return;
        preservation_json_sink<Sink> adapter{sink_};
        bioseq::common::json_utf8_state state;
        const auto encoded = bioseq::common::append_json_string_fragment(adapter,
            std::span<const char>{value->data(), value->size()}, state,
            bioseq::common::json_escape_mode::ascii, &bioseq::io::simd::scan_json_ascii_prefix);
        if (!encoded) { fail(encoded.error()); return; }
        const auto finished = bioseq::common::finish_json_string(state);
        if (!finished) { fail(finished.error()); return; }
        text("\"");
    }

    /** Serialize integer quantities as canonical decimal strings. */
    template<std::integral Integer>
    void integer(Integer value) {
        std::array<char, std::numeric_limits<Integer>::digits10 + 4> decimal{};
        const auto converted = std::to_chars(decimal.data(), decimal.data() + decimal.size(), value);
        if (converted.ec != std::errc{}) { fail("preservation integer conversion overflow"); return; }
        text("\"");
        text({decimal.data(), static_cast<std::size_t>(converted.ptr - decimal.data())});
        text("\"");
    }

    /** Unknown optional quantities stay null; zero remains a known quantity. */
    template<std::integral Integer>
    void integer(std::optional<Integer> value) {
        if (value) integer(*value); else text("null");
    }

    /** Encode one logical IEEE binary32 value as its eight uppercase hex digits. */
    void float_bits(std::uint32_t bits) {
        constexpr std::string_view digits = "0123456789ABCDEF";
        std::array<char, 8> hex{};
        for (std::size_t index = 0; index < hex.size(); ++index)
            hex[index] = digits[(bits >> ((hex.size() - index - 1) * 4)) & 15u];
        string(std::string_view{hex.data(), hex.size()});
    }

    /** Decode a bounded little-endian auxiliary scalar, without native pointer casts. */
    [[nodiscard]] static std::uint32_t unsigned_aux(std::span<const std::byte> bytes) {
        const std::span<const std::uint8_t> input{
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
        if (bytes.size() == 1) return bioseq::io::alignment::read_wire_unsigned<std::uint8_t>(input);
        if (bytes.size() == 2) return bioseq::io::alignment::read_wire_unsigned<std::uint16_t>(input);
        return bioseq::io::alignment::read_wire_unsigned<std::uint32_t>(input);
    }

    /** Return the SAM/BAM numeric auxiliary element width, or zero if unsupported. */
    [[nodiscard]] static constexpr std::size_t auxiliary_width(char type) {
        return type == 'A' ? 0 : bioseq::io::alignment::auxiliary_element_bytes(type);
    }

    /** Emit an auxiliary numeric value preserving signedness and float bits. */
    void numeric_aux(char type, std::span<const std::byte> bytes) {
        if (bytes.size() != auxiliary_width(type) || bytes.empty()) {
            fail("invalid preservation auxiliary numeric width"); return;
        }
        const auto value = unsigned_aux(bytes);
        switch (type) {
        case 'c': integer(std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(value))); break;
        case 's': integer(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value))); break;
        case 'i': integer(std::bit_cast<std::int32_t>(value)); break;
        case 'C': case 'S': case 'I': integer(value); break;
        case 'f': float_bits(value); break;
        default: fail("invalid preservation auxiliary numeric type");
        }
    }

    /** Serialize typed auxiliary views; B values retain ordered element strings. */
    void auxiliary(std::span<const auxiliary_field_view> fields) {
        std::bitset<52 * 62> occupied_tags;
        text("[");
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto& field = fields[index];
            if (const auto valid = bioseq::io::alignment::validate_auxiliary_field(field); !valid) {
                fail(valid.error()); return;
            }
            if (index != 0) text(",");
            if (field.tag == std::array<char, 2>{'C', 'G'}) {
                fail("transport-only CG cannot be preserved as ordinary auxiliary data"); return;
            }
            const auto tag = *bioseq::io::alignment::auxiliary_tag_index(field.tag);
            if (occupied_tags.test(tag)) { fail("duplicate preservation auxiliary tag"); return; }
            occupied_tags.set(tag);
            text("{\"tag\":"); string(std::string_view{field.tag.data(), field.tag.size()});
            text(",\"type\":");
            if (field.type == 'B') {
                const std::array<char, 3> type{'B', ':', field.array_element_type};
                string(std::string_view{type.data(), type.size()}); text(",\"value\":[");
                const auto width = auxiliary_width(field.array_element_type);
                if (width == 0 || field.payload.size() < 4) {
                    fail("invalid preservation auxiliary array"); return;
                }
                const auto count = unsigned_aux(field.payload.first(4));
                if ((field.payload.size() - 4) / width != count
                    || (field.payload.size() - 4) % width != 0) {
                    fail("preservation auxiliary array length mismatch"); return;
                }
                for (std::size_t element = 0; element < count; ++element) {
                    if (element != 0) text(",");
                    numeric_aux(field.array_element_type, field.payload.subspan(4 + element * width, width));
                }
                text("]");
            } else if (auxiliary_width(field.type) != 0) {
                string(field.type == 'f' ? "f" : "i"); text(",\"value\":");
                numeric_aux(field.type, field.payload);
            } else {
                if (field.type != 'A' && field.type != 'Z' && field.type != 'H') {
                    fail("unsupported preservation auxiliary type"); return;
                }
                string(std::string_view{&field.type, 1}); text(",\"value\":");
                string(std::string_view{reinterpret_cast<const char*>(field.payload.data()), field.payload.size()});
            }
            text("}");
        }
        text("]");
    }

    /** Emit a sequence descriptor, reading original ranks only in bounded windows. */
    void sequence(const std::optional<bioseq::storage::borrowed_rank_window_source>& source,
        preservation_storage storage, std::optional<std::uint8_t> absent_alphabet) {
        const auto code = source ? std::optional{source->alphabet_code} : absent_alphabet;
        text("{\"alphabet\":");
        if (code) {
            if (*code >= supported_storage_alphabets::alphabet_count) {
                fail("unknown preservation alphabet code"); return;
            }
            string(supported_storage_alphabets::name_of_alphabet_code(*code));
        } else string(std::nullopt);
        text(source ? ",\"present\":true,\"length\":" : ",\"present\":false,\"length\":");
        integer(source ? source->symbol_count : std::uint64_t{0});
        const bool ranks = source && storage == preservation_storage::rank_hex;
        text(ranks ? ",\"storage\":\"rank_hex\",\"ranks\":\"" : ",\"storage\":\"native\",\"ranks\":null");
        if (ranks) {
            const auto cardinality = supported_storage_alphabets::cardinality_for_code(*code);
            if (!cardinality) { fail("invalid preservation alphabet cardinality"); return; }
            if constexpr (requires(Sink& measurement, std::uint64_t count) {
                { measurement.count_only(count) } -> std::same_as<std::expected<void, std::string>>;
            }) {
                // Uppercase rank hex is exactly two unescaped ASCII bytes per symbol.
                // Actual emission performs the original rank/source validation once.
                if (source->symbol_count > std::numeric_limits<std::uint64_t>::max() / 2u) {
                    fail("preservation rank hex size overflows"); return;
                }
                if (auto counted = sink_.count_only(source->symbol_count * 2u); !counted) {
                    fail(counted.error()); return;
                }
            } else {
                std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> scratch{};
                std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity * 2> hex{};
                for (std::uint64_t position = 0; position < source->symbol_count;) {
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), source->symbol_count - position));
                    const auto window = source->read_checked(position, count, scratch);
                    if (!window) { fail("failed reading preservation rank window"); return; }
                    if (!bioseq::io::simd::encode_upper_hex_bytes(*window,
                            std::span{hex}.first(count * 2u), *cardinality)) {
                        fail("invalid original rank in preservation source"); return;
                    }
                    text({reinterpret_cast<const char*>(hex.data()), count * 2u});
                    if (error_) return;
                    position += count;
                }
            }
            text("\"");
        }
        text("}");
    }

    /** Replay canonical runs to JSON CIGAR text without expanding or retaining them. */
    void cigar(const std::optional<bioseq::borrowed_cigar_run_source>& source) {
        if (!source) { text("null"); return; }
        text("\"");
        bioseq::borrowed_cigar_cursor cursor{*source};
        for (;;) {
            const auto run = cursor.next();
            if (!run) { fail("failed reading preservation CIGAR"); return; }
            if (!*run) break;
            if ((**run).length == 0) { fail("zero-length preservation CIGAR operation"); return; }
            std::array<char, 32> decimal{};
            const auto converted = std::to_chars(decimal.data(), decimal.data() + decimal.size(), (**run).length);
            if (converted.ec != std::errc{}) { fail("preservation CIGAR count overflow"); return; }
            text({decimal.data(), static_cast<std::size_t>(converted.ptr - decimal.data())});
            const char operation = seqan3::to_char((**run).operation);
            text({&operation, 1});
        }
        text("\"");
    }

    /** Emit the shared alignment view in its original symbol/orientation domain. */
    void alignment(const std::optional<alignment_record_view>& value,
        const preservation_options& options) {
        if (!value) { text("null"); return; }
        const auto& alignment = *value;
        const auto original_flag = options.original_flag ? options.original_flag
            : alignment.flags ? alignment.flags : options.projected_flag;
        text("{\"flag\":"); integer(original_flag);
        text(",\"mapq\":"); integer(alignment.mapping_quality);
        text(",\"reference_name\":"); string(alignment.reference_name.empty() ? std::nullopt : std::optional{alignment.reference_name});
        text(",\"reference_length\":"); integer(alignment.reference_length);
        text(",\"target_start\":"); integer(alignment.target_start);
        text(",\"target_end\":"); integer(alignment.target_end);
        text(",\"query_start\":"); integer(alignment.query_start);
        text(",\"query_end\":"); integer(alignment.query_end);
        text(",\"query_length\":"); integer(alignment.query_length);
        text(",\"mate_reference\":"); string(alignment.mate_reference.empty() ? std::nullopt : std::optional{alignment.mate_reference});
        text(",\"mate_start\":"); integer(alignment.mate_start);
        text(",\"template_length\":"); integer(alignment.template_length);
        text(",\"cigar\":"); cigar(alignment.cigar);
        text(",\"cigar_form\":"); string(alignment.form == cigar_form::alignment_core ? "alignment_core" : "sam_complete");
        text(",\"source_orientation\":"); string(alignment.sequence_orientation == record_orientation::original_query ? "original_query" : "reference_forward");
        text(",\"aux\":"); auxiliary(alignment.auxiliary); text("}");
    }

    /** Emit scalar names/types and optionally values, preserving NULL separately. */
    void metadata(std::span<const scalar_field_view> fields, bool include_values) {
        text("[");
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto& field = fields[index];
            for (std::size_t prior = 0; prior < index; ++prior)
                if (fields[prior].name == field.name) { fail("duplicate metadata name requires a SQL alias or omission"); return; }
            if (index != 0) text(",");
            text("{\"name\":"); string(field.name);
            text(",\"type\":"); string(field.qualified_type);
            if (include_values) { text(",\"value\":"); string(field.is_null ? std::nullopt : std::optional{field.bytes}); }
            text("}");
        }
        text("]");
    }

    /** Return the first codec/IO failure after the caller finishes its structure. */
    [[nodiscard]] std::expected<void, std::string> result() const {
        if (error_) return std::unexpected(*error_);
        return {};
    }
};

/** Serialize the exact version-one record schema into a bounded synchronous sink. */
template<header_text_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_preservation_record(
    Sink& sink, const export_record_view& row, const preservation_options& options) {
    if (!options.text_format && options.projected_flag.has_value() != options.projected_qname.has_value())
        return std::unexpected("projection snapshot requires both FLAG and QNAME");
    if (options.projected_qname && !bioseq::io::alignment::valid_read_name(*options.projected_qname))
        return std::unexpected("invalid projected QNAME");
    preservation_encoder<Sink> encoder{sink};
    encoder.text("{\"magic\":\"bioseq.io\",\"version\":1,\"representation\":");
    encoder.string(options.text_format ? "text" : options.synthetic_codons ? "synthetic-codon-v1" : "native");
    if (options.text_format) encoder.text(row.header ? ",\"header_null\":false" : ",\"header_null\":true");
    else {
        encoder.text(",\"projected_flag\":"); encoder.integer(options.projected_flag);
        encoder.text(",\"projected_qname\":"); encoder.string(options.projected_qname);
        encoder.text(",\"projected_aux\":"); encoder.auxiliary(options.projected_auxiliary);
        encoder.text(",\"header\":"); encoder.string(row.header);
        encoder.text(",\"qname\":"); encoder.string(row.qname);
        encoder.text(",\"template_id\":"); encoder.integer(row.identity.template_id);
        encoder.text(",\"segment_id\":"); encoder.integer(row.identity.segment_id);
        encoder.text(",\"reference_id\":"); encoder.integer(row.identity.reference_id);
        encoder.text(",\"header_context\":");
        encoder.integer(row.alignment ? row.alignment->header_context_id : std::optional<std::uint64_t>{});
    }
    encoder.text(",\"sequence\":"); encoder.sequence(row.sequence, options.sequence_storage, options.absent_sequence_alphabet);
    encoder.text(",\"quality\":"); encoder.sequence(row.quality, options.quality_storage, options.absent_quality_alphabet);
    if (options.text_format) encoder.text(",\"scalar_types\":");
    else { encoder.text(",\"alignment\":"); encoder.alignment(row.alignment, options); encoder.text(",\"metadata\":"); }
    encoder.metadata(row.metadata, !options.text_format);
    encoder.text("}");
    return encoder.result();
}

/** Emit the manifest JSON (the caller writes the SAM @CO prefix and newline). */
template<header_text_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_preservation_manifest(
    Sink& sink, const record_source_metadata& metadata,
    std::span<const preservation_reference_view> references,
    std::span<const std::uint8_t> sequence_alphabets = {},
    std::span<const std::uint8_t> quality_alphabets = {}) {
    if (sequence_alphabets.empty()) sequence_alphabets = metadata.declared_alphabet_codes;
    if (quality_alphabets.empty()) quality_alphabets = metadata.declared_quality_alphabet_codes;
    preservation_encoder<Sink> encoder{sink};
    encoder.text("{\"magic\":\"bioseq.io\",\"version\":1,\"record_payload\":\"bq\",\"sequence_alphabets\":[");
    for (std::size_t index = 0; index < sequence_alphabets.size(); ++index) {
        if (index != 0) encoder.text(",");
        const auto code = sequence_alphabets[index];
        if (code >= supported_biological_alphabets::alphabet_count)
            return std::unexpected("manifest sequence alphabet must be biological");
        encoder.string(supported_biological_alphabets::name_of_alphabet_code(code));
    }
    encoder.text("],\"quality_alphabets\":[");
    for (std::size_t index = 0; index < quality_alphabets.size(); ++index) {
        if (index != 0) encoder.text(",");
        const auto code = quality_alphabets[index];
        if (code >= supported_storage_alphabets::alphabet_count || !is_quality_alphabet_code(code))
            return std::unexpected("manifest quality alphabet must encode quality scores");
        encoder.string(supported_storage_alphabets::name_of_alphabet_code(code));
    }
    encoder.text("],\"contexts\":[");
    for (std::size_t index = 0; index < metadata.header_contexts.size(); ++index) {
        if (index != 0) encoder.text(",");
        const auto& context = metadata.header_contexts[index];
        encoder.text("{\"id\":"); encoder.integer(context.id); encoder.text(",\"header_lines\":[");
        for (std::size_t line = 0; line < context.original_header_lines.size(); ++line) {
            if (context.original_header_lines[line].starts_with("@CO\tbioseq.io:"))
                return std::unexpected("original header context cannot contain a nested Bioseq manifest");
            if (line != 0) encoder.text(",");
            encoder.string(context.original_header_lines[line]);
        }
        encoder.text("]}");
    }
    encoder.text("],\"references\":[");
    for (std::size_t index = 0; index < references.size(); ++index) {
        if (index != 0) encoder.text(",");
        const auto& projected = references[index];
        const auto& source = projected.source;
        encoder.text("{\"wire_name\":"); encoder.string(projected.wire_name);
        encoder.text(",\"source_id\":"); encoder.integer(source.identity);
        encoder.text(",\"context\":"); encoder.integer(source.context_id);
        encoder.text(",\"original_name\":"); encoder.string(source.name);
        encoder.text(",\"alphabet\":");
        if (source.alphabet_code) {
            if (*source.alphabet_code >= supported_biological_alphabets::alphabet_count)
                return std::unexpected("manifest reference alphabet must be biological");
            encoder.string(supported_storage_alphabets::name_of_alphabet_code(*source.alphabet_code));
        }
        else encoder.string(std::nullopt);
        encoder.text(",\"length\":"); encoder.integer(source.length);
        encoder.text(",\"representation\":"); encoder.string(projected.synthetic_codons ? "synthetic-codon-v1" : "native");
        encoder.text(",\"topology\":"); encoder.string(source.circular ? "circular" : "linear");
        encoder.text(",\"attributes\":{");
        for (std::size_t attribute = 0; attribute < source.attributes.size(); ++attribute) {
            if (attribute != 0) encoder.text(",");
            const auto& field = source.attributes[attribute];
            if (field.tag == std::array<char, 2>{'S', 'N'} || field.tag == std::array<char, 2>{'L', 'N'})
                return std::unexpected("manifest reference attributes cannot override SN/LN");
            encoder.string(std::string_view{field.tag.data(), field.tag.size()}); encoder.text(":"); encoder.string(field.value);
        }
        encoder.text("}}");
    }
    encoder.text("]}");
    return encoder.result();
}

} // namespace bioseq::io::identifier_parser
