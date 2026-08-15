module;

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:reference_index;

import :doxygen_reference;
import :cpp_reference_index;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Canonical C++ entity merge (§11-12). Combines the Clang structural/export
// identity (the authoritative page-owner set) with Doxygen's documentation
// comments into one reference_entity graph keyed by canonical ID, in
// deterministic order. SQL catalog entities are merged later by the postgres
// policy stage.
namespace bioseq::codegen::website {

/// @brief Per-module / SQL-domain documentation totals (§11.4 breakdown).
struct reference_coverage_group {
    std::string group;              // C++ module/top namespace, or "sql:<schema>"
    std::size_t public_total{};
    std::size_t documented_total{};
};

/// @brief Documentation-coverage summary for the configured public surface (§11.4).
struct reference_coverage_report {
    std::size_t public_total{};
    std::size_t documented_total{};
    std::vector<std::string> missing_summary;    // public, undocumented, non-fatal
    std::vector<std::string> missing_parameters; // functions with undocumented parameters
    std::vector<std::string> missing_returns;    // value-returning functions with no @return
    std::vector<std::string> fatal_missing;      // required/tutorial-referenced, undocumented
    std::vector<reference_coverage_group> groups;   // totals by module / SQL domain
};

/// @brief Maps an entity kind to its canonical-ID and route token (§12.1).
///
/// Centralized here so the canonical-ID kind tag and the output route segment
/// (build-pipeline route builder) are always the same token and never drift.
[[nodiscard]] std::string reference_kind_token(entity_kind kind) {
    switch (kind) {
    case entity_kind::namespace_: return "namespace";
    case entity_kind::module: return "module";
    case entity_kind::class_: return "class";
    case entity_kind::struct_: return "struct";
    case entity_kind::union_: return "union";
    case entity_kind::enum_: return "enum";
    case entity_kind::enum_value: return "enumerator";
    case entity_kind::concept_: return "concept";
    case entity_kind::type_alias: return "type_alias";
    case entity_kind::function: return "function";
    case entity_kind::variable: return "variable";
    case entity_kind::sql_schema: return "schema";
    case entity_kind::sql_type: return "type";
    case entity_kind::sql_function: return "function";
    case entity_kind::overload_group: return "overload_group";
    }
    return "entity";
}

/// @brief Computes the canonical ID for a documented entity (§12.1).
/// @param entity Entity whose kind/qualified name/signature seed the ID.
/// @return A normalized, kind-tagged, domain-prefixed canonical identifier.
///
/// The identifier is `<domain>:<kind>:<body>`. The body is the entity's
/// normalized signature for functions - so overloads such as two constructors
/// stay distinct - and its qualified name for every other kind. Both forms
/// preserve `::` and overload-distinguishing template arguments and carry no
/// parameter names or default arguments (the signature already carries the
/// cv/ref/noexcept qualifiers that distinguish overloads). Insignificant
/// whitespace is collapsed to single spaces and a leading `::` is stripped.
[[nodiscard]] std::string make_reference_entity_id(const reference_entity& entity) {
    const std::string prefix = entity.domain == entity_domain::sql ? "sql:" : "cpp:";
    const bool signature_keyed = entity.kind == entity_kind::function
        || entity.kind == entity_kind::sql_function;
    std::string_view body = (signature_keyed && !entity.normalized_signature.empty())
        ? std::string_view(entity.normalized_signature)
        : std::string_view(entity.qualified_name);
    while (body.starts_with("::")) {
        body.remove_prefix(2);
    }
    std::string normalized;
    normalized.reserve(body.size());
    bool pending_space = false;
    for (const char character : body) {
        const bool is_space = character == ' ' || character == '\t'
            || character == '\n' || character == '\r'
            || character == '\f' || character == '\v';
        if (is_space) {
            if (!normalized.empty()) {
                pending_space = true;
            }
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(character);
    }
    return prefix + reference_kind_token(entity.kind) + ":" + normalized;
}

namespace {

// Compares parameter lists by arity. Clang prints fully-qualified parameter
// types while Doxygen keeps the as-written (usually unqualified) spelling, so an
// exact type-text comparison almost never holds across the two producers; arity
// plus qualified-name-and-kind is the reliable name-based discriminator, with
// the location rule disambiguating same-arity overloads.
[[nodiscard]] bool same_parameter_arity(
    const reference_entity& lhs, const reference_entity& rhs) {
    return lhs.parameters.size() == rhs.parameters.size();
}

[[nodiscard]] bool within_two_lines(
    const reference_entity& lhs, const reference_entity& rhs) {
    if (!lhs.declaration || !rhs.declaration) {
        return false;
    }
    // Clang emits absolute paths; Doxygen emits STRIP_FROM_PATH-relative paths,
    // so compare by the final path component within the two-line tolerance.
    if (lhs.declaration->path.filename() != rhs.declaration->path.filename()) {
        return false;
    }
    const std::size_t a = lhs.declaration->first_line;
    const std::size_t b = rhs.declaration->first_line;
    return (a > b ? a - b : b - a) <= 2;
}

// Attaches Doxygen documentation onto a Clang entity (Clang keeps identity).
void attach_reference_content(reference_entity& target, const reference_entity& source) {
    target.summary_text = source.summary_text;
    target.detail_content = source.detail_content;
    for (parameter_description& parameter : target.parameters) {
        for (const parameter_description& documented : source.parameters) {
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
/// @return Canonical, canonical-ID-sorted reference_entity page graph.
[[nodiscard]] std::vector<reference_entity> merge_cpp_reference_entities(
    const cpp_reference_index& structural,
    const std::vector<reference_entity>& documented) {
    std::vector<reference_entity> merged = structural.entities;
    for (reference_entity& entity : merged) {
        if (entity.canonical_id.empty()) {
            entity.canonical_id = make_reference_entity_id(entity);
        }
    }
    std::vector<bool> clang_matched(merged.size(), false);
    std::vector<bool> doxygen_used(documented.size(), false);

    const auto apply_rule = [
                                &clang_matched,
                                &documented,
                                &doxygen_used,
                                &merged](auto predicate) -> void {
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
                attach_reference_content(merged[clang], documented[found]);
                clang_matched[clang] = true;
                doxygen_used[found] = true;
            }
        }
    };

    apply_rule([](const reference_entity& clang, const reference_entity& doxygen) {
        return !doxygen.normalized_signature.empty()
            && doxygen.normalized_signature == clang.normalized_signature;
    });
    apply_rule([](const reference_entity& clang, const reference_entity& doxygen) {
        return doxygen.qualified_name == clang.qualified_name
            && doxygen.kind == clang.kind && same_parameter_arity(clang, doxygen);
    });
    apply_rule([](const reference_entity& clang, const reference_entity& doxygen) {
        return within_two_lines(clang, doxygen);
    });

    for (std::size_t doxygen = 0; doxygen < documented.size(); ++doxygen) {
        if (doxygen_used[doxygen]) {
            continue;
        }
        for (const reference_entity& clang : merged) {
            if (clang.qualified_name == documented[doxygen].qualified_name) {
                throw std::runtime_error(
                    "documentation/signature mismatch for exported entity "
                    + clang.qualified_name + " [clang: " + clang.normalized_signature
                    + "] vs [doxygen: " + documented[doxygen].normalized_signature + "]");
            }
        }
    }

    std::ranges::sort(merged, [](const reference_entity& lhs,
                                 const reference_entity& rhs) {
        return lhs.canonical_id < rhs.canonical_id;
    });
    return merged;
}

// Does the documentation tree carry a Doxygen admonition of the given kind?
// @param/@return are projected as admonition nodes whose "kind" attribute is
// "param:<direction>"/"return" (see :doxygen_reference), so a prefix match locates
// any parameter documentation and an exact match locates the return documentation.
[[nodiscard]] bool detail_has_admonition(
    const std::vector<content_node>& nodes, std::string_view kind, bool prefix) {
    for (const content_node& node : nodes) {
        if (node.kind == content_node_kind::admonition) {
            if (const auto found = node.attributes.find("kind");
                found != node.attributes.end()
                && (prefix ? std::string_view(found->second).starts_with(kind)
                           : std::string_view(found->second) == kind)) {
                return true;
            }
        }
        if (detail_has_admonition(node.children, kind, prefix)) {
            return true;
        }
    }
    return false;
}

// Coverage bucket (§11.4 breakdown): SQL entities group by "sql:<schema>", C++
// entities by their owning module, falling back to the top namespace component.
[[nodiscard]] std::string coverage_group_of(const reference_entity& entity) {
    if (entity.domain == entity_domain::sql) {
        const std::size_t dot = entity.qualified_name.find('.');
        return "sql:" + (dot == std::string::npos ? std::string("public")
                                                   : entity.qualified_name.substr(0, dot));
    }
    if (entity.module_name && !entity.module_name->empty()) {
        return *entity.module_name;
    }
    const std::size_t sep = entity.qualified_name.find("::");
    return sep == std::string::npos ? entity.qualified_name
                                    : entity.qualified_name.substr(0, sep);
}

/// @brief Computes documentation coverage over the public surface (§11.4).
/// @param entities Canonical merged public entities.
/// @param required_ids Canonical IDs marked public-documentation-required.
/// @param referenced_ids Canonical IDs referenced by any tutorial.
/// @return The coverage report; missing required/referenced docs are fatal.
[[nodiscard]] reference_coverage_report evaluate_reference_coverage(
    const std::vector<reference_entity>& entities,
    const std::vector<std::string>& required_ids,
    const std::vector<std::string>& referenced_ids) {
    const auto listed = [](const std::vector<std::string>& ids, std::string_view id) {
        return std::ranges::find(ids, id) != ids.end();
    };
    reference_coverage_report report;
    for (const reference_entity& entity : entities) {
        ++report.public_total;
        const std::string group_key = coverage_group_of(entity);
        reference_coverage_group* group = nullptr;
        for (reference_coverage_group& candidate : report.groups) {
            if (candidate.group == group_key) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            report.groups.push_back(reference_coverage_group{group_key, 0, 0});
            group = &report.groups.back();
        }
        ++group->public_total;
        if (!entity.summary_text.empty()) {
            ++report.documented_total;
            ++group->documented_total;
        } else if (listed(required_ids, entity.canonical_id)
                   || listed(referenced_ids, entity.canonical_id)) {
            report.fatal_missing.push_back(entity.canonical_id);
        } else {
            report.missing_summary.push_back(entity.canonical_id);
        }
        // Parameter/return documentation applies to C++ functions, whose @param/@return
        // live in the Doxygen tree; SQL functions carry only a catalog summary.
        if (entity.domain == entity_domain::cpp
            && entity.kind == entity_kind::function) {
            if (!entity.parameters.empty()
                && !detail_has_admonition(entity.detail_content, "param:", true)) {
                report.missing_parameters.push_back(entity.canonical_id);
            }
            if (entity.return_type && *entity.return_type != "void"
                && !detail_has_admonition(entity.detail_content, "return", false)) {
                report.missing_returns.push_back(entity.canonical_id);
            }
        }
    }
    std::ranges::sort(report.fatal_missing);
    std::ranges::sort(report.missing_summary);
    std::ranges::sort(report.missing_parameters);
    std::ranges::sort(report.missing_returns);
    std::ranges::sort(report.groups, [](const reference_coverage_group& lhs,
                                        const reference_coverage_group& rhs) {
        return lhs.group < rhs.group;
    });
    return report;
}

}  // namespace bioseq::codegen::website
