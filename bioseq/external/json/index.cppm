/**
 * @file external/json/index.cppm
 * @brief JSON values that own the data returned by external providers.
 */
module;

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.external.json;

export namespace bioseq::external::json {

/// Identifies the JSON value stored in this object.
enum class value_kind : std::uint8_t {
    null,
    boolean,
    signed_integer,
    unsigned_integer,
    floating_point,
    string,
    array,
    object,
};

/// Forward declaration for recursive object members.
struct object_member;

/// Owns a JSON value, including its strings and child values.
struct value {
    value_kind kind{value_kind::null};
    bool boolean{};
    std::int64_t signed_integer{};
    std::uint64_t unsigned_integer{};
    double floating_point{};
    std::string string;
    std::vector<value> array_elements;
    std::vector<object_member> object_members;

    /// Returns whether this value represents JSON null.
    [[nodiscard]] bool is_null() const noexcept {
        return kind == value_kind::null;
    }

    /// Returns whether this value is a JSON object.
    [[nodiscard]] bool is_object() const noexcept {
        return kind == value_kind::object;
    }

    /// Returns whether this value is a JSON array.
    [[nodiscard]] bool is_array() const noexcept {
        return kind == value_kind::array;
    }

    /// Returns whether this value is a JSON string.
    [[nodiscard]] bool is_string() const noexcept {
        return kind == value_kind::string;
    }

    /// Finds the first object member with this key. Returns nullptr for a
    /// missing key or a value that is not an object.
    [[nodiscard]] const value* find_member(std::string_view key) const noexcept;

    /// Returns a view of the stored string, or no value for another JSON type.
    [[nodiscard]] std::optional<std::string_view> read_string() const noexcept {
        if (kind == value_kind::string) {
            return std::string_view{string};
        }
        return std::nullopt;
    }

    /// Reads an integer that fits in int64_t. Other JSON types return no value.
    [[nodiscard]] std::optional<std::int64_t> read_signed_integer() const noexcept {
        if (kind == value_kind::signed_integer) {
            return signed_integer;
        }
        if (kind == value_kind::unsigned_integer &&
            unsigned_integer <=
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return static_cast<std::int64_t>(unsigned_integer);
        }
        return std::nullopt;
    }

    /// Reads a nonnegative integer that fits in uint64_t. Other types return no value.
    [[nodiscard]] std::optional<std::uint64_t> read_unsigned_integer() const noexcept {
        if (kind == value_kind::unsigned_integer) {
            return unsigned_integer;
        }
        if (kind == value_kind::signed_integer && signed_integer >= 0) {
            return static_cast<std::uint64_t>(signed_integer);
        }
        return std::nullopt;
    }

    /// Reads a number as double. Large integers may lose precision.
    [[nodiscard]] std::optional<double> read_floating_point() const noexcept {
        if (kind == value_kind::floating_point) {
            return floating_point;
        }
        if (kind == value_kind::signed_integer) {
            return static_cast<double>(signed_integer);
        }
        if (kind == value_kind::unsigned_integer) {
            return static_cast<double>(unsigned_integer);
        }
        return std::nullopt;
    }

    /// Reads a boolean, or returns no value for another JSON type.
    [[nodiscard]] std::optional<bool> read_boolean() const noexcept {
        if (kind == value_kind::boolean) {
            return boolean;
        }
        return std::nullopt;
    }

    /// Finds an object member and reads it as a string.
    [[nodiscard]] std::optional<std::string_view>
    read_string_member(std::string_view key) const noexcept {
        if (const value* child = find_member(key)) {
            return child->read_string();
        }
        return std::nullopt;
    }

    /// Finds an object member and reads it as a signed integer.
    [[nodiscard]] std::optional<std::int64_t>
    read_signed_integer_member(std::string_view key) const noexcept {
        if (const value* child = find_member(key)) {
            return child->read_signed_integer();
        }
        return std::nullopt;
    }

    /// Finds an object member and reads it as a boolean.
    [[nodiscard]] std::optional<bool> read_boolean_member(std::string_view key) const noexcept {
        if (const value* child = find_member(key)) {
            return child->read_boolean();
        }
        return std::nullopt;
    }

    /// Reads a string value or a string in the first array element.
    [[nodiscard]] std::optional<std::string_view> read_first_string() const noexcept {
        if (kind == value_kind::string) {
            return std::string_view{string};
        }
        if (kind == value_kind::array && !array_elements.empty()) {
            return array_elements.front().read_string();
        }
        return std::nullopt;
    }

    /// Reads a member as a string or as an array whose first element is a string.
    [[nodiscard]] std::optional<std::string_view>
    read_first_string_member(std::string_view key) const noexcept {
        if (const value* child = find_member(key)) {
            return child->read_first_string();
        }
        return std::nullopt;
    }

    /// Returns the stored array elements.
    [[nodiscard]] const std::vector<value>& get_elements() const noexcept {
        return array_elements;
    }

    /// Returns the stored object members.
    [[nodiscard]] const std::vector<object_member>& get_members() const noexcept {
        return object_members;
    }
};

/// Owns one key/value pair in a JSON object.
struct object_member {
    std::string key;
    value element;
};

inline const value* value::find_member(std::string_view key) const noexcept {
    if (kind != value_kind::object) {
        return nullptr;
    }
    for (const object_member& member : object_members) {
        if (member.key == key) {
            return &member.element;
        }
    }
    return nullptr;
}

/// Parses UTF-8 JSON into an owned tree.
/// @param bytes JSON source bytes.
/// @return The owned document or simdjson's diagnostic text.
[[nodiscard]] std::expected<value, std::string> parse_document(std::string_view bytes);

} // namespace bioseq::external::json
