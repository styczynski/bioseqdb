// DOC0372 — a small, owned JSON DOM that isolates simdjson from the module
// boundary. simdjson's amalgamated header defines inline serializer templates
// (base_formatter<>::string) that reference an anonymous-namespace entity
// (escape_sequence); when the header lands in a C++26 named module's global
// module fragment, GCC rejects the CMI ("exposes TU-local entity"). The repo's
// established pattern (bioseq/io/io_bench_v10_main.cpp) only ever includes
// simdjson.h from a classic translation unit. This header therefore mentions no
// simdjson type: the provider adapter modules include it in their GMF and read
// this owned tree, while a single classic TU (json_dom.cpp) does the parse and
// materializes into it. Metadata payloads are small, so the extra copy is free.
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bioseq::external::json {

enum class kind : unsigned char {
    null_v,
    bool_v,
    int_v,
    uint_v,
    double_v,
    string_v,
    array_v,
    object_v,
};

struct member;  // key + value; defined after value so value stays first.

// An owned JSON value. Strings and children are owned, so string_view accessors
// stay valid for the value's lifetime. std::vector<member> over an incomplete
// member is well-formed since C++17 (member completes before any use).
struct value {
    kind tag{kind::null_v};
    bool bool_value{};
    std::int64_t int_value{};
    std::uint64_t uint_value{};
    double double_value{};
    std::string string_value;
    std::vector<value> array_items;
    std::vector<member> object_members;

    [[nodiscard]] bool is_null() const noexcept { return tag == kind::null_v; }
    [[nodiscard]] bool is_object() const noexcept {
        return tag == kind::object_v;
    }
    [[nodiscard]] bool is_array() const noexcept { return tag == kind::array_v; }
    [[nodiscard]] bool is_string() const noexcept {
        return tag == kind::string_v;
    }

    // Object member lookup. Returns nullptr when this is not an object or the
    // key is absent. Linear scan — object member counts here are tiny.
    [[nodiscard]] const value* find(std::string_view key) const noexcept;

    // Typed reads. Each yields nullopt on a type mismatch so callers degrade
    // gracefully rather than crash on an unexpected payload shape. Integers are
    // accepted across int/uint/double when they represent an exact value.
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept {
        if (tag == kind::string_v) {
            return std::string_view{string_value};
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::int64_t> as_int64() const noexcept {
        if (tag == kind::int_v) {
            return int_value;
        }
        if (tag == kind::uint_v &&
            uint_value <=
                static_cast<std::uint64_t>((std::numeric_limits<
                                            std::int64_t>::max)())) {
            return static_cast<std::int64_t>(uint_value);
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::uint64_t> as_uint64() const noexcept {
        if (tag == kind::uint_v) {
            return uint_value;
        }
        if (tag == kind::int_v && int_value >= 0) {
            return static_cast<std::uint64_t>(int_value);
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<double> as_double() const noexcept {
        if (tag == kind::double_v) {
            return double_value;
        }
        if (tag == kind::int_v) {
            return static_cast<double>(int_value);
        }
        if (tag == kind::uint_v) {
            return static_cast<double>(uint_value);
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<bool> as_bool() const noexcept {
        if (tag == kind::bool_v) {
            return bool_value;
        }
        return std::nullopt;
    }

    // Convenience: object field then typed read, in one call.
    [[nodiscard]] std::optional<std::string_view> string_at(
        std::string_view key) const noexcept {
        if (const value* child = find(key)) {
            return child->as_string();
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::int64_t> int_at(
        std::string_view key) const noexcept {
        if (const value* child = find(key)) {
            return child->as_int64();
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<bool> bool_at(
        std::string_view key) const noexcept {
        if (const value* child = find(key)) {
            return child->as_bool();
        }
        return std::nullopt;
    }

    // EBI Search returns each field as an array of strings; this reads the first
    // element as a string (or, if this value is itself a string, itself).
    [[nodiscard]] std::optional<std::string_view> first_string()
        const noexcept {
        if (tag == kind::string_v) {
            return std::string_view{string_value};
        }
        if (tag == kind::array_v && !array_items.empty()) {
            return array_items.front().as_string();
        }
        return std::nullopt;
    }
    // As first_string(), but selects the array's first element for an object
    // field named key (common EBI shape: fields.name -> ["value"]).
    [[nodiscard]] std::optional<std::string_view> first_string_at(
        std::string_view key) const noexcept {
        if (const value* child = find(key)) {
            return child->first_string();
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<value>& items() const noexcept {
        return array_items;
    }
    [[nodiscard]] const std::vector<member>& members() const noexcept {
        return object_members;
    }
};

struct member {
    std::string key;
    value val;
};

// Parse UTF-8 JSON bytes into an owned tree. Returns nullopt and fills error on
// malformed input. Implemented in json_dom.cpp (the only simdjson TU here).
[[nodiscard]] std::optional<value> parse(std::string_view bytes,
                                         std::string& error);

}  // namespace bioseq::external::json
