module;

#include <boost/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

module bioseq.codegen.website:json;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Canonical JSON emission (§5.1). Uses the Boost.JSON already built for the
// website-enabled Jinja2Cpp component for every plan/manifest/search/tooltip/
// coverage artifact. Object keys are inserted in a fixed schema order by the
// caller; this unit guarantees the terminal newline and the HTML-context
// safety post-pass for <, >, &, U+2028, and U+2029.
namespace bioseq::codegen::website {

/// @brief Escapes JSON text that may be embedded in an HTML context.
/// @param serialized Canonical JSON text.
/// @return The text with <, >, &, U+2028, and U+2029 replaced by \uXXXX.
[[nodiscard]] std::string apply_html_safe_escapes(std::string_view serialized) {
    std::string result;
    result.reserve(serialized.size());
    for (std::size_t index = 0; index < serialized.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(serialized[index]);
        if (character == '<') {
            result += "\\u003c";
        } else if (character == '>') {
            result += "\\u003e";
        } else if (character == '&') {
            result += "\\u0026";
        } else if (character == 0xE2 && index + 2 < serialized.size()
                   && static_cast<unsigned char>(serialized[index + 1]) == 0x80
                   && (static_cast<unsigned char>(serialized[index + 2]) == 0xA8
                       || static_cast<unsigned char>(serialized[index + 2]) == 0xA9)) {
            result += static_cast<unsigned char>(serialized[index + 2]) == 0xA8
                ? "\\u2028"
                : "\\u2029";
            index += 2;
        } else {
            result.push_back(serialized[index]);
        }
    }
    return result;
}

/// @brief Serializes a Boost.JSON value into canonical HTML-safe artifact text.
/// @param value Fully-built JSON value with keys already in schema order.
/// @return Canonical serialization ending in exactly one newline.
[[nodiscard]] std::string serialize_canonical(const boost::json::value& value) {
    std::string serialized = boost::json::serialize(value);
    std::string safe = apply_html_safe_escapes(serialized);
    safe.push_back('\n');
    return safe;
}

/// @brief Parses artifact JSON text, rejecting trailing garbage.
/// @param text Canonical JSON text.
/// @return The parsed value.
[[nodiscard]] boost::json::value parse_json(std::string_view text) {
    boost::json::parse_options options;
    options.allow_comments = false;
    options.allow_trailing_commas = false;
    return boost::json::parse(text, {}, options);
}

}  // namespace bioseq::codegen::website
