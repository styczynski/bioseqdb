module;

#include <optional>
#include <string>
#include <string_view>
#include <utility>

module bioseq.codegen.website:markdown_extensions;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Bioseq Markdown extensions layered on MD4C's typed events (§10.3-10.4):
// admonition classification and `entity:` reference parsing. These helpers are
// source-neutral and shared by the markdown driver and later resolution stages.
namespace bioseq::codegen::website {

/// @brief Maps an admonition marker keyword to its rendered severity class.
/// @param keyword Uppercase leading token such as NOTE or WARNING.
/// @return The normalized class name, or empty for a non-admonition block.
[[nodiscard]] std::string admonition_class(std::string_view keyword) {
    if (keyword == "NOTE") return "note";
    if (keyword == "TIP") return "tip";
    if (keyword == "IMPORTANT") return "important";
    if (keyword == "WARNING") return "warning";
    if (keyword == "CAUTION") return "caution";
    return {};
}

/// @brief Parses an `entity:` reference target into a typed query.
/// @param spelling Raw link target such as `entity:sql:type:bioseq.seq`.
/// @param span Source span for diagnostics.
/// @return The typed query, or nullopt when the target is an ordinary link.
[[nodiscard]] std::optional<unresolved_entity_query> parse_entity_reference(
    std::string_view spelling,
    source_span span) {
    constexpr std::string_view prefix = "entity:";
    if (!spelling.starts_with(prefix)) {
        return std::nullopt;
    }
    std::string_view remainder = spelling.substr(prefix.size());
    unresolved_entity_query query;
    query.source = std::move(span);
    if (remainder.starts_with("cpp:")) {
        query.domain = entity_query_domain::cpp;
        remainder = remainder.substr(4);
    } else if (remainder.starts_with("sql:")) {
        query.domain = entity_query_domain::sql;
        remainder = remainder.substr(4);
    } else if (remainder.starts_with("id:")) {
        query.domain = entity_query_domain::canonical_id;
        remainder = remainder.substr(3);
    } else {
        query.domain = entity_query_domain::any;
    }
    query.spelling = std::string(remainder);
    if (query.spelling.empty()) {
        return std::nullopt;
    }
    return query;
}

}  // namespace bioseq::codegen::website
