module;

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

module bioseq.codegen.website:entity_index;

import :doxygen_xml;
import :clang_entity_index;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Canonical C++ entity merge (§11-12). Combines the Clang structural/export
// identity with Doxygen's documentation comments into one documented_entity
// graph keyed by canonical ID, in deterministic order. SQL catalog entities are
// merged later by the postgres policy stage.
namespace bioseq::codegen::website {

/// @brief Computes the canonical ID for a documented entity (§12.1).
/// @param entity Entity whose qualified name/domain seed the ID.
/// @return A normalized domain-prefixed canonical identifier.
[[nodiscard]] std::string canonical_entity_id(const documented_entity& entity) {
    const std::string prefix = entity.domain == entity_domain::sql ? "sql:" : "cpp:";
    std::string normalized;
    normalized.reserve(entity.qualified_name.size());
    bool previous_separator = false;
    for (const char character : entity.qualified_name) {
        if (character == ':' || character == ' ') {
            if (!previous_separator) {
                normalized.push_back('.');
                previous_separator = true;
            }
        } else {
            normalized.push_back(character);
            previous_separator = false;
        }
    }
    return prefix + normalized;
}

/// @brief Merges Clang structural entities with Doxygen documentation.
/// @param structural Clang structural/export index.
/// @param documented Doxygen-parsed entities (documentation source).
/// @return Canonical, canonical-ID-sorted documented_entity graph.
[[nodiscard]] std::vector<documented_entity> merge_cpp_entities(
    const clang_structural_index& structural,
    const std::vector<documented_entity>& documented) {
    std::vector<documented_entity> merged = structural.entities;
    for (const documented_entity& doxygen_entity : documented) {
        documented_entity entity = doxygen_entity;
        if (entity.canonical_id.empty()) {
            entity.canonical_id = canonical_entity_id(entity);
        }
        const auto existing = std::ranges::find_if(
            merged, [&](const documented_entity& candidate) {
                return candidate.qualified_name == entity.qualified_name;
            });
        if (existing == merged.end()) {
            merged.push_back(std::move(entity));
        } else {
            existing->summary_text = entity.summary_text;
            existing->documentation = entity.documentation;
            for (auto& [key, value] : entity.attributes) {
                existing->attributes.insert_or_assign(key, value);
            }
        }
    }
    std::ranges::sort(merged, [](const documented_entity& lhs,
                                 const documented_entity& rhs) {
        return lhs.canonical_id < rhs.canonical_id;
    });
    return merged;
}

}  // namespace bioseq::codegen::website
