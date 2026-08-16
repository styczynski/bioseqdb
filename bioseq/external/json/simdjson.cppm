/**
 * @file external/json/simdjson.cppm
 * @brief simdjson conversion into the provider-owned JSON value model.
 */
module;

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

module bioseq.external.json;

namespace bioseq::external::json {
namespace {

/// Recursively converts one simdjson DOM element into the owned value model.
[[nodiscard]] value convert_element(simdjson::dom::element element) {
    value converted;
    switch (element.type()) {
    case simdjson::dom::element_type::ARRAY: {
        converted.kind = value_kind::array;
        simdjson::dom::array array;
        if (element.get(array) == simdjson::SUCCESS) {
            for (simdjson::dom::element child : array) {
                converted.array_elements.push_back(convert_element(child));
            }
        }
        break;
    }
    case simdjson::dom::element_type::OBJECT: {
        converted.kind = value_kind::object;
        simdjson::dom::object object;
        if (element.get(object) == simdjson::SUCCESS) {
            for (simdjson::dom::key_value_pair field : object) {
                object_member member;
                member.key.assign(field.key.data(), field.key.size());
                member.element = convert_element(field.value);
                converted.object_members.push_back(std::move(member));
            }
        }
        break;
    }
    case simdjson::dom::element_type::STRING: {
        converted.kind = value_kind::string;
        std::string_view text;
        if (element.get(text) == simdjson::SUCCESS) {
            converted.string.assign(text.data(), text.size());
        }
        break;
    }
    case simdjson::dom::element_type::INT64: {
        converted.kind = value_kind::signed_integer;
        std::int64_t number = 0;
        if (element.get(number) == simdjson::SUCCESS) {
            converted.signed_integer = number;
        }
        break;
    }
    case simdjson::dom::element_type::UINT64: {
        converted.kind = value_kind::unsigned_integer;
        std::uint64_t number = 0;
        if (element.get(number) == simdjson::SUCCESS) {
            converted.unsigned_integer = number;
        }
        break;
    }
    case simdjson::dom::element_type::DOUBLE: {
        converted.kind = value_kind::floating_point;
        double number = 0.0;
        if (element.get(number) == simdjson::SUCCESS) {
            converted.floating_point = number;
        }
        break;
    }
    case simdjson::dom::element_type::BOOL: {
        converted.kind = value_kind::boolean;
        bool flag = false;
        if (element.get(flag) == simdjson::SUCCESS) {
            converted.boolean = flag;
        }
        break;
    }
    case simdjson::dom::element_type::NULL_VALUE:
    default:
        converted.kind = value_kind::null;
        break;
    }
    return converted;
}

} // namespace

/// @copydoc bioseq::external::json::parse_document
std::expected<value, std::string> parse_document(std::string_view bytes) {
    simdjson::dom::parser parser;
    simdjson::padded_string buffer(bytes);
    simdjson::dom::element document;
    const auto status = parser.parse(buffer).get(document);
    if (status != simdjson::SUCCESS) {
        return std::unexpected(std::string{simdjson::error_message(status)});
    }
    return convert_element(document);
}

} // namespace bioseq::external::json
