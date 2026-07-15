module;

#include <boost/json.hpp>

#include <string>
#include <utility>
#include <vector>

module bioseq.codegen.website:search;

import :entity_index;
import :json;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Precomputed static search index (§12.5). Emits a small deterministic JSON
// document consumed by the accessible vanilla-JS search widget; no client
// framework and no server. Records derive from the one canonical entity graph.
namespace bioseq::codegen::website {

/// @brief Projects a documented entity onto its search record.
/// @param entity Canonical documented entity.
/// @return The search record used by the static index.
[[nodiscard]] search_record make_search_record(const documented_entity& entity) {
    search_record record;
    record.canonical_id = entity.canonical_id;
    record.title = entity.display_name;
    record.qualified_name = entity.qualified_name;
    record.summary = entity.summary_text;
    record.route_path = entity.route_path;
    record.keywords.reserve(entity.parameters.size());
    for (const parameter_documentation& parameter : entity.parameters) {
        record.keywords.push_back(parameter.name);
    }
    return record;
}

/// @brief Builds the static search index JSON for a set of entities.
/// @param entities Canonical documented entities.
/// @return Canonical HTML-safe JSON text ending in one newline.
[[nodiscard]] std::string build_search_index(
    const std::vector<documented_entity>& entities) {
    boost::json::array records;
    for (const documented_entity& entity : entities) {
        const search_record record = make_search_record(entity);
        boost::json::object object;
        object["id"] = record.canonical_id;
        object["title"] = record.title;
        object["qualified_name"] = record.qualified_name;
        object["summary"] = record.summary;
        object["route"] = record.route_path;
        records.push_back(std::move(object));
    }
    boost::json::object document;
    document["schema"] = "bioseq.website.search/v1";
    document["records"] = std::move(records);
    return serialize_canonical(document);
}

}  // namespace bioseq::codegen::website
