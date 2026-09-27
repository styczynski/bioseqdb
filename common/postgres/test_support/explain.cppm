module;

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.test.postgres:explain;

import bioseq.libpq.client;

export namespace bioseq::postgres_test {

/// Invalid JSON or an unexpected EXPLAIN result shape.
class explain_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Own a parsed JSON value used to inspect EXPLAIN output.
class explain_json_value {
public:
    using array = std::vector<explain_json_value>;
    using object = std::map<std::string, explain_json_value, std::less<>>;
    using storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                                 std::string, array, object>;

    explain_json_value() : value_(nullptr) {}
    explicit explain_json_value(std::nullptr_t) : value_(nullptr) {}
    explicit explain_json_value(bool value) : value_(value) {}
    explicit explain_json_value(std::int64_t value) : value_(value) {}
    explicit explain_json_value(double value) : value_(value) {}
    explicit explain_json_value(std::string value) : value_(std::move(value)) {}
    explicit explain_json_value(array value) : value_(std::move(value)) {}
    explicit explain_json_value(object value) : value_(std::move(value)) {}

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::nullptr_t>(value_);
    }
    [[nodiscard]] const bool* as_bool() const noexcept {
        return std::get_if<bool>(&value_);
    }
    [[nodiscard]] const std::int64_t* as_integer() const noexcept {
        return std::get_if<std::int64_t>(&value_);
    }
    [[nodiscard]] const double* as_double() const noexcept {
        return std::get_if<double>(&value_);
    }
    [[nodiscard]] const std::string* as_string() const noexcept {
        return std::get_if<std::string>(&value_);
    }
    [[nodiscard]] const array* as_array() const noexcept {
        return std::get_if<array>(&value_);
    }
    [[nodiscard]] const object* as_object() const noexcept {
        return std::get_if<object>(&value_);
    }

    /// Find an object property, or return nullptr if this is not an object or the key is absent.
    [[nodiscard]] const explain_json_value* find(std::string_view key) const noexcept {
        const object* properties = as_object();
        if (properties == nullptr) {
            return nullptr;
        }
        const auto found = properties->find(key);
        return found == properties->end() ? nullptr : &found->second;
    }

private:
    storage value_;
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::explain_detail {

/// Parse nested JSON values for EXPLAIN, rejecting duplicate object keys.
class parser {
public:
    explicit parser(std::string_view input) : input_(input) {}

    /// @brief Parses one complete JSON document with no trailing tokens.
    [[nodiscard]] explain_json_value parse() {
        skip_whitespace();
        explain_json_value value = parse_value(0);
        skip_whitespace();
        if (position_ != input_.size()) {
            fail("trailing data");
        }
        return value;
    }

private:
    /// Parse a JSON value and reject nesting beyond the depth limit.
    [[nodiscard]] explain_json_value parse_value(std::size_t depth) {
        if (depth > 256) {
            fail("nesting exceeds 256 levels");
        }
        skip_whitespace();
        if (position_ >= input_.size()) {
            fail("unexpected end of input");
        }
        switch (input_[position_]) {
        case 'n':
            consume_literal("null");
            return explain_json_value(nullptr);
        case 't':
            consume_literal("true");
            return explain_json_value(true);
        case 'f':
            consume_literal("false");
            return explain_json_value(false);
        case '"':
            return explain_json_value(parse_string());
        case '[':
            return explain_json_value(parse_array(depth + 1));
        case '{':
            return explain_json_value(parse_object(depth + 1));
        default:
            return parse_number();
        }
    }

    /// @brief Parses an array including empty arrays.
    [[nodiscard]] explain_json_value::array parse_array(std::size_t depth) {
        consume('[');
        explain_json_value::array values;
        skip_whitespace();
        if (consume_if(']')) {
            return values;
        }
        for (;;) {
            values.push_back(parse_value(depth));
            skip_whitespace();
            if (consume_if(']')) {
                return values;
            }
            consume(',');
        }
    }

    /// @brief Parses an object and rejects duplicate property names.
    [[nodiscard]] explain_json_value::object parse_object(std::size_t depth) {
        consume('{');
        explain_json_value::object properties;
        skip_whitespace();
        if (consume_if('}')) {
            return properties;
        }
        for (;;) {
            skip_whitespace();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("object key must be a string");
            }
            std::string key = parse_string();
            skip_whitespace();
            consume(':');
            explain_json_value value = parse_value(depth);
            if (!properties.emplace(std::move(key), std::move(value)).second) {
                fail("duplicate object key");
            }
            skip_whitespace();
            if (consume_if('}')) {
                return properties;
            }
            consume(',');
        }
    }

    /// @brief Parses a JSON string and emits valid UTF-8 for \\u escapes.
    [[nodiscard]] std::string parse_string() {
        consume('"');
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                return result;
            }
            if (character < 0x20U) {
                fail("unescaped control character");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) {
                fail("unterminated escape");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
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
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (position_ + 2 > input_.size()
                        || input_[position_] != '\\'
                        || input_[position_ + 1] != 'u') {
                        fail("high surrogate has no low surrogate");
                    }
                    position_ += 2;
                    const std::uint32_t low = parse_hex_quad();
                    if (low < 0xdc00U || low > 0xdfffU) {
                        fail("invalid low surrogate");
                    }
                    codepoint = 0x10000U
                        + ((codepoint - 0xd800U) << 10U)
                        + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    fail("unpaired low surrogate");
                }
                append_utf8(result, codepoint);
                break;
            }
            default:
                fail("unknown string escape");
            }
        }
        fail("unterminated string");
    }

    /// Read a JSON number as int64 when possible, otherwise as a finite double.
    [[nodiscard]] explain_json_value parse_number() {
        const std::size_t begin = position_;
        consume_if('-');
        if (consume_if('0')) {
            if (position_ < input_.size()
                && input_[position_] >= '0' && input_[position_] <= '9') {
                fail("number has a leading zero");
            }
        } else {
            require_digits();
        }
        bool floating = false;
        if (consume_if('.')) {
            floating = true;
            require_digits();
        }
        if (position_ < input_.size()
            && (input_[position_] == 'e' || input_[position_] == 'E')) {
            floating = true;
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            require_digits();
        }
        const std::string_view token = input_.substr(begin, position_ - begin);
        if (!floating) {
            std::int64_t integer{};
            const auto [end, error] = std::from_chars(
                token.data(), token.data() + token.size(), integer);
            if (error == std::errc{}
                && end == token.data() + token.size()) {
                return explain_json_value(integer);
            }
        }
        std::string owned(token);
        char* end = nullptr;
        const double number = std::strtod(owned.c_str(), &end);
        if (end != owned.c_str() + owned.size() || !std::isfinite(number)) {
            fail("invalid or non-finite number");
        }
        return explain_json_value(number);
    }

    /// @brief Requires one or more decimal digits.
    void require_digits() {
        const std::size_t begin = position_;
        while (position_ < input_.size()
               && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (position_ == begin) {
            fail("expected decimal digits");
        }
    }

    /// @brief Parses exactly four hexadecimal digits.
    [[nodiscard]] std::uint32_t parse_hex_quad() {
        if (position_ + 4 > input_.size()) {
            fail("short unicode escape");
        }
        std::uint32_t value = 0;
        for (std::size_t count = 0; count < 4; ++count) {
            const char character = input_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                fail("invalid unicode escape");
            }
        }
        return value;
    }

    /// @brief Appends one Unicode scalar value as UTF-8.
    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0x10ffffU) {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            throw explain_error("JSON unicode scalar is out of range");
        }
    }

    /// Skip whitespace and require the expected character.
    void consume(char expected) {
        skip_whitespace();
        if (position_ >= input_.size() || input_[position_] != expected) {
            fail(std::string("expected '") + expected + "'");
        }
        ++position_;
    }

    /// Consume the expected character if it is next, without skipping whitespace.
    [[nodiscard]] bool consume_if(char expected) noexcept {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    /// @brief Consumes one exact keyword.
    void consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
        }
        position_ += literal.size();
    }

    /// @brief Advances over JSON whitespace.
    void skip_whitespace() noexcept {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t'
                && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[noreturn]] void fail(std::string message) const {
        throw explain_error("JSON parse error at byte "
                            + std::to_string(position_) + ": "
                            + std::move(message));
    }

    std::string_view input_;
    std::size_t position_{};
};

/// Collect nested objects with a Node Type or Custom Plan Provider property.
inline void collect_plan_nodes(
    const explain_json_value& value,
    std::vector<const explain_json_value*>& output) {
    if (const explain_json_value::object* object = value.as_object()) {
        if (object->contains("Node Type")
            || object->contains("Custom Plan Provider")) {
            output.push_back(&value);
        }
        for (const auto& [name, child] : *object) {
            static_cast<void>(name);
            collect_plan_nodes(child, output);
        }
    } else if (const explain_json_value::array* array = value.as_array()) {
        for (const explain_json_value& child : *array) {
            collect_plan_nodes(child, output);
        }
    }
}

}  // namespace bioseq::postgres_test::explain_detail

export namespace bioseq::postgres_test {

/// Own an EXPLAIN JSON document and search its nested plan nodes.
class explain_plan_document {
public:
    /// @brief Parses a complete JSON document.
    [[nodiscard]] static explain_plan_document parse(std::string_view json) {
        return explain_plan_document(explain_detail::parser(json).parse());
    }

    /// Parse an EXPLAIN result containing exactly one non-null text cell.
    [[nodiscard]] static explain_plan_document from_result(
        const bioseq::libpq::command_result& result) {
        if (result.row_count() != 1 || result.column_count() != 1
            || result.cell_is_null(0, 0)) {
            throw explain_error(
                "EXPLAIN JSON must return one non-null cell");
        }
        return parse(result.text_value(0, 0));
    }

    /**
     * Return borrowed pointers to all detected plan nodes.
     * Object children are visited in property-name order, and array children in
     * array order. The pointers remain valid while this document stays alive.
     */
    [[nodiscard]] std::vector<const explain_json_value*> plan_nodes() const {
        std::vector<const explain_json_value*> result;
        explain_detail::collect_plan_nodes(root_, result);
        return result;
    }

    /// @brief Returns plan nodes whose string property equals the requested value.
    [[nodiscard]] std::vector<const explain_json_value*> find_plan_nodes(
        std::string_view property,
        std::string_view expected) const {
        std::vector<const explain_json_value*> matches;
        for (const explain_json_value* node : plan_nodes()) {
            const explain_json_value* value = node->find(property);
            if (value != nullptr && value->as_string() != nullptr
                && *value->as_string() == expected) {
                matches.push_back(node);
            }
        }
        return matches;
    }

    /// Borrow the JSON root for additional test assertions.
    [[nodiscard]] const explain_json_value& root() const noexcept { return root_; }

private:
    explicit explain_plan_document(explain_json_value root) : root_(std::move(root)) {}
    explain_json_value root_;
};

/// Borrow a string property, or return nullopt if it is absent or has another type.
[[nodiscard]] inline std::optional<std::string_view>
read_plan_string_property(
    const explain_json_value& object, std::string_view name) noexcept {
    const explain_json_value* value = object.find(name);
    return value != nullptr && value->as_string() != nullptr
        ? std::optional<std::string_view>(*value->as_string())
        : std::nullopt;
}

/// Read an integer property, or return nullopt if it is absent or has another type.
[[nodiscard]] inline std::optional<std::int64_t>
read_plan_integer_property(
    const explain_json_value& object, std::string_view name) noexcept {
    const explain_json_value* value = object.find(name);
    return value != nullptr && value->as_integer() != nullptr
        ? std::optional<std::int64_t>(*value->as_integer())
        : std::nullopt;
}

/// @brief Returns one numeric plan property, accepting integer or floating JSON.
[[nodiscard]] inline std::optional<double> read_plan_number_property(
    const explain_json_value& object, std::string_view name) noexcept {
    const explain_json_value* value = object.find(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const double* number = value->as_double()) {
        return *number;
    }
    if (const std::int64_t* integer = value->as_integer()) {
        return static_cast<double>(*integer);
    }
    return std::nullopt;
}

/// Read a Boolean property, or return nullopt if it is absent or has another type.
[[nodiscard]] inline std::optional<bool> read_plan_boolean_property(
    const explain_json_value& object, std::string_view name) noexcept {
    const explain_json_value* value = object.find(name);
    return value != nullptr && value->as_bool() != nullptr
        ? std::optional<bool>(*value->as_bool())
        : std::nullopt;
}

/**
 * Run EXPLAIN in JSON format and parse the result.
 * When analyze is true, execute the SQL as well and include runtime and
 * buffer statistics.
 */
[[nodiscard]] inline explain_plan_document execute_explain(
    bioseq::libpq::client_connection& connection,
    std::string_view sql,
    bool analyze,
    std::chrono::milliseconds deadline,
    bioseq::libpq::transaction_policy policy =
        bioseq::libpq::transaction_policy::require_idle) {
    const std::string command = analyze
        ? "EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS, FORMAT JSON) "
              + std::string(sql)
        : "EXPLAIN (VERBOSE, COSTS, FORMAT JSON) " + std::string(sql);
    return explain_plan_document::from_result(
        connection.execute(command, deadline, policy));
}

}  // namespace bioseq::postgres_test
