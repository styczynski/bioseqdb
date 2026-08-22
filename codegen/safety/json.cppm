module;

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.codegen.safety:json;

export namespace bioseq::codegen::safety {

/// Minimal deterministic JSON value used by safety wire formats.
///
/// Objects use ordered keys so rendering is byte-stable. Integers are limited
/// to the signed 64-bit range accepted by the parser, and non-finite floating
/// values are rejected before they can produce invalid JSON.
struct json_value {
    using array = std::vector<json_value>;
    using object =
        std::map<std::string, json_value, std::less<>>;
    using storage = std::variant<
        std::nullptr_t,
        bool,
        std::int64_t,
        double,
        std::string,
        array,
        object>;

    storage value{nullptr};

    json_value() = default;
    json_value(std::nullptr_t) : value{nullptr} {}
    json_value(bool input) : value{input} {}
    json_value(std::int64_t input) : value{input} {}
    json_value(std::size_t input)
        : value{checked_integer(input)} {}
    json_value(double input) : value{checked_number(input)} {}
    json_value(std::string input) : value{std::move(input)} {}
    json_value(std::string_view input)
        : value{std::string{input}} {}
    json_value(const char* input)
        : value{checked_string(input)} {}
    json_value(array input) : value{std::move(input)} {}
    json_value(object input) : value{std::move(input)} {}

  private:
    [[nodiscard]] static std::int64_t checked_integer(
        std::size_t input) {
        if (input
            > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error(
                "JSON integer is outside the signed 64-bit range");
        }
        return static_cast<std::int64_t>(input);
    }

    [[nodiscard]] static double checked_number(double input) {
        if (!std::isfinite(input)) {
            throw std::runtime_error(
                "JSON number must be finite");
        }
        return input;
    }

    [[nodiscard]] static std::string checked_string(
        const char* input) {
        if (input == nullptr) {
            throw std::runtime_error(
                "JSON string pointer must not be null");
        }
        return std::string{input};
    }
};

namespace json_detail {

[[noreturn]] inline void json_failure(
    std::size_t offset,
    std::string_view message) {
    throw std::runtime_error(
        "invalid JSON at byte " + std::to_string(offset)
        + ": " + std::string{message});
}

[[nodiscard]] inline int hex_digit(unsigned char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(
            static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(
            static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(
            static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(
            static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(
            static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0x10ffffU) {
        output.push_back(
            static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(
            static_cast<char>(
                0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(
            static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(
            static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        throw std::runtime_error("invalid Unicode code point");
    }
}

class parser {
  public:
    explicit parser(std::string_view input) : input_{input} {}

    [[nodiscard]] json_value parse() {
        skip_space();
        json_value result = parse_value();
        skip_space();
        if (offset_ != input_.size()) {
            json_failure(offset_, "trailing input");
        }
        return result;
    }

  private:
    [[nodiscard]] char peek() const noexcept {
        return offset_ == input_.size() ? '\0' : input_[offset_];
    }

    [[nodiscard]] char take() {
        if (offset_ == input_.size()) {
            json_failure(offset_, "unexpected end of input");
        }
        return input_[offset_++];
    }

    void skip_space() noexcept {
        while (offset_ < input_.size()
               && (input_[offset_] == ' '
                   || input_[offset_] == '\t'
                   || input_[offset_] == '\r'
                   || input_[offset_] == '\n')) {
            ++offset_;
        }
    }

    void expect(std::string_view token) {
        if (!input_.substr(offset_).starts_with(token)) {
            json_failure(offset_, "expected '" + std::string{token} + "'");
        }
        offset_ += token.size();
    }

    [[nodiscard]] json_value parse_value() {
        switch (peek()) {
            case 'n':
                expect("null");
                return nullptr;
            case 't':
                expect("true");
                return true;
            case 'f':
                expect("false");
                return false;
            case '"': return parse_string();
            case '[': return parse_array();
            case '{': return parse_object();
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                    return parse_number();
                }
                json_failure(offset_, "expected a value");
        }
    }

    [[nodiscard]] std::uint32_t parse_hex_quad() {
        if (input_.size() - offset_ < 4) {
            json_failure(offset_, "truncated Unicode escape");
        }
        std::uint32_t result = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const int digit = hex_digit(
                static_cast<unsigned char>(input_[offset_++]));
            if (digit < 0) {
                json_failure(
                    offset_ - 1, "invalid Unicode escape");
            }
            result = (result << 4U)
                | static_cast<std::uint32_t>(digit);
        }
        return result;
    }

    [[nodiscard]] std::string parse_string() {
        if (take() != '"') {
            json_failure(offset_, "expected a string");
        }
        std::string result;
        while (true) {
            const unsigned char value =
                static_cast<unsigned char>(take());
            if (value == '"') return result;
            if (value < 0x20U) {
                json_failure(
                    offset_ - 1,
                    "unescaped control byte in string");
            }
            if (value != '\\') {
                result.push_back(static_cast<char>(value));
                continue;
            }
            switch (take()) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = parse_hex_quad();
                    if (codepoint >= 0xd800U
                        && codepoint <= 0xdbffU) {
                        if (take() != '\\' || take() != 'u') {
                            json_failure(
                                offset_,
                                "high surrogate has no low surrogate");
                        }
                        const std::uint32_t low = parse_hex_quad();
                        if (low < 0xdc00U || low > 0xdfffU) {
                            json_failure(
                                offset_,
                                "invalid low surrogate");
                        }
                        codepoint = 0x10000U
                            + ((codepoint - 0xd800U) << 10U)
                            + (low - 0xdc00U);
                    } else if (codepoint >= 0xdc00U
                               && codepoint <= 0xdfffU) {
                        json_failure(
                            offset_, "unpaired low surrogate");
                    }
                    append_utf8(result, codepoint);
                    break;
                }
                default:
                    json_failure(
                        offset_ - 1, "unknown string escape");
            }
        }
    }

    [[nodiscard]] json_value parse_number() {
        const std::size_t begin = offset_;
        if (peek() == '-') ++offset_;
        if (peek() == '0') {
            ++offset_;
            if (peek() >= '0' && peek() <= '9') {
                json_failure(offset_, "leading zero in number");
            }
        } else {
            if (peek() < '1' || peek() > '9') {
                json_failure(offset_, "invalid number");
            }
            while (peek() >= '0' && peek() <= '9') ++offset_;
        }
        bool floating = false;
        if (peek() == '.') {
            floating = true;
            ++offset_;
            if (peek() < '0' || peek() > '9') {
                json_failure(offset_, "empty fraction");
            }
            while (peek() >= '0' && peek() <= '9') ++offset_;
        }
        if (peek() == 'e' || peek() == 'E') {
            floating = true;
            ++offset_;
            if (peek() == '+' || peek() == '-') ++offset_;
            if (peek() < '0' || peek() > '9') {
                json_failure(offset_, "empty exponent");
            }
            while (peek() >= '0' && peek() <= '9') ++offset_;
        }
        const std::string_view token =
            input_.substr(begin, offset_ - begin);
        if (!floating) {
            std::int64_t integer{};
            const auto [end, error] = std::from_chars(
                token.data(), token.data() + token.size(), integer);
            if (error != std::errc{} || end != token.data() + token.size()) {
                json_failure(begin, "integer is outside signed 64-bit range");
            }
            return integer;
        }
        double number{};
        std::istringstream stream{std::string{token}};
        stream.imbue(std::locale::classic());
        stream >> std::noskipws >> number;
        if (!stream || stream.peek() != std::char_traits<char>::eof()
            || !std::isfinite(number)) {
            json_failure(begin, "invalid finite number");
        }
        return number;
    }

    [[nodiscard]] json_value::array parse_array() {
        static_cast<void>(take());
        skip_space();
        json_value::array result;
        if (peek() == ']') {
            static_cast<void>(take());
            return result;
        }
        while (true) {
            result.push_back(parse_value());
            skip_space();
            const char delimiter = take();
            if (delimiter == ']') return result;
            if (delimiter != ',') {
                json_failure(offset_ - 1, "expected ',' or ']'");
            }
            skip_space();
        }
    }

    [[nodiscard]] json_value::object parse_object() {
        static_cast<void>(take());
        skip_space();
        json_value::object result;
        if (peek() == '}') {
            static_cast<void>(take());
            return result;
        }
        while (true) {
            if (peek() != '"') {
                json_failure(offset_, "object key is not a string");
            }
            std::string key = parse_string();
            skip_space();
            if (take() != ':') {
                json_failure(offset_ - 1, "expected ':'");
            }
            skip_space();
            if (!result.emplace(key, parse_value()).second) {
                json_failure(offset_, "duplicate object key '" + key + "'");
            }
            skip_space();
            const char delimiter = take();
            if (delimiter == '}') return result;
            if (delimiter != ',') {
                json_failure(offset_ - 1, "expected ',' or '}'");
            }
            skip_space();
        }
    }

    std::string_view input_;
    std::size_t offset_{};
};

[[nodiscard]] inline std::string escape(std::string_view input) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(input.size() + 8);
    for (const unsigned char value : input) {
        switch (value) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (value < 0x20U) {
                    result += "\\u00";
                    result.push_back(digits[value >> 4U]);
                    result.push_back(digits[value & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(value));
                }
        }
    }
    return result;
}

inline void append_indentation(
    std::string& output,
    std::size_t indentation,
    std::size_t depth) {
    output.append(depth * indentation, ' ');
}

inline void render_into(
    std::string& output,
    const json_value& input,
    std::size_t indentation,
    std::size_t depth) {
    if (std::holds_alternative<std::nullptr_t>(input.value)) {
        output += "null";
    } else if (const bool* boolean =
                   std::get_if<bool>(&input.value)) {
        output += *boolean ? "true" : "false";
    } else if (const std::int64_t* integer =
                   std::get_if<std::int64_t>(&input.value)) {
        output += std::to_string(*integer);
    } else if (const double* number =
                   std::get_if<double>(&input.value)) {
        output += std::to_string(*number);
    } else if (const std::string* text =
                   std::get_if<std::string>(&input.value)) {
        output.push_back('"');
        output += escape(*text);
        output.push_back('"');
    } else if (const json_value::array* elements =
                   std::get_if<json_value::array>(&input.value)) {
        output.push_back('[');
        for (std::size_t index = 0;
             index < elements->size();
             ++index) {
            output += index == 0 ? "\n" : ",\n";
            append_indentation(output, indentation, depth + 1);
            render_into(
                output, (*elements)[index], indentation, depth + 1);
        }
        if (!elements->empty()) {
            output.push_back('\n');
            append_indentation(output, indentation, depth);
        }
        output.push_back(']');
    } else {
        const json_value::object& members =
            std::get<json_value::object>(input.value);
        output.push_back('{');
        std::size_t index = 0;
        for (const auto& [key, item] : members) {
            output += index++ == 0 ? "\n" : ",\n";
            append_indentation(output, indentation, depth + 1);
            output.push_back('"');
            output += escape(key);
            output += "\": ";
            render_into(output, item, indentation, depth + 1);
        }
        if (!members.empty()) {
            output.push_back('\n');
            append_indentation(output, indentation, depth);
        }
        output.push_back('}');
    }
}

}  // namespace json_detail

/// Parses one complete RFC 8259 JSON value.
[[nodiscard]] inline json_value parse_json(std::string_view input) {
    return json_detail::parser{input}.parse();
}

/// Renders canonical, ordered, newline-terminated JSON bytes.
[[nodiscard]] inline std::string render_json(
    const json_value& input,
    std::size_t indentation = 2) {
    std::string result;
    json_detail::render_into(result, input, indentation, 0);
    result.push_back('\n');
    return result;
}

/// Returns an object value or throws a context-rich type error.
[[nodiscard]] inline const json_value::object& json_object(
    const json_value& input,
    std::string_view context) {
    if (const auto* result =
            std::get_if<json_value::object>(&input.value)) {
        return *result;
    }
    throw std::runtime_error(
        std::string{context} + " must be a JSON object");
}

/// Returns an array value or throws a context-rich type error.
[[nodiscard]] inline const json_value::array& json_array(
    const json_value& input,
    std::string_view context) {
    if (const auto* result =
            std::get_if<json_value::array>(&input.value)) {
        return *result;
    }
    throw std::runtime_error(
        std::string{context} + " must be a JSON array");
}

/// Returns a required object member or throws a missing-field error.
[[nodiscard]] inline const json_value& json_field(
    const json_value::object& input,
    std::string_view name,
    std::string_view context) {
    const auto found = input.find(name);
    if (found == input.end()) {
        throw std::runtime_error(
            std::string{context} + " is missing JSON field '"
            + std::string{name} + "'");
    }
    return found->second;
}

/// Returns a string value or throws a context-rich type error.
[[nodiscard]] inline std::string_view json_string(
    const json_value& input,
    std::string_view context) {
    if (const auto* result =
            std::get_if<std::string>(&input.value)) {
        return *result;
    }
    throw std::runtime_error(
        std::string{context} + " must be a JSON string");
}

/// Returns a Boolean value or throws a context-rich type error.
[[nodiscard]] inline bool json_boolean(
    const json_value& input,
    std::string_view context) {
    if (const auto* result = std::get_if<bool>(&input.value)) {
        return *result;
    }
    throw std::runtime_error(
        std::string{context} + " must be a JSON boolean");
}

/// Returns a signed integer value or throws a context-rich type error.
[[nodiscard]] inline std::int64_t json_integer(
    const json_value& input,
    std::string_view context) {
    if (const auto* result =
            std::get_if<std::int64_t>(&input.value)) {
        return *result;
    }
    throw std::runtime_error(
        std::string{context} + " must be a JSON integer");
}

}  // namespace bioseq::codegen::safety
