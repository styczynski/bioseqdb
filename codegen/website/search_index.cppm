module;

#include <boost/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:search_index;

import :reference_index;
import :serialization;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Builds the static JSON search index from reference entries, tutorials,
// and index pages. The browser matches queries against the downloaded index.
namespace bioseq::codegen::website {

// Keep this token conversion in sync with website/static/js/search_tokens.mjs.
// Lowercase ASCII letters and split on ASCII punctuation or whitespace.
// Preserve non-ASCII UTF-8 bytes, including their case and Unicode spelling.
// Both implementations use website/tests/fixtures/search_tokens_golden.json.
[[nodiscard]] std::string normalize_search_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool token_started = false;
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 0x80) {
            if (byte >= 'A' && byte <= 'Z') {
                out.push_back(static_cast<char>(byte + 0x20));
                token_started = true;
            } else if ((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z')) {
                out.push_back(static_cast<char>(byte));
                token_started = true;
            } else if (token_started) {
                out.push_back(' ');
                token_started = false;
            }
        } else {
            out.push_back(static_cast<char>(byte));
            token_started = true;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// Reject invalid UTF-8 before writing a search record, including overlong
// encodings, surrogate code points, and values beyond the Unicode range.
[[nodiscard]] bool is_valid_utf8(std::string_view text) {
    std::size_t index = 0;
    const std::size_t size = text.size();
    while (index < size) {
        const auto byte = static_cast<unsigned char>(text[index]);
        std::size_t extra = 0;
        std::uint32_t code_point = 0;
        std::uint32_t lower_bound = 0;
        if (byte < 0x80) {
            index += 1;
            continue;
        }
        if ((byte & 0xE0) == 0xC0) {
            extra = 1;
            code_point = byte & 0x1F;
            lower_bound = 0x80;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
            code_point = byte & 0x0F;
            lower_bound = 0x800;
        } else if ((byte & 0xF8) == 0xF0) {
            extra = 3;
            code_point = byte & 0x07;
            lower_bound = 0x10000;
        } else {
            return false;
        }
        if (index + extra >= size) {
            return false;
        }
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        if (code_point < lower_bound || code_point > 0x10FFFF
            || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return false;
        }
        index += extra + 1;
    }
    return true;
}

// Search entry with separate name and description text for ranking.
// Those two fields become normalized tokens. Display text stays unchanged.
struct search_index_entry {
    std::string id;
    std::string group;  // "Tutorials" | "SQL" | "C++" | "Pages"
    std::string kind;
    std::string title;
    std::string qualified_name;
    std::string route;
    std::string summary;
    std::string name_source;
    std::string text_source;
};

// Returns the entity-kind label shown in search results.
[[nodiscard]] std::string_view kind_label(entity_kind kind) {
    switch (kind) {
    case entity_kind::namespace_: return "namespace";
    case entity_kind::module: return "module";
    case entity_kind::class_: return "class";
    case entity_kind::struct_: return "struct";
    case entity_kind::union_: return "union";
    case entity_kind::enum_: return "enum";
    case entity_kind::enum_value: return "enum_value";
    case entity_kind::concept_: return "concept";
    case entity_kind::type_alias: return "type_alias";
    case entity_kind::function: return "function";
    case entity_kind::variable: return "variable";
    case entity_kind::sql_schema: return "sql_schema";
    case entity_kind::sql_type: return "sql_type";
    case entity_kind::sql_function: return "sql_function";
    case entity_kind::overload_group: return "overload_group";
    }
    return "unknown";
}

/// @brief Collects text from heading nodes in the supplied list.
/// @param nodes Document nodes to inspect.
/// @return Heading text joined with spaces for tutorial search.
[[nodiscard]] std::string collect_heading_text(const std::vector<content_node>& nodes) {
    std::string joined;
    const auto append_text = [&joined](const content_node& node, auto&& self) -> void {
        if (!node.text.empty()) {
            if (!joined.empty()) {
                joined.push_back(' ');
            }
            joined += node.text;
        }
        for (const content_node& child : node.children) {
            self(child, self);
        }
    };
    for (const content_node& node : nodes) {
        if (node.kind == content_node_kind::heading) {
            append_text(node, append_text);
        }
    }
    return joined;
}

/// @brief Builds a search entry for a reference page.
/// @param entity Reference entry with an assigned route.
/// @return Display fields and text to index.
[[nodiscard]] search_index_entry build_entity_search_entry(const reference_entity& entity) {
    search_index_entry entry;
    entry.id = entity.canonical_id;
    entry.group = entity.domain == entity_domain::sql ? "SQL" : "C++";
    entry.kind = std::string(kind_label(entity.kind));
    entry.title = entity.display_name;
    entry.qualified_name = entity.qualified_name;
    entry.route = entity.route_path;
    entry.summary = entity.summary_text;
    std::string names = entity.display_name;
    names += ' ';
    names += entity.qualified_name;
    for (const parameter_description& parameter : entity.parameters) {
        names += ' ';
        names += parameter.name;
    }
    entry.name_source = std::move(names);
    std::string text = entity.summary_text;
    if (!entity.normalized_signature.empty()) {
        text += ' ';
        text += entity.normalized_signature;
    }
    if (entity.module_name) {
        text += ' ';
        text += *entity.module_name;
    }
    entry.text_source = std::move(text);
    return entry;
}

/// @brief Writes the static search index as JSON.
///
/// Sorts entries by route and ID, then normalizes search tokens. Rejects
/// duplicate IDs, empty routes, and invalid UTF-8 in the indexed text.
/// @param entries Entries from reference pages, tutorials, and index pages.
/// @return JSON escaped for HTML embedding, with a final newline.
[[nodiscard]] std::string serialize_search_index(std::vector<search_index_entry> entries) {
    std::sort(entries.begin(), entries.end(),
              [](const search_index_entry& lhs, const search_index_entry& rhs) {
                  if (lhs.route != rhs.route) {
                      return lhs.route < rhs.route;
                  }
                  return lhs.id < rhs.id;
              });
    std::set<std::string, std::less<>> seen_ids;
    boost::json::array records;
    for (const search_index_entry& entry : entries) {
        if (entry.route.empty()) {
            throw std::runtime_error("search record missing target route: " + entry.id);
        }
        if (!seen_ids.insert(entry.id).second) {
            throw std::runtime_error("duplicate search record id: " + entry.id);
        }
        for (const std::string* field :
             {&entry.title, &entry.qualified_name, &entry.summary,
              &entry.name_source, &entry.text_source}) {
            if (!is_valid_utf8(*field)) {
                throw std::runtime_error("invalid UTF-8 in search record: " + entry.id);
            }
        }
        boost::json::object object;
        object["id"] = entry.id;
        object["group"] = entry.group;
        object["kind"] = entry.kind;
        object["title"] = entry.title;
        object["qualified_name"] = entry.qualified_name;
        object["route"] = entry.route;
        object["summary"] = entry.summary;
        object["name_tokens"] = normalize_search_text(entry.name_source);
        object["text_tokens"] = normalize_search_text(entry.text_source);
        records.push_back(std::move(object));
    }
    boost::json::object document;
    document["schema"] = "bioseq.website.search";
    document["schema_revision"] = 2;
    document["records"] = std::move(records);
    return serialize_canonical(document);
}

}  // namespace bioseq::codegen::website
