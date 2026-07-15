module;

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:entity_index;

import :doxygen_xml;
import :clang_entity_index;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Canonical C++ entity merge (§11-12). Combines the Clang structural/export
// identity (the authoritative page-owner set) with Doxygen's documentation
// comments into one documented_entity graph keyed by canonical ID, in
// deterministic order. SQL catalog entities are merged later by the postgres
// policy stage.
namespace bioseq::codegen::website {

/// @brief Documentation-coverage summary for the configured public surface (§11.4).
struct coverage_report {
    std::size_t public_total{};
    std::size_t documented_total{};
    std::vector<std::string> missing_summary;   // public, undocumented, non-fatal
    std::vector<std::string> fatal_missing;     // required/tutorial-referenced, undocumented
};

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

namespace {

[[nodiscard]] bool same_parameter_types(
    const documented_entity& lhs, const documented_entity& rhs) {
    if (lhs.parameters.size() != rhs.parameters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.parameters.size(); ++index) {
        if (lhs.parameters[index].type != rhs.parameters[index].type) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool within_two_lines(
    const documented_entity& lhs, const documented_entity& rhs) {
    if (!lhs.declaration || !rhs.declaration) {
        return false;
    }
    if (lhs.declaration->path != rhs.declaration->path) {
        return false;
    }
    const std::size_t a = lhs.declaration->first_line;
    const std::size_t b = rhs.declaration->first_line;
    return (a > b ? a - b : b - a) <= 2;
}

// Attaches Doxygen documentation onto a Clang entity (Clang keeps identity).
void attach_documentation(documented_entity& target, const documented_entity& source) {
    target.summary_text = source.summary_text;
    target.documentation = source.documentation;
    for (parameter_documentation& parameter : target.parameters) {
        for (const parameter_documentation& documented : source.parameters) {
            if (documented.name == parameter.name && !documented.description.empty()) {
                parameter.description = documented.description;
            }
        }
    }
    for (const auto& [key, value] : source.attributes) {
        target.attributes.emplace(key, value);
    }
}

}  // namespace

/// @brief Merges Clang structural entities with Doxygen documentation (§11.6).
///
/// Clang owns identity/visibility; the returned entities are exactly the Clang
/// exported/public page-owners, each enriched with its unique Doxygen match by,
/// in order: (1) exact normalized signature, (2) qualified name + kind +
/// parameter types, (3) declaration path/line within two lines. Each rule is
/// applied only when the match is unique. An unmatched Doxygen entity that
/// shares a Clang page-owner's qualified name is a fatal signature mismatch.
/// @param structural Clang structural/export index.
/// @param documented Doxygen-parsed entities (documentation source).
/// @return Canonical, canonical-ID-sorted documented_entity page graph.
[[nodiscard]] std::vector<documented_entity> merge_cpp_entities(
    const clang_structural_index& structural,
    const std::vector<documented_entity>& documented) {
    std::vector<documented_entity> merged = structural.entities;
    for (documented_entity& entity : merged) {
        if (entity.canonical_id.empty()) {
            entity.canonical_id = canonical_entity_id(entity);
        }
    }
    std::vector<bool> clang_matched(merged.size(), false);
    std::vector<bool> doxygen_used(documented.size(), false);

    const auto apply_rule = [&](auto predicate) {
        for (std::size_t clang = 0; clang < merged.size(); ++clang) {
            if (clang_matched[clang]) {
                continue;
            }
            std::size_t found = documented.size();
            std::size_t count = 0;
            for (std::size_t doxygen = 0; doxygen < documented.size(); ++doxygen) {
                if (!doxygen_used[doxygen]
                    && predicate(merged[clang], documented[doxygen])) {
                    found = doxygen;
                    ++count;
                }
            }
            if (count == 1) {
                attach_documentation(merged[clang], documented[found]);
                clang_matched[clang] = true;
                doxygen_used[found] = true;
            }
        }
    };

    apply_rule([](const documented_entity& clang, const documented_entity& doxygen) {
        return !doxygen.normalized_signature.empty()
            && doxygen.normalized_signature == clang.normalized_signature;
    });
    apply_rule([](const documented_entity& clang, const documented_entity& doxygen) {
        return doxygen.qualified_name == clang.qualified_name
            && doxygen.kind == clang.kind && same_parameter_types(clang, doxygen);
    });
    apply_rule([](const documented_entity& clang, const documented_entity& doxygen) {
        return within_two_lines(clang, doxygen);
    });

    for (std::size_t doxygen = 0; doxygen < documented.size(); ++doxygen) {
        if (doxygen_used[doxygen]) {
            continue;
        }
        for (const documented_entity& clang : merged) {
            if (clang.qualified_name == documented[doxygen].qualified_name) {
                throw std::runtime_error(
                    "documentation/signature mismatch for exported entity "
                    + clang.qualified_name + " [clang: " + clang.normalized_signature
                    + "] vs [doxygen: " + documented[doxygen].normalized_signature + "]");
            }
        }
    }

    std::ranges::sort(merged, [](const documented_entity& lhs,
                                 const documented_entity& rhs) {
        return lhs.canonical_id < rhs.canonical_id;
    });
    return merged;
}

/// @brief Computes documentation coverage over the public surface (§11.4).
/// @param entities Canonical merged public entities.
/// @param required_ids Canonical IDs marked public-documentation-required.
/// @param referenced_ids Canonical IDs referenced by any tutorial.
/// @return The coverage report; missing required/referenced docs are fatal.
[[nodiscard]] coverage_report compute_coverage(
    const std::vector<documented_entity>& entities,
    const std::vector<std::string>& required_ids,
    const std::vector<std::string>& referenced_ids) {
    const auto listed = [](const std::vector<std::string>& ids, std::string_view id) {
        return std::ranges::find(ids, id) != ids.end();
    };
    coverage_report report;
    for (const documented_entity& entity : entities) {
        ++report.public_total;
        if (!entity.summary_text.empty()) {
            ++report.documented_total;
            continue;
        }
        if (listed(required_ids, entity.canonical_id)
            || listed(referenced_ids, entity.canonical_id)) {
            report.fatal_missing.push_back(entity.canonical_id);
        } else {
            report.missing_summary.push_back(entity.canonical_id);
        }
    }
    std::ranges::sort(report.fatal_missing);
    std::ranges::sort(report.missing_summary);
    return report;
}

}  // namespace bioseq::codegen::website
