// DOC0372 — the single classic translation unit that is allowed to include
// simdjson. It materializes a parsed simdjson DOM into the owned
// bioseq::external::json::value tree declared in json_dom.hpp, keeping the
// simdjson serializer templates (which expose a TU-local escape_sequence) out of
// every module CMI. Mirrors the repo's existing simdjson usage in
// bioseq/io/io_bench_v10_main.cpp: simdjson is a plain include here, never an
// import.
#include <simdjson.h>

#include <string>
#include <string_view>
#include <utility>

#include "json_dom.hpp"

namespace bioseq::external::json {

const value* value::find(std::string_view key) const noexcept {
    if (tag != kind::object_v) {
        return nullptr;
    }
    for (const member& entry : object_members) {
        if (entry.key == key) {
            return &entry.val;
        }
    }
    return nullptr;
}

namespace {

value convert(simdjson::dom::element element) {
    value node;
    switch (element.type()) {
        case simdjson::dom::element_type::ARRAY: {
            node.tag = kind::array_v;
            simdjson::dom::array array;
            if (element.get(array) == simdjson::SUCCESS) {
                for (simdjson::dom::element child : array) {
                    node.array_items.push_back(convert(child));
                }
            }
            break;
        }
        case simdjson::dom::element_type::OBJECT: {
            node.tag = kind::object_v;
            simdjson::dom::object object;
            if (element.get(object) == simdjson::SUCCESS) {
                for (simdjson::dom::key_value_pair field : object) {
                    member entry;
                    entry.key.assign(field.key.data(), field.key.size());
                    entry.val = convert(field.value);
                    node.object_members.push_back(std::move(entry));
                }
            }
            break;
        }
        case simdjson::dom::element_type::STRING: {
            node.tag = kind::string_v;
            std::string_view text;
            if (element.get(text) == simdjson::SUCCESS) {
                node.string_value.assign(text.data(), text.size());
            }
            break;
        }
        case simdjson::dom::element_type::INT64: {
            node.tag = kind::int_v;
            std::int64_t number = 0;
            if (element.get(number) == simdjson::SUCCESS) {
                node.int_value = number;
            }
            break;
        }
        case simdjson::dom::element_type::UINT64: {
            node.tag = kind::uint_v;
            std::uint64_t number = 0;
            if (element.get(number) == simdjson::SUCCESS) {
                node.uint_value = number;
            }
            break;
        }
        case simdjson::dom::element_type::DOUBLE: {
            node.tag = kind::double_v;
            double number = 0.0;
            if (element.get(number) == simdjson::SUCCESS) {
                node.double_value = number;
            }
            break;
        }
        case simdjson::dom::element_type::BOOL: {
            node.tag = kind::bool_v;
            bool flag = false;
            if (element.get(flag) == simdjson::SUCCESS) {
                node.bool_value = flag;
            }
            break;
        }
        case simdjson::dom::element_type::NULL_VALUE:
        default:
            node.tag = kind::null_v;
            break;
    }
    return node;
}

}  // namespace

std::optional<value> parse(std::string_view bytes, std::string& error) {
    simdjson::dom::parser parser;
    simdjson::padded_string buffer(bytes);
    simdjson::dom::element document;
    const auto status = parser.parse(buffer).get(document);
    if (status != simdjson::SUCCESS) {
        error.assign(simdjson::error_message(status));
        return std::nullopt;
    }
    return convert(document);
}

}  // namespace bioseq::external::json
